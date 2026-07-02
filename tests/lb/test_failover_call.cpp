#include "lb/failover_call.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "lb/endpoint_manager.h"

namespace {

using grpc::Status;
using grpc::StatusCode;

lb::EndpointManager::Options ManagerOptions() {
    return {std::chrono::milliseconds(1000), std::chrono::milliseconds(30000)};
}

TEST(CallWithFailover, FailsOverUntilAnEndpointSucceeds) {
    lb::EndpointManager manager(3, ManagerOptions());
    std::vector<std::size_t> calls;
    const Status status = lb::CallWithFailover(
        manager, lb::FailoverOptions{},
        [&](grpc::ClientContext&, std::size_t index) {
            calls.push_back(index);
            return index == 2 ? Status::OK
                              : Status(StatusCode::UNAVAILABLE, "connection refused");
        });
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(calls, (std::vector<std::size_t>{0, 1, 2}));
}

TEST(CallWithFailover, StopsOnFirstSuccess) {
    lb::EndpointManager manager(3, ManagerOptions());
    int call_count = 0;
    const Status status = lb::CallWithFailover(
        manager, lb::FailoverOptions{},
        [&](grpc::ClientContext&, std::size_t) {
            ++call_count;
            return Status::OK;
        });
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(call_count, 1);
}

TEST(CallWithFailover, DoesNotFailOverOnApplicationError) {
    lb::EndpointManager manager(3, ManagerOptions());
    int call_count = 0;
    const Status status = lb::CallWithFailover(
        manager, lb::FailoverOptions{},
        [&](grpc::ClientContext&, std::size_t) {
            ++call_count;
            return Status(StatusCode::INVALID_ARGUMENT, "bad request");
        });
    EXPECT_EQ(status.error_code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(call_count, 1);  // an app error can never succeed elsewhere
}

TEST(CallWithFailover, RespectsAttemptCap) {
    lb::EndpointManager manager(3, ManagerOptions());
    lb::FailoverOptions options;
    options.max_attempts = 2;
    int call_count = 0;
    const Status status = lb::CallWithFailover(
        manager, options,
        [&](grpc::ClientContext&, std::size_t) {
            ++call_count;
            return Status(StatusCode::UNAVAILABLE, "down");
        });
    EXPECT_EQ(status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_EQ(call_count, 2);
}

TEST(CallWithFailover, SingleEndpointMeansSingleAttempt) {
    // Built-in channel retry owns the single-endpoint case; no app-level loop.
    lb::EndpointManager manager(1, ManagerOptions());
    int call_count = 0;
    const Status status = lb::CallWithFailover(
        manager, lb::FailoverOptions{},
        [&](grpc::ClientContext&, std::size_t) {
            ++call_count;
            return Status(StatusCode::UNAVAILABLE, "down");
        });
    EXPECT_EQ(status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_EQ(call_count, 1);
}

TEST(CallWithFailover, TriesEachEndpointAtMostOnce) {
    lb::EndpointManager manager(3, ManagerOptions());
    std::vector<std::size_t> calls;
    lb::CallWithFailover(manager, lb::FailoverOptions{},
                         [&](grpc::ClientContext&, std::size_t index) {
                             calls.push_back(index);
                             return Status(StatusCode::UNAVAILABLE, "down");
                         });
    std::vector<std::size_t> sorted = calls;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, (std::vector<std::size_t>{0, 1, 2}));  // all distinct
}

TEST(CallWithFailover, SetsDeadlineOnEachAttempt) {
    lb::EndpointManager manager(2, ManagerOptions());
    lb::FailoverOptions options;
    options.attempt_timeout = std::chrono::milliseconds(2000);
    int checked = 0;
    lb::CallWithFailover(
        manager, options, [&](grpc::ClientContext& context, std::size_t) {
            const auto now = std::chrono::system_clock::now();
            EXPECT_GT(context.deadline(), now);
            EXPECT_LE(context.deadline(), now + std::chrono::milliseconds(2500));
            ++checked;
            return Status(StatusCode::UNAVAILABLE, "down");
        });
    EXPECT_EQ(checked, 2);
}

TEST(CallWithFailover, ReportsFailuresToManager) {
    lb::EndpointManager manager(2, ManagerOptions());
    lb::CallWithFailover(manager, lb::FailoverOptions{},
                         [&](grpc::ClientContext&, std::size_t) {
                             return Status(StatusCode::UNAVAILABLE, "down");
                         });
    EXPECT_EQ(manager.GetSnapshot(0).failures, 1u);
    EXPECT_EQ(manager.GetSnapshot(1).failures, 1u);
    EXPECT_TRUE(manager.GetSnapshot(0).in_cooldown);
}

}  // namespace
