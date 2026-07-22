#include "sensitive_field_redactor.h"

#include <openssl/evp.h>

#include <mutex>
#include <utility>

namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

// One-shot SHA-256. string fields get the hex digest; bytes fields get the
// raw digest so protobuf JSON renders it as standard base64 rather than
// base64-of-hex.
std::string Sha256For(const FieldDescriptor& field, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &digest_len, EVP_sha256(), nullptr) != 1) {
        return "<sha256-error>";
    }
    if (field.type() == FieldDescriptor::TYPE_BYTES) {
        return std::string(reinterpret_cast<const char*>(digest), digest_len);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex(2 * digest_len, '\0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex[2 * i] = kHex[digest[i] >> 4];
        hex[2 * i + 1] = kHex[digest[i] & 0x0f];
    }
    return hex;
}

bool IsAnyType(const Descriptor* descriptor) {
    return descriptor->full_name() == "google.protobuf.Any";
}

// Generated-pool descriptors live for the whole process; anything else has a
// caller-owned lifetime and must never enter the redactor's shared caches.
bool IsGenerated(const Descriptor* descriptor) {
    return descriptor->file()->pool() == google::protobuf::DescriptorPool::generated_pool();
}

}  // namespace

SensitiveFieldRedactor::SensitiveFieldRedactor(std::unordered_set<std::string> sensitive_names)
    : sensitive_names_(std::move(sensitive_names)) {
}

bool SensitiveFieldRedactor::ContainsSensitive(const Descriptor& descriptor) const {
    DynamicScope scope;
    return !PlanFor(&descriptor, scope).Empty();
}

std::unique_ptr<Message> SensitiveFieldRedactor::Redact(const Message& message) const {
    DynamicScope scope;
    const Plan& plan = PlanFor(message.GetDescriptor(), scope);
    if (plan.Empty()) {
        return nullptr;
    }
    std::unique_ptr<Message> copy(message.New());
    copy->CopyFrom(message);
    ApplyPlan(copy.get(), plan, scope);
    return copy;
}

bool SensitiveFieldRedactor::IsSensitiveLeaf(const FieldDescriptor& field) const {
    if (sensitive_names_.count(std::string(field.name())) == 0) {
        return false;
    }
    if (field.is_map()) {
        return field.message_type()->map_value()->cpp_type() == FieldDescriptor::CPPTYPE_STRING;
    }
    return field.cpp_type() == FieldDescriptor::CPPTYPE_STRING;
}

bool SensitiveFieldRedactor::TypeContainsSensitive(const Descriptor* descriptor) const {
    std::vector<const Descriptor*> pending{descriptor};
    std::unordered_set<const Descriptor*> visited{descriptor};
    while (!pending.empty()) {
        const Descriptor* current = pending.back();
        pending.pop_back();
        for (int i = 0; i < current->field_count(); ++i) {
            const FieldDescriptor& field = *current->field(i);
            if (IsSensitiveLeaf(field)) {
                return true;
            }
            if (field.cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
                // An Any can pack anything, so it may always hold secrets.
                if (IsAnyType(field.message_type())) {
                    return true;
                }
                if (visited.insert(field.message_type()).second) {
                    pending.push_back(field.message_type());
                }
            }
        }
    }
    return false;
}

const SensitiveFieldRedactor::Plan& SensitiveFieldRedactor::PlanFor(const Descriptor* descriptor,
                                                                    DynamicScope& scope) const {
    if (IsGenerated(descriptor)) {
        return CachedPlanFor(descriptor);
    }
    auto it = scope.plans.find(descriptor);
    if (it != scope.plans.end()) {
        return *it->second;
    }
    return BuildDynamicPlan(descriptor, scope);
}

const SensitiveFieldRedactor::Plan& SensitiveFieldRedactor::CachedPlanFor(const Descriptor* descriptor) const {
    {
        std::shared_lock lock(mutex_);
        auto it = plans_.find(descriptor);
        if (it != plans_.end()) {
            return *it->second;
        }
    }
    std::unique_lock lock(mutex_);
    return BuildPlanLocked(descriptor);
}

// Pre-resolves each type's sensitive and nested fields once so ApplyPlan does
// no name matching or cache lookups. Requires mutex_ held exclusively; only
// generated-pool descriptors reach here (they can never reference dynamic
// types, so the recursion stays inside the shared cache). On a cycle the
// re-entered call returns the in-progress plan, which is complete by the time
// the outermost build releases the lock.
SensitiveFieldRedactor::Plan& SensitiveFieldRedactor::BuildPlanLocked(const Descriptor* descriptor) const {
    auto [it, inserted] = plans_.try_emplace(descriptor);
    if (!inserted) {
        return *it->second;
    }
    it->second = std::make_unique<Plan>();
    Plan& plan = *it->second;  // stable address; recursion below may rehash and invalidate `it`
    FillPlan(plan, descriptor,
             [this](const Descriptor* child) -> const Plan* { return &BuildPlanLocked(child); });
    return plan;
}

// Same as BuildPlanLocked but for caller-owned descriptors: plans live in the
// per-call scope, and children may be generated (shared cache) or dynamic.
SensitiveFieldRedactor::Plan& SensitiveFieldRedactor::BuildDynamicPlan(const Descriptor* descriptor,
                                                                       DynamicScope& scope) const {
    auto [it, inserted] = scope.plans.try_emplace(descriptor);
    if (!inserted) {
        return *it->second;
    }
    it->second = std::make_unique<Plan>();
    Plan& plan = *it->second;  // stable address; recursion below may rehash and invalidate `it`
    FillPlan(plan, descriptor, [this, &scope](const Descriptor* child) -> const Plan* {
        return IsGenerated(child) ? &CachedPlanFor(child) : &BuildDynamicPlan(child, scope);
    });
    return plan;
}

void SensitiveFieldRedactor::FillPlan(
    Plan& plan, const Descriptor* descriptor,
    const std::function<const Plan*(const Descriptor*)>& plan_for_child) const {
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const FieldDescriptor* field = descriptor->field(i);
        if (IsSensitiveLeaf(*field)) {
            plan.sensitive_fields.push_back(field);
        } else if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
            continue;
        } else if (IsAnyType(field->message_type())) {
            plan.any_fields.push_back(field);
        } else if (TypeContainsSensitive(field->message_type())) {
            plan.nested_fields.push_back({field, plan_for_child(field->message_type())});
        }
    }
}

void SensitiveFieldRedactor::ApplyPlan(Message* message, const Plan& plan,
                                       DynamicScope& scope) const {
    const Reflection* reflection = message->GetReflection();
    std::string scratch;
    for (const FieldDescriptor* field : plan.sensitive_fields) {
        if (field->is_map()) {
            // Matching map field with string/bytes values: hash each value.
            const FieldDescriptor* value_field = field->message_type()->map_value();
            const int size = reflection->FieldSize(*message, field);
            for (int i = 0; i < size; ++i) {
                Message* entry = reflection->MutableRepeatedMessage(message, field, i);
                const Reflection* entry_reflection = entry->GetReflection();
                const std::string& value =
                    entry_reflection->GetStringReference(*entry, value_field, &scratch);
                if (!value.empty()) {
                    std::string hashed = Sha256For(*value_field, value);
                    entry_reflection->SetString(entry, value_field, std::move(hashed));
                }
            }
        } else if (field->is_repeated()) {
            const int size = reflection->FieldSize(*message, field);
            for (int i = 0; i < size; ++i) {
                const std::string& value =
                    reflection->GetRepeatedStringReference(*message, field, i, &scratch);
                if (!value.empty()) {
                    std::string hashed = Sha256For(*field, value);
                    reflection->SetRepeatedString(message, field, i, std::move(hashed));
                }
            }
        } else {
            const std::string& value = reflection->GetStringReference(*message, field, &scratch);
            if (!value.empty()) {
                std::string hashed = Sha256For(*field, value);
                reflection->SetString(message, field, std::move(hashed));
            }
        }
    }
    for (const NestedField& nested : plan.nested_fields) {
        if (nested.field->is_repeated()) {  // includes maps with message values
            const int size = reflection->FieldSize(*message, nested.field);
            for (int i = 0; i < size; ++i) {
                ApplyPlan(reflection->MutableRepeatedMessage(message, nested.field, i),
                          *nested.plan, scope);
            }
        } else if (reflection->HasField(*message, nested.field)) {
            ApplyPlan(reflection->MutableMessage(message, nested.field), *nested.plan, scope);
        }
    }
    for (const FieldDescriptor* field : plan.any_fields) {
        if (field->is_repeated()) {
            const int size = reflection->FieldSize(*message, field);
            for (int i = 0; i < size; ++i) {
                RedactAnyInPlace(reflection->MutableRepeatedMessage(message, field, i), scope);
            }
        } else if (reflection->HasField(*message, field)) {
            RedactAnyInPlace(reflection->MutableMessage(message, field), scope);
        }
    }
}

// Unpacks a google.protobuf.Any, redacts the payload, and re-packs it. JSON
// logging expands Any payloads, so they must be redacted like inline fields.
// The packed type is resolved in the pool the Any's descriptor came from —
// which is what the JSON serializer uses too — so generated and dynamic
// descriptors both redact. An unresolvable type_url is left alone because
// JSON expansion fails the same lookup and logs nothing; a payload that
// cannot be parsed is likewise left alone because JSON expansion fails on it
// identically.
void SensitiveFieldRedactor::RedactAnyInPlace(google::protobuf::Message* any,
                                              DynamicScope& scope) const {
    const Descriptor* any_descriptor = any->GetDescriptor();
    const FieldDescriptor* url_field = any_descriptor->FindFieldByName("type_url");
    const FieldDescriptor* value_field = any_descriptor->FindFieldByName("value");
    const Reflection* reflection = any->GetReflection();

    std::string scratch;
    const std::string& type_url = reflection->GetStringReference(*any, url_field, &scratch);
    const size_t slash = type_url.find_last_of('/');
    const std::string type_name =
        slash == std::string::npos ? type_url : type_url.substr(slash + 1);
    const Descriptor* packed_descriptor =
        any_descriptor->file()->pool()->FindMessageTypeByName(type_name);
    if (!packed_descriptor) {
        return;
    }
    const Plan& packed_plan = PlanFor(packed_descriptor, scope);
    if (packed_plan.Empty()) {
        return;  // packed type is clean: skip the parse entirely
    }

    // The scope's factory only lives for this call, during which the message
    // (and thus its descriptor pool) is necessarily alive — satisfying the
    // DynamicMessageFactory requirement that descriptors outlive the factory.
    const Message* prototype = nullptr;
    if (IsGenerated(packed_descriptor)) {
        prototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(packed_descriptor);
    }
    if (!prototype) {
        prototype = scope.factory.GetPrototype(packed_descriptor);
    }
    if (!prototype) {
        // Fail closed: JSON can still expand the payload through the Any's
        // pool, so an unredactable payload must not survive.
        reflection->SetString(any, value_field, "");
        return;
    }
    std::unique_ptr<Message> unpacked(prototype->New());
    std::string value_scratch;
    const std::string& value =
        reflection->GetStringReference(*any, value_field, &value_scratch);
    if (!unpacked->ParseFromString(value)) {
        return;
    }
    ApplyPlan(unpacked.get(), packed_plan, scope);
    reflection->SetString(any, value_field, unpacked->SerializeAsString());
}
