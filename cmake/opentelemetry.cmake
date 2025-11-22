# OpenTelemetry C++ SDK Configuration
# This module finds and configures the OpenTelemetry C++ SDK for tracing

message(STATUS "Configuring OpenTelemetry C++ SDK...")

# Find OpenTelemetry package
# Using OTLP HTTP exporter instead of gRPC to avoid conflicts with application's gRPC usage
find_package(opentelemetry-cpp CONFIG REQUIRED
    COMPONENTS
        api
        sdk
        exporters_otlp_http
)

if(opentelemetry-cpp_FOUND)
    message(STATUS "Found OpenTelemetry C++ SDK")
    message(STATUS "  Version: ${opentelemetry-cpp_VERSION}")
else()
    message(FATAL_ERROR "OpenTelemetry C++ SDK not found. Please install it first.")
endif()

# Define library targets for linking
set(OPENTELEMETRY_LIBS
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::otlp_http_exporter
    CACHE INTERNAL "OpenTelemetry libraries"
)

message(STATUS "OpenTelemetry libraries: ${OPENTELEMETRY_LIBS}")
