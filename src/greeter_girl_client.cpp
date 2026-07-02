#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <utf8ansi.h>

#include "hello_girl.grpc.pb.h"
#include "otel_tracing.h"

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
    std::string SayHello(const std::string& name, const std::string& spouse, int first_round, const std::string& secret_note) {
        // Data we are sending to the server.
        HelloGirlRequest request;
        request.set_name(name);
        request.set_spouse(spouse);
        request.set_first_round(first_round);
        request.set_secret_note(secret_note);

        // Container for the data we expect from the server.
        HelloGirlReply reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;
        context.AddMetadata("special_msg", "greetings-from-girl-client");

        // The actual RPC.
        Status status = stub_->SayHello(&context, request, &reply);
        const std::string trace_id = otel::TraceIdForClientContext(&context);

        // Act upon its status.
        if (status.ok()) {
            std::ostringstream oss;
            oss << "message='" << reply.message() << "'\n"
                << "marriage='" << reply.marriage() << "'\n"
                << "size=" << reply.size() << "'\n"
                << "reply_secret=" << reply.reply_secret();
            std::cout << "[trace_id: " << trace_id << "] RPC succeeded"
                      << std::endl;
            otel::ClearClientContextTraceId(&context);
            return oss.str();
        } else {
            std::cout << "[trace_id: " << trace_id << "] "
                      << status.error_code() << ": "
                      << status.error_message() << std::endl;
            otel::ClearClientContextTraceId(&context);
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<GirlGreeter::Stub> stub_;
};

int main(int argc, char** argv) {
    otel::TracingOptions tracing_options;
    tracing_options.service_name = "greeter_girl_client";
    otel::InitTracing(tracing_options);

    std::string target_str = "localhost:50051";
    GirlGreeterClient client(
        otel::CreateTracingChannel(target_str, grpc::InsecureChannelCredentials()));

    // Example values similar to greeter_client.cpp style
    std::string name = "賴柔瑤";
    std::string spouse = "me 英國人";
    int first_round = 38;
    std::string secret_note = utf8ansi::utf8_to_big5("奇蹟女身");

    auto reply = client.SayHello(name, spouse, first_round, secret_note);
    std::cout << "GirlGreeter received:\n" << reply << std::endl;

    otel::ShutdownTracing();
    return 0;
}
