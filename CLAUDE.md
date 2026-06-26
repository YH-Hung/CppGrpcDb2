# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See `AGENTS.md` for the canonical agent-facing guide and `Readme.md` for platform setup, metrics endpoints, and PromQL examples. This file captures only what's not already there.

## Build & Test

Out-of-source build with deps installed under `$HOME/.local` (Homebrew or source builds):

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a single test binary directly (faster than ctest) — each GTest target is a standalone executable:

```bash
./build/worker_pool_tests --gtest_filter=WorkerPool.SubmitRunsTask
./build/test_sql_util
```

Db2 integration tests (`greeting_store_tests`) are gated by `-DBUILD_DB2_TESTS=ON`. The live cases run only when `DB2_CONN_STR` is set; otherwise they skip. A local Db2 is available via `docker run -d --name db2 --privileged -e LICENSE=accept -e DB2INST1_PASSWORD=halcyon -e DBNAME=SAMPLE -e PERSISTENT_HOME=false -p 50000:50000 --health-cmd="su - db2inst1 -c 'db2 connect to SAMPLE' || exit 1" --health-interval=20s --health-timeout=10s --health-retries=30 icr.io/db2_community/db2:11.5.9.0` (DSN: `DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;`).

**macOS one-time driver setup.** The vendored `third_party/clidriver` is consumed by the Halcyon client. Two one-time steps are needed on Apple Silicon, or any Halcyon-linked binary is SIGKILLed at load:
- Clear quarantine: `chmod -R u+w third_party/clidriver && xattr -r -d com.apple.quarantine third_party/clidriver`
- Re-sign libdb2 (CMake rewrites its install name to `@rpath`, invalidating the ad-hoc signature): `codesign --force --sign - third_party/clidriver/lib/libdb2.dylib`

CLion's `cmake-build-debug/` is also used; both build dirs coexist.

## Architecture

This is a C++20 gRPC/protobuf playground demonstrating several server styles backed by Db2 (via the Halcyon client), with cross-cutting interceptors and Prometheus metrics. The codebase is organized by concern, not by feature:

- **`protos/`** → `.proto` sources. CMake auto-globs them and generates `<name>_proto` libraries into `${build}/gen_proto/`. Adding a `.proto` requires no CMake edits, but generated `.pb.*` files must never be checked in.
- **`src/<server>.cpp`** → multiple top-level server/client binaries demonstrating different gRPC patterns:
  - `greeter_server` (sync), `greeter_callback_server` (callback API) — both link DB2 + metrics interceptor
  - `greeter_callback_server_no_db2` — same callback style, no DB2 (useful when driver is absent)
  - `complex_proto_async` — async single-completion-queue server using `src/call_data/CallData` hierarchy
- **`src/interceptor/`** → three independent `grpc::experimental::Interceptor` libraries (`string_transform_interceptor`, `metrics_interceptor`, `message_logging_interceptor`). They are mixed into servers via `add_interceptor_support()` in `cmake/common.cmake`.
- **`src/metrics/`** + **`include/{calldata_metrics,cq_worker_metrics}.h`** → Prometheus instrumentation. `metrics_interceptor` covers sync/callback servers; `calldata_metrics` + `cq_worker_metrics` cover the async CallData/CQ-worker path used by `complex_proto_async`.
- **`src/greeting/` + `include/greeting/greeting_store.hpp`** → `GreetingStore`, the only unit that talks to Db2. It wraps a pooled `halcyon::Database` (from the [Halcyon](https://github.com/) C++ Db2 client, found via `find_package(Halcyon)`) and exposes `OpenFromEnv` / `EnsureSchema` / `GreetingFor`. The `greeting_store` static lib is linked into the two servers via `add_db2_support()`. `greeter_server` and `greeter_callback_server` use it to personalize the salutation from a `greetings` table, falling back to "Hello" when `DB2_CONN_STR` is unset.
- **`src/worker/WorkerPool.h`** + **`src/resource/resource_pool.hpp`** → header-only utilities. `src/` is on the include path globally, so they're included by relative path from anywhere.
- **`src/util/`** → `sql_util`, `string_util` etc. These are deliberately built into each test target from sources (not a shared lib) so tests stay narrow.
- **`src/msvc/`** → portability shims (`strset`, `strupr`, `stricmp`, ...) with one GTest per shim under `tests/msvc/`.

### CMake conventions

- All targets pin `cxx_std_20`. Don't drop to lower.
- `cmake/common.cmake` provides `create_grpc_executable`, `add_db2_support`, `add_interceptor_support`, `create_test_executable` — prefer these over hand-rolling new targets.
- Dependency resolution is split per-package under `cmake/*.cmake` (halcyon, grpc, spdlog, jsoncpp, prometheus, utf8ansi, gtest). `cmake/halcyon.cmake` does `find_package(Halcyon)`, which transitively imports `DB2::CLI` (the vendored `third_party/clidriver`). Add new deps in the same pattern.
- Headers shared across targets go under `include/`. Implementation-only headers stay next to their `.cpp` under `src/`.

### Metrics topology

Servers expose `/metrics` on different ports — sync/callback on `127.0.0.1:8124`, async on `127.0.0.1:8125`. The async server additionally publishes single-CQ-worker metrics (`grpc_cq_worker_busy*`, `grpc_cq_worker_events_total`, `grpc_cq_worker_dispatch_duration_seconds`) — these are the canonical signal for async utilization; the instantaneous `grpc_cq_worker_busy` gauge is for debugging only. See `Readme.md` for the full metric list and PromQL examples.
