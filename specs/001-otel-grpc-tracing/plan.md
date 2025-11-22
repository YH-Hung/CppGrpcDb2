# Implementation Plan: OpenTelemetry Tracing for gRPC Services

**Branch**: `001-otel-grpc-tracing` | **Date**: 2025-11-21 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-otel-grpc-tracing/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/commands/plan.md` for the execution workflow.

## Summary

Enable distributed tracing across all gRPC servers and clients using OpenTelemetry C++ SDK. Implement automatic span creation for all RPC calls with W3C trace context propagation. Augment all log messages with trace ID and span ID for correlation. Export trace data to OTLP collector (grafana/otel-lgtm) for visualization and analysis. Primary goal is to achieve end-to-end observability for diagnosing distributed system issues and reducing mean time to resolution by 30%.

## Technical Context

**Language/Version**: C++ 20
**Primary Dependencies**:
- opentelemetry-cpp (tracing SDK, OTLP exporter)
- gRPC C++ 1.72.0 (for RPC communication and interceptors)
- spdlog (for structured logging with trace context)
- prometheus-cpp (existing metrics, will coexist with traces)

**Storage**: N/A (traces exported to external OTLP collector, not persisted locally)
**Testing**:
- Google Test (existing test framework)
- Integration tests with local OTLP collector (grafana/otel-lgtm Docker container)
- Contract tests for trace context propagation

**Target Platform**: macOS (primary development), Linux (secondary/production)
**Project Type**: Single C++ project with multiple gRPC servers and clients
**Performance Goals**:
- <5% latency overhead from tracing instrumentation
- <10MB additional memory footprint for trace buffers
- 100% trace export success rate under normal conditions

**Constraints**:
- Must not block request processing if OTLP collector unavailable
- Must handle multi-threaded gRPC server/client contexts safely
- Must integrate with existing metrics_interceptor pattern
- Must maintain compatibility with existing greeter_server, greeter_callback_server, greeter_client, greeter_girl_client

**Scale/Scope**:
- 4 existing binaries to instrument (2 servers, 2 clients)
- Expected load: 100-1000 requests/second per server
- Trace retention handled by OTLP collector (not application concern)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### I. Test-Driven Development (NON-NEGOTIABLE)
**Status**: ✅ PASS (with plan)
- **Plan**: Write integration tests FIRST that:
  1. Verify spans are created for server and client calls
  2. Verify trace context propagates across service boundaries
  3. Verify logs contain trace ID and span ID
  4. Verify traces are exported to OTLP collector
- Tests will initially FAIL (Red phase), then implementation will make them pass (Green phase)
- User approval required on test design before implementation

### II. Concurrency Safety
**Status**: ✅ PASS (with review focus)
- **Considerations**:
  - OpenTelemetry context propagation must be thread-safe in gRPC multi-threaded servers
  - Trace provider and OTLP exporter initialization must be thread-safe (singleton pattern with proper locking)
  - Log formatter accessing trace context must handle concurrent writes safely
  - gRPC interceptors execute in request handler threads - must not introduce data races
- **Mitigation**: Use OpenTelemetry's thread-safe context APIs, document concurrency model, require ThreadSanitizer checks

### III. Code Quality
**Status**: ✅ PASS (with review plan)
- **Review checklist**:
  - Memory safety: Verify RAII for tracer provider, exporter lifecycle
  - Error handling: Verify graceful degradation when collector unavailable
  - Clarity: Document trace context propagation flow and span lifecycle
  - Performance: Verify no blocking calls in hot paths

### IV. Best Practices
**Status**: ✅ PASS (aligned)
- Use smart pointers for tracer provider and exporter resources
- Use RAII for span lifecycle management (automatic span end on destruction)
- Prefer OpenTelemetry standard APIs over custom implementations
- Follow existing code style (snake_case, consistent formatting)

### V. Documentation Standards
**Status**: ✅ PASS (with deliverables)
- **Required documentation**:
  - Design document: `doc/opentelemetry_tracing.md` explaining architecture
  - API documentation: Public tracing initialization and configuration functions
  - Usage examples: How to add custom spans and attributes
  - Quickstart guide: Setting up OTLP collector and viewing traces

### VI. Bug Prevention
**Status**: ✅ PASS (with validation plan)
- **Validation layers**:
  - Compiler warnings: -Wall -Wextra -Werror
  - Static analysis: clang-tidy checks for OpenTelemetry usage
  - Integration tests: Critical paths (span creation, propagation, export)
  - Memory safety: AddressSanitizer for leak detection, ThreadSanitizer for race conditions
  - Edge case tests: Collector unavailable, missing trace context, high concurrency

### Quality Standards Check
- **Compilation**: ✅ Code will compile on macOS and Linux
- **Warnings**: ✅ Will maintain -Wall -Wextra -Werror compliance
- **Tests**: ✅ Integration tests will cover all functional requirements
- **Memory Safety**: ✅ AddressSanitizer and ThreadSanitizer will be run
- **Performance**: ✅ Prometheus metrics will benchmark overhead
- **Documentation**: ✅ Design doc and usage guide will be created

**GATE RESULT**: ✅ ALL CHECKS PASS - Proceed to Phase 0 Research

## Project Structure

### Documentation (this feature)

```text
specs/001-otel-grpc-tracing/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output: OpenTelemetry C++ patterns, gRPC interceptor integration
├── data-model.md        # Phase 1 output: Trace/Span entity model
├── quickstart.md        # Phase 1 output: Setup OTLP collector, run instrumented servers
├── contracts/           # Phase 1 output: Trace context metadata schema
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
src/
├── tracing/                      # NEW: OpenTelemetry tracing infrastructure
│   ├── tracer_provider.h         # Singleton tracer provider initialization
│   ├── tracer_provider.cpp       # TracerProvider setup with OTLP exporter
│   ├── grpc_tracing_interceptor.h     # gRPC server/client interceptor for automatic span creation
│   ├── grpc_tracing_interceptor.cpp   # Interceptor implementation
│   └── trace_log_formatter.h     # spdlog custom formatter to inject trace context
├── greeter_server.cpp            # MODIFY: Add tracing interceptor
├── greeter_callback_server.cpp   # MODIFY: Add tracing interceptor
├── greeter_client.cpp            # MODIFY: Add tracing interceptor
├── greeter_girl_client.cpp       # MODIFY: Add tracing interceptor
└── metrics_interceptor.cpp       # EXISTING: Coexist with tracing interceptor

include/
└── tracing/                      # NEW: Public tracing headers
    ├── tracer_provider.h         # Public API for initialization
    └── trace_log_formatter.h     # Public API for log integration

tests/
├── integration/
│   ├── test_tracing_e2e.cpp      # NEW: End-to-end tracing test with OTLP collector
│   ├── test_trace_propagation.cpp # NEW: Test trace context across client-server
│   └── test_log_correlation.cpp  # NEW: Test logs contain trace IDs
└── unit/
    ├── test_tracer_provider.cpp  # NEW: Unit tests for tracer initialization
    └── test_grpc_interceptor.cpp # NEW: Unit tests for span creation logic

doc/
└── opentelemetry_tracing.md      # NEW: Architecture and design decisions

cmake/
└── opentelemetry.cmake           # NEW: Find OpenTelemetry package configuration

docker/
└── otel-collector-config.yaml    # NEW: OTLP collector configuration for testing
```

**Structure Decision**: Single C++ project structure retained. New `tracing/` subdirectory under `src/` and `include/` for OpenTelemetry components, following existing pattern (similar to `metrics_interceptor`). Integration tests added under `tests/integration/` for end-to-end validation.

## Complexity Tracking

No constitution violations. All principles satisfied.
