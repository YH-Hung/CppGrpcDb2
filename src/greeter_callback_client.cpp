#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include "helloworld.grpc.pb.h"
#include "otel_tracing.h"

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

    std::string SayHello(const std::string& user) {
        HelloRequest request;
        request.set_name(user);

        HelloReply reply;
        ClientContext context;
        Status status;

        std::mutex mu;
        std::condition_variable cv;
        bool done = false;

        stub_->async()->SayHello(&context, &request, &reply,
            [&status, &mu, &cv, &done](grpc::Status s) {
                status = std::move(s);
                std::lock_guard<std::mutex> lock(mu);
                done = true;
                cv.notify_one();
            });

        // Wait for the callback to complete
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&done] { return done; });
        const std::string trace_id = otel::TraceIdForClientContext(&context);

        if (status.ok()) {
            std::cout << "[trace_id: " << trace_id << "] RPC succeeded"
                      << std::endl;
            otel::ClearClientContextTraceId(&context);
            return reply.message();
        } else {
            std::cout << "[trace_id: " << trace_id << "] "
                      << status.error_code() << ": "
                      << status.error_message() << std::endl;
            otel::ClearClientContextTraceId(&context);
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<Greeter::Stub> stub_;
};

int main(int argc, char** argv) {
    otel::TracingOptions tracing_options;
    tracing_options.service_name = "greeter_callback_client";
    otel::InitTracing(tracing_options);

    std::string target_str = "localhost:50051";
    GreeterClient greeter(
        otel::CreateTracingChannel(target_str, grpc::InsecureChannelCredentials()));
    std::string user("賴柔瑤");
    std::string reply = greeter.SayHello(user);
    std::cout << "Greeter received: " << reply << std::endl;

    otel::ShutdownTracing();
    return 0;
}
