# Research: OpenTelemetry Tracing for gRPC Services

**Feature**: 001-otel-grpc-tracing
**Date**: 2025-11-21
**Purpose**: Document technical research and design decisions for implementing OpenTelemetry tracing

## Overview

This document consolidates research findings on integrating OpenTelemetry tracing with C++ gRPC applications, focusing on automatic span creation, trace context propagation, and log correlation.

## Key Technical Decisions

### 1. OpenTelemetry SDK Integration Pattern

**Decision**: Use OpenTelemetry C++ SDK with OTLP gRPC exporter

**Rationale**:
- OpenTelemetry is the industry standard for distributed tracing (merged OpenCensus + OpenTracing)
- C++ SDK provides native performance with minimal overhead
- OTLP (OpenTelemetry Protocol) is the standard export format supported by all major observability backends
- gRPC-based OTLP exporter provides reliable, efficient transport

**Alternatives Considered**:
- **Jaeger C++ client**: Rejected - vendor-specific, less ecosystem support than OpenTelemetry
- **Zipkin C++ client**: Rejected - older standard, less feature-rich than OpenTelemetry
- **HTTP-based OTLP exporter**: Considered but gRPC exporter preferred for efficiency and streaming support

**Implementation Notes**:
- Link against `opentelemetry::trace`, `opentelemetry::exporter::otlp::otlp_grpc_exporter`
- Initialize TracerProvider as singleton during application startup
- Configure OTLP endpoint via environment variable `OTEL_EXPORTER_OTLP_ENDPOINT` (default: localhost:4317)

### 2. gRPC Interceptor Architecture for Automatic Span Creation

**Decision**: Implement custom gRPC interceptors for both server and client to automatically create spans

**Rationale**:
- gRPC interceptors provide hook points for all RPC calls without modifying business logic
- Automatic instrumentation ensures consistent tracing coverage
- Follows OpenTelemetry semantic conventions for RPC spans
- Similar pattern to existing `metrics_interceptor.cpp` in the codebase

**Alternatives Considered**:
- **Manual span creation in each RPC handler**: Rejected - error-prone, difficult to maintain, inconsistent coverage
- **gRPC OpenTelemetry plugin**: Considered but requires specific gRPC build configuration and less control over span attributes

**Implementation Notes**:
- Server interceptor: `grpc::experimental::ServerInterceptorFactoryInterface`
  - Extract trace context from incoming metadata (W3C traceparent header)
  - Create server span with operation name = RPC method
  - Set span attributes: `rpc.system=grpc`, `rpc.service`, `rpc.method`
  - Propagate context to handler thread via `grpc::ServerContext`
- Client interceptor: `grpc::experimental::ClientInterceptorFactoryInterface`
  - Create client span before RPC call
  - Inject trace context into outgoing metadata (W3C traceparent header)
  - Set span attributes: `rpc.system=grpc`, `net.peer.name`, `net.peer.port`

### 3. Trace Context Propagation Standard

**Decision**: Use W3C Trace Context standard (traceparent and tracestate headers)

**Rationale**:
- W3C Trace Context is the industry standard for cross-service trace propagation
- OpenTelemetry SDK provides built-in W3C propagator
- Ensures interoperability with other services and observability tools
- Supported by all major cloud providers and APM vendors

**Alternatives Considered**:
- **B3 propagation (Zipkin)**: Rejected - older standard, W3C is now preferred
- **Custom propagation format**: Rejected - reinventing the wheel, no interoperability

**Implementation Notes**:
- Use `opentelemetry::trace::propagation::HttpTraceContext` propagator
- Format: `traceparent: 00-{trace-id}-{span-id}-{flags}`
- Inject into gRPC metadata as `traceparent` key
- Extract from gRPC metadata on receiving end

### 4. Log Correlation Strategy

**Decision**: Create custom spdlog formatter that reads OpenTelemetry context and appends trace_id and span_id

**Rationale**:
- spdlog is already used in the project for logging
- Custom formatter is non-invasive (no changes to existing log calls)
- OpenTelemetry provides thread-local context storage that can be accessed from any thread
- Enables seamless correlation between logs and traces

**Alternatives Considered**:
- **Structured logging library (e.g., Boost.Log)**: Rejected - major dependency change, unnecessary complexity
- **Manual trace ID injection at each log site**: Rejected - error-prone, requires modifying all log calls
- **Log4cpp with OpenTelemetry appender**: Rejected - would require migrating away from spdlog

**Implementation Notes**:
- Extend `spdlog::formatter` base class
- In `format()` method:
  - Call `opentelemetry::trace::Tracer::GetCurrentSpan()`
  - Extract `trace_id` and `span_id` from span context
  - Format as hex strings and append to log message
- Log format: `[timestamp] [level] [trace_id={id}] [span_id={id}] message`
- Handle case where no active span exists (log without trace info)

### 5. OTLP Collector Setup for Local Development

**Decision**: Use grafana/otel-lgtm Docker image for local testing (LGTM = Loki, Grafana, Tempo, Mimir)

**Rationale**:
- All-in-one observability stack (logs, traces, metrics) in single container
- Grafana provides excellent trace visualization
- Tempo is purpose-built for distributed tracing at scale
- Simple Docker command for developers to spin up locally
- No separate OTLP collector configuration needed (built-in)

**Alternatives Considered**:
- **OpenTelemetry Collector + Jaeger**: Rejected - multiple containers to orchestrate
- **Zipkin**: Rejected - less feature-rich trace UI compared to Grafana
- **Cloud-hosted observability (e.g., Honeycomb, Lightstep)**: Considered for production but local option needed for development

**Implementation Notes**:
- Run: `docker run -p 3000:3000 -p 4317:4317 -p 4318:4318 --rm -ti grafana/otel-lgtm`
- Grafana UI: http://localhost:3000 (default user/pass: admin/admin)
- OTLP gRPC endpoint: localhost:4317
- OTLP HTTP endpoint: localhost:4318
- Tempo datasource pre-configured in Grafana

### 6. Span Lifecycle and RAII Pattern

**Decision**: Use OpenTelemetry's Scope pattern with RAII for automatic span end

**Rationale**:
- RAII ensures span end is called even if exceptions occur
- Prevents span leaks and incorrect timing measurements
- Idiomatic C++ pattern aligned with project best practices
- OpenTelemetry SDK provides `Scope` class for this purpose

**Implementation Notes**:
```cpp
auto span = tracer->StartSpan("operation_name");
auto scope = tracer->WithActiveSpan(span);
// span automatically ended when scope goes out of scope
```

### 7. Thread Safety and Context Propagation

**Decision**: Use OpenTelemetry's built-in thread-local context storage

**Rationale**:
- gRPC request handlers execute in worker thread pool
- Each request needs isolated trace context
- OpenTelemetry SDK provides `Context::SetCurrent()` for thread-local storage
- Automatically handles context propagation across async operations

**Alternatives Considered**:
- **Manual thread-local storage**: Rejected - OpenTelemetry provides this out-of-box
- **Pass context as function parameter**: Rejected - invasive changes to all function signatures

**Concurrency Considerations**:
- TracerProvider is thread-safe (can be accessed from multiple threads)
- OTLP exporter uses internal queue and background thread for export (non-blocking)
- Span creation/end operations are thread-safe
- Context storage is thread-local (no shared state between threads)

### 8. Performance Optimization Strategy

**Decision**: Use batch span processor with async export

**Rationale**:
- Batch processor amortizes export overhead across multiple spans
- Async export prevents blocking request processing
- Configurable batch size and timeout for tuning
- Default OpenTelemetry SDK configuration optimized for production

**Configuration Parameters**:
- `max_queue_size`: 2048 spans (buffer before dropping)
- `schedule_delay_millis`: 5000 (export every 5 seconds)
- `max_export_batch_size`: 512 (spans per export batch)

**Sampling Strategy** (Phase 2, not initial implementation):
- Start with 100% sampling (all requests traced)
- Consider adding probability-based sampling if overhead >5%
- Use `TraceIdRatioBased` sampler if needed

### 9. Error Handling and Graceful Degradation

**Decision**: Wrap OTLP export in try-catch, log errors but continue request processing

**Rationale**:
- Tracing failures should not impact application availability
- OpenTelemetry SDK handles most errors internally
- Log export failures for operational visibility
- Batch processor has retry logic and backpressure handling

**Edge Cases Handled**:
- OTLP collector unreachable: Spans buffered up to max_queue_size, then dropped with warning
- Invalid trace context in headers: Create new root trace
- Span creation failure: Log error, continue without tracing for that request
- Thread-local context unavailable: Log without trace info

### 10. Testing Strategy

**Decision**: Multi-layer testing approach

**Test Layers**:

1. **Unit Tests** (Google Test):
   - Test tracer provider initialization
   - Test span creation with correct attributes
   - Test trace context extraction/injection
   - Test log formatter with/without active span
   - Mock OTLP exporter to verify span data

2. **Integration Tests** (Google Test + Docker):
   - Start grafana/otel-lgtm container
   - Run instrumented server and client
   - Make RPC calls
   - Verify traces appear in Tempo via Grafana API
   - Verify trace context propagates across service boundaries
   - Verify logs contain matching trace IDs

3. **Performance Tests** (Prometheus metrics):
   - Measure latency with/without tracing enabled
   - Verify <5% overhead requirement
   - Measure memory footprint of trace buffers
   - Load test with 1000 req/s to verify stability

## CMake Integration

**Decision**: Create `cmake/opentelemetry.cmake` module for dependency resolution

**Content**:
```cmake
find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS api sdk trace exporters)

# Provide targets for linking
set(OPENTELEMETRY_LIBS
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_grpc_exporter
)
```

**Installation Check**:
- Verify `find_package(opentelemetry-cpp)` succeeds
- On macOS: `brew list opentelemetry-cpp` should show installed
- On Linux: Check `$HOME/.local/lib/cmake/opentelemetry-cpp`

## Environment Configuration

**Environment Variables**:
- `OTEL_EXPORTER_OTLP_ENDPOINT`: OTLP collector address (default: http://localhost:4317)
- `OTEL_SERVICE_NAME`: Service name in traces (default: binary name)
- `OTEL_RESOURCE_ATTRIBUTES`: Additional resource attributes (e.g., `environment=dev`)

**Configuration File** (future enhancement):
- Consider adding YAML config for advanced options (sampling, batch size, etc.)
- Not in initial implementation (use defaults)

## Migration Path

**Phase 1: Core Infrastructure** (this implementation):
- Tracer provider initialization
- gRPC interceptors for server/client
- Log correlation
- OTLP export
- Integration tests

**Phase 2: Enhancements** (future work):
- Custom span attributes for business context
- Sampling configuration
- Span events for key operations (DB queries, external API calls)
- Performance tuning based on production metrics

**Rollout Strategy**:
1. Deploy with tracing enabled but 100% sampling to canary servers
2. Monitor performance metrics (latency, CPU, memory)
3. Validate trace data quality in Grafana
4. Gradually roll out to all servers
5. Adjust sampling if needed based on observed overhead

## References

- OpenTelemetry C++ Documentation: https://opentelemetry.io/docs/languages/cpp/
- OpenTelemetry Specification: https://opentelemetry.io/docs/specs/otel/
- W3C Trace Context: https://www.w3.org/TR/trace-context/
- gRPC Interceptors Guide: https://grpc.io/docs/languages/cpp/interceptors/
- Grafana OTLP-LGTM: https://github.com/grafana/docker-otel-lgtm
- OpenTelemetry Semantic Conventions for RPC: https://opentelemetry.io/docs/specs/semconv/rpc/

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Performance overhead >5% | User experience degradation | Use batch processor, async export, add sampling if needed |
| Thread safety issues in multi-threaded gRPC | Data races, crashes | Use OpenTelemetry's thread-local context, run ThreadSanitizer |
| OTLP collector downtime blocks requests | Service unavailability | Non-blocking export with buffering, graceful degradation |
| Trace context not propagated correctly | Broken traces | Integration tests for propagation, validate with Grafana |
| Memory leak from span accumulation | OOM crashes | Set max_queue_size, monitor memory metrics, use AddressSanitizer |

## Open Questions / Future Considerations

1. **Span attributes**: What custom attributes should we add? (e.g., user_id, request_id, db2_connection_id)
2. **Sampling strategy**: When should we reduce sampling from 100%? What ratio?
3. **Integration with DB2 tracing**: Should DB2 queries be child spans?
4. **Resource pool tracing**: Should resource acquisition/release create spans?
5. **Production OTLP collector**: What backend will be used in production? (Tempo, Jaeger, cloud-hosted)

These questions will be addressed in Phase 2 based on initial implementation experience.
