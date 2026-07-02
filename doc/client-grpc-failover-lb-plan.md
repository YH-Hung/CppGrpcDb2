# Client-Side gRPC Failover LB Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Client-side load balancing across multiple FQDN gRPC endpoints (from `GRPC_TARGET_ENDPOINTS`) with failover to the next endpoint on `UNAVAILABLE`, degenerating to gRPC built-in LB + retry when a single endpoint is configured.

**Architecture:** A compiled static library `grpc_client_lb` under `src/lb/` with three units — env/config parsing (pure), an adaptive round-robin + exponential-cooldown `EndpointManager` (gRPC-free, injectable clock), and a channel factory (per-endpoint channels sharing retry service config + keepalive args) — plus a header-only `CallWithFailover` function template and a demo binary `greeter_failover_client`.

**Tech Stack:** C++20, gRPC C++ (sync stubs), jsoncpp (service config JSON), spdlog, GTest, CMake.

**Spec:** `doc/client-grpc-failover-lb-design.md` (committed as e13130c).

## Global Constraints

- All targets pin `cxx_std_20` (repo rule; never lower).
- Namespace is `lb` — one namespace per `src/` subdirectory, matching `util`, `worker`, `resource`.
- `src/` is globally on the include path (root `CMakeLists.txt:44`); include headers as `"lb/endpoint_config.h"`.
- Env contract: `GRPC_TARGET_ENDPOINTS` (comma-separated `host:port`, no default port, fallback `localhost:50051`), `GRPC_LB_COOLDOWN_BASE_MS` (default 1000), `GRPC_LB_MAX_ATTEMPTS` (default = endpoint count, clamped to endpoint count).
- Cooldown: `base × 2^(consecutive_failures − 1)`, capped at 30 000 ms.
- Built-in retry `maxAttempts`: 2 in multi-endpoint mode, 4 in single-endpoint mode; `retryableStatusCodes = ["UNAVAILABLE"]`.
- Failover retriable set defaults to `{UNAVAILABLE}`; non-retriable statuses return immediately.
- Fresh `grpc::ClientContext` per attempt — contexts are single-use.
- Never check in generated `.pb.*` files.
- Build/test commands: `cmake --build build --target <t>` and `ctest --test-dir build -R <regex> --output-on-failure`. The build dir is preconfigured with `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local`.
- Commit message style: short lowercase phrase (see `git log --oneline`).

---

### Task 1: Endpoint config parsing (`lb::ParseEndpoints`, `lb::LoadLbConfigFromEnv`)

**Files:**
- Create: `src/lb/endpoint_config.h`
- Create: `src/lb/endpoint_config.cpp`
- Create: `tests/lb/test_endpoint_config.cpp`
- Modify: `CMakeLists.txt` (after the `test_string_util` block, ~line 358)

**Interfaces:**
- Consumes: nothing (pure + `std::getenv`).
- Produces:
  - `struct lb::Endpoint { std::string host; int port; std::string Target() const; }` (`Target()` returns `"host:port"`)
  - `struct lb::ParseResult { std::vector<Endpoint> endpoints; std::vector<std::string> rejected; }`
  - `lb::ParseResult lb::ParseEndpoints(std::string_view csv)`
  - `struct lb::LbConfig { std::vector<Endpoint> endpoints; std::chrono::milliseconds cooldown_base; int max_attempts; std::vector<std::string> rejected; }`
  - `lb::LbConfig lb::LoadLbConfigFromEnv()`

Parsing rules (refines the spec): entries that are empty after trimming (e.g. trailing comma) are skipped silently; non-empty malformed entries (no colon, empty host, bad port) go into `rejected`. Parsing never logs — callers log `rejected`.

- [ ] **Step 1: Write the failing test**

Create `tests/lb/test_endpoint_config.cpp`:

```cpp
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
    }
    void TearDown() override { SetUp(); }
};

TEST_F(LoadLbConfigFromEnvTest, DefaultsWhenUnset) {
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    ASSERT_EQ(config.endpoints.size(), 1u);
    EXPECT_EQ(config.endpoints[0], (lb::Endpoint{"localhost", 50051}));
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(1000));
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
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(250));
    EXPECT_EQ(config.max_attempts, 2);  // clamped to endpoint count
}

TEST_F(LoadLbConfigFromEnvTest, InvalidNumericEnvFallsBack) {
    setenv("GRPC_TARGET_ENDPOINTS", "a.example.com:1", 1);
    setenv("GRPC_LB_COOLDOWN_BASE_MS", "soon", 1);
    setenv("GRPC_LB_MAX_ATTEMPTS", "-3", 1);
    const lb::LbConfig config = lb::LoadLbConfigFromEnv();
    EXPECT_EQ(config.cooldown_base, std::chrono::milliseconds(1000));
    EXPECT_EQ(config.max_attempts, 1);
}

}  // namespace
```

- [ ] **Step 2: Add CMake target and declaration-only header + stub impl**

Append to `CMakeLists.txt` after the `test_string_util` block (~line 358), matching the narrow-test pattern used by `test_sql_util`:

```cmake
# LB endpoint config unit tests
add_executable(lb_endpoint_config_tests tests/lb/test_endpoint_config.cpp src/lb/endpoint_config.cpp)
target_include_directories(lb_endpoint_config_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lb_endpoint_config_tests PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME lb_endpoint_config_tests COMMAND lb_endpoint_config_tests)
target_compile_features(lb_endpoint_config_tests PRIVATE cxx_std_20)
```

Create `src/lb/endpoint_config.h`:

```cpp
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace lb {

struct Endpoint {
    std::string host;
    int port = 0;

    std::string Target() const { return host + ":" + std::to_string(port); }
    bool operator==(const Endpoint&) const = default;
};

struct ParseResult {
    std::vector<Endpoint> endpoints;
    std::vector<std::string> rejected;  // non-empty entries that failed validation
};

// Splits a comma-separated "host:port" list. Entries empty after trimming are
// skipped silently. Pure: no logging, no env access — callers log `rejected`.
ParseResult ParseEndpoints(std::string_view csv);

struct LbConfig {
    std::vector<Endpoint> endpoints;
    std::chrono::milliseconds cooldown_base{1000};
    int max_attempts = 1;               // resolved: min(env value, endpoint count)
    std::vector<std::string> rejected;  // for caller-side logging
};

// Reads GRPC_TARGET_ENDPOINTS, GRPC_LB_COOLDOWN_BASE_MS, GRPC_LB_MAX_ATTEMPTS.
// Unset, empty, or fully rejected endpoint list falls back to localhost:50051.
LbConfig LoadLbConfigFromEnv();

}  // namespace lb
```

Create `src/lb/endpoint_config.cpp` with stubs so the target compiles but tests fail red:

```cpp
#include "lb/endpoint_config.h"

namespace lb {

ParseResult ParseEndpoints(std::string_view csv) {
    (void)csv;
    return {};
}

LbConfig LoadLbConfigFromEnv() {
    return {};
}

}  // namespace lb
```

- [ ] **Step 3: Build and run to verify the tests fail**

```bash
cmake --build build --target lb_endpoint_config_tests
./build/lb_endpoint_config_tests
```

Expected: compiles, then FAILED — e.g. `ParseEndpoints.SingleEntry` asserts `result.endpoints.size() == 1` but gets 0.

- [ ] **Step 4: Implement**

Replace `src/lb/endpoint_config.cpp` with:

```cpp
#include "lb/endpoint_config.h"

#include <algorithm>
#include <cstdlib>

namespace lb {
namespace {

std::string_view Trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

bool ParsePort(std::string_view s, int& out) {
    if (s.empty() || s.size() > 5) return false;
    int value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 65535) return false;
    out = value;
    return true;
}

int ParsePositiveIntEnv(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 1000000) return fallback;
    return static_cast<int>(parsed);
}

}  // namespace

ParseResult ParseEndpoints(std::string_view csv) {
    ParseResult result;
    std::size_t pos = 0;
    while (pos < csv.size()) {
        const std::size_t comma = std::min(csv.find(',', pos), csv.size());
        const std::string_view entry = Trim(csv.substr(pos, comma - pos));
        pos = comma + 1;

        if (entry.empty()) continue;  // trailing/duplicate commas are harmless
        const std::size_t colon = entry.rfind(':');
        int port = 0;
        if (colon == std::string_view::npos || colon == 0 ||
            !ParsePort(entry.substr(colon + 1), port)) {
            result.rejected.emplace_back(entry);
            continue;
        }
        result.endpoints.push_back({std::string(entry.substr(0, colon)), port});
    }
    return result;
}

LbConfig LoadLbConfigFromEnv() {
    LbConfig config;
    const char* csv = std::getenv("GRPC_TARGET_ENDPOINTS");
    ParseResult parsed =
        ParseEndpoints(csv == nullptr ? std::string_view{} : std::string_view{csv});
    config.endpoints = std::move(parsed.endpoints);
    config.rejected = std::move(parsed.rejected);
    if (config.endpoints.empty()) {
        config.endpoints.push_back({"localhost", 50051});
    }
    config.cooldown_base =
        std::chrono::milliseconds(ParsePositiveIntEnv("GRPC_LB_COOLDOWN_BASE_MS", 1000));
    const int endpoint_count = static_cast<int>(config.endpoints.size());
    config.max_attempts =
        std::min(ParsePositiveIntEnv("GRPC_LB_MAX_ATTEMPTS", endpoint_count), endpoint_count);
    return config;
}

}  // namespace lb
```

- [ ] **Step 5: Build and run to verify the tests pass**

```bash
cmake --build build --target lb_endpoint_config_tests
./build/lb_endpoint_config_tests
```

Expected: `[  PASSED  ] 10 tests.`

- [ ] **Step 6: Commit**

```bash
git add src/lb/endpoint_config.h src/lb/endpoint_config.cpp tests/lb/test_endpoint_config.cpp CMakeLists.txt
git commit -m "add lb endpoint config parsing"
```

---

### Task 2: `lb::EndpointManager` — round-robin selection with exponential cooldown

**Files:**
- Create: `src/lb/endpoint_manager.h`
- Create: `src/lb/endpoint_manager.cpp`
- Create: `tests/lb/test_endpoint_manager.cpp`
- Modify: `CMakeLists.txt` (right after the `lb_endpoint_config_tests` block from Task 1)

**Interfaces:**
- Consumes: nothing from other tasks (standalone; indices refer to a caller-owned parallel container of channels/stubs).
- Produces:
  - `class lb::EndpointManager` with:
    - `using TimePoint = std::chrono::steady_clock::time_point;`
    - `using Clock = std::function<TimePoint()>;`
    - `struct Options { std::chrono::milliseconds cooldown_base{1000}; std::chrono::milliseconds cooldown_cap{30000}; };`
    - `struct Snapshot { std::uint64_t successes; std::uint64_t failures; int consecutive_failures; bool in_cooldown; };`
    - `explicit EndpointManager(std::size_t endpoint_count, Options options = {}, Clock clock = <steady_clock::now>);`
    - `std::size_t Select();`
    - `bool ReportSuccess(std::size_t index);` — returns true if this ended a failure streak
    - `void ReportFailure(std::size_t index);`
    - `Snapshot GetSnapshot(std::size_t index) const;`
    - `std::size_t size() const;`

- [ ] **Step 1: Write the failing test**

Create `tests/lb/test_endpoint_manager.cpp`:

```cpp
#include "lb/endpoint_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
```

- [ ] **Step 2: Add CMake target and header + stub impl**

Append to `CMakeLists.txt` after the `lb_endpoint_config_tests` block:

```cmake
# LB endpoint manager unit tests
add_executable(lb_endpoint_manager_tests tests/lb/test_endpoint_manager.cpp src/lb/endpoint_manager.cpp)
target_include_directories(lb_endpoint_manager_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lb_endpoint_manager_tests PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME lb_endpoint_manager_tests COMMAND lb_endpoint_manager_tests)
target_compile_features(lb_endpoint_manager_tests PRIVATE cxx_std_20)
```

Create `src/lb/endpoint_manager.h`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lb {

// Round-robin endpoint selection with exponential cooldown for failing
// endpoints. Indices refer to a caller-owned parallel container of
// channels/stubs. Thread-safe. gRPC-free; the clock is injectable so cooldown
// logic is testable without sleeping.
class EndpointManager {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;

    struct Options {
        std::chrono::milliseconds cooldown_base{1000};
        std::chrono::milliseconds cooldown_cap{30000};
    };

    struct Snapshot {
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
        int consecutive_failures = 0;
        bool in_cooldown = false;
    };

    explicit EndpointManager(
        std::size_t endpoint_count, Options options = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });

    // Next endpoint to try: round-robin over endpoints not in cooldown. If all
    // are cooling down, returns the one whose cooldown expires soonest rather
    // than failing the call outright.
    std::size_t Select();

    // Returns true if this success ended a failure streak (endpoint recovered).
    bool ReportSuccess(std::size_t index);

    void ReportFailure(std::size_t index);

    Snapshot GetSnapshot(std::size_t index) const;

    std::size_t size() const { return states_.size(); }

private:
    struct State {
        int consecutive_failures = 0;
        TimePoint unavailable_until{};
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
    };

    Options options_;
    Clock clock_;
    mutable std::mutex mu_;
    std::vector<State> states_;
    std::size_t cursor_ = 0;
};

}  // namespace lb
```

Create `src/lb/endpoint_manager.cpp` with stubs (compiles, tests fail red):

```cpp
#include "lb/endpoint_manager.h"

namespace lb {

EndpointManager::EndpointManager(std::size_t endpoint_count, Options options, Clock clock)
    : options_(options), clock_(std::move(clock)), states_(endpoint_count) {}

std::size_t EndpointManager::Select() { return 0; }

bool EndpointManager::ReportSuccess(std::size_t index) {
    (void)index;
    return false;
}

void EndpointManager::ReportFailure(std::size_t index) { (void)index; }

EndpointManager::Snapshot EndpointManager::GetSnapshot(std::size_t index) const {
    (void)index;
    return {};
}

}  // namespace lb
```

- [ ] **Step 3: Build and run to verify the tests fail**

```bash
cmake --build build --target lb_endpoint_manager_tests
./build/lb_endpoint_manager_tests
```

Expected: FAILED — e.g. `RoundRobinRotation` expects `Select()` to return 1 on the second call but gets 0.

- [ ] **Step 4: Implement**

Replace `src/lb/endpoint_manager.cpp` with:

```cpp
#include "lb/endpoint_manager.h"

#include <algorithm>
#include <cassert>

namespace lb {

EndpointManager::EndpointManager(std::size_t endpoint_count, Options options, Clock clock)
    : options_(options), clock_(std::move(clock)), states_(endpoint_count) {
    assert(endpoint_count > 0);
}

std::size_t EndpointManager::Select() {
    std::lock_guard<std::mutex> lock(mu_);
    const TimePoint now = clock_();
    for (std::size_t k = 0; k < states_.size(); ++k) {
        const std::size_t index = (cursor_ + k) % states_.size();
        if (states_[index].unavailable_until <= now) {
            cursor_ = (index + 1) % states_.size();
            return index;
        }
    }
    // All endpoints cooling down: pick the one recovering soonest.
    const auto soonest = std::min_element(
        states_.begin(), states_.end(), [](const State& a, const State& b) {
            return a.unavailable_until < b.unavailable_until;
        });
    return static_cast<std::size_t>(soonest - states_.begin());
}

bool EndpointManager::ReportSuccess(std::size_t index) {
    std::lock_guard<std::mutex> lock(mu_);
    State& state = states_.at(index);
    ++state.successes;
    const bool recovered = state.consecutive_failures > 0;
    state.consecutive_failures = 0;
    state.unavailable_until = TimePoint{};
    return recovered;
}

void EndpointManager::ReportFailure(std::size_t index) {
    std::lock_guard<std::mutex> lock(mu_);
    State& state = states_.at(index);
    ++state.failures;
    ++state.consecutive_failures;
    const int exponent = std::min(state.consecutive_failures - 1, 20);  // bound the shift
    auto cooldown = options_.cooldown_base * (1LL << exponent);
    cooldown = std::min<std::chrono::milliseconds>(cooldown, options_.cooldown_cap);
    state.unavailable_until = clock_() + cooldown;
}

EndpointManager::Snapshot EndpointManager::GetSnapshot(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    const State& state = states_.at(index);
    return {state.successes, state.failures, state.consecutive_failures,
            state.unavailable_until > clock_()};
}

}  // namespace lb
```

- [ ] **Step 5: Build and run to verify the tests pass**

```bash
cmake --build build --target lb_endpoint_manager_tests
./build/lb_endpoint_manager_tests
```

Expected: `[  PASSED  ] 8 tests.`

- [ ] **Step 6: Commit**

```bash
git add src/lb/endpoint_manager.h src/lb/endpoint_manager.cpp tests/lb/test_endpoint_manager.cpp CMakeLists.txt
git commit -m "add lb endpoint manager with cooldown"
```

---

### Task 3: Channel factory + `grpc_client_lb` library target

**Files:**
- Create: `src/lb/channel_factory.h`
- Create: `src/lb/channel_factory.cpp`
- Create: `tests/lb/test_channel_factory.cpp`
- Modify: `CMakeLists.txt` — library block after the `calldata_metrics` block (~line 177), test block after `lb_endpoint_manager_tests`

**Interfaces:**
- Consumes: `lb::Endpoint` from Task 1 (`endpoint.Target()`).
- Produces:
  - `struct lb::ChannelFactoryOptions { bool multi_endpoint = false; }`
  - `std::string lb::BuildServiceConfigJson(bool multi_endpoint)` — retryPolicy matching all methods; `maxAttempts` 2 (multi) / 4 (single)
  - `grpc::ChannelArguments lb::MakeChannelArguments(const ChannelFactoryOptions&)`
  - `using lb::ChannelBuilder = std::function<std::shared_ptr<grpc::Channel>(const Endpoint&, const grpc::ChannelArguments&)>;`
  - `std::vector<std::shared_ptr<grpc::Channel>> lb::BuildChannels(const std::vector<Endpoint>&, const ChannelFactoryOptions&, ChannelBuilder builder = nullptr)` — default builder: `CreateCustomChannel` + `InsecureChannelCredentials`
  - CMake target `grpc_client_lb` (static lib: endpoint_config.cpp, endpoint_manager.cpp, channel_factory.cpp)

- [ ] **Step 1: Write the failing test**

Create `tests/lb/test_channel_factory.cpp`:

```cpp
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
```

- [ ] **Step 2: Add the library target, test target, header + stub impl**

Insert into `CMakeLists.txt` after the `calldata_metrics` block (before the DB2 Wrapper Library section, ~line 178):

```cmake
# ==============================================================================
# Client-side LB Library
# ==============================================================================

add_library(grpc_client_lb
        src/lb/endpoint_config.cpp
        src/lb/endpoint_manager.cpp
        src/lb/channel_factory.cpp
)

target_include_directories(grpc_client_lb
    PUBLIC ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(grpc_client_lb
    PUBLIC ${_GRPC_GRPCPP}
    PUBLIC ${SPDLOG_TARGET}
    PRIVATE ${JSONCPP_TARGET}
)

target_compile_features(grpc_client_lb PUBLIC cxx_std_20)
```

Append the test target after the `lb_endpoint_manager_tests` block (links the library; jsoncpp is PRIVATE to the library so the test links it itself for parsing):

```cmake
# LB channel factory unit tests
add_executable(lb_channel_factory_tests tests/lb/test_channel_factory.cpp)
target_link_libraries(lb_channel_factory_tests
    PRIVATE GTest::gtest
    PRIVATE GTest::gtest_main
    PRIVATE grpc_client_lb
    PRIVATE ${JSONCPP_TARGET}
)
add_test(NAME lb_channel_factory_tests COMMAND lb_channel_factory_tests)
target_compile_features(lb_channel_factory_tests PRIVATE cxx_std_20)
```

Create `src/lb/channel_factory.h`:

```cpp
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
```

Create `src/lb/channel_factory.cpp` with stubs:

```cpp
#include "lb/channel_factory.h"

namespace lb {

std::string BuildServiceConfigJson(bool multi_endpoint) {
    (void)multi_endpoint;
    return "{}";
}

grpc::ChannelArguments MakeChannelArguments(const ChannelFactoryOptions& options) {
    (void)options;
    return {};
}

std::vector<std::shared_ptr<grpc::Channel>> BuildChannels(
    const std::vector<Endpoint>& endpoints, const ChannelFactoryOptions& options,
    ChannelBuilder builder) {
    (void)endpoints;
    (void)options;
    (void)builder;
    return {};
}

}  // namespace lb
```

- [ ] **Step 3: Build and run to verify the tests fail**

```bash
cmake --build build --target lb_channel_factory_tests
./build/lb_channel_factory_tests
```

Expected: FAILED — e.g. `BuildServiceConfigJson.MultiEndpointLowersRetryBudget` gets `maxAttempts` 0 from the empty JSON.

- [ ] **Step 4: Implement**

Replace `src/lb/channel_factory.cpp` with:

```cpp
#include "lb/channel_factory.h"

#include <json/json.h>

namespace lb {

std::string BuildServiceConfigJson(bool multi_endpoint) {
    Json::Value retry_policy;
    retry_policy["maxAttempts"] = multi_endpoint ? 2 : 4;
    retry_policy["initialBackoff"] = "0.1s";
    retry_policy["maxBackoff"] = "1s";
    retry_policy["backoffMultiplier"] = 2;
    Json::Value retryable_codes(Json::arrayValue);
    retryable_codes.append("UNAVAILABLE");
    retry_policy["retryableStatusCodes"] = retryable_codes;

    Json::Value method_entry;
    // An empty name entry matches every service and method on the channel.
    Json::Value name_array(Json::arrayValue);
    name_array.append(Json::Value(Json::objectValue));
    method_entry["name"] = name_array;
    method_entry["retryPolicy"] = retry_policy;

    Json::Value method_config(Json::arrayValue);
    method_config.append(method_entry);
    Json::Value service_config;
    service_config["methodConfig"] = method_config;

    Json::StreamWriterBuilder writer_builder;
    writer_builder["commentStyle"] = "None";
    writer_builder["indentation"] = "";
    return Json::writeString(writer_builder, service_config);
}

grpc::ChannelArguments MakeChannelArguments(const ChannelFactoryOptions& options) {
    grpc::ChannelArguments args;
    args.SetServiceConfigJSON(BuildServiceConfigJson(options.multi_endpoint));
    args.SetInt(GRPC_ARG_ENABLE_RETRIES, 1);
    args.SetLoadBalancingPolicyName("round_robin");
    // Detect half-dead connections in seconds instead of at the TCP timeout.
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    return args;
}

std::vector<std::shared_ptr<grpc::Channel>> BuildChannels(
    const std::vector<Endpoint>& endpoints, const ChannelFactoryOptions& options,
    ChannelBuilder builder) {
    if (!builder) {
        builder = [](const Endpoint& endpoint, const grpc::ChannelArguments& args) {
            return grpc::CreateCustomChannel(endpoint.Target(),
                                             grpc::InsecureChannelCredentials(), args);
        };
    }
    const grpc::ChannelArguments args = MakeChannelArguments(options);
    std::vector<std::shared_ptr<grpc::Channel>> channels;
    channels.reserve(endpoints.size());
    for (const Endpoint& endpoint : endpoints) {
        channels.push_back(builder(endpoint, args));
    }
    return channels;
}

}  // namespace lb
```

- [ ] **Step 5: Build and run to verify the tests pass; re-run earlier lb tests**

```bash
cmake --build build --target lb_channel_factory_tests
./build/lb_channel_factory_tests
ctest --test-dir build -R "^lb_" --output-on-failure
```

Expected: `[  PASSED  ] 4 tests.` then ctest: `3/3 tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/lb/channel_factory.h src/lb/channel_factory.cpp tests/lb/test_channel_factory.cpp CMakeLists.txt
git commit -m "add lb channel factory and grpc_client_lb library"
```

---

### Task 4: `lb::CallWithFailover` function template

**Files:**
- Create: `src/lb/failover_call.h` (header-only — it's a function template)
- Create: `tests/lb/test_failover_call.cpp`
- Modify: `CMakeLists.txt` (after the `lb_channel_factory_tests` block)

**Interfaces:**
- Consumes: `lb::EndpointManager` from Task 2 (`Select()`, `ReportSuccess(i)`, `ReportFailure(i)`, `size()`).
- Produces:
  - `struct lb::FailoverOptions { std::chrono::milliseconds attempt_timeout{2000}; int max_attempts = 0; std::vector<grpc::StatusCode> retriable_codes{grpc::StatusCode::UNAVAILABLE}; }` (`max_attempts <= 0` means one attempt per endpoint)
  - `template <typename Fn> grpc::Status lb::CallWithFailover(EndpointManager& manager, const FailoverOptions& options, Fn&& rpc)` where `rpc` is called as `grpc::Status rpc(grpc::ClientContext& context, std::size_t endpoint_index)`

- [ ] **Step 1: Write the failing test**

Create `tests/lb/test_failover_call.cpp`:

```cpp
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
```

- [ ] **Step 2: Add CMake target and a stub header**

Append to `CMakeLists.txt` after the `lb_channel_factory_tests` block:

```cmake
# LB failover call unit tests
add_executable(lb_failover_call_tests tests/lb/test_failover_call.cpp)
target_link_libraries(lb_failover_call_tests
    PRIVATE GTest::gtest
    PRIVATE GTest::gtest_main
    PRIVATE grpc_client_lb
)
add_test(NAME lb_failover_call_tests COMMAND lb_failover_call_tests)
target_compile_features(lb_failover_call_tests PRIVATE cxx_std_20)
```

Create `src/lb/failover_call.h` with a stub that compiles but fails red (immediately returns the first attempt's status without failover):

```cpp
#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "lb/endpoint_manager.h"

namespace lb {

struct FailoverOptions {
    std::chrono::milliseconds attempt_timeout{2000};
    int max_attempts = 0;  // <= 0: one attempt per endpoint
    std::vector<grpc::StatusCode> retriable_codes{grpc::StatusCode::UNAVAILABLE};
};

template <typename Fn>
grpc::Status CallWithFailover(EndpointManager& manager, const FailoverOptions& options,
                              Fn&& rpc) {
    (void)options;
    grpc::ClientContext context;
    return rpc(context, manager.Select());
}

}  // namespace lb
```

- [ ] **Step 3: Build and run to verify the tests fail**

```bash
cmake --build build --target lb_failover_call_tests
./build/lb_failover_call_tests
```

Expected: FAILED — e.g. `FailsOverUntilAnEndpointSucceeds` returns UNAVAILABLE after one call instead of failing over to endpoint 2.

- [ ] **Step 4: Implement**

Replace `src/lb/failover_call.h` with:

```cpp
#pragma once

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "lb/endpoint_manager.h"

namespace lb {

struct FailoverOptions {
    std::chrono::milliseconds attempt_timeout{2000};
    int max_attempts = 0;  // <= 0: one attempt per endpoint
    std::vector<grpc::StatusCode> retriable_codes{grpc::StatusCode::UNAVAILABLE};
};

// Runs `rpc` against endpoints chosen by `manager`, failing over to the next
// endpoint on a retriable status. `rpc` is invoked as
//   grpc::Status rpc(grpc::ClientContext& context, std::size_t endpoint_index)
// and must issue the RPC through the stub matching `endpoint_index`.
//
// Layering: gRPC's built-in retry (service config) handles transient blips
// within an endpoint; this loop handles endpoint-level failure by moving to
// the next FQDN. Each endpoint is tried at most once per logical call, each
// attempt on a fresh ClientContext (contexts are single-use) with its own
// deadline. Non-retriable statuses (application errors) return immediately —
// they can never succeed on another endpoint.
template <typename Fn>
grpc::Status CallWithFailover(EndpointManager& manager, const FailoverOptions& options,
                              Fn&& rpc) {
    const std::size_t endpoint_count = manager.size();
    const std::size_t attempt_cap =
        options.max_attempts > 0
            ? std::min<std::size_t>(static_cast<std::size_t>(options.max_attempts),
                                    endpoint_count)
            : endpoint_count;

    std::vector<bool> tried(endpoint_count, false);
    grpc::Status last_status(grpc::StatusCode::UNAVAILABLE, "no endpoint attempted");

    for (std::size_t attempt = 0; attempt < attempt_cap; ++attempt) {
        std::size_t index = manager.Select();
        if (tried[index]) {
            // Cooldowns can steer Select() back to a tried endpoint; probe forward.
            for (std::size_t k = 0; k < endpoint_count && tried[index]; ++k) {
                index = (index + 1) % endpoint_count;
            }
            if (tried[index]) break;  // every endpoint tried
        }
        tried[index] = true;

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + options.attempt_timeout);
        grpc::Status status = rpc(context, index);

        if (status.ok()) {
            if (manager.ReportSuccess(index)) {
                spdlog::info("lb: endpoint {} recovered", index);
            }
            return status;
        }
        const bool retriable =
            std::find(options.retriable_codes.begin(), options.retriable_codes.end(),
                      status.error_code()) != options.retriable_codes.end();
        if (!retriable) {
            return status;
        }
        manager.ReportFailure(index);
        spdlog::warn("lb: endpoint {} unavailable ({}), failing over", index,
                     status.error_message());
        last_status = std::move(status);
    }
    return last_status;
}

}  // namespace lb
```

- [ ] **Step 5: Build and run to verify the tests pass; run the whole lb suite**

```bash
cmake --build build --target lb_failover_call_tests
./build/lb_failover_call_tests
ctest --test-dir build -R "^lb_" --output-on-failure
```

Expected: `[  PASSED  ] 8 tests.` then ctest: `4/4 tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/lb/failover_call.h tests/lb/test_failover_call.cpp CMakeLists.txt
git commit -m "add lb failover call template"
```

---

### Task 5: `greeter_failover_client` demo binary + docs

**Files:**
- Create: `src/greeter_failover_client.cpp`
- Modify: `CMakeLists.txt` (after the `greeter_girl_client` link block, ~line 232)
- Modify: `Readme.md` (new section; place near the other client/server run instructions)
- Modify: `CLAUDE.md` (one bullet in the Architecture list, after the `src/util/` bullet)

**Interfaces:**
- Consumes: everything from Tasks 1–4 — `lb::LoadLbConfigFromEnv()`, `lb::BuildChannels(endpoints, options)`, `lb::EndpointManager(count, options)`, `lb::CallWithFailover(manager, options, rpc)`; generated `helloworld::Greeter::NewStub` (same proto the other greeter clients use).
- Produces: the `greeter_failover_client` binary.

- [ ] **Step 1: Write the demo client**

Create `src/greeter_failover_client.cpp`:

```cpp
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
```

- [ ] **Step 2: Wire the CMake target**

Insert into `CMakeLists.txt` after the `greeter_girl_client` link block (~line 232). It is deliberately NOT added to the `GRPC_TARGETS` loop — that loop adds DB2 support, which this client doesn't need:

```cmake
# Failover LB demo client (no DB2)
create_grpc_executable(greeter_failover_client "src/greeter_failover_client.cpp")
target_link_libraries(greeter_failover_client
    PRIVATE grpc_client_lb
)
```

- [ ] **Step 3: Build everything and run the full test suite**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `greeter_failover_client` builds; all tests pass (pre-existing suite + 4 `lb_*` targets).

- [ ] **Step 4: Manually verify failover behavior**

Use `complex_proto_async` as the server — it serves `helloworld.Greeter` on
port 50051 and links no DB2. (Do NOT use `greeter_callback_server_no_db2`: it
registers only `hellogirl.GirlGreeter`, so `SayHello` returns `UNIMPLEMENTED`.)
Nothing listens on 60000, so endpoint 0 is dead:

```bash
./build/complex_proto_async 50051 &
SERVER_PID=$!
sleep 1
GRPC_TARGET_ENDPOINTS="localhost:60000,localhost:50051" ./build/greeter_failover_client
kill $SERVER_PID
```

Expected output: a `lb: endpoint 0 unavailable (...), failing over` warning, then `Greeter received: ... (via localhost:50051)`, exit code 0.

Also verify the single-endpoint fallback path:

```bash
./build/complex_proto_async 50051 &
SERVER_PID=$!
sleep 1
GRPC_TARGET_ENDPOINTS="localhost:50051" ./build/greeter_failover_client
kill $SERVER_PID
```

Expected: `Greeter received: ... (via localhost:50051)` with no failover warnings.

- [ ] **Step 5: Document**

Append to `Readme.md` (near the other run instructions):

````markdown
## Client-side failover load balancing

`greeter_failover_client` demonstrates client-side LB across multiple FQDN
endpoints with failover on `UNAVAILABLE`:

```bash
GRPC_TARGET_ENDPOINTS="svc-a.example.com:50051,svc-b.example.com:50051" ./build/greeter_failover_client
```

- `GRPC_TARGET_ENDPOINTS` — comma-separated `host:port` list (default `localhost:50051`)
- `GRPC_LB_COOLDOWN_BASE_MS` — base cooldown for a failing endpoint, doubles per
  consecutive failure, capped at 30 s (default `1000`)
- `GRPC_LB_MAX_ATTEMPTS` — max endpoints tried per call (default: all)

With a single endpoint the client relies purely on gRPC's built-in retry
(service config) and `round_robin` over the FQDN's DNS records. With multiple
endpoints, the built-in retry budget per channel is lowered (`maxAttempts` 2)
and endpoint-level failover moves the call to the next FQDN. Design:
`doc/client-grpc-failover-lb-design.md`.
````

Add one bullet to `CLAUDE.md`'s Architecture list after the `src/util/` bullet:

```markdown
- **`src/lb/` (`grpc_client_lb`)** → client-side failover LB across multiple FQDN endpoints (`GRPC_TARGET_ENDPOINTS`); see `doc/client-grpc-failover-lb-design.md`. Demo: `greeter_failover_client`.
```

- [ ] **Step 6: Commit**

```bash
git add src/greeter_failover_client.cpp CMakeLists.txt Readme.md CLAUDE.md
git commit -m "add greeter_failover_client demo"
```
