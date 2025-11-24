/**
 * Integration Tests: OTLP Export and Collector Connectivity
 *
 * These tests verify:
 * - T061: OTLP collector connectivity check
 * - T062: Graceful degradation when collector unavailable
 *
 * Prerequisites:
 * - For T061: OTLP collector running at configured endpoint
 * - For T062: No collector running (intentionally)
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cstdlib>

#include "tracing/tracer_provider.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/trace/span_context.h"

namespace trace_api = opentelemetry::trace;

// Test fixture for OTLP export tests
class OTLPExportTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean environment before each test
        unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
        unsetenv("OTEL_SERVICE_NAME");
    }

    void TearDown() override {
        // Shutdown tracer provider after each test
        tracing::TracerProvider::Shutdown();
    }

    // Helper: Create a test span
    void CreateTestSpan(const std::string& span_name) {
        auto tracer = tracing::TracerProvider::GetTracer("test_tracer", "1.0.0");

        // Create a span
        auto span = tracer->StartSpan(span_name);

        // Add some attributes
        span->SetAttribute("test.attribute", "test_value");
        span->SetAttribute("test.number", 42);

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // End the span
        span->End();
    }
};

/**
 * T061: Test OTLP collector connectivity
 *
 * Acceptance Scenario:
 * Given an OTLP collector is running at the configured endpoint
 * When TracerProvider is initialized
 * Then the tracer provider should successfully connect and export spans
 */
TEST_F(OTLPExportTest, CollectorConnectivityCheck) {
    // Set environment to use local OTLP collector (HTTP endpoint)
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://localhost:4318", 1);
    setenv("OTEL_SERVICE_NAME", "test_otlp_connectivity", 1);

    // Initialize tracer provider
    tracing::TracerProvider::Initialize();

    // Verify initialization succeeded
    EXPECT_TRUE(tracing::TracerProvider::IsInitialized())
        << "TracerProvider should be initialized when collector is available";

    // Create a test span to verify export works
    CreateTestSpan("test_connectivity_span");

    // Force flush to ensure span is exported immediately
    bool flush_result = tracing::TracerProvider::ForceFlush(5000);
    EXPECT_TRUE(flush_result)
        << "ForceFlush should succeed when collector is available";

    // Note: We can't verify the span was actually received by the collector
    // in this test without querying the collector's API. The test passes
    // if no exceptions are thrown and ForceFlush returns true.

    // Create another span to verify continued operation
    CreateTestSpan("test_connectivity_span_2");

    // Shutdown should also succeed
    bool shutdown_result = tracing::TracerProvider::Shutdown(5000);
    EXPECT_TRUE(shutdown_result)
        << "Shutdown should succeed when collector is available";
}

/**
 * T062: Test graceful degradation when collector unavailable
 *
 * Acceptance Scenario:
 * Given the OTLP collector is NOT running
 * When TracerProvider is initialized and spans are created
 * Then the application should continue normally without crashes
 * And span creation should still work (buffered)
 * And no exceptions should be thrown
 */
TEST_F(OTLPExportTest, GracefulDegradationCollectorUnavailable) {
    // Set environment to use non-existent collector endpoint
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://localhost:19999", 1);
    setenv("OTEL_SERVICE_NAME", "test_graceful_degradation", 1);

    // Initialize tracer provider (should not throw even if collector unavailable)
    EXPECT_NO_THROW({
        tracing::TracerProvider::Initialize();
    }) << "Initialize should not throw when collector is unavailable";

    // TracerProvider should still be marked as initialized
    // (graceful degradation means we continue with a no-op or buffering tracer)
    EXPECT_TRUE(tracing::TracerProvider::IsInitialized())
        << "TracerProvider should still initialize even when collector is unavailable";

    // Create test spans (should not throw)
    EXPECT_NO_THROW({
        CreateTestSpan("test_span_1");
        CreateTestSpan("test_span_2");
        CreateTestSpan("test_span_3");
    }) << "Span creation should not throw when collector is unavailable";

    // Force flush may time out or fail, but should not crash
    EXPECT_NO_THROW({
        bool flush_result = tracing::TracerProvider::ForceFlush(2000);
        // Flush might fail (return false) when collector is unavailable
        // but the application should continue
        if (!flush_result) {
            std::cout << "ForceFlush failed as expected when collector unavailable\n";
        }
    }) << "ForceFlush should not throw when collector is unavailable";

    // Shutdown should not crash
    EXPECT_NO_THROW({
        tracing::TracerProvider::Shutdown(2000);
    }) << "Shutdown should not throw when collector is unavailable";
}

/**
 * Test collector reconnection after initial failure
 *
 * Acceptance Scenario:
 * Given the collector was initially unavailable
 * When spans are created and buffered
 * Then the application continues normally
 *
 * Note: True reconnection testing would require starting/stopping the
 * collector during the test, which is complex. This test verifies that
 * the application remains stable after a failed initialization.
 */
TEST_F(OTLPExportTest, ContinuedOperationAfterFailure) {
    // Set invalid endpoint
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://invalid-host:9999", 1);
    setenv("OTEL_SERVICE_NAME", "test_continued_operation", 1);

    // Initialize with invalid endpoint
    tracing::TracerProvider::Initialize();

    // Create multiple spans over time
    for (int i = 0; i < 10; i++) {
        EXPECT_NO_THROW({
            std::string span_name = "test_span_" + std::to_string(i);
            CreateTestSpan(span_name);
        }) << "Creating span " << i << " should not throw";

        // Small delay between spans
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Verify application is still stable
    EXPECT_TRUE(tracing::TracerProvider::IsInitialized())
        << "TracerProvider should remain initialized after multiple operations";

    // Graceful shutdown
    EXPECT_NO_THROW({
        tracing::TracerProvider::Shutdown(1000);
    }) << "Shutdown after failed exports should not throw";
}

/**
 * Test OTLP endpoint validation
 *
 * Acceptance Scenario:
 * Given various endpoint formats
 * When TracerProvider is initialized
 * Then it should handle different endpoint formats correctly
 */
TEST_F(OTLPExportTest, EndpointFormatHandling) {
    // Test with HTTP protocol explicitly specified
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://localhost:4318", 1);
    setenv("OTEL_SERVICE_NAME", "test_http_endpoint", 1);

    EXPECT_NO_THROW({
        tracing::TracerProvider::Initialize();
        EXPECT_TRUE(tracing::TracerProvider::IsInitialized());
        tracing::TracerProvider::Shutdown();
    }) << "HTTP endpoint format should be accepted";
}

/**
 * Test default endpoint when environment variable not set
 *
 * Acceptance Scenario:
 * Given OTEL_EXPORTER_OTLP_ENDPOINT is not set
 * When TracerProvider is initialized
 * Then it should use the default endpoint (localhost:4317)
 */
TEST_F(OTLPExportTest, DefaultEndpoint) {
    // Don't set OTEL_EXPORTER_OTLP_ENDPOINT
    setenv("OTEL_SERVICE_NAME", "test_default_endpoint", 1);

    EXPECT_NO_THROW({
        tracing::TracerProvider::Initialize();
        EXPECT_TRUE(tracing::TracerProvider::IsInitialized());

        // Create a test span
        CreateTestSpan("test_default_endpoint_span");

        tracing::TracerProvider::Shutdown();
    }) << "Default endpoint should be used when env var not set";
}

/**
 * Test export timeout handling
 *
 * Acceptance Scenario:
 * Given an OTLP collector that is slow or unresponsive
 * When ForceFlush is called with a timeout
 * Then it should return within the timeout period
 */
TEST_F(OTLPExportTest, ExportTimeoutHandling) {
    // Use a likely unavailable endpoint
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://localhost:29999", 1);
    setenv("OTEL_SERVICE_NAME", "test_timeout", 1);

    tracing::TracerProvider::Initialize();

    // Create spans
    CreateTestSpan("test_timeout_span_1");
    CreateTestSpan("test_timeout_span_2");

    // Force flush with a short timeout (1 second)
    auto start_time = std::chrono::steady_clock::now();
    bool flush_result = tracing::TracerProvider::ForceFlush(1000);
    auto end_time = std::chrono::steady_clock::now();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    // Flush should complete within reasonable time (allow 2x timeout for overhead)
    EXPECT_LE(elapsed_ms, 2500)
        << "ForceFlush should respect timeout and not hang indefinitely";

    // Cleanup
    tracing::TracerProvider::Shutdown(1000);
}

/**
 * Main test runner
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << "Running OTLP Export and Connectivity Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "NOTE: T061 (CollectorConnectivityCheck) requires OTLP collector" << std::endl;
    std::cout << "      running at localhost:4318" << std::endl;
    std::cout << "      Start with: docker run -p 4318:4318 grafana/otel-lgtm" << std::endl;
    std::cout << "========================================" << std::endl;

    return RUN_ALL_TESTS();
}
