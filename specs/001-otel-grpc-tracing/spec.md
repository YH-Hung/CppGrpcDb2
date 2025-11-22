# Feature Specification: OpenTelemetry Tracing for gRPC Services

**Feature Branch**: `001-otel-grpc-tracing`
**Created**: 2025-11-21
**Status**: Draft
**Input**: User description: "enable opentelemetry tracing on all grpc servers and clients. logs should be appended with opentelemetry trace id and span id. report trace to otlp collector."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Distributed Request Tracing (Priority: P1)

As a DevOps engineer, I need to trace requests across all gRPC services to understand the complete request path, identify performance bottlenecks, and diagnose failures in the distributed system.

**Why this priority**: This is the core functionality - without end-to-end tracing, we cannot achieve the primary goal of distributed observability. This enables troubleshooting production issues that span multiple services.

**Independent Test**: Can be fully tested by sending a request through the gRPC client to the server and verifying that trace spans are created, exported to the OTLP collector, and contain the correct trace context propagation.

**Acceptance Scenarios**:

1. **Given** a gRPC server is running with OpenTelemetry tracing enabled, **When** a client makes a request, **Then** the server creates a trace span with unique trace ID and span ID
2. **Given** a gRPC client makes an outbound request, **When** the request is sent, **Then** a client-side span is created and trace context is propagated in request metadata
3. **Given** multiple gRPC services are involved in handling a request, **When** the request flows through all services, **Then** all spans share the same trace ID and maintain parent-child relationships
4. **Given** trace spans are created, **When** the request completes, **Then** all spans are exported to the OTLP collector with timing information and status

---

### User Story 2 - Correlated Log Debugging (Priority: P2)

As a developer debugging production issues, I need to see trace ID and span ID in every log message so I can correlate logs with traces and understand the exact sequence of events for a specific request.

**Why this priority**: Log correlation is essential for debugging but depends on tracing being enabled first (P1). This dramatically reduces mean time to resolution (MTTR) when investigating issues.

**Independent Test**: Can be fully tested by triggering a request that produces log messages, then verifying that each log line contains the current trace ID and span ID from the active OpenTelemetry context.

**Acceptance Scenarios**:

1. **Given** a gRPC request is being processed with an active trace span, **When** application code writes a log message, **Then** the log message includes the trace ID and span ID
2. **Given** multiple operations occur within a single request, **When** logs are written at different points, **Then** all logs for that request share the same trace ID
3. **Given** nested operations with parent-child spans, **When** logs are written in child spans, **Then** logs contain the specific span ID of the active span
4. **Given** no active trace context exists, **When** a log is written, **Then** the log is written successfully without trace information (graceful degradation)

---

### User Story 3 - Trace Data Analysis (Priority: P3)

As a site reliability engineer, I need all trace data reliably delivered to an OTLP collector so I can analyze performance trends, create dashboards, and set up alerts based on service latency and error rates.

**Why this priority**: While critical for long-term observability, this builds on the tracing infrastructure (P1) and can be validated after basic tracing works. This enables proactive monitoring and capacity planning.

**Independent Test**: Can be fully tested by configuring an OTLP collector endpoint, generating traces, and verifying that the collector receives complete trace data with all expected attributes and timing information.

**Acceptance Scenarios**:

1. **Given** an OTLP collector endpoint is configured, **When** traces are generated, **Then** trace data is exported to the collector in OTLP format
2. **Given** network connectivity issues with the collector, **When** traces are generated, **Then** the application continues functioning and retries or buffers trace data appropriately
3. **Given** high request volume, **When** many traces are generated, **Then** the tracing overhead does not degrade request processing performance by more than 5%
4. **Given** trace data is exported, **When** the collector receives the data, **Then** it includes service name, operation name, span attributes, timing, and status information

---

### Edge Cases

- What happens when the OTLP collector is unreachable during startup or runtime?
- How does the system handle trace context when clients do not provide any trace headers?
- What happens when trace sampling is configured to reduce overhead in high-traffic scenarios?
- How are errors during span creation or export handled without impacting request processing?
- What happens when log messages are written from threads not associated with any gRPC request context?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: All gRPC servers MUST automatically create server-side spans for incoming requests with trace ID, span ID, operation name, and timing information
- **FR-002**: All gRPC clients MUST automatically create client-side spans for outgoing requests and propagate trace context via gRPC metadata
- **FR-003**: Trace context (trace ID and parent span ID) MUST be propagated across service boundaries using W3C Trace Context standard in gRPC metadata
- **FR-004**: Every log message written during request processing MUST be automatically appended with the current trace ID and span ID from the active OpenTelemetry context
- **FR-005**: All generated trace spans MUST be exported to a configurable OTLP collector endpoint using the OTLP/gRPC protocol
- **FR-006**: The system MUST support configuring the OTLP collector endpoint via environment variables or configuration file
- **FR-007**: Trace export failures MUST NOT cause request processing to fail or block
- **FR-008**: The system MUST include service name, operation name (RPC method), start time, end time, and status (success/error) in each span
- **FR-009**: Client and server spans MUST be linked via parent-child relationships to form a complete trace tree
- **FR-010**: The system MUST handle cases where no trace context is present in incoming requests by creating a new root trace

### Assumptions

- The project already has OpenTelemetry C++ SDK installed and available (as indicated by the README showing opentelemetry-cpp installation)
- The OTLP collector will be deployed separately and its endpoint URL will be provided via configuration
- Default OTLP/gRPC protocol will be used for trace export (port 4317 standard)
- All existing gRPC servers (greeter_server, greeter_callback_server) and clients (greeter_client, greeter_girl_client) should be instrumented
- Trace sampling will initially be 100% (all requests traced) with option to configure sampling later
- Log format will follow existing spdlog conventions with trace ID and span ID as additional structured fields

### Key Entities

- **Trace**: Represents a complete request journey across all services, identified by a unique trace ID, containing multiple related spans
- **Span**: Represents a single operation within a trace (e.g., a gRPC server handling a request or client making a call), with span ID, parent ID, timing, and attributes
- **Trace Context**: Propagated metadata containing trace ID and parent span ID that flows across service boundaries to maintain trace continuity
- **OTLP Collector**: External service that receives, processes, and stores trace data from instrumented applications

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Operators can view complete request traces spanning all gRPC services with correct parent-child relationships in the observability backend
- **SC-002**: Every log message during request processing includes trace ID and span ID, enabling correlation of logs to traces
- **SC-003**: 100% of request traces are successfully exported to the OTLP collector under normal operating conditions
- **SC-004**: Tracing overhead adds less than 5% latency to request processing under normal load (measured via existing Prometheus metrics)
- **SC-005**: Developers can filter logs by trace ID to retrieve all log messages for a specific request
- **SC-006**: Mean time to resolution (MTTR) for distributed system issues decreases by at least 30% through improved trace visibility
- **SC-007**: System continues processing requests successfully even when the OTLP collector is unavailable (graceful degradation)
