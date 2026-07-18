#include "lb/channel_factory.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <chrono>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lb/keepalive.h"

namespace {

Json::Value ParseJson(const std::string& text) {
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    std::istringstream stream(text);
    EXPECT_TRUE(Json::parseFromStream(builder, stream, &root, &errors)) << errors;
    return root;
}

TEST(BuildServiceConfigJson, MultiEndpointLowersRetryBudget) {
    const Json::Value config =
        ParseJson(lb::BuildServiceConfigJson(lb::ChannelFactoryOptions{true}));
    const Json::Value& entry = config["methodConfig"][0];
    EXPECT_EQ(entry["retryPolicy"]["maxAttempts"].asInt(), 2);
    // An empty name entry matches every service and method on the channel.
    ASSERT_EQ(entry["name"].size(), 1u);
    EXPECT_TRUE(entry["name"][0].isObject());
    EXPECT_TRUE(entry["name"][0].empty());
    EXPECT_EQ(entry["retryPolicy"]["retryableStatusCodes"][0].asString(), "UNAVAILABLE");
}

TEST(BuildServiceConfigJson, SingleEndpointKeepsFullRetryBudget) {
    const Json::Value config =
        ParseJson(lb::BuildServiceConfigJson(lb::ChannelFactoryOptions{false}));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 4);
}

TEST(BuildServiceConfigJson, DefaultBackoffsMatchPreviousHardcodedValues) {
    const Json::Value config =
        ParseJson(lb::BuildServiceConfigJson(lb::ChannelFactoryOptions{}));
    const Json::Value& policy = config["methodConfig"][0]["retryPolicy"];
    EXPECT_EQ(policy["initialBackoff"].asString(), "0.1s");
    EXPECT_EQ(policy["maxBackoff"].asString(), "1s");
}

TEST(BuildServiceConfigJson, SingleEndpointHonorsMaxAttemptsOverride) {
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(
        lb::ChannelFactoryOptions{.multi_endpoint = false, .max_attempts = 3}));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 3);
}

TEST(BuildServiceConfigJson, MaxAttemptsOverrideClampedToGrpcMinimum) {
    // gRPC requires maxAttempts >= 2; an invalid service config takes the
    // whole channel down, far worse than the typo deserves.
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(
        lb::ChannelFactoryOptions{.multi_endpoint = false, .max_attempts = 1}));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 2);
}

TEST(BuildServiceConfigJson, MaxAttemptsOverrideClampedToGrpcMaximum) {
    // gRPC treats values above 5 as 5; emit the effective value.
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(
        lb::ChannelFactoryOptions{.multi_endpoint = false, .max_attempts = 10}));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 5);
}

TEST(BuildServiceConfigJson, MultiEndpointIgnoresMaxAttemptsOverride) {
    // The lowered budget bounds retry amplification: app-level failover
    // attempts multiply with in-channel retries.
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(
        lb::ChannelFactoryOptions{.multi_endpoint = true, .max_attempts = 5}));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 2);
}

TEST(BuildServiceConfigJson, CustomBackoffsAreFormattedAsSeconds) {
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(
        lb::ChannelFactoryOptions{.initial_backoff = std::chrono::milliseconds{250},
                                  .max_backoff = std::chrono::milliseconds{5000}}));
    const Json::Value& policy = config["methodConfig"][0]["retryPolicy"];
    EXPECT_EQ(policy["initialBackoff"].asString(), "0.25s");
    EXPECT_EQ(policy["maxBackoff"].asString(), "5s");
}

// Returns the integer channel arg named `key`, or nullopt if absent.
std::optional<int> FindIntArg(const grpc::ChannelArguments& args, std::string_view key) {
    grpc_channel_args c_args;
    args.SetChannelArgs(&c_args);
    for (std::size_t i = 0; i < c_args.num_args; ++i) {
        if (c_args.args[i].key == key && c_args.args[i].type == GRPC_ARG_INTEGER) {
            return c_args.args[i].value.integer;
        }
    }
    return std::nullopt;
}

TEST(MakeChannelArguments, DefaultLeavesClientKeepaliveDisabled) {
    // GRPC_TARGET_ENDPOINTS may name arbitrary servers whose ping-strike
    // policy we don't control. Even during an active-but-quiet long-lived RPC
    // an unmodified server permits only one ping per 5 minutes without
    // intervening data before striking (GOAWAY "too_many_pings"), so the only
    // safe uncoordinated default is gRPC's own: no client keepalive at all.
    const grpc::ChannelArguments args =
        lb::MakeChannelArguments(lb::ChannelFactoryOptions{});
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_TIME_MS), std::nullopt);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS), std::nullopt);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS), std::nullopt);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA), std::nullopt);
}

TEST(MakeChannelArguments, AggressiveKeepaliveIsOptIn) {
    // Clients talking to coordinated servers (the in-repo ones apply
    // AddKeepaliveServerArgs) can opt into fast half-dead detection: short
    // interval, pings on idle connections, and no idle-ping throttle (gRPC
    // otherwise stops pinging after 2 pings without data).
    lb::ChannelFactoryOptions options;
    options.keepalive_time = std::chrono::milliseconds(lb::kKeepaliveTimeMs);
    options.keepalive_timeout = std::chrono::milliseconds(lb::kKeepaliveTimeoutMs);
    options.keepalive_permit_without_calls = true;
    const grpc::ChannelArguments args = lb::MakeChannelArguments(options);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_TIME_MS), lb::kKeepaliveTimeMs);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS), lb::kKeepaliveTimeoutMs);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS), 1);
    EXPECT_EQ(FindIntArg(args, GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA), 0);
}

TEST(AddKeepaliveServerArgs, ToleratesClientPingRate) {
    // gRPC servers default to tolerating unsolicited pings only every 5
    // minutes (two violations => GOAWAY "too_many_pings"), so every in-repo
    // server an lb client targets must accept pings at least as frequent as
    // the client sends them.
    struct FakeBuilder {
        std::map<std::string, int> ints;
        void AddChannelArgument(const std::string& key, int value) { ints[key] = value; }
    } builder;
    lb::AddKeepaliveServerArgs(builder);
    const auto it = builder.ints.find(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS);
    ASSERT_NE(it, builder.ints.end());
    EXPECT_GT(it->second, 0);
    EXPECT_LE(it->second, lb::kKeepaliveTimeMs);
}

TEST(BuildChannels, UsesInjectedBuilderOncePerEndpoint) {
    const std::vector<lb::Endpoint> endpoints = {{"a.example.com", 1}, {"b.example.com", 2}};
    std::vector<std::string> seen_targets;
    const auto channels = lb::BuildChannels(
        endpoints, lb::ChannelFactoryOptions{true},
        [&](const lb::Endpoint& endpoint, const grpc::ChannelArguments&) {
            seen_targets.push_back(endpoint.Target());
            return std::shared_ptr<grpc::Channel>();
        });
    EXPECT_EQ(channels.size(), 2u);
    ASSERT_EQ(seen_targets.size(), 2u);
    EXPECT_EQ(seen_targets[0], "a.example.com:1");
    EXPECT_EQ(seen_targets[1], "b.example.com:2");
}

TEST(BuildChannels, DefaultBuilderProducesChannels) {
    // No connection is attempted at channel creation; this is offline-safe.
    const std::vector<lb::Endpoint> endpoints = {{"localhost", 50051}};
    const auto channels = lb::BuildChannels(endpoints, lb::ChannelFactoryOptions{false});
    ASSERT_EQ(channels.size(), 1u);
    EXPECT_NE(channels[0], nullptr);
}

}  // namespace
