# Local Environment Setup

## macOS

### Install CMake

```bash
export MY_INSTALL_DIR=$HOME/.local
export PATH="$MY_INSTALL_DIR/bin:$PATH"

brew install cmake
cmake --version
```

### Install required tools

```bash
brew install autoconf automake libtool pkg-config spdlog cpr
```

### Install gRPC and protobuf

Directly install through brew. Noticed that protobuf is the protoc. 

```bash
brew install grpc protobuf
```

Or like linux, build and install from source.

### DB2 Driver

- Download DB2 CLI driver from https://public.dhe.ibm.com/ibmdl/export/pub/software/data/db2/drivers/odbc_cli/
- Extract and put clidriver folder under third_party/
- Env: DYLD_LIBRARY_PATH=$HOME/clidriver/lib:$DYLD_LIBRARY_PATH

### OpenTelemetry Client

Directly install through brew.

```bash
brew install opentelemetry-cpp
```

Or like linux, build and install from source

### Prometheus C++ (prometheus-cpp)

This project uses prometheus-cpp and expects its CMake config package to be available (find_package(prometheus-cpp CONFIG REQUIRED) providing targets like prometheus-cpp::pull).

- Install via Homebrew (recommended on macOS):

```bash
brew install prometheus-cpp
```

- Build from source (if you prefer a custom prefix or don’t use Homebrew):

```bash
export MY_INSTALL_DIR=$HOME/.local

git clone --depth 1 -b v1.2.4 https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
mkdir -p build && cd build
cmake -DENABLE_PULL=ON \
      -DENABLE_PUSH=OFF \
      -DENABLE_COMPRESSION=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      .. --fresh
cmake --build . --parallel 4
cmake --install .
```

Note: If you install to a non-system prefix (like $HOME/.local) or use a non-default Homebrew prefix, make sure your project configure step includes:

```bash
-DCMAKE_PREFIX_PATH=$HOME/.local
```

The project already demonstrates this in the Build a project section.

## Linux

### Install CMake

You can install cmake from your package manager. However, it's usually outdated comparing to mac os.

```bash
sudo apt install cmake
```

Or download the binary from https://cmake.org/download/

### Install required tools

```bash
sudo apt install libcurl4-openssl-dev build-essential autoconf libtool pkg-config
```

### Install gRPC and protobuf

You MUST check out a specific version rather than staying on the main branch.

```bash
export MY_INSTALL_DIR=$HOME/.local
#export PATH="$MY_INSTALL_DIR/bin:$PATH"

git clone --recurse-submodules -b v1.72.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      ../.. --fresh
make -j 4
make install
popd
```

If you want to enable gRPC OTEL plugin,
rebuild gRPC with ```-DgRPC_BUILD_GRPCPP_OTEL_PLUGIN=ON``` after installed ```opeentelemetry-cpp```.

### DB2 Driver

Same as macOS, but chose the right os and arch.

### OpenTelemetry Client

. Following dependencies are required.

- Abseil
- Protobuf
- gRPC

You MUST check out a specific version rather than staying on the main branch.

```bash
export MY_INSTALL_DIR=$HOME/.local

git clone --recurse-submodules https://github.com/open-telemetry/opentelemetry-cpp
cd opentelemetry-cpp
mkdir -p build
pushd build
cmake -DBUILD_TESTING=OFF \
      -DBUILD_SHARED_LIBS=ON \
      -DWITH_OTLP_GRPC=ON \
      -DWITH_OTLP_HTTP=ON \
      -DWITH_PROMETHEUS=ON \
      -DOPENTELEMETRY_INSTALL=ON \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      .. --fresh
cmake --build . --parallel 4 --target all
cmake --install .
```

### Prometheus C++ (prometheus-cpp)

This project uses prometheus-cpp and expects its CMake config package and targets (prometheus-cpp::core and prometheus-cpp::pull).

- Install via package manager (if available):

```bash
sudo apt-get update
sudo apt-get install -y libprometheus-cpp-dev
```

- Build from source (works on any distro):

```bash
export MY_INSTALL_DIR=$HOME/.local

git clone --depth 1 -b v1.2.4 https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
mkdir -p build && cd build
cmake -DENABLE_PULL=ON \
      -DENABLE_PUSH=OFF \
      -DENABLE_COMPRESSION=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      .. --fresh
cmake --build . --parallel 4
cmake --install .
```

Notes:
- ENABLE_PULL=ON is required because the code links to prometheus-cpp::pull (for the HTTP Exposer).
- If installed to a custom prefix like $HOME/.local, configure this project with:

```bash
-DCMAKE_PREFIX_PATH=$HOME/.local
```

### cpr

Unlike mac os, there is no way to install cpr through package manager.
The only possible solution is to build it from source.

```bash
git clone https://github.com/libcpr/cpr.git
cd cpr && mkdir build && cd build
cmake .. -DCPR_USE_SYSTEM_CURL=ON -DBUILD_SHARED_LIBS=ON
cmake --build . --parallel
sudo cmake --install .
```

### Build a project

Go to the build directory (CppGrpcDb2/build)

```bash
cmake -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local --fresh .. 
cmake --build .
```

## Dev container

Build image first to avoid redundant docker build.
Copy the Dockerfile next to grpc repository to build the image if required. 

```bash
docker build -t dev-container-cpp-db2:alpha1 .
#docker build -f .devcontainer/Dockerfile -t dev-container-cpp-db2:alpha1 . > build.log 2>&1
```

## Run Tests

Tests are only able to run after build completed.

```bash
make test
```

Instead of build and run in terminal, clion test run will make your life much easier.
## Prometheus metrics

This project exposes basic Prometheus metrics for the gRPC servers via a server interceptor and a Prometheus HTTP exposer.

- Which binaries expose metrics:
  - greeter_server
  - greeter_callback_server
  - Note: greeter_callback_server_no_db2 does not expose metrics.

- Default metrics endpoint URLs (served by the Prometheus exposer):
  - http://127.0.0.1:8124/metrics
  - http://localhost:8124/metrics
  - The root path (e.g., http://localhost:8124) returns 404 by design; use /metrics.

- Binding address:
  - By default, the exposer binds to 127.0.0.1:8124, which is only reachable from the local machine.
  - To allow scraping from another host, change the exposer bind address to 0.0.0.0:8124 in:
    - src/greeter_server.cpp
    - src/greeter_callback_server.cpp

- Metrics exported by the interceptor (no labels):
  - grpc_requests_total (counter): Total number of gRPC requests observed by the server.
  - grpc_request_duration_seconds (histogram): Request duration in seconds. Default buckets: 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0.

Quick start

1) Build and run a server (example: greeter_server) and then query the metrics endpoint:

```bash
# Build and run (paths depend on your build setup)
cmake --build cmake-build-debug --target greeter_server && \
  ./cmake-build-debug/greeter_server &

# Fetch metrics
curl -s http://127.0.0.1:8124/metrics | head
```

2) Example output (abbreviated):

```
# HELP grpc_requests_total Total number of gRPC requests
# TYPE grpc_requests_total counter
grpc_requests_total 3
# HELP grpc_request_duration_seconds gRPC request duration in seconds
# TYPE grpc_request_duration_seconds histogram
...
```

Prometheus scrape configuration

Add a scrape job to your Prometheus configuration (prometheus.yml):

```yaml
scrape_configs:
  - job_name: 'cpp-grpc-db2'
    static_configs:
      - targets: ['127.0.0.1:8124']
```

Troubleshooting

- If you get 404 on the root path, use /metrics.
- Ensure one of the servers that creates the exposer is running (greeter_server or greeter_callback_server).
- If scraping from another machine, update the exposer bind address to 0.0.0.0:8124.

## Health Check

- The health.proto must manually add to the project and run gRPC code generation against it.
- Must add reference to the health service or it will be removed by link-time GC.

```bash
grpcurl -plaintext localhost:50051 describe
grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check
```