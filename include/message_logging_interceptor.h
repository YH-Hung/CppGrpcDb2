#pragma once

#include <string>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>
#include <google/protobuf/message.h>

// A gRPC server interceptor that logs the content of unary request and reply messages
// for each gRPC call. Uses protobuf's ShortDebugString() for readable output.
class MessageLoggingServerInterceptor : public grpc::experimental::Interceptor {
public:
    explicit MessageLoggingServerInterceptor(const std::string& method_name);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    std::string method_name_;
};

class MessageLoggingServerInterceptorFactory 
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    MessageLoggingServerInterceptorFactory() = default;

    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;
};

