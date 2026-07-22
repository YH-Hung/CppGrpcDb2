#include "sensitive_field_redactor.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <grpcpp/impl/codegen/config_protobuf.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "redaction_probe.pb.h"

namespace {

using redaction_probe::AnyEnvelope;
using redaction_probe::CleanRequest;
using redaction_probe::Credentials;
using redaction_probe::ProbeRequest;

// Digests produced with: printf '%s' <value> | shasum -a 256
constexpr char kHunter2Sha[] =
    "f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7";
constexpr char kS3cretSha[] =
    "1ec1c26b50d5d3c58d9583181af8076655fe00756bf7285940ba3670f99fcba0";
constexpr char kBytepwdSha[] =
    "d1db826f38df31555d222ae6c6771e0efe389a7db73213eb2fed7591e5eda143";
constexpr char kMapV1Sha[] =
    "84ad90502054d480a6b2dfea062386c7a4ae8230a7e0dd8ed000c040f36aca51";
constexpr char kMapV2Sha[] =
    "4f76dd297953f38390adee229159958bcefd9cc101ad692fadd9666789c72128";
constexpr char kRepASha[] =
    "dcaec3c0813a26c95608c4df799f121aa5cce8e846637c3af25d4f1c2d024269";
constexpr char kRepBSha[] =
    "52c0da213b004164b94eb3841b0b1649e4c33d8a43e568666d972d9ad06efba4";
constexpr char kKey123Sha[] =
    "8fefe692f690a3173176ecdff4318225afaeb97fdd6f60c866ed823d59221665";
// printf '%s' bytepwd | openssl dgst -sha256 -binary | base64
// (bytes fields hold the raw digest; protobuf JSON base64-encodes it)
constexpr char kBytepwdShaB64[] = "0duCbzjfMVVdIirmxnceDv44mn23MhPrL+11keXtoUM=";

// Redacts msg and downcasts the copy back to its concrete type.
template <typename T>
T RedactAs(const SensitiveFieldRedactor& redactor, const T& msg) {
    std::unique_ptr<google::protobuf::Message> redacted = redactor.Redact(msg);
    EXPECT_NE(redacted, nullptr);
    T result;
    if (redacted) result.CopyFrom(*redacted);
    return result;
}

// The exact serialization the logging interceptor performs.
std::string ToJson(const google::protobuf::Message& msg) {
    grpc::protobuf::json::JsonPrintOptions options;
    options.preserve_proto_field_names = true;
    std::string json;
    const auto status = grpc::protobuf::json::MessageToJsonString(msg, &json, options);
    EXPECT_TRUE(status.ok()) << status.ToString();
    return json;
}

std::string HexDecode(const std::string& hex) {
    std::string bytes(hex.size() / 2, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    }
    return bytes;
}

TEST(SensitiveFieldRedactor, CleanTypeNeedsNoRedaction) {
    SensitiveFieldRedactor redactor;

    CleanRequest msg;
    msg.set_name("bob");
    msg.set_count(3);

    EXPECT_FALSE(redactor.ContainsSensitive(*CleanRequest::descriptor()));
    EXPECT_EQ(redactor.Redact(msg), nullptr);
}

TEST(SensitiveFieldRedactor, HashesTopLevelPasswordAndKeepsOriginal) {
    SensitiveFieldRedactor redactor;

    ProbeRequest msg;
    msg.set_name("bob");
    msg.set_password("hunter2");

    EXPECT_TRUE(redactor.ContainsSensitive(*ProbeRequest::descriptor()));
    const ProbeRequest redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.name(), "bob");
    EXPECT_EQ(redacted.password(), kHunter2Sha);
    // The input message must never be mutated.
    EXPECT_EQ(msg.password(), "hunter2");
}

TEST(SensitiveFieldRedactor, LeavesUnsetPasswordEmpty) {
    SensitiveFieldRedactor redactor;

    ProbeRequest msg;
    msg.set_name("bob");

    const ProbeRequest redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.password(), "");
}

TEST(SensitiveFieldRedactor, HashesNestedRepeatedAndMapValues) {
    SensitiveFieldRedactor redactor;

    ProbeRequest msg;
    msg.set_name("bob");
    msg.mutable_credentials()->set_username("alice");
    msg.mutable_credentials()->set_password("s3cret");
    msg.mutable_credentials()->set_pwd("bytepwd");
    msg.add_cred_list()->set_password("hunter2");
    (*msg.mutable_cred_map())["svc"].set_password("s3cret");
    (*msg.mutable_pwd_by_user())["u1"] = "map-v1";
    (*msg.mutable_pwd())["u1"] = "map-v1";
    (*msg.mutable_pwd())["u2"] = "map-v2";

    const ProbeRequest redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.name(), "bob");
    EXPECT_EQ(redacted.credentials().username(), "alice");
    EXPECT_EQ(redacted.credentials().password(), kS3cretSha);
    // bytes fields hold the raw digest, not its hex spelling.
    EXPECT_EQ(redacted.credentials().pwd(), HexDecode(kBytepwdSha));
    ASSERT_EQ(redacted.cred_list_size(), 1);
    EXPECT_EQ(redacted.cred_list(0).password(), kHunter2Sha);
    EXPECT_EQ(redacted.cred_map().at("svc").password(), kS3cretSha);
    // Map field whose name does not match keeps its values.
    EXPECT_EQ(redacted.pwd_by_user().at("u1"), "map-v1");
    // Map field whose name matches has each value hashed.
    EXPECT_EQ(redacted.pwd().at("u1"), kMapV1Sha);
    EXPECT_EQ(redacted.pwd().at("u2"), kMapV2Sha);
}

TEST(SensitiveFieldRedactor, HashesThroughRecursiveChain) {
    SensitiveFieldRedactor redactor;

    ProbeRequest msg;
    msg.mutable_chain()->add_pwd("rep-a");
    msg.mutable_chain()->mutable_next()->add_pwd("rep-a");
    msg.mutable_chain()->mutable_next()->add_pwd("rep-b");

    const ProbeRequest redacted = RedactAs(redactor, msg);
    ASSERT_EQ(redacted.chain().pwd_size(), 1);
    EXPECT_EQ(redacted.chain().pwd(0), kRepASha);
    ASSERT_EQ(redacted.chain().next().pwd_size(), 2);
    EXPECT_EQ(redacted.chain().next().pwd(0), kRepASha);
    EXPECT_EQ(redacted.chain().next().pwd(1), kRepBSha);
}

TEST(SensitiveFieldRedactor, FinalJsonShowsHexForStringsAndBase64DigestForBytes) {
    SensitiveFieldRedactor redactor;

    ProbeRequest msg;
    msg.mutable_credentials()->set_password("s3cret");
    msg.mutable_credentials()->set_pwd("bytepwd");

    const std::string json = ToJson(RedactAs(redactor, msg));
    EXPECT_NE(json.find(std::string("\"password\":\"") + kS3cretSha + "\""),
              std::string::npos)
        << json;
    EXPECT_NE(json.find(std::string("\"pwd\":\"") + kBytepwdShaB64 + "\""),
              std::string::npos)
        << json;
    EXPECT_EQ(json.find("s3cret"), std::string::npos) << json;
    // base64("bytepwd") — the plaintext must not survive in any encoding.
    EXPECT_EQ(json.find("Ynl0ZXB3ZA"), std::string::npos) << json;
}

TEST(SensitiveFieldRedactor, AnyPayloadIsUnpackedAndRedacted) {
    SensitiveFieldRedactor redactor;

    Credentials creds;
    creds.set_username("alice");
    creds.set_password("hunter2");

    AnyEnvelope msg;
    msg.set_note("hi");
    ASSERT_TRUE(msg.mutable_payload()->PackFrom(creds));
    ASSERT_TRUE(msg.add_extras()->PackFrom(creds));

    EXPECT_TRUE(redactor.ContainsSensitive(*AnyEnvelope::descriptor()));
    const AnyEnvelope redacted = RedactAs(redactor, msg);

    Credentials unpacked;
    ASSERT_TRUE(redacted.payload().UnpackTo(&unpacked));
    EXPECT_EQ(unpacked.username(), "alice");
    EXPECT_EQ(unpacked.password(), kHunter2Sha);
    ASSERT_EQ(redacted.extras_size(), 1);
    Credentials unpacked_extra;
    ASSERT_TRUE(redacted.extras(0).UnpackTo(&unpacked_extra));
    EXPECT_EQ(unpacked_extra.password(), kHunter2Sha);

    // The input message must never be mutated.
    Credentials original;
    ASSERT_TRUE(msg.payload().UnpackTo(&original));
    EXPECT_EQ(original.password(), "hunter2");

    // JSON expands the Any payload; the digest must appear, the secret not.
    const std::string json = ToJson(redacted);
    EXPECT_NE(json.find(kHunter2Sha), std::string::npos) << json;
    EXPECT_EQ(json.find("hunter2"), std::string::npos) << json;
}

TEST(SensitiveFieldRedactor, AnyWithCleanPayloadKeptIntact) {
    SensitiveFieldRedactor redactor;

    CleanRequest clean;
    clean.set_name("bob");
    AnyEnvelope msg;
    ASSERT_TRUE(msg.mutable_payload()->PackFrom(clean));

    const AnyEnvelope redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.payload().value(), msg.payload().value());
}

// Rebuilds redaction_probe.proto (and its any.proto dependency) in `pool`,
// mirroring a schema loaded at runtime, and returns an AnyEnvelope created
// from `factory` with Credentials{password} hand-packed into payload
// (PackFrom only exists on the generated Any class). The returned message is
// only valid while pool and factory live.
std::unique_ptr<google::protobuf::Message> MakeDynamicEnvelope(
    google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory, const std::string& password) {
    google::protobuf::FileDescriptorProto file;
    google::protobuf::Any::descriptor()->file()->CopyTo(&file);
    if (!pool.BuildFile(file)) return nullptr;
    file.Clear();
    AnyEnvelope::descriptor()->file()->CopyTo(&file);
    if (!pool.BuildFile(file)) return nullptr;

    const google::protobuf::Descriptor* envelope_descriptor =
        pool.FindMessageTypeByName("redaction_probe.AnyEnvelope");
    if (!envelope_descriptor) return nullptr;
    std::unique_ptr<google::protobuf::Message> msg(
        factory.GetPrototype(envelope_descriptor)->New());

    Credentials creds;
    creds.set_password(password);
    const google::protobuf::Reflection* reflection = msg->GetReflection();
    google::protobuf::Message* payload = reflection->MutableMessage(
        msg.get(), envelope_descriptor->FindFieldByName("payload"));
    const google::protobuf::Descriptor* any_descriptor = payload->GetDescriptor();
    const google::protobuf::Reflection* payload_reflection = payload->GetReflection();
    payload_reflection->SetString(
        payload, any_descriptor->FindFieldByName("type_url"),
        "type.googleapis.com/redaction_probe.Credentials");
    payload_reflection->SetString(payload, any_descriptor->FindFieldByName("value"),
                                  creds.SerializeAsString());
    return msg;
}

TEST(SensitiveFieldRedactor, DynamicPoolAnyPayloadIsRedacted) {
    // Deliberately constructed before the pool: the redactor must not retain
    // anything from a pool that dies before it.
    SensitiveFieldRedactor redactor;

    // JSON serialization resolves the Any payload through the message's own
    // (dynamic) pool, so redaction must too.
    google::protobuf::DescriptorPool pool;
    google::protobuf::DynamicMessageFactory factory(&pool);
    std::unique_ptr<google::protobuf::Message> msg =
        MakeDynamicEnvelope(pool, factory, "hunter2");
    ASSERT_NE(msg, nullptr);

    std::unique_ptr<google::protobuf::Message> redacted = redactor.Redact(*msg);
    ASSERT_NE(redacted, nullptr);
    const std::string json = ToJson(*redacted);
    EXPECT_NE(json.find(kHunter2Sha), std::string::npos) << json;
    EXPECT_EQ(json.find("hunter2"), std::string::npos) << json;
}

TEST(SensitiveFieldRedactor, RetainsNoStateFromDestroyedDynamicPools) {
    SensitiveFieldRedactor redactor;
    {
        google::protobuf::DescriptorPool pool;
        google::protobuf::DynamicMessageFactory factory(&pool);
        std::unique_ptr<google::protobuf::Message> msg =
            MakeDynamicEnvelope(pool, factory, "hunter2");
        ASSERT_NE(msg, nullptr);
        std::unique_ptr<google::protobuf::Message> redacted = redactor.Redact(*msg);
        ASSERT_NE(redacted, nullptr);
        EXPECT_EQ(ToJson(*redacted).find("hunter2"), std::string::npos);
    }
    // The pool and its factory are gone; the redactor must keep working with
    // no dangling plan or prototype state (crashes under ASan if regressed).
    ProbeRequest msg;
    msg.set_password("hunter2");
    const ProbeRequest redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.password(), kHunter2Sha);
}

TEST(SensitiveFieldRedactor, AnyWithUnknownTypeLeftAlone) {
    SensitiveFieldRedactor redactor;

    AnyEnvelope msg;
    msg.mutable_payload()->set_type_url("type.googleapis.com/unknown.Type");
    msg.mutable_payload()->set_value("junk");

    const AnyEnvelope redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.payload().type_url(), "type.googleapis.com/unknown.Type");
    EXPECT_EQ(redacted.payload().value(), "junk");
}

TEST(SensitiveFieldRedactor, CustomFieldNamesReplaceDefaults) {
    SensitiveFieldRedactor redactor({"api_key"});

    ProbeRequest msg;
    msg.set_password("hunter2");
    msg.set_api_key("key123");

    const ProbeRequest redacted = RedactAs(redactor, msg);
    EXPECT_EQ(redacted.api_key(), kKey123Sha);
    // "password" is not in the custom set, so it stays plaintext.
    EXPECT_EQ(redacted.password(), "hunter2");
}

}  // namespace
