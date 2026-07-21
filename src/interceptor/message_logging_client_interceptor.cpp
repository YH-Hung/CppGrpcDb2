#include "message_logging_client_interceptor.h"

#include <grpcpp/impl/codegen/config_protobuf.h>
#include <spdlog/spdlog.h>

using HP = grpc::experimental::InterceptionHookPoints;

namespace {

// Serializes msg as compact JSON with snake_case field names and logs it at info;
// a conversion failure logs at warn instead.
void LogMessageJson(const std::string& method_name, const char* direction, const google::protobuf::Message& msg) {

    grpc::protobuf::json::JsonPrintOptions options;
    options.preserve_proto_field_names = true;  // Use snake_case field names

    std::string json_str;
    auto status = grpc::protobuf::json::MessageToJsonString(msg, &json_str, options);
    if (status.ok()) {
        spdlog::info("[{}] {} message (JSON): {}", method_name, direction, json_str);
    } else {
        spdlog::warn("[{}] Failed to convert {} to JSON: {}", method_name,
                     direction, status.ToString());
    }
}

}  // namespace

MessageLoggingClientInterceptor::MessageLoggingClientInterceptor(const std::string& method_name)
    : method_name_(method_name) {
}

void MessageLoggingClientInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    // Log the outgoing request before it is sent.
    if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_MESSAGE)) {
        const void* send_msg_ptr = methods->GetSendMessage();
        if (send_msg_ptr) {
            try {
                LogMessageJson(method_name_, "Request",
                    *static_cast<const google::protobuf::Message*>(send_msg_ptr));
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
                    *static_cast<google::protobuf::Message*>(recv_msg_ptr));
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log response message: {}", method_name_, e.what());
            }
        }
    }

    methods->Proceed();
}

grpc::experimental::Interceptor* MessageLoggingClientInterceptorFactory::CreateClientInterceptor(
    grpc::experimental::ClientRpcInfo* info) {
    if (!info || !info->method()) {
        return nullptr;
    }
    return new MessageLoggingClientInterceptor(info->method());
}
