#include "lb/endpoint_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using std::chrono::milliseconds;

struct FakeClock {
    lb::EndpointManager::TimePoint now{};
    lb::EndpointManager::Clock Fn() {
        return [this] { return now; };
    }
    void Advance(milliseconds d) { now += d; }
};

lb::EndpointManager::Options TestOptions() {
    return {milliseconds(1000), milliseconds(30000)};
}

TEST(EndpointManager, RoundRobinRotation) {
    FakeClock clock;
    lb::EndpointManager manager(3, TestOptions(), clock.Fn());
    EXPECT_EQ(manager.Select(), 0u);
    EXPECT_EQ(manager.Select(), 1u);
    EXPECT_EQ(manager.Select(), 2u);
    EXPECT_EQ(manager.Select(), 0u);
}

TEST(EndpointManager, CooldownSkipsFailedEndpoint) {
    FakeClock clock;
    lb::EndpointManager manager(3, TestOptions(), clock.Fn());
    manager.ReportFailure(1);
    EXPECT_EQ(manager.Select(), 0u);
    EXPECT_EQ(manager.Select(), 2u);  // 1 skipped
    EXPECT_EQ(manager.Select(), 0u);
}

TEST(EndpointManager, EndpointRejoinsAfterCooldownLapses) {
    FakeClock clock;
    lb::EndpointManager manager(2, TestOptions(), clock.Fn());
    manager.ReportFailure(0);
    EXPECT_EQ(manager.Select(), 1u);
    clock.Advance(milliseconds(1000));  // cooldown_base elapsed
    EXPECT_EQ(manager.Select(), 0u);    // rejoined, cursor was at 0
}

TEST(EndpointManager, CooldownGrowsExponentiallyAndCaps) {
    FakeClock clock;
    lb::EndpointManager manager(2, TestOptions(), clock.Fn());

    manager.ReportFailure(0);  // 1st failure: 1000ms
    clock.Advance(milliseconds(999));
    EXPECT_TRUE(manager.GetSnapshot(0).in_cooldown);
    clock.Advance(milliseconds(1));
    EXPECT_FALSE(manager.GetSnapshot(0).in_cooldown);

    manager.ReportFailure(0);  // 2nd consecutive failure: 2000ms
    clock.Advance(milliseconds(1999));
    EXPECT_TRUE(manager.GetSnapshot(0).in_cooldown);
    clock.Advance(milliseconds(1));
    EXPECT_FALSE(manager.GetSnapshot(0).in_cooldown);

    for (int i = 0; i < 10; ++i) manager.ReportFailure(0);  // way past the cap
    clock.Advance(milliseconds(29999));
    EXPECT_TRUE(manager.GetSnapshot(0).in_cooldown);
    clock.Advance(milliseconds(1));  // 30000ms cap reached
    EXPECT_FALSE(manager.GetSnapshot(0).in_cooldown);
}

TEST(EndpointManager, AllDownPicksSoonestRecovering) {
    FakeClock clock;
    lb::EndpointManager manager(2, TestOptions(), clock.Fn());
    manager.ReportFailure(1);  // until t0+1000
    manager.ReportFailure(0);  // 1st failure for 0, also until t0+1000
    manager.ReportFailure(0);  // 2nd consecutive: until t0+2000
    EXPECT_EQ(manager.Select(), 1u);  // recovers soonest
}

TEST(EndpointManager, SuccessResetsStreakAndReportsRecovery) {
    FakeClock clock;
    lb::EndpointManager manager(2, TestOptions(), clock.Fn());
    EXPECT_FALSE(manager.ReportSuccess(0));  // no streak to end
    manager.ReportFailure(0);
    manager.ReportFailure(0);
    EXPECT_TRUE(manager.ReportSuccess(0));   // recovered
    EXPECT_FALSE(manager.GetSnapshot(0).in_cooldown);
    EXPECT_EQ(manager.GetSnapshot(0).consecutive_failures, 0);
    EXPECT_EQ(manager.GetSnapshot(0).successes, 2u);
    EXPECT_EQ(manager.GetSnapshot(0).failures, 2u);
    manager.ReportFailure(0);  // streak reset: cooldown is base again
    clock.Advance(milliseconds(1000));
    EXPECT_FALSE(manager.GetSnapshot(0).in_cooldown);
}

TEST(EndpointManager, RejectsZeroEndpoints) {
    EXPECT_THROW(lb::EndpointManager(0), std::invalid_argument);
}

TEST(EndpointManager, SelectSkipsAlreadyTriedEndpoints) {
    FakeClock clock;
    lb::EndpointManager manager(3, TestOptions(), clock.Fn());
    std::vector<bool> tried(3, false);
    tried[0] = true;  // excluded even though round-robin would start here
    EXPECT_EQ(manager.Select(tried), 1u);
}

TEST(EndpointManager, SelectThrowsWhenEveryEndpointExcluded) {
    lb::EndpointManager manager(2, TestOptions());
    std::vector<bool> tried(2, true);
    EXPECT_THROW(manager.Select(tried), std::invalid_argument);
}

TEST(EndpointManager, SelectAllDownPicksSoonestUntried) {
    // Regression for the failover fallback: when every endpoint is cooling down
    // and the soonest-recovering one has already been tried, selection must land
    // on the untried endpoint recovering soonest — not probe forward by index.
    FakeClock clock;
    lb::EndpointManager manager(3, TestOptions(), clock.Fn());
    manager.ReportFailure(0);  // tried below; until t0+1000
    manager.ReportFailure(2);  // until t0+1000 (soonest untried)
    manager.ReportFailure(1);
    manager.ReportFailure(1);  // 2nd consecutive: until t0+2000 (later untried)

    std::vector<bool> tried(3, false);
    tried[0] = true;
    EXPECT_EQ(manager.Select(tried), 2u);  // not 1 (index-forward from 0)
}

TEST(EndpointManager, SingleEndpointAlwaysSelectedEvenInCooldown) {
    FakeClock clock;
    lb::EndpointManager manager(1, TestOptions(), clock.Fn());
    manager.ReportFailure(0);
    EXPECT_EQ(manager.Select(), 0u);
}

TEST(EndpointManager, ConcurrentSelectSmoke) {
    lb::EndpointManager manager(4, TestOptions());  // real clock
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                const std::size_t index = manager.Select();
                if (index >= 4) failed = true;
                if (i % 3 == 0) manager.ReportFailure(index);
                else manager.ReportSuccess(index);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_FALSE(failed);
}

}  // namespace
