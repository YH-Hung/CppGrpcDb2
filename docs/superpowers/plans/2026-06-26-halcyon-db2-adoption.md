# Halcyon Db2 Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the home-grown `db2_wrapper` with the installed Halcyon Db2 client and have both Db2-linked gRPC servers run a real, parameterized query (a personalized-greeting lookup) against a live Db2, with graceful fallback when no database is configured.

**Architecture:** A new `GreetingStore` library wraps a pooled `halcyon::Database` behind a tiny interface (`OpenFromEnv` / `EnsureSchema` / `GreetingFor`); it is the only code that touches Halcyon. `greeter_server` and `greeter_callback_server` hold an `std::optional<GreetingStore>` and use it in `SayHello`. CMake gains `find_package(Halcyon)`; the old `db2_wrapper` target, `cmake/db2.cmake`, `src/db2/`, and `include/db2/` are removed.

**Tech Stack:** C++20, CMake, gRPC, Halcyon (C++17 static lib at `$HOME/.local`), IBM Db2 CLI driver (`third_party/clidriver`), GoogleTest, Docker (`icr.io/db2_community/db2`).

---

## File Structure

- Create: `cmake/halcyon.cmake` — `find_package(Halcyon REQUIRED)`.
- Create: `include/greeting/greeting_store.hpp` — `GreetingStore` interface.
- Create: `src/greeting/greeting_store.cpp` — `GreetingStore` implementation (only file including `halcyon/halcyon.hpp`).
- Document: Db2 service `docker run` commands for live runs/tests.
- Create: `tests/greeting/test_greeting_store.cpp` — gated integration test.
- Modify: `CMakeLists.txt` — swap db2 include, drop `db2_wrapper`, add `greeting_store`, link only the two servers, swap tests.
- Modify: `cmake/common.cmake` — repoint `add_db2_support` at `greeting_store`.
- Modify: `src/greeter_server.cpp` — use `GreetingStore` in `SayHello`.
- Modify: `src/greeter_callback_server.cpp` — drop `ResourcePool<db2::Connection>`, use `GreetingStore`.
- Modify: `Readme.md`, `AGENTS.md`, `CLAUDE.md` — docs.
- Delete: `cmake/db2.cmake`, `src/db2/db2.cpp`, `include/db2/db2.hpp`.

---

## Task 1: Wire Halcyon into CMake and prove it compiles under C++20

**Files:**
- Create: `cmake/halcyon.cmake`
- Modify: `CMakeLists.txt` (line 14 region: `include(cmake/db2.cmake)`)

- [ ] **Step 1: Create `cmake/halcyon.cmake`**

```cmake
# Resolve the Halcyon Db2 client (installed under $HOME/.local).
# find_package transitively imports DB2::CLI via Halcyon's bundled
# FindDB2CLI.cmake, whose default DB2_CLIDRIVER_ROOT is
# ${CMAKE_SOURCE_DIR}/third_party/clidriver — exactly where this repo
# vendors the driver — so the CLI driver resolves with no extra config.
find_package(Halcyon REQUIRED)
message(STATUS "Halcyon found: target halcyon::halcyon")
```

- [ ] **Step 2: Swap the include in `CMakeLists.txt`**

Replace:
```cmake
# Resolve DB2 CLI Driver
include(cmake/db2.cmake)
```
with:
```cmake
# Resolve the Halcyon Db2 client (brings DB2::CLI transitively)
include(cmake/halcyon.cmake)
```

- [ ] **Step 3: Configure to verify Halcyon resolves**

Run:
```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
```
Expected: configure succeeds and prints `Halcyon found: target halcyon::halcyon`. (Build will fail later because `db2_wrapper` sources are still referenced — that is fixed in Task 3.)

- [ ] **Step 4: Commit**

```bash
git add cmake/halcyon.cmake CMakeLists.txt
git commit -m "build: resolve Halcyon Db2 client via find_package"
```

---

## Task 2: Create the `GreetingStore` library

**Files:**
- Create: `include/greeting/greeting_store.hpp`
- Create: `src/greeting/greeting_store.cpp`

- [ ] **Step 1: Write the header `include/greeting/greeting_store.hpp`**

```cpp
// Db2-backed greeting lookup, built on the Halcyon client.
// This is the only unit in CppGrpcDb2 that talks to Halcyon directly.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace halcyon { class Database; }

namespace greeting {

// Wraps a pooled halcyon::Database and exposes a tiny, request-friendly API.
// Construct only via OpenFromEnv(), so a returned store is always usable.
class GreetingStore {
 public:
  GreetingStore(GreetingStore&&) noexcept;
  GreetingStore& operator=(GreetingStore&&) noexcept;
  GreetingStore(const GreetingStore&) = delete;
  GreetingStore& operator=(const GreetingStore&) = delete;
  ~GreetingStore();

  // Opens a pooled Database from the DB2_CONN_STR environment variable.
  // Returns std::nullopt (and logs) when the var is unset or open fails.
  static std::optional<GreetingStore> OpenFromEnv();

  // Idempotently (re)creates and seeds the `greetings` demo table. Safe to call
  // on every startup; DROP errors (table absent) are ignored.
  void EnsureSchema();

  // Returns the stored salutation for `name` (case-insensitive), or "Hello"
  // when no row matches or the query fails. Never throws.
  std::string GreetingFor(std::string_view name);

 private:
  explicit GreetingStore(std::unique_ptr<halcyon::Database> db);
  std::unique_ptr<halcyon::Database> db_;
};

}  // namespace greeting
```

Note: `halcyon::Database` is held via `unique_ptr` with a forward declaration so the heavy `halcyon/halcyon.hpp` include stays out of the public header (consumers include only `<grpcpp>` etc.).

- [ ] **Step 2: Write the implementation `src/greeting/greeting_store.cpp`**

```cpp
#include "greeting/greeting_store.hpp"

#include <cstdlib>
#include <tuple>
#include <utility>

#include "halcyon/halcyon.hpp"
#include "spdlog/spdlog.h"

namespace greeting {

GreetingStore::GreetingStore(std::unique_ptr<halcyon::Database> db)
    : db_(std::move(db)) {}

GreetingStore::GreetingStore(GreetingStore&&) noexcept = default;
GreetingStore& GreetingStore::operator=(GreetingStore&&) noexcept = default;
GreetingStore::~GreetingStore() = default;

std::optional<GreetingStore> GreetingStore::OpenFromEnv() {
  const char* dsn = std::getenv("DB2_CONN_STR");
  if (!dsn || *dsn == '\0') {
    spdlog::warn("DB2_CONN_STR not set; greetings will use the default prefix");
    return std::nullopt;
  }
  halcyon::PoolConfig cfg;
  cfg.min = 2;
  cfg.max = 8;
  auto db = halcyon::Database::open(dsn, cfg);
  if (!db.ok()) {
    spdlog::error("Failed to open Db2 via Halcyon: {}", db.error().message);
    return std::nullopt;
  }
  spdlog::info("Connected to Db2 via Halcyon");
  return GreetingStore(
      std::make_unique<halcyon::Database>(std::move(db.value())));
}

void GreetingStore::EnsureSchema() {
  db_->execute("DROP TABLE greetings");  // ignore error if absent
  auto created = db_->execute(
      "CREATE TABLE greetings("
      "name VARCHAR(64) NOT NULL PRIMARY KEY, salutation VARCHAR(64))");
  if (!created.ok()) {
    spdlog::error("Failed to create greetings table: {}",
                  created.error().message);
    return;
  }
  const std::pair<const char*, const char*> seed[] = {
      {"alice", "Bonjour"}, {"bob", "Hola"}, {"yuki", "Konnichiwa"}};
  for (const auto& [name, salutation] : seed) {
    auto r = db_->execute(
        "INSERT INTO greetings(name, salutation) VALUES (?, ?)",
        std::string(name), std::string(salutation));
    if (!r.ok())
      spdlog::error("Failed to seed greeting for {}: {}", name,
                    r.error().message);
  }
  spdlog::info("greetings table ready ({} rows seeded)",
               sizeof(seed) / sizeof(seed[0]));
}

std::string GreetingFor_default() { return "Hello"; }

std::string GreetingStore::GreetingFor(std::string_view name) {
  auto rs = db_->query(
      "SELECT salutation FROM greetings WHERE UPPER(name) = UPPER(?)",
      std::string(name));
  if (!rs.ok()) {
    spdlog::error("greeting lookup failed for '{}': {}", name,
                  rs.error().message);
    return GreetingFor_default();
  }
  for (auto& row : rs.value()) {
    return std::get<0>(row.as<std::string>());
  }
  return GreetingFor_default();
}

}  // namespace greeting
```

- [ ] **Step 3: Add the `greeting_store` target to `CMakeLists.txt`**

Insert after the DB2 wrapper section is removed (see Task 3); for now add this block in the libraries area (after the `otel_tracing` target, before the old DB2 section):

```cmake
# ==============================================================================
# Greeting store (Db2 access via Halcyon)
# ==============================================================================

add_library(greeting_store
    src/greeting/greeting_store.cpp
)

target_include_directories(greeting_store
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(greeting_store
    PUBLIC halcyon::halcyon
    PUBLIC ${SPDLOG_TARGET}
)

target_compile_features(greeting_store PUBLIC cxx_std_20)
```

- [ ] **Step 4: Verify the library compiles (proves Halcyon headers are C++20-clean)**

Run:
```bash
cmake --build build --target greeting_store
```
Expected: `greeting_store` compiles and archives with no errors. This is the early C++20-compatibility check called out in the spec — if Halcyon's headers fail under C++20, it surfaces here.

- [ ] **Step 5: Commit**

```bash
git add include/greeting/greeting_store.hpp src/greeting/greeting_store.cpp CMakeLists.txt
git commit -m "feat: add Halcyon-backed GreetingStore library"
```

---

## Task 3: Remove the old db2_wrapper and repoint add_db2_support

**Files:**
- Modify: `cmake/common.cmake:26-31` (`add_db2_support`)
- Modify: `CMakeLists.txt` (db2_wrapper section ~203-220; GRPC_TARGETS loop ~227-252)
- Delete: `cmake/db2.cmake`, `src/db2/db2.cpp`, `include/db2/db2.hpp`

- [ ] **Step 1: Repoint `add_db2_support` in `cmake/common.cmake`**

Replace the function body:
```cmake
# Function to add DB2 support to a target
function(add_db2_support target_name)
    # Link against our DB2 wrapper (which itself links DB2::db2)
    target_link_libraries(${target_name}
        PRIVATE db2_wrapper
    )
endfunction()
```
with:
```cmake
# Function to add Db2 access (via the Halcyon-backed GreetingStore) to a target.
function(add_db2_support target_name)
    target_link_libraries(${target_name}
        PRIVATE greeting_store
    )
endfunction()
```

- [ ] **Step 2: Delete the `db2_wrapper` target block in `CMakeLists.txt`**

Remove the entire section:
```cmake
# ==============================================================================
# DB2 Wrapper Library
# ==============================================================================

add_library(db2_wrapper
    src/db2/db2.cpp
        src/util/sql_util.cpp
        src/util/sql_util.h
)

target_include_directories(db2_wrapper
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(db2_wrapper
    PUBLIC DB2::db2
)

target_compile_features(db2_wrapper PUBLIC cxx_std_20)
```

- [ ] **Step 3: Apply `add_db2_support` to only the two servers**

In the `GRPC_TARGETS` foreach loop, remove the unconditional `add_db2_support(${target_name})` call and instead guard it:
```cmake
foreach(target_name ${GRPC_TARGETS})
    # Create the base gRPC executable
    create_grpc_executable(${target_name} "src/${target_name}.cpp")

    # Add Db2 access (Halcyon-backed) only to the server binaries that query.
    if(target_name STREQUAL "greeter_server" OR target_name STREQUAL "greeter_callback_server")
        add_db2_support(${target_name})
    endif()

    # Add tracing support to all gRPC binaries
    add_tracing_support(${target_name})

    # Add interceptor support based on target type
    if(target_name STREQUAL "greeter_callback_server")
        add_interceptor_support(${target_name} STRING_TRANSFORM METRICS)
    elseif(target_name STREQUAL "greeter_server")
        add_interceptor_support(${target_name} METRICS)
    endif()
endforeach()
```

- [ ] **Step 4: Delete obsolete files**

```bash
git rm cmake/db2.cmake src/db2/db2.cpp include/db2/db2.hpp
```

- [ ] **Step 5: Reconfigure (build will still fail until servers are migrated in Tasks 4-5)**

Run:
```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
```
Expected: configure succeeds (no reference to `cmake/db2.cmake` or `db2_wrapper`).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "build: remove db2_wrapper; route Db2 support through greeting_store"
```

---

## Task 4: Migrate greeter_server (sync) to GreetingStore

**Files:**
- Modify: `src/greeter_server.cpp`

- [ ] **Step 1: Add the include and a store member**

Add near the existing includes (after `#include "otel_tracing.h"`):
```cpp
#include "greeting/greeting_store.hpp"
#include <optional>
```

Change `GreeterServiceImpl` to hold a store and use it:
```cpp
class GreeterServiceImpl final : public Greeter::Service {
public:
    explicit GreeterServiceImpl(std::optional<greeting::GreetingStore> store)
        : store_(std::move(store)) {}

    Status SayHello(ServerContext* context, const HelloRequest* request,
                    HelloReply* reply) override {
        if (context && context->IsCancelled()) {
            std::cout << "[trace_id: " << otel::TraceIdForServerContext(context)
                      << "] Request cancelled" << std::endl;
            return Status(grpc::StatusCode::CANCELLED, "Request cancelled");
        }
        std::string salutation =
            store_ ? store_->GreetingFor(request->name()) : "Hello";
        reply->set_message(salutation + " " + request->name());
        std::cout << "[trace_id: " << otel::TraceIdForServerContext(context)
                  << "] Received request for name: " << request->name() << std::endl;
        return Status::OK;
    }

private:
    std::optional<greeting::GreetingStore> store_;
};
```

- [ ] **Step 2: Open the store in `RunServer` before constructing the service**

Replace `GreeterServiceImpl service;` with:
```cpp
    auto store = greeting::GreetingStore::OpenFromEnv();
    if (store) store->EnsureSchema();
    GreeterServiceImpl service(std::move(store));
```

- [ ] **Step 3: Build the target**

Run:
```bash
cmake --build build --target greeter_server
```
Expected: links successfully against `greeting_store` / `halcyon::halcyon`.

- [ ] **Step 4: Commit**

```bash
git add src/greeter_server.cpp
git commit -m "feat: greeter_server greets via Db2 GreetingStore lookup"
```

---

## Task 5: Migrate greeter_callback_server to GreetingStore

**Files:**
- Modify: `src/greeter_callback_server.cpp`

- [ ] **Step 1: Replace the db2/resource_pool includes**

Remove:
```cpp
#include "db2/db2.hpp"
#include "resource/resource_pool.hpp"
```
Add (near `#include "otel_tracing.h"`):
```cpp
#include "greeting/greeting_store.hpp"
#include <optional>
```

- [ ] **Step 2: Replace the service impl's pool with a store**

Change the class to:
```cpp
class GreeterServiceImpl final : public Greeter::CallbackService {
 public:
  explicit GreeterServiceImpl(std::optional<greeting::GreetingStore> store)
      : store_(std::move(store)) {}

  ServerUnaryReactor* SayHello(CallbackServerContext* context,
                               const HelloRequest* request,
                               HelloReply* reply) override {
    const std::string trace_id = otel::TraceIdForServerContext(context);
    spdlog::info("[trace_id: {}] Received request for name: {}", trace_id, request->name());

    ServerUnaryReactor* reactor = context->DefaultReactor();
    if (context->IsCancelled()) {
      spdlog::warn("[trace_id: {}] Request was cancelled by client before processing.", trace_id);
      reactor->Finish(Status(grpc::StatusCode::CANCELLED, "Request cancelled"));
      return reactor;
    }

    std::string salutation =
        store_ ? store_->GreetingFor(request->name()) : "Hello";
    reply->set_message(salutation + " " + request->name());

    reactor->Finish(Status::OK);
    return reactor;
  }

 private:
  std::optional<greeting::GreetingStore> store_;
};
```

- [ ] **Step 3: Replace the pool creation in `RunServer`**

Remove the `using Db2Pool = ...; auto db2_pool = Db2Pool::create(...);` block and replace the service construction:
```cpp
  auto store = greeting::GreetingStore::OpenFromEnv();
  if (store) store->EnsureSchema();
  GreeterServiceImpl service(std::move(store));
```

- [ ] **Step 4: Build the target**

Run:
```bash
cmake --build build --target greeter_callback_server
```
Expected: compiles and links (no more `db2::` / `resource::ResourcePool` references).

- [ ] **Step 5: Commit**

```bash
git add src/greeter_callback_server.cpp
git commit -m "feat: greeter_callback_server greets via Db2 GreetingStore lookup"
```

---

## Task 6: Swap the Db2 tests

**Files:**
- Modify: `CMakeLists.txt` (BUILD_DB2_TESTS block ~413-451)
- Create: `tests/greeting/test_greeting_store.cpp`
- Delete: `tests/db2/test_db2_wrapper.cpp`

- [ ] **Step 1: Write the gated integration test `tests/greeting/test_greeting_store.cpp`**

```cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

#include "greeting/greeting_store.hpp"

namespace {
bool has_dsn() {
  const char* v = std::getenv("DB2_CONN_STR");
  return v && *v != '\0';
}
}  // namespace

TEST(GreetingStore, OpenFromEnvIsNulloptWithoutDsn) {
  if (has_dsn()) GTEST_SKIP() << "DB2_CONN_STR is set; covered by live test";
  auto store = greeting::GreetingStore::OpenFromEnv();
  EXPECT_FALSE(store.has_value());
}

TEST(GreetingStore, SeededNameReturnsSalutationLive) {
  if (!has_dsn()) GTEST_SKIP() << "DB2_CONN_STR not set; skipping live Db2 test";
  auto store = greeting::GreetingStore::OpenFromEnv();
  ASSERT_TRUE(store.has_value());
  store->EnsureSchema();
  EXPECT_EQ(store->GreetingFor("alice"), "Bonjour");
  EXPECT_EQ(store->GreetingFor("ALICE"), "Bonjour");  // case-insensitive
  EXPECT_EQ(store->GreetingFor("nobody"), "Hello");   // default fallback
}
```

- [ ] **Step 2: Replace the BUILD_DB2_TESTS block in `CMakeLists.txt`**

Replace the `db2_wrapper_tests` target (keep `resource_handle_refactor_tests` as-is) with:
```cmake
if(BUILD_DB2_TESTS)
    add_executable(greeting_store_tests
        tests/greeting/test_greeting_store.cpp
    )

    target_include_directories(greeting_store_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(greeting_store_tests
        PRIVATE GTest::gtest
        PRIVATE GTest::gtest_main
        PRIVATE greeting_store
    )

    add_test(NAME greeting_store_tests COMMAND greeting_store_tests)

    target_compile_features(greeting_store_tests PRIVATE cxx_std_20)
```
(Leave the existing `resource_handle_refactor_tests` block and the closing `endif()` intact.)

- [ ] **Step 3: Delete the old wrapper test**

```bash
git rm tests/db2/test_db2_wrapper.cpp
```

- [ ] **Step 4: Configure + build the test**

Run:
```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local -DBUILD_DB2_TESTS=ON
cmake --build build --target greeting_store_tests
```
Expected: builds. Without `DB2_CONN_STR`, run it:
```bash
./build/greeting_store_tests
```
Expected: `OpenFromEnvIsNulloptWithoutDsn` PASS, live test SKIPPED.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "test: replace db2_wrapper_tests with gated greeting_store_tests"
```

---

## Task 7: Document the Db2 Docker service commands

**Files:** none (documentation only)

- [ ] **Step 1: Document `docker run` command**

We document the equivalent `docker run` command to start a live Db2 container:

```bash
docker run -d \
  --name db2 \
  --privileged \
  -e LICENSE=accept \
  -e DB2INST1_PASSWORD=halcyon \
  -e DBNAME=SAMPLE \
  -e PERSISTENT_HOME=false \
  -p 50000:50000 \
  --health-cmd="su - db2inst1 -c 'db2 connect to SAMPLE' || exit 1" \
  --health-interval=20s \
  --health-timeout=10s \
  --health-retries=30 \
  icr.io/db2_community/db2:11.5.9.0
```

- [ ] **Step 2: Update documentation**

Update `AGENTS.md`, `Readme.md`, and `CLAUDE.md` to document the simple `docker run` commands instead of docker-compose.

---

## Task 8: Full build, no-DB run, and docs

**Files:**
- Modify: `Readme.md`, `AGENTS.md`, `CLAUDE.md`

- [ ] **Step 1: Full build of the whole tree**

Run:
```bash
cmake --build build
```
Expected: every target builds, including both servers, `greeting_store`, and `greeting_store_tests`.

- [ ] **Step 2: Run the unit test suite**

Run:
```bash
ctest --test-dir build --output-on-failure
```
Expected: all pass; `greeting_store_tests` live case skipped (no DSN).

- [ ] **Step 3: Smoke-run a server with no DB (graceful fallback)**

Run:
```bash
./build/greeter_server &
SERVER_PID=$!
sleep 1
grpcurl -plaintext -d '{"name":"alice"}' localhost:50051 helloworld.Greeter/SayHello
kill $SERVER_PID
```
Expected: a log line warning `DB2_CONN_STR not set` and a reply `{"message": "Hello alice"}`.

- [ ] **Step 4: Update docs**

In `CLAUDE.md`, `AGENTS.md`, and `Readme.md`:
- Replace mentions of `db2_wrapper` / `src/db2/db2.hpp` / `db2::Connection` with: Db2 access is provided by the Halcyon client (`halcyon::halcyon`) behind `greeting::GreetingStore` (`include/greeting/greeting_store.hpp`).
- Document `DB2_CONN_STR` (DSN format `DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;`), and the `docker run` workflow.
- Update the `BUILD_DB2_TESTS` note to point at `greeting_store_tests`.

- [ ] **Step 5: Commit**

```bash
git add Readme.md AGENTS.md CLAUDE.md
git commit -m "docs: document Halcyon adoption, GreetingStore, and Db2 docker workflow"
```

---

## Task 9 (verification, environment-permitting): live Db2 end-to-end

**Files:** none (verification only)

- [ ] **Step 1: Bring up Db2**

```bash
docker run -d \
  --name db2 \
  --privileged \
  -e LICENSE=accept \
  -e DB2INST1_PASSWORD=halcyon \
  -e DBNAME=SAMPLE \
  -e PERSISTENT_HOME=false \
  -p 50000:50000 \
  --health-cmd="su - db2inst1 -c 'db2 connect to SAMPLE' || exit 1" \
  --health-interval=20s \
  --health-timeout=10s \
  --health-retries=30 \
  icr.io/db2_community/db2:11.5.9.0

docker ps   # wait for STATUS = healthy (~2 min+)
```

- [ ] **Step 2: Run the live integration test**

```bash
export DB2_CONN_STR="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
./build/greeting_store_tests
```
Expected: `SeededNameReturnsSalutationLive` PASS (not skipped).

- [ ] **Step 3: End-to-end greeting via a server**

```bash
./build/greeter_callback_server &
SERVER_PID=$!
sleep 2
grpcurl -plaintext -d '{"name":"alice"}' localhost:50051 helloworld.Greeter/SayHello
kill $SERVER_PID
```
Expected: reply contains `Bonjour` for alice (the callback server's interceptors may also transform the string; the salutation must be Db2-sourced).

- [ ] **Step 4: Tear down**

```bash
docker stop db2 && docker rm db2
```

---

## Notes / risks (from the spec)

- **C++20 vs C++17:** Halcyon's public headers must compile under this repo's C++20; Task 2 Step 4 is the early gate.
- **amd64-only Db2 image:** on Apple Silicon the container runs under Docker emulation and can take several minutes to become healthy; Apple `container` is not viable for this image. Task 9 depends on Docker availability.
- **macOS Gatekeeper:** if `connect` fails with `SQL1042C`/`SQLSTATE=58004`, clear quarantine once: `chmod -R u+w third_party/clidriver && xattr -r -d com.apple.quarantine third_party/clidriver`.
