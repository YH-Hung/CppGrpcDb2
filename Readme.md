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

### OpenTelemetry Client

Directly install through brew.

```bash
brew install opentelemetry-cpp
```

Or like linux, build and install from source

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
cmake -DCMAKE_INSTALL_PREFIX=$HOME/.local --fresh .. 
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