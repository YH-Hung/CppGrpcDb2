#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "helloworld.grpc.pb.h"
#include "lb/channel_factory.h"
#include "lb/endpoint_config.h"
#include "lb/endpoint_manager.h"
#include "lb/failover_call.h"

using grpc::ClientContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

int main(int argc, char** argv) {
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    for (const std::string& entry : config.rejected) {
        spdlog::warn("lb: ignoring malformed endpoint entry '{}'", entry);
    }
    for (std::size_t i = 0; i < config.endpoints.size(); ++i) {
        spdlog::info("lb: endpoint {} = {}", i, config.endpoints[i].Target());
    }

    const lb::ChannelFactoryOptions factory_options{config.endpoints.size() > 1};
    const auto channels = lb::BuildChannels(config.endpoints, factory_options);

    std::vector<std::unique_ptr<Greeter::Stub>> stubs;
    stubs.reserve(channels.size());
    for (const auto& channel : channels) {
        stubs.push_back(Greeter::NewStub(channel));
    }

    lb::EndpointManager manager(
        config.endpoints.size(),
        lb::EndpointManager::Options{config.cooldown_base, std::chrono::milliseconds(30000)});

    lb::FailoverOptions failover_options;
    failover_options.max_attempts = config.max_attempts;

    HelloRequest request;
    request.set_name("賴柔瑤");
    HelloReply reply;
    std::size_t served_by = 0;

    const Status status = lb::CallWithFailover(
        manager, failover_options, [&](ClientContext& context, std::size_t index) {
            served_by = index;
            return stubs[index]->SayHello(&context, request, &reply);
        });

    if (status.ok()) {
        std::cout << "Greeter received: " << reply.message() << " (via "
                  << config.endpoints[served_by].Target() << ")" << std::endl;
        return 0;
    }
    std::cout << status.error_code() << ": " << status.error_message() << std::endl;
    return 1;
}
