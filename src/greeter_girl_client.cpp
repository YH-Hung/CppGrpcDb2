#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include "hello_girl.grpc.pb.h"
#include "tracing/tracer_provider.h"
#include "tracing/grpc_tracing_interceptor.h"
#include "tracing/trace_log_formatter.h"
#include "spdlog/spdlog.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using hellogirl::GirlGreeter;
using hellogirl::HelloGirlReply;
using hellogirl::HelloGirlRequest;

class GirlGreeterClient {
public:
    explicit GirlGreeterClient(std::shared_ptr<Channel> channel)
        : stub_(GirlGreeter::NewStub(channel)) {}

    // Sends a greeting request and returns the combined string output
    std::string SayHello(const std::string& name, const std::string& spouse, int first_round) {
        // Data we are sending to the server.
        HelloGirlRequest request;
        request.set_name(name);
        request.set_spouse(spouse);
        request.set_first_round(first_round);

        // Container for the data we expect from the server.
        HelloGirlReply reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SayHello(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            std::ostringstream oss;
            oss << "message='" << reply.message() << "'\n"
                << "marriage='" << reply.marriage() << "'\n"
                << "size=" << reply.size();
            return oss.str();
        } else {
            spdlog::error("RPC failed: {} - {}", static_cast<int>(status.error_code()), status.error_message());
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<GirlGreeter::Stub> stub_;
};

int main(int argc, char** argv) {
    // Initialize OpenTelemetry tracing
    tracing::TracerProvider::Initialize();

    // Set up trace-aware logging so all spdlog messages carry trace/span ids
    tracing::SetTraceLogging();

    std::string target_str = "localhost:50051";
    // Create a traced channel with automatic span creation and context propagation
    GirlGreeterClient client(
        tracing::CreateTracedChannel(target_str, grpc::InsecureChannelCredentials()));

    // Example values similar to greeter_client.cpp style
    std::string name = "賴柔瑤";
    std::string spouse = "me 英國人";
    int first_round = 38;

    // Keep a parent span active so client-side logs include trace context
    auto tracer = tracing::TracerProvider::GetTracer("greeter-girl-client");
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> parent_span;
    if (tracer) {
        parent_span = tracer->StartSpan("GirlGreeterClientMain");
    }
    opentelemetry::nostd::unique_ptr<opentelemetry::trace::Scope> scope;
    if (parent_span) {
        scope.reset(new opentelemetry::trace::Scope(parent_span));
    }

    auto reply = client.SayHello(name, spouse, first_round);
    spdlog::info("GirlGreeter received:\n{}", reply);

    if (parent_span) {
        parent_span->End();
    }

    // Shutdown tracing and flush pending spans
    tracing::TracerProvider::Shutdown();

    return 0;
}
