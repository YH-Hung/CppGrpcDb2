#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lb/endpoint_config.h"

namespace lb {

struct ChannelFactoryOptions {
    // Multi-endpoint mode lowers the built-in retry budget: app-level failover
    // attempts multiply with in-channel retry attempts.
    bool multi_endpoint = false;
    // Overrides the default retry budget of 4 in single-endpoint mode; ignored
    // in multi-endpoint mode, where the budget is pinned to 2 to bound retry
    // amplification. Clamped to gRPC's valid range [2, 5].
    std::optional<int> max_attempts;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{1000};
    // Keepalive posture. nullopt (the default) leaves client keepalive
    // disabled — gRPC's own default and the only setting safe against
    // uncoordinated servers: their default ping-strike policy permits one
    // unsolicited ping per 5 minutes even during an active-but-quiet
    // long-lived RPC, then answers GOAWAY "too_many_pings". Setting a time is
    // a coordinated opt-in (the in-repo servers apply AddKeepaliveServerArgs,
    // see lb/keepalive.h); keepalive_timeout and the idle-ping flag apply only
    // then, and enabling idle pings also lifts gRPC's idle-ping throttle,
    // which would otherwise stop pinging after 2 pings without data.
    std::optional<std::chrono::milliseconds> keepalive_time;
    std::chrono::milliseconds keepalive_timeout{20000};
    bool keepalive_permit_without_calls = false;
};

// Service config JSON with a retryPolicy matching all services and methods.
// maxAttempts defaults to 2 in multi-endpoint mode, 4 in single-endpoint mode.
std::string BuildServiceConfigJson(const ChannelFactoryOptions& options);

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
