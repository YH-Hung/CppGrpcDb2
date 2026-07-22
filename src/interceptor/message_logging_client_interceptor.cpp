#include "message_logging_client_interceptor.h"

#include <grpcpp/impl/codegen/config_protobuf.h>
#include <spdlog/spdlog.h>

#include <memory>

using HP = grpc::experimental::InterceptionHookPoints;

namespace {

// Serializes msg as compact JSON with snake_case field names and logs it at
// info; sensitive fields are replaced by their SHA-256 digest first (hex for
// string fields, base64 of the raw digest for bytes fields). A conversion
// failure logs at warn instead.
void LogMessageJson(const std::string& method_name, const char* direction,
                    const google::protobuf::Message& msg,
                    const SensitiveFieldRedactor& redactor) {

    grpc::protobuf::json::JsonPrintOptions options;
    options.preserve_proto_field_names = true;  // Use snake_case field names

    const std::unique_ptr<google::protobuf::Message> redacted = redactor.Redact(msg);
    const google::protobuf::Message& loggable = redacted ? *redacted : msg;

    std::string json_str;
    auto status = grpc::protobuf::json::MessageToJsonString(loggable, &json_str, options);
    if (status.ok()) {
        spdlog::info("[{}] {} message (JSON): {}", method_name, direction, json_str);
    } else {
        spdlog::warn("[{}] Failed to convert {} to JSON: {}", method_name,
                     direction, status.ToString());
    }
}

}  // namespace

MessageLoggingClientInterceptor::MessageLoggingClientInterceptor(
    const std::string& method_name, const SensitiveFieldRedactor* redactor)
    : method_name_(method_name), redactor_(redactor) {
}

void MessageLoggingClientInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    // Log the outgoing request before it is sent.
    if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_MESSAGE)) {
        const void* send_msg_ptr = methods->GetSendMessage();
        if (send_msg_ptr) {
            try {
                LogMessageJson(method_name_, "Request",
                    *static_cast<const google::protobuf::Message*>(send_msg_ptr),
                    *redactor_);
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log request message: {}",method_name_, e.what());
            }
        }
    }

    // Log the incoming response after it is received.
    if (methods->QueryInterceptionHookPoint(HP::POST_RECV_MESSAGE)) {
        void* recv_msg_ptr = methods->GetRecvMessage();
        if (recv_msg_ptr) {
            try {
                LogMessageJson(method_name_, "Response",
                    *static_cast<google::protobuf::Message*>(recv_msg_ptr),
                    *redactor_);
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log response message: {}", method_name_, e.what());
            }
        }
    }

    methods->Proceed();
}

MessageLoggingClientInterceptorFactory::MessageLoggingClientInterceptorFactory(
    std::unordered_set<std::string> sensitive_field_names)
    : redactor_(std::move(sensitive_field_names)) {
}

grpc::experimental::Interceptor* MessageLoggingClientInterceptorFactory::CreateClientInterceptor(
    grpc::experimental::ClientRpcInfo* info) {
    if (!info || !info->method()) {
        return nullptr;
    }
    return new MessageLoggingClientInterceptor(info->method(), &redactor_);
}
