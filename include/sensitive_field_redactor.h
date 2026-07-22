#pragma once

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Replaces the values of sensitive protobuf fields — matched by exact field
// name (e.g. "password", "pwd") — with their SHA-256 digest so messages can
// be logged without exposing secrets. Matches string and bytes fields
// (singular, repeated, and map values of a matching map field) at any nesting
// depth. string fields get the hex digest; bytes fields get the raw digest,
// which protobuf JSON renders as its standard base64. google.protobuf.Any
// payloads are unpacked, redacted, and re-packed, since JSON logging expands
// them: packed types are resolved in the Any's own descriptor pool
// (generated or dynamic); if no prototype can be built the payload value is
// cleared (fail closed), and unresolvable type_urls are left alone — JSON
// expansion fails the same lookup, so nothing leaks.
//
// A redaction plan is compiled per message type on first sight and cached, so
// types with no sensitive field (and no Any) anywhere pay only one map lookup
// per Redact call and are logged without copying. Only descriptors from the
// generated pool (process lifetime) enter that cache; descriptors from
// caller-owned pools are planned per call and never retained, so such pools
// need not outlive the redactor. Thread-safe; one instance is shared by every
// interceptor a factory creates.
class SensitiveFieldRedactor {
public:
    static std::unordered_set<std::string> DefaultSensitiveNames() {
        return {"password", "pwd"};
    }

    explicit SensitiveFieldRedactor(std::unordered_set<std::string> sensitive_names = DefaultSensitiveNames());

    // True if messages of this type can transitively contain a sensitive field.
    bool ContainsSensitive(const google::protobuf::Descriptor& descriptor) const;

    // Returns nullptr when the message's type holds no sensitive fields (log
    // the original as-is); otherwise returns a copy with every non-empty
    // sensitive value replaced by its SHA-256 digest: hex for string fields,
    // the raw digest for bytes fields (base64 in the JSON log).
    std::unique_ptr<google::protobuf::Message> Redact(const google::protobuf::Message& message) const;

private:
    struct Plan;
    struct NestedField {
        const google::protobuf::FieldDescriptor* field;
        const Plan* plan;
    };
    struct Plan {
        // string/bytes fields (or map fields with string/bytes values) to hash.
        std::vector<const google::protobuf::FieldDescriptor*> sensitive_fields;
        // message fields whose type transitively contains sensitive fields.
        std::vector<NestedField> nested_fields;
        // google.protobuf.Any fields; their packed type is only known at
        // redaction time, so their plan is resolved then.
        std::vector<const google::protobuf::FieldDescriptor*> any_fields;

        bool Empty() const {
            return sensitive_fields.empty() && nested_fields.empty() && any_fields.empty();
        }
    };

    // Call-scoped state for descriptors from non-generated pools. Their
    // lifetime belongs to the caller, so no plan or prototype derived from
    // them may outlive the Redact/ContainsSensitive call that saw them
    // (DynamicMessageFactory requires its descriptors to outlive it).
    struct DynamicScope {
        std::unordered_map<const google::protobuf::Descriptor*, std::unique_ptr<Plan>> plans;
        google::protobuf::DynamicMessageFactory factory;
    };

    const Plan& PlanFor(const google::protobuf::Descriptor* descriptor, DynamicScope& scope) const;
    const Plan& CachedPlanFor(const google::protobuf::Descriptor* descriptor) const;
    Plan& BuildPlanLocked(const google::protobuf::Descriptor* descriptor) const;
    Plan& BuildDynamicPlan(const google::protobuf::Descriptor* descriptor, DynamicScope& scope) const;
    void FillPlan(Plan& plan, const google::protobuf::Descriptor* descriptor,
                  const std::function<const Plan*(const google::protobuf::Descriptor*)>&
                      plan_for_child) const;
    bool IsSensitiveLeaf(const google::protobuf::FieldDescriptor& field) const;
    bool TypeContainsSensitive(const google::protobuf::Descriptor* descriptor) const;
    void ApplyPlan(google::protobuf::Message* message, const Plan& plan, DynamicScope& scope) const;
    void RedactAnyInPlace(google::protobuf::Message* any, DynamicScope& scope) const;

    std::unordered_set<std::string> sensitive_names_;
    mutable std::shared_mutex mutex_;
    // Shared cache for generated-pool descriptors only; those live for the
    // whole process, so caching them is safe. unique_ptr keeps Plan addresses
    // stable so NestedField::plan can point at sibling entries across rehashes.
    mutable std::unordered_map<const google::protobuf::Descriptor*, std::unique_ptr<Plan>> plans_;
};
