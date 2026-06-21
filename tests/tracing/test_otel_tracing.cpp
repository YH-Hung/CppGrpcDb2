#include "otel_tracing.h"

#include <gtest/gtest.h>

#include <cstdlib>

TEST(OtelTracingTest, DisabledTracingReturnsEmptyIds) {
    setenv("OTEL_TRACING_ENABLED", "false", 1);

    otel::TracingOptions options;
    options.service_name = "otel_tracing_tests";
    otel::InitTracing(options);

    EXPECT_FALSE(otel::IsTracingEnabled());
    EXPECT_TRUE(otel::CurrentTraceIdHex().empty());
    EXPECT_TRUE(otel::CurrentSpanIdHex().empty());

    otel::ShutdownTracing();
    unsetenv("OTEL_TRACING_ENABLED");
}
