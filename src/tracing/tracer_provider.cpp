// Copyright 2025 CppGrpcDb2 Project
// SPDX-License-Identifier: Apache-2.0

#include "tracing/tracer_provider.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/resource/resource_detector.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/semconv/service_attributes.h"
#include "opentelemetry/semconv/incubating/host_attributes.h"
#include "opentelemetry/semconv/incubating/process_attributes.h"
#include "opentelemetry/semconv/telemetry_attributes.h"
#include "opentelemetry/version.h"

// For host name and process ID
#include <unistd.h>
#include <limits.h>

// Define HOST_NAME_MAX if not available (macOS)
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#include <spdlog/spdlog.h>

namespace tracing {

// Static member initialization
std::atomic<bool> TracerProvider::initialized_{false};

// Mutex for thread-safe initialization
static std::mutex g_init_mutex;

void TracerProvider::Initialize() {
    // Fast path: already initialized
    if (initialized_.load(std::memory_order_acquire)) {
        spdlog::debug("TracerProvider already initialized, skipping");
        return;
    }

    // Slow path: acquire lock and initialize
    std::lock_guard<std::mutex> lock(g_init_mutex);

    // Double-check after acquiring lock
    if (initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    try {
        spdlog::info("Initializing OpenTelemetry TracerProvider...");

        // Read configuration from environment variables
        std::string otlp_endpoint;
        std::string service_name;
        ReadConfiguration(otlp_endpoint, service_name);

        spdlog::info("  OTLP Endpoint: {}", otlp_endpoint);
        spdlog::info("  Service Name: {}", service_name);

        // Create resource attributes
        spdlog::debug("Creating resource attributes...");
        auto resource = CreateResource(service_name);
        spdlog::debug("Resource attributes created");

        // Configure OTLP HTTP exporter
        spdlog::debug("Configuring OTLP HTTP exporter...");
        opentelemetry::exporter::otlp::OtlpHttpExporterOptions exporter_options;
        // Convert endpoint to HTTP format (OTLP HTTP uses port 4318 by default)
        // If endpoint already starts with http:// or https://, use as-is and append /v1/traces if needed
        if (otlp_endpoint.find("http://") == 0 || otlp_endpoint.find("https://") == 0) {
            // Endpoint already has protocol
            if (otlp_endpoint.find("/v1/traces") == std::string::npos) {
                exporter_options.url = otlp_endpoint + "/v1/traces";
            } else {
                exporter_options.url = otlp_endpoint;
            }
        } else if (otlp_endpoint == "localhost:4317") {
            // Convert gRPC port to HTTP port
            exporter_options.url = "http://localhost:4318/v1/traces";
        } else {
            // Add protocol prefix
            exporter_options.url = "http://" + otlp_endpoint + "/v1/traces";
        }
        exporter_options.timeout = std::chrono::seconds(10);

        spdlog::debug("Creating OTLP HTTP exporter...");
        auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(exporter_options);
        spdlog::debug("OTLP HTTP exporter created");

        if (!exporter) {
            spdlog::error("Failed to create OTLP HTTP exporter");
            throw std::runtime_error("Failed to create OTLP HTTP exporter");
        }

        // Configure BatchSpanProcessor for async export
        spdlog::debug("Configuring BatchSpanProcessor...");
        opentelemetry::sdk::trace::BatchSpanProcessorOptions processor_options;
        processor_options.max_queue_size = 2048;              // Max spans in queue
        processor_options.schedule_delay_millis = std::chrono::milliseconds(5000);  // Batch interval
        processor_options.max_export_batch_size = 512;        // Max spans per batch

        spdlog::debug("Creating BatchSpanProcessor...");
        auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
            std::move(exporter),
            processor_options
        );
        spdlog::debug("BatchSpanProcessor created");

        if (!processor) {
            spdlog::error("Failed to create BatchSpanProcessor");
            throw std::runtime_error("Failed to create BatchSpanProcessor");
        }

        // Create and configure TracerProvider
        spdlog::debug("Creating TracerProvider...");
        auto provider_unique = opentelemetry::sdk::trace::TracerProviderFactory::Create(
            std::move(processor),
            resource
        );
        spdlog::debug("TracerProvider created");

        if (!provider_unique) {
            spdlog::error("Failed to create TracerProvider");
            throw std::runtime_error("Failed to create TracerProvider");
        }

        // Convert std::unique_ptr to nostd::shared_ptr
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> provider{
            std::unique_ptr<opentelemetry::trace::TracerProvider>{std::move(provider_unique)}
        };

        // Set as global tracer provider
        opentelemetry::trace::Provider::SetTracerProvider(provider);

        // Mark as initialized
        initialized_.store(true, std::memory_order_release);

        spdlog::info("OpenTelemetry TracerProvider initialized successfully");

    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize TracerProvider: {}", e.what());
        spdlog::warn("Tracing will be disabled, but application will continue");
        // Don't throw - allow application to continue without tracing
        // This is graceful degradation as per requirements
    }
}

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> TracerProvider::GetTracer(
    const std::string& instrumentation_scope,
    const std::string& version
) {
    // Get the global tracer provider
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();

    if (!provider) {
        spdlog::warn("TracerProvider not initialized, returning no-op tracer");
        // Return a no-op tracer if not initialized (graceful degradation)
        return opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(
            instrumentation_scope, version);
    }

    return provider->GetTracer(instrumentation_scope, version);
}

bool TracerProvider::Shutdown(uint32_t timeout_millis) {
    if (!initialized_.load(std::memory_order_acquire)) {
        spdlog::debug("TracerProvider not initialized, nothing to shutdown");
        return true;
    }

    spdlog::info("Shutting down TracerProvider...");

    try {
        auto provider = opentelemetry::trace::Provider::GetTracerProvider();
        if (provider) {
            // Cast to SDK TracerProvider using .get() and dynamic_cast
            auto* sdk_provider_ptr = dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(provider.get());
            if (sdk_provider_ptr) {
                bool result = sdk_provider_ptr->Shutdown(std::chrono::milliseconds(timeout_millis));
                if (result) {
                    spdlog::info("TracerProvider shutdown successfully");
                } else {
                    spdlog::warn("TracerProvider shutdown timed out or failed");
                }
                return result;
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error during TracerProvider shutdown: {}", e.what());
        return false;
    }

    return true;
}

bool TracerProvider::ForceFlush(uint32_t timeout_millis) {
    if (!initialized_.load(std::memory_order_acquire)) {
        spdlog::debug("TracerProvider not initialized, nothing to flush");
        return true;
    }

    spdlog::debug("Force flushing TracerProvider...");

    try {
        auto provider = opentelemetry::trace::Provider::GetTracerProvider();
        if (provider) {
            // Cast to SDK TracerProvider using .get() and dynamic_cast
            auto* sdk_provider_ptr = dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(provider.get());
            if (sdk_provider_ptr) {
                bool result = sdk_provider_ptr->ForceFlush(std::chrono::milliseconds(timeout_millis));
                if (!result) {
                    spdlog::warn("TracerProvider force flush timed out or failed");
                }
                return result;
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error during TracerProvider force flush: {}", e.what());
        return false;
    }

    return true;
}

bool TracerProvider::IsInitialized() {
    return initialized_.load(std::memory_order_acquire);
}

void TracerProvider::ReadConfiguration(std::string& otlp_endpoint, std::string& service_name) {
    // Read OTEL_EXPORTER_OTLP_ENDPOINT environment variable
    const char* endpoint_env = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    if (endpoint_env && endpoint_env[0] != '\0') {
        otlp_endpoint = endpoint_env;
    } else {
        // Default to localhost:4317 for local development
        otlp_endpoint = "localhost:4317";
    }

    // Read OTEL_SERVICE_NAME environment variable
    const char* service_env = std::getenv("OTEL_SERVICE_NAME");
    if (service_env && service_env[0] != '\0') {
        service_name = service_env;
    } else {
        // Default service name
        service_name = "cpp-grpc-service";
    }
}

opentelemetry::sdk::resource::Resource TracerProvider::CreateResource(const std::string& service_name) {
    namespace resource = opentelemetry::sdk::resource;
    namespace semconv_service = opentelemetry::semconv::service;
    namespace semconv_host = opentelemetry::semconv::host;
    namespace semconv_process = opentelemetry::semconv::process;
    namespace semconv_telemetry = opentelemetry::semconv::telemetry;

    // Get host name
    char hostname[HOST_NAME_MAX + 1] = {0};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        spdlog::warn("Failed to get hostname, using 'unknown'");
        std::strncpy(hostname, "unknown", sizeof(hostname) - 1);
    }

    // Get process ID
    pid_t pid = getpid();

    // Create resource attributes
    auto attributes = resource::ResourceAttributes{
        {semconv_service::kServiceName, service_name},
        {semconv_host::kHostName, std::string(hostname)},
        {semconv_process::kProcessPid, static_cast<int32_t>(pid)},
        {semconv_telemetry::kTelemetrySdkName, "opentelemetry-cpp"},
        {semconv_telemetry::kTelemetrySdkLanguage, "cpp"},
        {semconv_telemetry::kTelemetrySdkVersion, OPENTELEMETRY_VERSION}
    };

    return resource::Resource::Create(attributes);
}

}  // namespace tracing
