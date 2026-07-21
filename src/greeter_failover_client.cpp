#include <grpcpp/grpcpp.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "helloworld.grpc.pb.h"
#include "lb/failover_client.h"
#include "message_logging_client_interceptor.h"

using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

namespace {

// Parses a decimal integer argument >= min_value. Returns false on trailing
// garbage, out-of-range, or below-minimum values.
bool ParseIntArg(const char* arg, int min_value, int& out) {
    try {
        std::size_t pos = 0;
        const int value = std::stoi(arg, &pos);
        if (pos != std::string(arg).size() || value < min_value) return false;
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

// usage: greeter_failover_client [count] [delay_ms]
//   count    number of sequential calls, >= 1 (default 1)
//   delay_ms sleep between calls in milliseconds, >= 0 (default 0)
// One FailoverClient serves all calls, so round-robin rotation and cooldown
// recovery are observable across the run. Exit 0 iff every call succeeded.
int main(int argc, char** argv) {
    int count = 1;
    int delay_ms = 0;
    if ((argc > 1 && !ParseIntArg(argv[1], 1, count)) ||
        (argc > 2 && !ParseIntArg(argv[2], 0, delay_ms)) || argc > 3) {
        std::cerr << "usage: " << argv[0] << " [count] [delay_ms]" << std::endl;
        return 2;
    }

    // Call results go to stdout (a stable interface the live test script
    // asserts on); route the lb library's spdlog diagnostics to stderr.
    spdlog::set_default_logger(spdlog::stderr_color_mt("lb_client"));

    // Same target/credentials/args as the default builder, plus a per-RPC
    // message logging interceptor (request/response JSON on stderr).
    lb::ChannelBuilder logging_channel_builder =
        [](const lb::Endpoint& endpoint, const grpc::ChannelArguments& args) {
            std::vector<std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>> creators;
            creators.push_back(std::make_unique<MessageLoggingClientInterceptorFactory>());

            return grpc::experimental::CreateCustomChannelWithInterceptors(endpoint.Target(),
                grpc::InsecureChannelCredentials(), args,std::move(creators));
        };

    lb::FailoverClient<Greeter> client = lb::FailoverClient<Greeter>::FromEnv(
        [](std::shared_ptr<grpc::Channel> channel) {
            return Greeter::NewStub(std::move(channel));
        },
        std::move(logging_channel_builder));

    bool all_ok = true;
    for (int i = 0; i < count; ++i) {
        if (i > 0 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        HelloRequest request;
        request.set_name("賴柔瑤");
        HelloReply reply;

        const lb::CallResult result =
            client.Call(request, reply, &Greeter::Stub::SayHello);

        if (result.status.ok()) {
            std::cout << "Greeter received: " << reply.message()
                      << " (via " << result.served_by << ")" << std::endl;
        } else {
            std::cout << result.status.error_code() << ": "
                      << result.status.error_message() << std::endl;
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
