# OpenTelemetry C++ Build Notes

This project resolves OpenTelemetry C++ through the CMake config package:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local
```

The project expects these imported targets to be available:

- `opentelemetry-cpp::api`
- `opentelemetry-cpp::sdk`
- `opentelemetry-cpp::otlp_grpc_exporter`

## Homebrew

On macOS, Homebrew packages provide these targets when
`opentelemetry-cpp` is installed with OTLP gRPC exporter support. The current
repository configuration was verified against Homebrew `opentelemetry-cpp`
1.27.0 and gRPC 1.81.1.

Homebrew OpenSSL upgrades can leave stale CMake cache paths behind. If configure
fails while looking for `OpenSSL::SSL` or points at an older removed OpenSSL
Cellar version, clear the cached OpenSSL entries during configure:

```bash
cmake -S . -B build '-UOPENSSL_*' -DCMAKE_PREFIX_PATH=$HOME/.local
```

Homebrew dependency upgrades can also leave an installed `re2` dylib pointing
through `/opt/homebrew/opt/abseil` to an older Abseil ABI than the currently
linked Abseil package. In that state, gRPC-linked test binaries can abort before
test code runs with a missing `libabsl_*.2508.0.0.dylib`. The durable fix is to
reinstall or rebuild `re2` against the current Abseil package. For an existing
local installation where the older Abseil Cellar is still present, patching the
`re2` install names to the exact old Cellar paths and ad-hoc re-signing the
patched dylib also restores test execution.

## From Source

When package manager binaries are unavailable, build and install
`opentelemetry-cpp` with the OTLP gRPC exporter enabled, then point
`CMAKE_PREFIX_PATH` at the install prefix:

```bash
git clone --recurse-submodules https://github.com/open-telemetry/opentelemetry-cpp.git
cmake -S opentelemetry-cpp -B opentelemetry-cpp/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$HOME/.local \
  -DBUILD_TESTING=OFF \
  -DWITH_OTLP_GRPC=ON
cmake --build opentelemetry-cpp/build
cmake --install opentelemetry-cpp/build
```

The OTLP gRPC exporter uses protobuf, gRPC, and `nlohmann_json`, so keep those
packages visible to CMake while building OpenTelemetry C++.
