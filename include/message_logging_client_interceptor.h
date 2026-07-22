#pragma once

#include <string>
#include <unordered_set>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_interceptor.h>
#include <google/protobuf/message.h>

#include "sensitive_field_redactor.h"

// A gRPC client interceptor that logs the content of unary request and
// response messages for each RPC as compact JSON (snake_case field names).
// Sensitive fields (by default "password"/"pwd", configurable on the factory)
// are logged as their SHA-256 digest instead of plaintext: hex for string
// fields, base64 of the raw digest for bytes fields (the standard protobuf
// JSON encoding for bytes).
// Client-side counterpart of MessageLoggingServerInterceptor; unlike the
// server variant it logs no otel trace id — clients using it (e.g.
// greeter_failover_client) set up no tracer, so the id would be all zeros.
class MessageLoggingClientInterceptor : public grpc::experimental::Interceptor {
public:
    MessageLoggingClientInterceptor(const std::string& method_name,
                                    const SensitiveFieldRedactor* redactor);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    std::string method_name_;
    // Owned by the factory, which outlives the per-call interceptors.
    const SensitiveFieldRedactor* redactor_;
};

class MessageLoggingClientInterceptorFactory
    : public grpc::experimental::ClientInterceptorFactoryInterface {
public:
    // Fields whose name is in sensitive_field_names are logged hashed.
    explicit MessageLoggingClientInterceptorFactory(
        std::unordered_set<std::string> sensitive_field_names =
            SensitiveFieldRedactor::DefaultSensitiveNames());

    grpc::experimental::Interceptor* CreateClientInterceptor(
        grpc::experimental::ClientRpcInfo* info) override;

private:
    // Shared by all created interceptors so the per-type redaction plan cache
    // warms up once per channel, not once per call.
    SensitiveFieldRedactor redactor_;
};
