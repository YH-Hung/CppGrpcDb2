#pragma once

#include <grpcpp/grpcpp.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lb/endpoint_config.h"

namespace lb {

struct ChannelFactoryOptions {
    // Multi-endpoint mode lowers the built-in retry budget: app-level failover
    // attempts multiply with in-channel retry attempts.
    bool multi_endpoint = false;
};

// Service config JSON with a retryPolicy matching all services and methods.
// maxAttempts: 2 in multi-endpoint mode, 4 in single-endpoint mode.
std::string BuildServiceConfigJson(bool multi_endpoint);

// Shared arguments for every per-endpoint channel: retry service config,
// round_robin (balances across a single FQDN's A/AAAA records), keepalive.
grpc::ChannelArguments MakeChannelArguments(const ChannelFactoryOptions& options);

using ChannelBuilder = std::function<std::shared_ptr<grpc::Channel>(
    const Endpoint&, const grpc::ChannelArguments&)>;

// One channel per endpoint. The default builder uses CreateCustomChannel with
// InsecureChannelCredentials (swap here for TLS); tests inject a fake builder.
std::vector<std::shared_ptr<grpc::Channel>> BuildChannels(
    const std::vector<Endpoint>& endpoints, const ChannelFactoryOptions& options,
    ChannelBuilder builder = nullptr);

}  // namespace lb
