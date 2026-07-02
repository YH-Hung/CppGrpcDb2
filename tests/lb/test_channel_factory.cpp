#include "lb/channel_factory.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <sstream>
#include <string>
#include <vector>

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
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(true));
    const Json::Value& entry = config["methodConfig"][0];
    EXPECT_EQ(entry["retryPolicy"]["maxAttempts"].asInt(), 2);
    // An empty name entry matches every service and method on the channel.
    ASSERT_EQ(entry["name"].size(), 1u);
    EXPECT_TRUE(entry["name"][0].isObject());
    EXPECT_TRUE(entry["name"][0].empty());
    EXPECT_EQ(entry["retryPolicy"]["retryableStatusCodes"][0].asString(), "UNAVAILABLE");
}

TEST(BuildServiceConfigJson, SingleEndpointKeepsFullRetryBudget) {
    const Json::Value config = ParseJson(lb::BuildServiceConfigJson(false));
    EXPECT_EQ(config["methodConfig"][0]["retryPolicy"]["maxAttempts"].asInt(), 4);
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
