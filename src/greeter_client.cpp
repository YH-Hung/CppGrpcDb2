#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include "helloworld.grpc.pb.h"
#include "tracing/tracer_provider.h"
#include "tracing/grpc_tracing_interceptor.h"
#include "tracing/trace_log_formatter.h"
#include "spdlog/spdlog.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterClient {
public:
    GreeterClient(std::shared_ptr<Channel> channel)
        : stub_(Greeter::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    std::string SayHello(const std::string& user) {
        // Data we are sending to the server.
        HelloRequest request;
        request.set_name(user);

        // Container for the data we expect from the server.
        HelloReply reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SayHello(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            return reply.message();
        } else {
            spdlog::error("RPC failed: {} - {}", static_cast<int>(status.error_code()), status.error_message());
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<Greeter::Stub> stub_;
};

int main(int argc, char** argv) {
    // Initialize OpenTelemetry tracing
    tracing::TracerProvider::Initialize();

    // Set up trace-aware logging so all spdlog messages carry trace/span ids
    tracing::SetTraceLogging();

    std::string target_str = "localhost:50051";
    // Create a traced channel with automatic span creation and context propagation
    GreeterClient greeter(
        tracing::CreateTracedChannel(target_str, grpc::InsecureChannelCredentials()));
    std::string user("賴柔瑤");
    // std::string user("黃美晴");
    // Create a parent span to keep trace context active for local logs around the RPC
    auto tracer = tracing::TracerProvider::GetTracer("greeter-client");
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> parent_span;
    if (tracer) {
        parent_span = tracer->StartSpan("GreeterClientMain");
    }
    opentelemetry::nostd::unique_ptr<opentelemetry::trace::Scope> scope;
    if (parent_span) {
        scope.reset(new opentelemetry::trace::Scope(parent_span));
    }

    std::string reply = greeter.SayHello(user);
    spdlog::info("Greeter received: {}", reply);

    if (parent_span) {
        parent_span->End();
    }

    // Shutdown tracing and flush pending spans
    tracing::TracerProvider::Shutdown();

    return 0;
}