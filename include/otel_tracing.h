#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include <memory>
#include <string>
#include <vector>

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/tracer.h>

namespace otel {

struct TracingOptions {
    std::string service_name;
    std::string exporter_otlp_endpoint;
};

void InitTracing(const TracingOptions& options);
void ShutdownTracing();
bool IsTracingEnabled();

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
GetTracer(const std::string& name);

std::string CurrentTraceIdHex();
std::string CurrentSpanIdHex();
std::string TraceIdForServerContext(const grpc::ServerContextBase* context);
std::string TraceIdForClientContext(const grpc::ClientContext* context);
void ClearClientContextTraceId(const grpc::ClientContext* context);

class TracingServerInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;
};

class TracingClientInterceptorFactory
    : public grpc::experimental::ClientInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateClientInterceptor(
        grpc::experimental::ClientRpcInfo* info) override;
};

std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>
MakeTracingServerInterceptorFactory();

std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>
MakeTracingClientInterceptorFactory();

std::shared_ptr<grpc::Channel> CreateTracingChannel(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& credentials);

std::shared_ptr<grpc::Channel> CreateTracingChannel(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& credentials,
    const grpc::ChannelArguments& args);

}  // namespace otel
