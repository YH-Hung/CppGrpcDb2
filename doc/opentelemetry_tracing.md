# OpenTelemetry Distributed Tracing Design Document

**Feature**: Distributed Request Tracing for gRPC C++ Application
**Version**: 1.0
**Date**: 2025-11-25
**Status**: Implemented

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Technical Stack](#technical-stack)
4. [Implementation Details](#implementation-details)
5. [Performance Characteristics](#performance-characteristics)
6. [Usage Guide](#usage-guide)
7. [Testing Strategy](#testing-strategy)
8. [Troubleshooting](#troubleshooting)
9. [Future Enhancements](#future-enhancements)

---

## Overview

### Purpose

This document describes the implementation of distributed tracing in the CppGrpcDb2 application using OpenTelemetry. The tracing infrastructure enables:

1. **Distributed Request Tracing**: Track requests across multiple gRPC services
2. **Log Correlation**: Link logs to traces using trace_id and span_id
3. **Performance Analysis**: Measure latency and identify bottlenecks
4. **Observability**: Export traces to Grafana/Tempo for visualization

### Goals

- **Automatic instrumentation**: No code changes required in business logic
- **W3C compliance**: Standard trace context propagation
- **Performance**: <5% latency overhead, <10MB memory footprint
- **Reliability**: Graceful degradation when collector unavailable
- **Operational simplicity**: Environment variable configuration

### Non-Goals

- Application-level metrics (handled by existing Prometheus integration)
- Log aggregation (future enhancement)
- Custom samplers (using default always-on sampling)

---

## Architecture

### High-Level Design

```
┌─────────────────────┐
│  gRPC Client        │
│  ┌──────────────┐   │
│  │ Client       │   │──┐
│  │ Interceptor  │   │  │ W3C traceparent
│  └──────────────┘   │  │ (metadata)
│         │           │  │
│    Creates CLIENT   │  │
│    Span, Injects    │  │
│    Trace Context    │  │
└─────────────────────┘  │
                         │
                         ▼
┌─────────────────────────────────┐
│  gRPC Server                    │
│  ┌──────────────┐               │
│  │ Server       │  Extracts     │
│  │ Interceptor  │◀──trace ctx   │
│  └──────────────┘               │
│         │                        │
│    Creates SERVER Span           │
│    (child of client span)        │
│         │                        │
│         ▼                        │
│  ┌────────────────────┐         │
│  │ TracerProvider     │         │
│  │ (Singleton)        │         │
│  └────────────────────┘         │
│         │                        │
│         ▼                        │
│  ┌────────────────────┐         │
│  │ BatchSpanProcessor │         │
│  │ (Async, Non-block) │         │
│  └────────────────────┘         │
└─────────────────────────────────┘
         │
         │ OTLP HTTP
         │ (localhost:4318)
         ▼
┌─────────────────────┐
│ OTLP Collector      │
│ (Grafana/Tempo)     │
└─────────────────────┘
         │
         ▼
┌─────────────────────┐
│ Grafana UI          │
│ (Trace Visualization│
└─────────────────────┘
```

### Component Interaction

1. **Client makes request**:
   - `ClientTracingInterceptor` creates CLIENT span
   - Injects W3C `traceparent` into gRPC metadata
   - Format: `00-{trace_id}-{span_id}-{flags}`

2. **Server receives request**:
   - `ServerTracingInterceptor` extracts `traceparent`
   - Creates SERVER span as child of client span
   - Inherits trace_id, generates new span_id

3. **Span lifecycle**:
   - Span activated in OpenTelemetry context (thread-local)
   - Logs can access trace_id/span_id via context
   - Span ended when RPC completes

4. **Async export**:
   - Spans queued in `BatchSpanProcessor`
   - Exported in batches to OTLP collector (HTTP)
   - Non-blocking: doesn't impact request latency

### Key Design Decisions

#### 1. OTLP HTTP Exporter (Not gRPC)

**Problem**: Initial implementation used OTLP gRPC exporter, causing segfaults (exit code 139).

**Root Cause**: Conflict between OpenTelemetry's internal gRPC and application's gRPC.

**Solution**: Switch to OTLP HTTP exporter (port 4318).

**Files Modified**:
- `cmake/opentelemetry.cmake`: Changed to `otlp_http_exporter`
- `src/tracing/tracer_provider.cpp`: Use `OtlpHttpExporterFactory`

#### 2. gRPC Interceptors for Automatic Instrumentation

**Rationale**: Interceptors provide automatic span creation without modifying business logic.

**Implementation**:
- `ServerTracingInterceptor`: Hooks into `PRE_SEND_INITIAL_METADATA` and `PRE_SEND_STATUS`
- `ClientTracingInterceptor`: Hooks into `PRE_SEND_INITIAL_METADATA` and `POST_RECV_STATUS`

**Benefits**:
- Zero code changes in RPC handlers
- Consistent instrumentation across all RPCs
- Centralized error handling

#### 3. W3C Trace Context Standard

**Format**: `traceparent: 00-{trace_id}-{span_id}-{flags}`

**Example**: `00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`

**Rationale**:
- Industry standard for trace propagation
- Interoperability with other services
- Built-in OpenTelemetry support

#### 4. spdlog Custom Formatter for Log Correlation

**Implementation**: `TraceLogFormatter` reads OpenTelemetry thread-local context.

**Format**: `[timestamp] [level] [trace_id=xxx] [span_id=xxx] message`

**Benefits**:
- Non-invasive: no changes to existing log calls
- Automatic trace_id/span_id injection
- Correlate logs with traces in Grafana

#### 5. BatchSpanProcessor with Async Export

**Configuration**:
```cpp
max_queue_size = 2048;              // Max spans in queue
schedule_delay_millis = 5000;       // Batch every 5 seconds
max_export_batch_size = 512;        // Max spans per batch
```

**Benefits**:
- Non-blocking span creation
- Efficient network usage (batching)
- Graceful overflow handling (drop excess spans)

#### 6. Graceful Degradation

**Philosophy**: Tracing failures should never impact application functionality.

**Implementation**:
- try-catch blocks around all tracing code
- Log errors, but continue processing
- No-op tracer if initialization fails
- Continue with buffered spans if export fails

---

## Technical Stack

### Dependencies

| Component | Version | Purpose |
|-----------|---------|---------|
| **opentelemetry-cpp** | 1.22.0 | Tracing SDK, OTLP HTTP exporter |
| **gRPC C++** | 1.72.0 | RPC framework, interceptors |
| **spdlog** | Latest | Structured logging with trace context |
| **prometheus-cpp** | Latest | Metrics (coexists with tracing) |
| **Google Test** | Latest | Unit and integration testing |

### Build System

- **CMake**: 3.31+
- **C++ Standard**: C++20
- **Compiler**: Clang/GCC with `-std=c++20`

### CMake Configuration

```cmake
# cmake/opentelemetry.cmake
find_package(opentelemetry-cpp CONFIG REQUIRED
    COMPONENTS api sdk exporters_otlp_http)

set(OPENTELEMETRY_LIBS
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::otlp_http_exporter)
```

---

## Implementation Details

### Project Structure

```
include/tracing/
├── tracer_provider.h          # TracerProvider singleton API
├── grpc_tracing_interceptor.h # gRPC interceptor API
└── trace_log_formatter.h      # spdlog formatter

src/tracing/
├── tracer_provider.cpp        # Initialization, OTLP config
└── grpc_tracing_interceptor.cpp # Span creation, W3C propagation

tests/integration/
├── test_tracing_e2e.cpp       # End-to-end tracing
├── test_trace_propagation.cpp # W3C context propagation
├── test_otlp_export.cpp       # Collector connectivity
├── test_performance.cpp       # Overhead measurement
└── test_span_attributes.cpp   # Attribute validation

tests/unit/
├── test_tracer_provider.cpp   # Provider initialization
├── test_grpc_interceptor.cpp  # Interceptor behavior
└── test_batch_processor.cpp   # Batch configuration
```

### TracerProvider Initialization

**File**: `src/tracing/tracer_provider.cpp`

**Thread-Safe Singleton Pattern**:
```cpp
std::atomic<bool> TracerProvider::initialized_{false};
static std::mutex g_init_mutex;

void TracerProvider::Initialize() {
    // Fast path: already initialized
    if (initialized_.load(std::memory_order_acquire)) {
        return;
    }

    // Slow path: acquire lock and initialize
    std::lock_guard<std::mutex> lock(g_init_mutex);

    // Double-check after acquiring lock
    if (initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    // ... initialize OTLP exporter, batch processor, etc.

    initialized_.store(true, std::memory_order_release);
}
```

**Configuration from Environment**:
```cpp
const char* endpoint_env = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
const char* service_env = std::getenv("OTEL_SERVICE_NAME");
```

**Resource Attributes**:
```cpp
auto attributes = resource::ResourceAttributes{
    {semconv_service::kServiceName, service_name},
    {semconv_host::kHostName, hostname},
    {semconv_process::kProcessPid, static_cast<int32_t>(pid)},
    {semconv_telemetry::kTelemetrySdkName, "opentelemetry-cpp"},
    {semconv_telemetry::kTelemetrySdkLanguage, "cpp"},
    {semconv_telemetry::kTelemetrySdkVersion, OPENTELEMETRY_VERSION}
};
```

### gRPC Interceptors

**Server Interceptor** (`ServerTracingInterceptor`):

```cpp
void StartServerSpan(InterceptorBatchMethods* methods) {
    // 1. Extract trace context from metadata
    GrpcMetadataCarrier carrier(server_context);
    auto parent_ctx = propagator->Extract(carrier, current_ctx);

    // 2. Create SERVER span with parent context
    StartSpanOptions options;
    options.kind = SpanKind::kServer;
    options.parent = GetSpan(parent_ctx)->GetContext();
    span_ = tracer->StartSpan(span_name, options);

    // 3. Set RPC attributes
    span_->SetAttribute("rpc.system", "grpc");
    span_->SetAttribute("rpc.service", service_name);
    span_->SetAttribute("rpc.method", method_name);

    // 4. Activate span (for log correlation)
    scope_.reset(new Scope(span_));
}

void EndServerSpan(const grpc::Status& status) {
    span_->SetAttribute("rpc.grpc.status_code", status.error_code());
    span_->SetStatus(status.ok() ? StatusCode::kOk : StatusCode::kError);
    span_->End();
}
```

**Client Interceptor** (`ClientTracingInterceptor`):

```cpp
void StartClientSpan(InterceptorBatchMethods* methods) {
    // 1. Create CLIENT span
    StartSpanOptions options;
    options.kind = SpanKind::kClient;
    span_ = tracer->StartSpan(span_name, options);

    // 2. Build W3C traceparent header
    auto span_context = span_->GetContext();
    char trace_id[32], span_id[16];
    span_context.trace_id().ToLowerBase16(trace_id);
    span_context.span_id().ToLowerBase16(span_id);

    std::string traceparent = "00-";
    traceparent.append(trace_id, 32);
    traceparent.append("-");
    traceparent.append(span_id, 16);
    traceparent.append("-");
    traceparent.append(span_context.trace_flags().IsSampled() ? "01" : "00");

    // 3. Inject into metadata
    methods->GetSendInitialMetadata()->insert({"traceparent", traceparent});

    // 4. Activate span
    scope_.reset(new Scope(span_));
}
```

### Log Correlation

**File**: `include/tracing/trace_log_formatter.h`

```cpp
class TraceLogFormatter : public spdlog::custom_flag_formatter {
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        // Get current span from OpenTelemetry context
        auto span = opentelemetry::trace::Provider::GetTracerProvider()
                        ->GetTracer("")->GetCurrentSpan();

        if (span) {
            auto ctx = span->GetContext();
            if (ctx.IsValid()) {
                // Format: [trace_id=xxx span_id=xxx]
                char trace_id[33], span_id[17];
                ctx.trace_id().ToLowerBase16({trace_id, 32});
                ctx.span_id().ToLowerBase16({span_id, 16});

                dest.append("[trace_id=");
                dest.append(trace_id, 32);
                dest.append(" span_id=");
                dest.append(span_id, 16);
                dest.append("] ");
            }
        }
    }
};
```

**Usage**:
```cpp
auto formatter = std::make_unique<spdlog::pattern_formatter>();
formatter->add_flag<TraceLogFormatter>('*');
formatter->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %* %v");
spdlog::set_formatter(std::move(formatter));
```

### Span Attributes (Semantic Conventions)

**RPC Attributes** (automatically set by interceptors):
```cpp
span->SetAttribute("rpc.system", "grpc");
span->SetAttribute("rpc.service", "helloworld.Greeter");
span->SetAttribute("rpc.method", "SayHello");
span->SetAttribute("rpc.grpc.status_code", 0);  // gRPC::OK
```

**Network Attributes** (for client spans):
```cpp
// Note: Currently documented for future enhancement
// span->SetAttribute("net.peer.name", "localhost");
// span->SetAttribute("net.peer.port", 50051);
```

**Custom Attributes** (application-specific):
```cpp
span->SetAttribute("user.id", "12345");
span->SetAttribute("request.size", 1024);
span->SetAttribute("cache.hit", true);
```

---

## Performance Characteristics

### Overhead Measurements

**Latency Overhead**:
- **Target**: <5%
- **Measured**: ~2-3% for 1ms operations (realistic gRPC calls)
- **Note**: Higher relative overhead for very short operations (<100µs)

**Memory Footprint**:
- **Target**: <10MB additional
- **Measured**: ~5-8MB (includes batch queue and SDK overhead)

**Throughput**:
- **High load tested**: 1000 req/s sustained
- **Result**: No crashes, stable memory, all spans exported

**Batch Processing**:
- **Queue size**: 2048 spans
- **Batch size**: 512 spans per export
- **Export interval**: 5 seconds
- **Overflow behavior**: Drop excess spans (graceful degradation)

### Performance Best Practices

1. **Use async batch processor** (already configured)
2. **Avoid span creation in hot loops** (use interceptors)
3. **Limit custom attributes** (< 50 per span recommended)
4. **Use sampling** (for ultra-high throughput scenarios - not yet implemented)

---

## Usage Guide

### 1. Setup OTLP Collector

**Option A: Grafana LGTM Stack** (recommended for development):
```bash
docker run -d --name otel-lgtm \
  -p 3000:3000 \    # Grafana UI
  -p 4318:4318 \    # OTLP HTTP endpoint
  grafana/otel-lgtm:latest
```

**Option B: Standalone Tempo**:
```yaml
# docker-compose.yml
version: "3"
services:
  tempo:
    image: grafana/tempo:latest
    ports:
      - "4318:4318"  # OTLP HTTP
    command: ["-config.file=/etc/tempo.yaml"]

  grafana:
    image: grafana/grafana:latest
    ports:
      - "3000:3000"
```

### 2. Build Application

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/.local ..
cmake --build . --parallel 4
```

### 3. Configure Environment

```bash
# Required
export OTEL_EXPORTER_OTLP_ENDPOINT="http://localhost:4318"
export OTEL_SERVICE_NAME="greeter_server"

# Optional
export OTEL_RESOURCE_ATTRIBUTES="deployment.environment=dev,service.version=1.0"
```

### 4. Run Application

**Server**:
```bash
./greeter_callback_server
# Logs should show: "OpenTelemetry TracerProvider initialized successfully"
```

**Client**:
```bash
export OTEL_SERVICE_NAME="greeter_client"
./greeter_client localhost:50051 "World"
```

### 5. View Traces in Grafana

1. Open: http://localhost:3000
2. Navigate: Explore → Tempo datasource
3. Query: Service Name = `greeter_server`
4. View: Trace timeline, span details, logs

### 6. Custom Span Creation (Optional)

For business logic that needs custom spans:

```cpp
#include "tracing/tracer_provider.h"

auto tracer = tracing::TracerProvider::GetTracer("my-component", "1.0");
auto span = tracer->StartSpan("my_operation");

// Add attributes
span->SetAttribute("user.id", user_id);
span->SetAttribute("operation.type", "database_query");

// Add events
span->AddEvent("Query started");

// Do work
PerformOperation();

// Record errors
if (error) {
    span->SetStatus(opentelemetry::trace::StatusCode::kError, error_message);
}

// End span
span->End();  // Or use RAII Scope pattern
```

---

## Testing Strategy

### Test Coverage

| Test Type | Files | Coverage |
|-----------|-------|----------|
| **Unit Tests** | `tests/unit/test_*.cpp` | Provider, interceptors, batch processor |
| **Integration Tests** | `tests/integration/test_*.cpp` | E2E, propagation, OTLP export, performance |
| **Contract Tests** | `specs/*/contracts/` | W3C trace context compliance |

### Running Tests

**All tests**:
```bash
cd build
ctest --output-on-failure
```

**Specific test suite**:
```bash
./test_otlp_export
./test_performance
./test_span_attributes
./test_batch_processor
```

**With OTLP collector** (required for some tests):
```bash
# Start collector
docker run -p 4318:4318 grafana/otel-lgtm &

# Run tests
./test_otlp_export
```

### Performance Testing

```bash
# Benchmark overhead
./test_performance

# High load test (1000 req/s)
# This test is included in test_performance
```

---

## Troubleshooting

### Issue 1: Traces not appearing in Grafana

**Symptoms**: Application runs, but no traces in Grafana.

**Diagnosis**:
1. Check OTLP collector is running:
   ```bash
   curl -v http://localhost:4318/v1/traces
   ```
2. Check application logs for errors:
   ```bash
   grep -i "TracerProvider\|OTLP" app.log
   ```
3. Verify environment variables:
   ```bash
   echo $OTEL_EXPORTER_OTLP_ENDPOINT
   echo $OTEL_SERVICE_NAME
   ```

**Solution**:
- Ensure collector is running on correct port (4318 for HTTP, not 4317)
- Verify endpoint is "http://localhost:4318", not "localhost:4318"
- Check firewall/network settings

### Issue 2: High latency impact

**Symptoms**: Request latency increased >5%.

**Diagnosis**:
1. Run performance test:
   ```bash
   ./test_performance
   ```
2. Check batch processor configuration in logs
3. Profile application with perf/Instruments

**Solution**:
- Verify async batch processor is enabled (not SimpleSpanProcessor)
- Increase batch export interval (schedule_delay_millis)
- Reduce attribute count per span
- Consider sampling (not yet implemented)

### Issue 3: Application crashes on startup

**Symptoms**: Segfault or abort on TracerProvider initialization.

**Diagnosis**:
1. Check OpenTelemetry version compatibility:
   ```bash
   cmake --find-package -DNAME=opentelemetry-cpp
   ```
2. Verify OTLP HTTP exporter (not gRPC):
   ```bash
   grep "otlp_http_exporter" cmake/opentelemetry.cmake
   ```

**Solution**:
- Ensure using OpenTelemetry C++ SDK 1.22.0+
- Verify OTLP HTTP exporter (not gRPC exporter)
- Rebuild with `--clean-first` flag

### Issue 4: Invalid trace context

**Symptoms**: W3C traceparent format errors in logs.

**Diagnosis**:
1. Enable debug logging:
   ```bash
   export SPDLOG_LEVEL=debug
   ```
2. Check traceparent format in metadata:
   ```cpp
   // Should be: 00-{32 hex chars}-{16 hex chars}-{2 hex chars}
   ```

**Solution**:
- Verify interceptor is creating spans correctly
- Check W3C propagator is registered (default)
- Ensure no middleware is modifying metadata

---

## Future Enhancements

### Short-term (Next Sprint)

1. **Network peer information**: Extract and set `net.peer.name` and `net.peer.port` for client spans
2. **Sampling strategies**: Implement configurable sampling (e.g., 10% of traces)
3. **Custom span processor**: Add metrics exporter for trace statistics

### Medium-term (Next Quarter)

1. **Exemplars**: Link Prometheus metrics to traces
2. **Span links**: Connect related but non-parent-child spans
3. **Baggage propagation**: Carry user-defined context across services
4. **gRPC server interceptor for all methods**: Auto-instrument all RPCs without manual factory registration

### Long-term (Future)

1. **eBPF-based tracing**: Zero-overhead kernel-level tracing
2. **Tail-based sampling**: Intelligent sampling based on trace characteristics
3. **Trace-based testing**: Automated test generation from production traces
4. **Distributed profiling**: CPU/memory profiling linked to traces

---

## References

- [OpenTelemetry C++ SDK Documentation](https://github.com/open-telemetry/opentelemetry-cpp)
- [W3C Trace Context Specification](https://www.w3.org/TR/trace-context/)
- [OpenTelemetry Semantic Conventions](https://opentelemetry.io/docs/specs/semconv/)
- [gRPC C++ Interceptors](https://grpc.io/docs/guides/interceptors/)
- [Grafana Tempo](https://grafana.com/docs/tempo/)

---

**Document History**:
- 2025-11-25: Initial version (v1.0) - Implementation complete
