// Copyright 2025 CppGrpcDb2 Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/tracer.h"

namespace tracing {

/**
 * @brief TracerProvider singleton for managing OpenTelemetry tracing infrastructure
 *
 * This class provides a thread-safe singleton instance that initializes and manages
 * the OpenTelemetry tracing pipeline, including:
 * - OTLP gRPC exporter for sending traces to a collector
 * - BatchSpanProcessor for efficient async span export
 * - Resource attributes (service.name, host.name, process.pid)
 * - Environment variable configuration support
 *
 * Environment Variables:
 * - OTEL_EXPORTER_OTLP_ENDPOINT: OTLP collector endpoint (default: http://localhost:4317)
 * - OTEL_SERVICE_NAME: Service name for resource attributes (default: cpp-grpc-service)
 *
 * Thread Safety: All public methods are thread-safe
 *
 * Example Usage:
 * @code
 *   // Initialize the tracer provider (call once at application startup)
 *   tracing::TracerProvider::Initialize();
 *
 *   // Get a tracer for creating spans
 *   auto tracer = tracing::TracerProvider::GetTracer("my-component");
 *
 *   // Create a span
 *   auto span = tracer->StartSpan("operation-name");
 *   // ... do work ...
 *   span->End();
 *
 *   // Shutdown at application exit (flushes pending spans)
 *   tracing::TracerProvider::Shutdown();
 * @endcode
 */
class TracerProvider {
public:
    /**
     * @brief Initialize the global OpenTelemetry tracer provider
     *
     * This method must be called once at application startup before any spans are created.
     * It is thread-safe and idempotent (subsequent calls have no effect).
     *
     * Configuration is read from environment variables:
     * - OTEL_EXPORTER_OTLP_ENDPOINT: OTLP collector endpoint
     * - OTEL_SERVICE_NAME: Service name for traces
     *
     * @throws std::runtime_error if initialization fails critically
     */
    static void Initialize();

    /**
     * @brief Get a tracer instance for creating spans
     *
     * @param instrumentation_scope Name identifying the instrumentation scope
     *                              (e.g., "grpc-server", "grpc-client", "database")
     * @param version Optional version string for the instrumentation scope
     * @return Shared pointer to a Tracer instance
     *
     * @note This method is thread-safe and can be called from any thread
     */
    static opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer(
        const std::string& instrumentation_scope,
        const std::string& version = "1.0.0"
    );

    /**
     * @brief Shutdown the tracer provider and flush all pending spans
     *
     * This method should be called at application shutdown to ensure all spans
     * are exported before the process exits. It blocks until all pending spans
     * are flushed or a timeout occurs.
     *
     * @param timeout_millis Maximum time to wait for flush (default: 5000ms)
     * @return true if shutdown completed successfully, false on timeout/error
     *
     * @note After shutdown, new spans will not be exported
     */
    static bool Shutdown(uint32_t timeout_millis = 5000);

    /**
     * @brief Force flush all pending spans immediately
     *
     * Triggers immediate export of all batched spans without waiting for
     * the batch timeout. Useful for testing or before critical operations.
     *
     * @param timeout_millis Maximum time to wait for flush (default: 5000ms)
     * @return true if flush completed successfully, false on timeout/error
     */
    static bool ForceFlush(uint32_t timeout_millis = 5000);

    /**
     * @brief Check if the tracer provider has been initialized
     *
     * @return true if Initialize() has been called successfully, false otherwise
     */
    static bool IsInitialized();

    // Delete copy/move constructors and assignment operators (singleton pattern)
    TracerProvider(const TracerProvider&) = delete;
    TracerProvider& operator=(const TracerProvider&) = delete;
    TracerProvider(TracerProvider&&) = delete;
    TracerProvider& operator=(TracerProvider&&) = delete;

private:
    TracerProvider() = default;
    ~TracerProvider() = default;

    /**
     * @brief Read configuration from environment variables
     *
     * @param otlp_endpoint Output parameter for OTLP endpoint
     * @param service_name Output parameter for service name
     */
    static void ReadConfiguration(std::string& otlp_endpoint, std::string& service_name);

    /**
     * @brief Create resource attributes for the tracer provider
     *
     * Adds standard attributes like service.name, host.name, process.pid
     *
     * @param service_name Service name to include in resource attributes
     * @return Resource attributes
     */
    static opentelemetry::sdk::resource::Resource CreateResource(const std::string& service_name);

    // Initialization state flag (protected by mutex in implementation)
    static std::atomic<bool> initialized_;
};

}  // namespace tracing
