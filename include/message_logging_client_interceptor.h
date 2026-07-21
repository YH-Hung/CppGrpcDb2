#pragma once

#include <string>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_interceptor.h>
#include <google/protobuf/message.h>

// A gRPC client interceptor that logs the content of unary request and
// response messages for each RPC as compact JSON (snake_case field names).
// Client-side counterpart of MessageLoggingServerInterceptor; unlike the
// server variant it logs no otel trace id — clients using it (e.g.
// greeter_failover_client) set up no tracer, so the id would be all zeros.
class MessageLoggingClientInterceptor : public grpc::experimental::Interceptor {
public:
    explicit MessageLoggingClientInterceptor(const std::string& method_name);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    std::string method_name_;
};

class MessageLoggingClientInterceptorFactory
    : public grpc::experimental::ClientInterceptorFactoryInterface {
public:
    MessageLoggingClientInterceptorFactory() = default;

    grpc::experimental::Interceptor* CreateClientInterceptor(
        grpc::experimental::ClientRpcInfo* info) override;
};
