#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <chrono>
#include "helloworld.pb.h"
#include "helloworld.grpc.pb.h"
#include "absl/strings/str_format.h"
#include "metrics_interceptor.h"
#include "tracing/tracer_provider.h"
#include "tracing/grpc_tracing_interceptor.h"
#include "tracing/trace_log_formatter.h"
#include "spdlog/spdlog.h"
#include <vector>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service {
public:
    GreeterServiceImpl() = default;

    Status SayHello(ServerContext* context, const HelloRequest* request,
                    HelloReply* reply) override {
        // Check cancellation before preparing the reply
        if (context && context->IsCancelled()) {
            return Status(grpc::StatusCode::CANCELLED, "Request cancelled");
        }
        std::string prefix("Hello ");
        reply->set_message(prefix + request->name());
        return Status::OK;
    }
};

void RunServer(uint16_t port) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    // Create Prometheus exposer on localhost:8124 and a registry
    auto exposer = std::make_unique<prometheus::Exposer>("127.0.0.1:8124");
    auto registry = std::make_shared<prometheus::Registry>();
    exposer->RegisterCollectable(registry);

    GreeterServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Register interceptor factories (tracing + metrics)
    {
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;

        // Add tracing interceptor (runs first for complete span coverage)
        interceptors.push_back(std::make_unique<tracing::ServerTracingInterceptorFactory>());

        // Add metrics interceptor
        auto metrics_factory = std::make_unique<MetricsServerInterceptorFactory>(registry);
        interceptors.push_back(std::move(metrics_factory));

        builder.experimental().SetInterceptorCreators(std::move(interceptors));
    }
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    spdlog::info("Server listening on {}", server_address);

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {
    // Initialize OpenTelemetry tracing
    tracing::TracerProvider::Initialize();

    // Set up trace-aware logging so all spdlog messages carry trace/span ids
    tracing::SetTraceLogging();

    // Run the gRPC server
    RunServer(50051);

    // Shutdown tracing and flush pending spans
    tracing::TracerProvider::Shutdown();

    return 0;
}