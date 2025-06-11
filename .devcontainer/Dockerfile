FROM debian:12-slim

USER root
# Install required packages
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        clang \
        cmake \
        git unzip \
        build-essential autoconf libtool pkg-config \
        && rm -rf /var/lib/apt/lists/*

# Install gRPC globally
#ADD third_party/grpc /tmp/grpc
ADD grpc /tmp/grpc
RUN mkdir -p /tmp/grpc/cmake/build

RUN cd /tmp/grpc/cmake/build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF \
     -DCMAKE_CXX_STANDARD=17 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 --fresh ../.. && \
    make -j $(nproc) && make install && \
    cd / && rm -rf /tmp/grpc /tmp/grpc.zip

# Set working directory (matches workspaceFolder)
RUN mkdir -p /workspace
WORKDIR /workspace

# Default command for devcontainer
CMD [ "sleep", "infinity" ]