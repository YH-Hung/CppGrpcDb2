# Halcyon Db2 Adoption — Design Spec

**Date:** 2026-06-26
**Status:** Approved (design); pending implementation
**Topic:** Replace the home-grown `db2_wrapper` with the Halcyon Db2 client and run a real query in the Db2-linked servers.

## Goal

Make `CppGrpcDb2` adopt [Halcyon](file:///Users/yinghanhung/Projects/Halcyon) — a modern C++17 IBM Db2 client built on the Db2 CLI — as its database layer, replacing the existing hand-rolled `db2::Connection` / `db2_wrapper`. Both Db2-linked gRPC servers (`greeter_server`, `greeter_callback_server`) perform a **real, parameterized query against a live Db2** that drives actual service behavior, gated on a connection string so builds and runs without a database degrade gracefully.

## Context

- **Halcyon is installed** at `$HOME/.local`: public headers under `include/halcyon/`, static lib `libhalcyon.a`, and a CMake package `Halcyon` exporting target `halcyon::halcyon`. This repo already configures with `-DCMAKE_PREFIX_PATH=$HOME/.local`, so `find_package(Halcyon REQUIRED)` resolves cleanly.
- Halcyon's installed config calls `find_dependency(DB2CLI)` using its bundled `FindDB2CLI.cmake`, whose default `DB2_CLIDRIVER_ROOT` is `${CMAKE_SOURCE_DIR}/third_party/clidriver` — **exactly** where this repo vendors the Db2 CLI driver. The CLI driver therefore auto-resolves, including the macOS `@rpath` install-name rewrite for `libdb2.dylib`.
- The installed Halcyon was built **without** the Prometheus/OTel adapters, so adopting it pulls in no extra dependencies.
- Today, the only binary that actually uses Db2 is `greeter_callback_server.cpp`, and only as a no-op: it constructs `db2::Connection` objects via a `resource::ResourcePool` for demonstration but never connects or queries. `greeter_server` links `db2_wrapper` but does not use it. `greeter_callback_server_no_db2` deliberately excludes Db2.
- The `icr.io/db2_community/db2` image (used by Halcyon's compose) creates an **empty** `SAMPLE` database — the classic SAMPLE schema (EMPLOYEE, etc.) is not populated. Halcyon's own integration tests create and seed their own tables. The only always-present query is `SELECT 1 FROM SYSIBM.SYSDUMMY1`.

## Design

### 1. The meaningful query — `GreetingStore`

A new, well-bounded unit encapsulates all database access behind a small interface; nothing else in the codebase touches Halcyon directly.

- **Files:** `include/greeting/greeting_store.hpp`, `src/greeting/greeting_store.cpp`
- **CMake target:** `greeting_store` (static lib), `PUBLIC` links `halcyon::halcyon` and the spdlog target.
- **API:**
  - `static std::optional<GreetingStore> OpenFromEnv();`
    Reads the `DB2_CONN_STR` environment variable (the repo's existing convention). If set, opens a pooled `halcyon::Database`; on success returns a populated `GreetingStore`. If unset, or if open fails, logs and returns `std::nullopt`.
  - `void EnsureSchema();`
    Idempotent `DROP TABLE` (ignore-error) + `CREATE TABLE greetings(name VARCHAR(64) NOT NULL PRIMARY KEY, salutation VARCHAR(64))` + seed rows, e.g. `('alice','Bonjour'), ('bob','Hola')`. This mirrors how Halcyon's own integration tests provision schema, since the container's `SAMPLE` database is empty.
  - `std::string GreetingFor(std::string_view name);`
    Runs `SELECT salutation FROM greetings WHERE UPPER(name) = UPPER(?)` (parameterized; case-insensitive so the callback server's request-uppercasing interceptor still matches a seeded row). Returns the stored salutation when a row matches, otherwise the default `"Hello"`. On query error, logs and returns `"Hello"` (never throws into request handling).

The class wraps a `halcyon::Database` (which owns its own connection pool, reconnect, and retry). It is constructed only via `OpenFromEnv` so call sites never see a half-open store.

### 2. Both servers

- **`greeter_server`** (`src/greeter_server.cpp`, sync): the service impl holds an `std::optional<GreetingStore>` opened once at startup (`OpenFromEnv` → `EnsureSchema`). In `SayHello`, reply with `"{store.GreetingFor(name)} {name}"`. With no DB configured the store is empty and the reply falls back to `"Hello {name}"` (current behavior preserved).
- **`greeter_callback_server`** (`src/greeter_callback_server.cpp`, callback): **remove** the `resource::ResourcePool<db2::Connection>` demonstration and the `db2/db2.hpp` / `resource/resource_pool.hpp` includes. The service impl holds an `std::optional<GreetingStore>`; `SayHello` performs the same lookup. Interceptors (string transform, metrics), tracing, and health check are untouched.

The async server (`complex_proto_async`) and the clients are out of scope — they do not link Db2.

### 3. CMake

- Add `cmake/halcyon.cmake` containing `find_package(Halcyon REQUIRED)` (following the repo's per-package convention under `cmake/*.cmake`). This transitively imports `DB2::CLI` and resolves the vendored driver.
- In `CMakeLists.txt`, replace `include(cmake/db2.cmake)` with `include(cmake/halcyon.cmake)`. Delete `cmake/db2.cmake` (its `DB2::db2` imported target is no longer used).
- Delete the `db2_wrapper` library target, `src/db2/db2.cpp`, and `include/db2/db2.hpp`.
- Add the `greeting_store` library target.
- Repurpose `add_db2_support(target)` in `cmake/common.cmake` to link `greeting_store` (which carries Halcyon transitively). Apply it to **only** `greeter_server` and `greeter_callback_server` — not to the client binaries, which no longer need a database.
- `sql_util` (`src/util/sql_util.{h,cpp}`) is kept as-is; it is only consumed by `test_sql_util`, which compiles it directly. (It was incidentally bundled into `db2_wrapper`, but `db2.cpp` does not use it.)

### 4. Docker / live Db2

- Run a live Db2 container directly via docker: `image: icr.io/db2_community/db2:11.5.9.0`, `privileged: true`, `LICENSE=accept`, `DB2INST1_PASSWORD=halcyon`, `DBNAME=SAMPLE`, `PERSISTENT_HOME=false`, port `50000:50000`, and the `db2 connect to SAMPLE` healthcheck. (Password matches Halcyon's compose so an already-running Halcyon Db2 container is reusable.)
- Document the DSN and usage:
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

  docker ps   # wait for healthy (~2 min+)
  export DB2_CONN_STR="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
  ```

### 5. Tests

- Remove `db2_wrapper_tests` (it tested the deleted wrapper).
- Keep `resource_handle_refactor_tests` — it uses a dummy `Connection` struct and does not depend on Db2 (it was only incidentally gated under `BUILD_DB2_TESTS`).
- Add `greeting_store_tests`, gated under the existing `BUILD_DB2_TESTS` option: when `DB2_CONN_STR` is set, exercise `OpenFromEnv` → `EnsureSchema` → `GreetingFor` (asserting a seeded name returns its salutation and an unseeded name returns `"Hello"`); otherwise `GTEST_SKIP`.

### 6. Docs

Update `Readme.md`, `AGENTS.md`, and `CLAUDE.md`:
- Replace `db2_wrapper` / `db2::Connection` references in architecture sections with Halcyon + `GreetingStore`.
- Document the `DB2_CONN_STR` env var, the docker workflow, and the personalized-greeting query behavior.

## Verification

1. **No-DB build:** configure + build the full tree; both servers build and link Halcyon; run a server with `DB2_CONN_STR` unset and confirm `SayHello{name:"alice"}` → `"Hello alice"` and a log line noting no DB configured.
2. **Live DB:** run Db2 container, wait for healthy, `export DB2_CONN_STR`, run a server, and via `grpcurl` confirm `SayHello{name:"alice"}` → `"Bonjour alice"`; run `greeting_store_tests` (built with `-DBUILD_DB2_TESTS=ON`) and confirm it passes (not skipped).

## Risks & notes

- **C++ standard mismatch:** this repo is C++20; Halcyon is C++17. Halcyon's public headers must compile cleanly under C++20 — verify early in implementation (a trivial TU that includes `halcyon/halcyon.hpp` compiled at `cxx_std_20`).
- **Db2 image is amd64-only:** on Apple Silicon it runs under Docker's amd64 emulation and can take several minutes to become healthy; Apple's `container` runtime fails for this image per Halcyon's notes. Live verification therefore depends on Docker being available and the container reaching `healthy`.
- **macOS Gatekeeper:** the vendored driver's GSKit libraries may carry `com.apple.quarantine`; a one-time `xattr -r -d com.apple.quarantine third_party/clidriver` (after `chmod -R u+w`) may be required for `connect` to succeed.

## Out of scope

- Migrating the async server or clients to Halcyon (they do not use Db2).
- Adding Halcyon's optional Prometheus/OTel adapters (the installed Halcyon was built without them; the repo already has its own metrics/tracing).
- Deleting the generic `resource_pool.hpp` utility (it becomes unused by servers but remains a valid standalone utility).
