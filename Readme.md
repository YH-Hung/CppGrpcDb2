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
brew install autoconf automake libtool pkg-config
```

### Install gRPC and protobuf

Directly install through brew. Noticed that protobuf is the protoc. 

```bash
brew install grpc protobuf
```

Or like linux, build and install from source

```bash
git clone --recurse-submodules -b v1.72.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      ../..
make -j 4
make install
popd
```

### Build a project

Go to the build directory (CppGrpcDb2/build)

```bash
cmake -DCMAKE_INSTALL_PREFIX=$HOME/.local --fresh .. 
cmake --build .
```