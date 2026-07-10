# AGENTS.md

Canonical guidance for coding agents working in this repository.

## Project Overview

CppGrpcDb2 is a C++20 CMake project built around gRPC/protobuf services, Db2 access via the Halcyon C++ client, interceptors, async CallData handlers, Prometheus metrics, and focused GTest coverage.

Important paths:

- `CMakeLists.txt`: top-level build, generated protobuf targets, executable targets, and tests.
- `cmake/`: dependency resolution and helper functions.
- `protos/`: source `.proto` files. Generated `.pb.*` and `.grpc.pb.*` files belong in the CMake build tree, not source control.
- `src/`: application, library, interceptor, metrics, worker, utility, and Db2 access (`src/greeting/`, the Halcyon-backed `GreetingStore`) code.
- `src/call_data/`: async server CallData hierarchy used by `complex_proto_async`.
- `src/metrics/`: Prometheus helper implementations such as CallData and CQ worker metrics.
- `include/`: public headers for shared components.
- `tests/`: GTest-based unit and integration tests.
- `doc/`: design and reference notes.
- `CLAUDE.md`: supplemental Claude Code notes. Keep `AGENTS.md` as the canonical agent guide.

## Build Commands

Use an out-of-source build directory.

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build
```

Build a focused target when iterating:

```bash
cmake --build build --target complex_proto_async cq_worker_metrics_tests
```

Db2 access goes through the Halcyon client, resolved by `cmake/halcyon.cmake` via
`find_package(Halcyon REQUIRED)` (install Halcyon under `$HOME/.local`). Halcyon
transitively imports `DB2::CLI`; its bundled `FindDB2CLI.cmake` defaults
`DB2_CLIDRIVER_ROOT` to `third_party/clidriver`, so the vendored driver resolves
with no extra flags. To point at a driver elsewhere, configure with
`-DDB2_CLIDRIVER_ROOT=/path/to/clidriver`.

**macOS one-time driver setup (Apple Silicon).** Halcyon-linked binaries load the
vendored `libdb2.dylib`. Two one-time steps are required or the binary is SIGKILLed
at load with no output:

```bash
# 1. Clear the download quarantine on the vendored driver.
chmod -R u+w third_party/clidriver
xattr -r -d com.apple.quarantine third_party/clidriver
# 2. Re-sign libdb2: CMake rewrites its install name to @rpath, which invalidates
#    the ad-hoc signature, and arm64 macOS kills processes loading invalid sigs.
codesign --force --sign - third_party/clidriver/lib/libdb2.dylib
```

## Test Commands

Run the default unit tests with:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Run focused tests with:

```bash
ctest --test-dir build --output-on-failure -R cq_worker_metrics_tests
```

Db2 integration tests (`greeting_store_tests`) are opt-in via `-DBUILD_DB2_TESTS=ON`.
Their live cases run only when `DB2_CONN_STR` is set, and skip otherwise. Bring up a
local Db2 with docker:

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

cmake -S . -B build -DBUILD_DB2_TESTS=ON
export DB2_CONN_STR='DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;'
ctest --test-dir build --output-on-failure -R greeting_store_tests

docker stop db2 && docker rm db2
```

The `greeter_server` and `greeter_callback_server` binaries also read `DB2_CONN_STR`
at startup: when set they personalize the greeting from a `greetings` table
(`alice`→`Bonjour`, `bob`→`Hola`, `yuki`→`Konnichiwa`), otherwise they fall back to
"Hello". The Db2 community image is amd64-only, so on Apple Silicon it runs under
Docker's emulation and can take several minutes to become healthy.

## Dependencies

The project expects these dependencies to be available to CMake:

- gRPC and protobuf
- Halcyon C++ Db2 client (installed under `$HOME/.local`; brings the DB2 CLI driver transitively)
- spdlog
- jsoncpp
- prometheus-cpp with pull support
- utf8ansi
- GTest for tests

See `Readme.md` for platform-specific installation notes, metrics endpoints, and PromQL examples.

## Coding Conventions

- Keep the project on C++20.
- Prefer existing CMake helper functions in `cmake/common.cmake` when adding similar executable or test targets.
- Keep generated protobuf output in the build directory via the existing CMake generation flow.
- Add public/shared headers under `include/` only when they are meant to be consumed across targets.
- Keep implementation-local headers near their implementation under `src/` when they are not public API.
- Keep Prometheus helpers in `include/*_metrics.h` plus `src/metrics/*.cpp` when the instrumentation is shared across targets.
- Keep the existing `calldata_metrics` CMake library name unless a broader metrics-target rename is intentionally part of the change.
- Avoid broad refactors when making targeted fixes.
- Use existing namespace and directory patterns for `db2`, `worker`, `resource`, `interceptor`, `metrics`, and `util` code.

## Metrics Notes

- `greeter_server` and `greeter_callback_server` expose Prometheus metrics on `127.0.0.1:8124/metrics`.
- `complex_proto_async` exposes Prometheus metrics on `127.0.0.1:8125/metrics`.
- `metrics_interceptor` covers sync/callback server request metrics.
- `CallDataMetrics` and `CqWorkerMetrics` cover the async CallData path in `complex_proto_async`.
- Treat `grpc_cq_worker_busy` as an instantaneous diagnostic gauge only. Use `grpc_cq_worker_busy_seconds_total` with `rate()` for scrape-stable CQ worker utilization trends.
- Keep CQ worker metric labels bounded. The current worker metrics use only `ok="true|false"`.

## Testing Guidance

- Add or update focused GTest coverage when changing shared utilities, worker/resource behavior, SQL helpers, DB2 wrapper behavior, interceptors, or metrics helpers.
- Prefer narrow test targets over expanding integration scope.
- Metrics helper tests belong under `tests/metrics/` and should inspect `prometheus::Registry::Collect()` when practical.
- For DB2 behavior, include graceful skips or keep tests behind `BUILD_DB2_TESTS` when a live database is required.

## Agent Notes

- Check `git status` before editing when possible, and do not revert unrelated user changes.
- Do not commit generated build outputs, `cmake-build-*`, `.cache/`, `.air/`, or dependency installs.
- `.air/` is local planning/tooling state and is ignored.
- If build or test commands fail because local dependencies are missing, report the missing dependency and the command that failed.
