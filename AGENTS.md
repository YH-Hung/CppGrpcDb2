# AGENTS.md

Guidance for coding agents working in this repository.

## Project Overview

CppGrpcDb2 is a C++20 CMake project built around gRPC/protobuf services, DB2 CLI integration, interceptors, Prometheus metrics, and focused unit tests.

Important paths:

- `CMakeLists.txt`: top-level build, generated protobuf targets, executable targets, and tests.
- `cmake/`: dependency resolution and helper functions.
- `protos/`: source `.proto` files. Generated `.pb.*` and `.grpc.pb.*` files belong in the CMake build tree, not source control.
- `src/`: application, library, interceptor, worker, utility, and DB2 implementation code.
- `include/`: public headers for shared components.
- `tests/`: GTest-based unit and integration tests.
- `doc/`: design and reference notes.

## Build Commands

Use an out-of-source build directory.

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build
```

If the DB2 CLI driver is not in `third_party/clidriver`, configure with:

```bash
cmake -S . -B build -DDB2_CLI_INSTALL_PREFIX=/path/to/db2_cli_parent
```

The DB2 CMake module expects the actual driver under `${DB2_CLI_INSTALL_PREFIX}/clidriver`.

## Test Commands

Run the default unit tests with:

```bash
ctest --test-dir build --output-on-failure
```

DB2 wrapper tests are opt-in and require a DB2 CLI runtime plus `DB2_CONN_STR`:

```bash
cmake -S . -B build -DBUILD_DB2_TESTS=ON
DB2_CONN_STR='DATABASE=db;HOSTNAME=host;PORT=50000;PROTOCOL=TCPIP;UID=user;PWD=pass' ctest --test-dir build --output-on-failure
```

Do not enable or rely on DB2 integration tests unless the environment has the DB2 driver and a valid connection string.

## Dependencies

The project expects these dependencies to be available to CMake:

- gRPC and protobuf
- DB2 CLI driver
- spdlog
- jsoncpp
- prometheus-cpp with pull support
- utf8ansi
- GTest for tests

See `Readme.md` for platform-specific installation notes.

## Coding Conventions

- Keep the project on C++20.
- Prefer existing CMake helper functions in `cmake/common.cmake` when adding similar executable or test targets.
- Keep generated protobuf output in the build directory via the existing CMake generation flow.
- Add public/shared headers under `include/` only when they are meant to be consumed across targets.
- Keep implementation-local headers near their implementation under `src/` when they are not public API.
- Avoid broad refactors when making targeted fixes.
- Use existing namespace and directory patterns for `db2`, `worker`, `resource`, `interceptor`, `metrics`, and `util` code.

## Testing Guidance

- Add or update focused GTest coverage when changing shared utilities, worker/resource behavior, SQL helpers, DB2 wrapper behavior, or interceptors.
- Prefer narrow test targets over expanding integration scope.
- For DB2 behavior, include graceful skips or keep tests behind `BUILD_DB2_TESTS` when a live database is required.

## Agent Notes

- Check `git status` before editing when possible, and do not revert unrelated user changes.
- Do not commit generated build outputs, `cmake-build-*`, or dependency installs.
- If build or test commands fail because local dependencies are missing, report the missing dependency and the command that failed.
