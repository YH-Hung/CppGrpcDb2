// Simple test to check if TracerProvider initialization works
#include "tracing/tracer_provider.h"
#include <spdlog/spdlog.h>
#include <iostream>

int main() {
    spdlog::set_level(spdlog::level::debug);

    std::cout << "Starting TracerProvider initialization test..." << std::endl;

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
