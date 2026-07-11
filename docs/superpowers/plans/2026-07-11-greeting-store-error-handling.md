# GreetingStore Error Handling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GreetingStore::GreetingFor` return `"Hello"` and log every Halcyon query, cursor-fetch, or row-mapping failure without throwing, and repair the Halcyon README link.

**Architecture:** Keep the existing public `GreetingStore` API. Add a repository-local CLI test double that drives Halcyon's real `Database -> QueryResult -> Row` pipeline, and use a private friend peer solely to inject that real `Database` into `GreetingStore` for focused tests.

**Tech Stack:** C++20, CMake 3.31+, Halcyon C++ Db2 client, spdlog, GTest.

## Global Constraints

- Keep the public `GreetingStore` API and successful lookup behavior unchanged.
- Keep the existing `BUILD_DB2_TESTS` gate.
- Do not depend on `/Users/yinghanhung/Projects/Halcyon` or any other sibling checkout.
- Do not require a live Db2 instance for the new regression tests.
- Keep the live `DB2_CONN_STR` test behavior unchanged.

---

### Task 1: Add self-contained regression tests and verify RED

**Files:**
- Create: `tests/greeting/greeting_store_test_driver.hpp`
- Modify: `include/greeting/greeting_store.hpp:10-38`
- Modify: `tests/greeting/test_greeting_store.cpp:1-29`

**Interfaces:**
- Consumes: `halcyon::detail::cli::ICliDriver`, `halcyon::Database::open`, and the existing private `GreetingStore(std::unique_ptr<halcyon::Database>)` constructor.
- Produces: `greeting::testing::GreetingStoreTestDriver`, with `Outcome::kGreeting`, `Outcome::kNullSalutation`, and `Outcome::kFetchError`; and `greeting::testing::GreetingStoreTestPeer::FromDatabase(halcyon::Database)`.

- [x] **Step 1: Add the minimal self-contained CLI test double**

Create `tests/greeting/greeting_store_test_driver.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace greeting::testing {

class GreetingStoreTestDriver final
    : public halcyon::detail::cli::ICliDriver {
 public:
  using ConnectionHandle = halcyon::detail::cli::ConnectionHandle;
  using ConnectionParams = halcyon::detail::cli::ConnectionParams;
  using StatementHandle = halcyon::detail::cli::StatementHandle;
  using Value = halcyon::detail::cli::Value;

  enum class Outcome { kGreeting, kNullSalutation, kFetchError };

  explicit GreetingStoreTestDriver(Outcome outcome,
                                   std::string salutation = "Bonjour")
      : outcome_(outcome), salutation_(std::move(salutation)) {}

  halcyon::Result<ConnectionHandle> connect(
      const ConnectionParams&) override {
    return static_cast<ConnectionHandle>(1);
  }

  halcyon::Result<void> disconnect(ConnectionHandle) override { return {}; }

  halcyon::Result<bool> isAlive(ConnectionHandle) override { return true; }

  halcyon::Result<StatementHandle> prepare(ConnectionHandle,
                                           const std::string&) override {
    return static_cast<StatementHandle>(1);
  }

  halcyon::Result<void> bindParams(
      StatementHandle, const std::vector<Value>&) override {
    return {};
  }

  halcyon::Result<std::int64_t> execute(StatementHandle) override {
    fetched_ = false;
    return std::int64_t{0};
  }

  halcyon::Result<std::size_t> columnCount(StatementHandle) override {
    return std::size_t{1};
  }

  halcyon::Result<std::string> columnName(StatementHandle,
                                          std::size_t) override {
    return std::string("salutation");
  }

  halcyon::Result<bool> fetch(StatementHandle) override {
    if (outcome_ == Outcome::kFetchError && !fetched_) {
      fetched_ = true;
      return MakeError(halcyon::ErrorCode::Connection,
                       "scripted fetch failure");
    }
    if (fetched_) return false;
    fetched_ = true;
    return true;
  }

  halcyon::Result<Value> getColumn(StatementHandle, std::size_t) override {
    if (outcome_ == Outcome::kNullSalutation) {
      return Value{halcyon::detail::cli::Null{}};
    }
    return Value{salutation_};
  }

  halcyon::Result<void> finalize(StatementHandle) override { return {}; }

  halcyon::Result<void> closeCursor(StatementHandle) override { return {}; }

  halcyon::Result<void> setAutoCommit(ConnectionHandle, bool) override {
    return {};
  }

  halcyon::Result<void> commit(ConnectionHandle) override { return {}; }

  halcyon::Result<void> rollback(ConnectionHandle) override { return {}; }

 private:
  static halcyon::Error MakeError(halcyon::ErrorCode code,
                                  std::string message) {
    halcyon::Error error;
    error.code = code;
    error.message = std::move(message);
    return error;
  }

  Outcome outcome_;
  std::string salutation_;
  bool fetched_ = false;
};

}  // namespace greeting::testing
```

- [x] **Step 2: Grant a narrowly scoped test peer constructor access**

In `include/greeting/greeting_store.hpp`, forward-declare the peer before
`GreetingStore` and friend it in the private section:

```cpp
namespace greeting {

namespace testing {
class GreetingStoreTestPeer;
}

class GreetingStore {
 public:
  GreetingStore(GreetingStore&&) noexcept;
  GreetingStore& operator=(GreetingStore&&) noexcept;
  GreetingStore(const GreetingStore&) = delete;
  GreetingStore& operator=(const GreetingStore&) = delete;
  ~GreetingStore();

  static std::optional<GreetingStore> OpenFromEnv();
  void EnsureSchema();
  std::string GreetingFor(std::string_view name);

 private:
  friend class testing::GreetingStoreTestPeer;
  explicit GreetingStore(std::unique_ptr<halcyon::Database> db);
  std::unique_ptr<halcyon::Database> db_;
};
```

Preserve the existing public comments around these declarations.

- [x] **Step 3: Add offline success, mapping-error, and fetch-error tests**

Extend `tests/greeting/test_greeting_store.cpp` with these includes and helpers:

```cpp
#include <memory>
#include <sstream>
#include <utility>

#include "greeting_store_test_driver.hpp"
#include "halcyon/halcyon.hpp"
#include "spdlog/sinks/ostream_sink.h"
#include "spdlog/spdlog.h"

namespace greeting::testing {

class GreetingStoreTestPeer {
 public:
  static GreetingStore FromDatabase(halcyon::Database db) {
    return GreetingStore(
        std::make_unique<halcyon::Database>(std::move(db)));
  }
};

}  // namespace greeting::testing

namespace {

using Outcome = greeting::testing::GreetingStoreTestDriver::Outcome;

greeting::GreetingStore MakeStore(Outcome outcome) {
  auto driver =
      std::make_shared<greeting::testing::GreetingStoreTestDriver>(outcome);
  halcyon::PoolConfig config;
  config.min = 1;
  config.max = 1;
  config.startMaintenanceThread = false;
  config.statementCacheSize = 0;
  auto db = halcyon::Database::open(driver, "test-connection", config);
  if (!db.ok()) throw std::runtime_error(db.error().message);
  return greeting::testing::GreetingStoreTestPeer::FromDatabase(
      std::move(db.value()));
}

class ScopedLogCapture {
 public:
  ScopedLogCapture()
      : previous_(spdlog::default_logger()),
        logger_(std::make_shared<spdlog::logger>(
            "greeting-store-test",
            std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_))) {
    spdlog::set_default_logger(logger_);
  }

  ~ScopedLogCapture() { spdlog::set_default_logger(previous_); }

  std::string str() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
  std::shared_ptr<spdlog::logger> previous_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace

TEST(GreetingStore, GreetingForReturnsMappedSalutationWithoutLiveDb2) {
  auto store = MakeStore(Outcome::kGreeting);
  EXPECT_EQ(store.GreetingFor("alice"), "Bonjour");
}

TEST(GreetingStore, GreetingForFallsBackWithoutThrowingOnMappingError) {
  ScopedLogCapture logs;
  auto store = MakeStore(Outcome::kNullSalutation);
  EXPECT_NO_THROW(EXPECT_EQ(store.GreetingFor("alice"), "Hello"));
  EXPECT_NE(logs.str().find("greeting row mapping failed"), std::string::npos);
}

TEST(GreetingStore, GreetingForLogsAndFallsBackOnFetchError) {
  ScopedLogCapture logs;
  auto store = MakeStore(Outcome::kFetchError);
  EXPECT_NO_THROW(EXPECT_EQ(store.GreetingFor("alice"), "Hello"));
  EXPECT_NE(logs.str().find("greeting result fetch failed"), std::string::npos);
}
```

Also add `#include <stdexcept>` because `MakeStore` reports setup failure with
`std::runtime_error`. Keep the two existing environment/live tests below these
new offline tests.

- [x] **Step 4: Configure and build the focused test target**

Run:

```bash
cmake -S . -B build-review \
  -DCMAKE_INSTALL_PREFIX=$HOME/.local \
  -DCMAKE_PREFIX_PATH=$HOME/.local \
  -DBUILD_DB2_TESTS=ON
cmake --build build-review --target greeting_store_tests --parallel 4
```

Expected: configuration and compilation succeed.

- [x] **Step 5: Run the new tests and verify RED**

Run:

```bash
env -u DB2_CONN_STR ./build-review/greeting_store_tests \
  --gtest_filter='GreetingStore.GreetingFor*' --gtest_color=no
```

Expected: `GreetingForReturnsMappedSalutationWithoutLiveDb2` passes;
`GreetingForFallsBackWithoutThrowingOnMappingError` fails because `row.as()`
throws; and `GreetingForLogsAndFallsBackOnFetchError` fails because no fetch
error is logged.

---

### Task 2: Make GreetingFor non-throwing and verify GREEN

**Files:**
- Modify: `src/greeting/greeting_store.cpp:66-78`
- Test: `tests/greeting/test_greeting_store.cpp`

**Interfaces:**
- Consumes: Halcyon `Row::try_as<std::string>()`, `QueryResult::ok()`, and `QueryResult::error()`.
- Produces: unchanged `std::string GreetingStore::GreetingFor(std::string_view name)` behavior with logged fallback for mapping and fetch errors.

- [x] **Step 1: Replace the throwing row conversion and inspect cursor completion**

Replace `GreetingStore::GreetingFor` with:

```cpp
std::string GreetingStore::GreetingFor(std::string_view name) {
  auto rs = db_->query(
      "SELECT salutation FROM greetings WHERE UPPER(name) = UPPER(?)",
      std::string(name));
  if (!rs.ok()) {
    spdlog::error("greeting lookup failed for '{}': {}", name,
                  rs.error().message);
    return kDefaultSalutation;
  }

  auto& result = rs.value();
  for (auto& row : result) {
    auto salutation = row.try_as<std::string>();
    if (!salutation.ok()) {
      spdlog::error("greeting row mapping failed for '{}': {}", name,
                    salutation.error().message);
      return kDefaultSalutation;
    }
    return std::get<0>(std::move(salutation.value()));
  }

  if (!result.ok()) {
    spdlog::error("greeting result fetch failed for '{}': {}", name,
                  result.error()->message);
  }
  return kDefaultSalutation;
}
```

- [x] **Step 2: Run the focused tests and verify GREEN**

Run:

```bash
cmake --build build-review --target greeting_store_tests --parallel 4
env -u DB2_CONN_STR ./build-review/greeting_store_tests \
  --gtest_filter='GreetingStore.GreetingFor*' --gtest_color=no
```

Expected: all three offline `GreetingFor*` tests pass, with no uncaught
exceptions.

- [x] **Step 3: Run the complete GreetingStore test binary**

Run:

```bash
env -u DB2_CONN_STR ./build-review/greeting_store_tests --gtest_color=no
```

Expected: four offline tests pass and the live Db2 test skips because
`DB2_CONN_STR` is unset.

- [x] **Step 4: Commit the tested error-handling change**

Run:

```bash
git add include/greeting/greeting_store.hpp \
  src/greeting/greeting_store.cpp \
  tests/greeting/greeting_store_test_driver.hpp \
  tests/greeting/test_greeting_store.cpp
git commit -m "fix: make greeting lookup failures non-throwing"
```

---

### Task 3: Repair the Halcyon documentation link

**Files:**
- Modify: `Readme.md:33`

**Interfaces:**
- Consumes: the canonical Halcyon repository URL `https://github.com/YH-Hung/Halcyon`.
- Produces: an actionable dependency link in the local environment setup documentation.

- [x] **Step 1: Replace the placeholder URL**

Change the README sentence to:

```markdown
Db2 access goes through the [Halcyon](https://github.com/YH-Hung/Halcyon) C++ Db2 client. Install it
```

- [x] **Step 2: Verify no placeholder Halcyon links remain**

Run:

```bash
rg -n '\[Halcyon\]\(https://github\.com/\)' Readme.md
```

Expected: the command produces no matches.

- [x] **Step 3: Commit the documentation correction**

Run:

```bash
git add Readme.md
git commit -m "docs: link to the Halcyon repository"
```

---

### Task 4: Run full verification

**Files:**
- Verify: `CMakeLists.txt`
- Verify: all source and test targets registered in `build-review`

**Interfaces:**
- Consumes: the complete configured CMake target graph with `BUILD_DB2_TESTS=ON`.
- Produces: fresh build and test evidence for the final handoff.

- [x] **Step 1: Build the complete configured tree**

Run:

```bash
cmake --build build-review --parallel 4
```

Expected: all targets compile and link successfully.

- [x] **Step 2: Run all registered tests without a live Db2**

Run:

```bash
env -u DB2_CONN_STR ctest --test-dir build-review --output-on-failure
```

Expected: all 17 registered test executables pass; the live case inside
`greeting_store_tests` reports a GTest skip without failing the CTest test.

- [x] **Step 3: Check repository hygiene**

Run:

```bash
git diff --check
git status --short --branch
```

Expected: no whitespace errors; only the committed implementation-plan file
may remain uncommitted until the plan is archived or committed with the final
work.
