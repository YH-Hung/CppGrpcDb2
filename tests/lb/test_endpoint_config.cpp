#include "lb/endpoint_config.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

TEST(ParseEndpoints, SingleEntry) {
    const lb::ParseResult result = lb::ParseEndpoints("svc-a.corp.example.com:50051");
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].host, "svc-a.corp.example.com");
    EXPECT_EQ(result.endpoints[0].port, 50051);
    EXPECT_EQ(result.endpoints[0].Target(), "svc-a.corp.example.com:50051");
    EXPECT_TRUE(result.rejected.empty());
}

TEST(ParseEndpoints, MultipleEntriesWithWhitespace) {
    const lb::ParseResult result =
        lb::ParseEndpoints(" svc-a.example.com:50051 ,\tsvc-b.example.com:50052 ");
    ASSERT_EQ(result.endpoints.size(), 2u);
    EXPECT_EQ(result.endpoints[0], (lb::Endpoint{"svc-a.example.com", 50051}));
    EXPECT_EQ(result.endpoints[1], (lb::Endpoint{"svc-b.example.com", 50052}));
    EXPECT_TRUE(result.rejected.empty());
}

TEST(ParseEndpoints, EmptyEntriesSkippedSilently) {
    const lb::ParseResult result = lb::ParseEndpoints("a.example.com:1,,b.example.com:2,");
    ASSERT_EQ(result.endpoints.size(), 2u);
    EXPECT_TRUE(result.rejected.empty());
}

TEST(ParseEndpoints, MalformedEntriesRejected) {
    const lb::ParseResult result = lb::ParseEndpoints(
        "noport,:50051,host:,host:notaport,host:0,host:70000,ok.example.com:50051");
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].host, "ok.example.com");
    EXPECT_EQ(result.rejected.size(), 6u);
    EXPECT_EQ(result.rejected[0], "noport");
}

TEST(ParseEndpoints, EmptyInput) {
    const lb::ParseResult result = lb::ParseEndpoints("");
    EXPECT_TRUE(result.endpoints.empty());
    EXPECT_TRUE(result.rejected.empty());
}

class LoadLbConfigFromEnvTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("GRPC_TARGET_ENDPOINTS");
        unsetenv("GRPC_LB_COOLDOWN_BASE_MS");
        unsetenv("GRPC_LB_MAX_ATTEMPTS");
        unsetenv("GRPC_LB_ATTEMPT_TIMEOUT_MS");
    }
    void TearDown() override { SetUp(); }
};

TEST_F(LoadLbConfigFromEnvTest, DefaultsWhenUnset) {
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    ASSERT_EQ(config.endpoints.size(), 1u);
    EXPECT_EQ(config.endpoints[0], (lb::Endpoint{"localhost", 50051}));
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(1000));
    EXPECT_EQ(config.attempt_timeout, std::chrono::milliseconds(2000));
    EXPECT_EQ(config.max_attempts, 1);
    EXPECT_TRUE(config.rejected.empty());
}

TEST_F(LoadLbConfigFromEnvTest, ReadsEndpointList) {
    setenv("GRPC_TARGET_ENDPOINTS", "a.example.com:1, b.example.com:2", 1);
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    ASSERT_EQ(config.endpoints.size(), 2u);
    EXPECT_EQ(config.max_attempts, 2);  // default = endpoint count
}

TEST_F(LoadLbConfigFromEnvTest, AllMalformedFallsBackToDefaultAndReports) {
    setenv("GRPC_TARGET_ENDPOINTS", "bad,worse:", 1);
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    ASSERT_EQ(config.endpoints.size(), 1u);
    EXPECT_EQ(config.endpoints[0], (lb::Endpoint{"localhost", 50051}));
    EXPECT_EQ(config.rejected.size(), 2u);
}

TEST_F(LoadLbConfigFromEnvTest, ReadsCooldownAndClampsMaxAttempts) {
    setenv("GRPC_TARGET_ENDPOINTS", "a.example.com:1,b.example.com:2", 1);
    setenv("GRPC_LB_COOLDOWN_BASE_MS", "250", 1);
    setenv("GRPC_LB_MAX_ATTEMPTS", "99", 1);
    setenv("GRPC_LB_ATTEMPT_TIMEOUT_MS", "500", 1);
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(250));
    EXPECT_EQ(config.attempt_timeout, std::chrono::milliseconds(500));
    EXPECT_EQ(config.max_attempts, 2);  // clamped to endpoint count
}

TEST_F(LoadLbConfigFromEnvTest, InvalidNumericEnvFallsBack) {
    setenv("GRPC_TARGET_ENDPOINTS", "a.example.com:1", 1);
    setenv("GRPC_LB_COOLDOWN_BASE_MS", "soon", 1);
    setenv("GRPC_LB_MAX_ATTEMPTS", "-3", 1);
    setenv("GRPC_LB_ATTEMPT_TIMEOUT_MS", "0", 1);  // 0 rejected, falls back
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(1000));
    EXPECT_EQ(config.attempt_timeout, std::chrono::milliseconds(2000));
    EXPECT_EQ(config.max_attempts, 1);
}

}  // namespace
