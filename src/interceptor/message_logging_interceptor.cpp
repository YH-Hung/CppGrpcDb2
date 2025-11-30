#include "message_logging_interceptor.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>

using HP = grpc::experimental::InterceptionHookPoints;

MessageLoggingServerInterceptor::MessageLoggingServerInterceptor(const std::string& method_name)
    : method_name_(method_name) {
}

void MessageLoggingServerInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    // Skip health check and reflection methods to prevent crashes
    std::string method_lower = method_name_;
    std::transform(method_lower.begin(), method_lower.end(), method_lower.begin(), ::tolower);
    if (method_lower.find("grpc.health") != std::string::npos ||
        method_lower.find("grpc.reflection") != std::string::npos) {
        methods->Proceed();
        return;
    }

    // Log request message after it's received
    if (methods->QueryInterceptionHookPoint(HP::POST_RECV_MESSAGE)) {
        void* recv_msg_ptr = methods->GetRecvMessage();
        if (recv_msg_ptr) {
            try {
                auto* req_msg = static_cast<google::protobuf::Message*>(recv_msg_ptr);
                if (req_msg) {
                    // Use DebugString() for better UTF-8 handling (preserves Unicode characters)
                    std::string msg_str = req_msg->Utf8DebugString();
                    spdlog::info("[{}] Request message: {}", method_name_, msg_str);
                }
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log request message: {}", method_name_, e.what());
            }
        }
    }

    // Log reply message - PRE_SEND_MESSAGE only work for sync and callback, NOT async
    if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_MESSAGE)) {
        void* send_msg_ptr = const_cast<void*>(methods->GetSendMessage());
        if (send_msg_ptr) {
            try {
                auto* resp_msg = static_cast<google::protobuf::Message*>(send_msg_ptr);
                if (resp_msg) {
                    // Use DebugString() for better UTF-8 handling
                    std::string msg_str = resp_msg->Utf8DebugString();
                    spdlog::info("[{}] Reply message: {}", method_name_, msg_str);
                }
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log reply message: {}", method_name_, e.what());
            }
        } else {
            // Message might not be available yet for async servers
            spdlog::debug("[{}] Reply message not available at PRE_SEND_MESSAGE", method_name_);
        }
    }

    methods->Proceed();
}

grpc::experimental::Interceptor* MessageLoggingServerInterceptorFactory::CreateServerInterceptor(
    grpc::experimental::ServerRpcInfo* info) {
    if (!info) {
        return nullptr;
    }
    
    // Extract method name from ServerRpcInfo
    std::string method_name = info->method();
    return new MessageLoggingServerInterceptor(method_name);
}

