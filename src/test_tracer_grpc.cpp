// Test TracerProvider initialization with gRPC headers included
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include "tracing/tracer_provider.h"
#include <spdlog/spdlog.h>
#include <iostream>

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);
    spdlog::set_level(spdlog::level::debug);

    std::cout << "Starting TracerProvider initialization test with gRPC..." << std::endl;

    tracing::TracerProvider::Initialize();

    if (tracing::TracerProvider::IsInitialized()) {
        std::cout << "TracerProvider initialized successfully!" << std::endl;
    } else {
        std::cout << "TracerProvider initialization failed!" << std::endl;
    }

    std::cout << "Shutting down..." << std::endl;
    tracing::TracerProvider::Shutdown();

    std::cout << "Test complete!" << std::endl;
    return 0;
}
