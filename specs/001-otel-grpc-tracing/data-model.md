# Data Model: OpenTelemetry Tracing Entities

**Feature**: 001-otel-grpc-tracing
**Date**: 2025-11-21
**Purpose**: Define the data entities and their relationships for distributed tracing

## Overview

This document describes the core entities in the OpenTelemetry tracing data model as implemented for gRPC services. These entities follow the OpenTelemetry specification.

## Core Entities

### 1. Trace

**Description**: A Trace represents the complete journey of a request through the distributed system. It is a collection of related Spans that share the same Trace ID.

**Attributes**:
- **trace_id** (128-bit unique identifier): Globally unique identifier for the entire trace
  - Format: 32-character hexadecimal string (e.g., `4bf92f3577b34da6a3ce929d0e0e4736`)
  - Generated when the first span is created (root span)
  - Propagated to all subsequent spans in the trace
- **spans** (collection): All spans belonging to this trace
- **root_span** (reference): The first span in the trace (entry point to the system)

**Lifecycle**:
1. Created implicitly when the first (root) span starts
2. Grows as child spans are created for subsequent operations
3. Considered complete when all spans have ended
4. Exported to OTLP collector after all spans are processed

**Relationships**:
- A Trace has many Spans (1:N)
- A Trace has exactly one root Span (1:1)

**Example**:
```
Trace ID: 4bf92f3577b34da6a3ce929d0e0e4736
├── Span: /helloworld.Greeter/SayHello (server) [root]
│   ├── Span: ExecuteDB2Query (internal)
│   └── Span: /order.OrderService/GetOrder (client)
│       └── Span: /order.OrderService/GetOrder (server)
```

### 2. Span

**Description**: A Span represents a single operation within a trace. It has a start time, end time, and contains metadata about the operation.

**Attributes**:
- **span_id** (64-bit unique identifier): Unique identifier for this span within the trace
  - Format: 16-character hexadecimal string (e.g., `00f067aa0ba902b7`)
  - Generated when span starts
  - Used as parent_span_id by child spans
- **trace_id** (reference): Trace ID this span belongs to (inherited from parent or generated for root)
- **parent_span_id** (64-bit identifier, optional): Span ID of the parent span
  - Empty/null for root spans
  - Set to parent's span_id for child spans
- **name** (string): Human-readable operation name
  - For gRPC server: RPC method (e.g., `/helloworld.Greeter/SayHello`)
  - For gRPC client: RPC method being called
  - For custom spans: Developer-provided operation name
- **kind** (enum): Type of span
  - `SERVER`: Span for a gRPC server handling an incoming RPC
  - `CLIENT`: Span for a gRPC client making an outgoing RPC
  - `INTERNAL`: Span for internal operations (e.g., database queries, processing)
- **start_time** (timestamp): When the operation started (microsecond precision)
- **end_time** (timestamp): When the operation completed (microsecond precision)
- **duration** (derived): end_time - start_time
- **status** (object): Outcome of the operation
  - **code** (enum): `OK`, `ERROR`, `UNSET`
  - **message** (string, optional): Error message if status is ERROR
- **attributes** (key-value map): Metadata about the operation
  - Standard attributes (OpenTelemetry semantic conventions):
    - `rpc.system`: "grpc"
    - `rpc.service`: Service name (e.g., "helloworld.Greeter")
    - `rpc.method`: Method name (e.g., "SayHello")
    - `rpc.grpc.status_code`: gRPC status code (0 = OK)
    - `net.peer.name`: Remote host for client spans
    - `net.peer.port`: Remote port for client spans
  - Custom attributes (application-specific):
    - `db2.connection_id`: Database connection ID if applicable
    - `user_id`: User making the request (if available)
- **events** (collection, optional): Timestamped log events within the span
  - Not used in initial implementation
- **links** (collection, optional): Links to other spans (for batch processing scenarios)
  - Not used in initial implementation

**Lifecycle**:
1. **Start**: Span created via `Tracer::StartSpan(name)`, start_time recorded
2. **Active**: Span is set as current span in thread-local context via `Scope`
3. **Attributes Set**: Metadata added via `Span::SetAttribute(key, value)`
4. **End**: Span ended (automatically via Scope destructor or manually), end_time recorded
5. **Export**: Span queued for export to OTLP collector via BatchSpanProcessor

**Relationships**:
- A Span belongs to exactly one Trace (N:1)
- A Span may have one parent Span (N:1, optional)
- A Span may have multiple child Spans (1:N, optional)

**Span Tree Example**:
```
Server Span (parent_span_id=null) [ROOT]
├── Internal Span (parent_span_id=server_span_id)
│   └── DB Query Span (parent_span_id=internal_span_id)
└── Client Span (parent_span_id=server_span_id)
    └── Server Span (parent_span_id=client_span_id) [on remote service]
```

### 3. Trace Context

**Description**: Trace Context is the metadata propagated across service boundaries to maintain trace continuity. It contains the trace_id and parent_span_id.

**Attributes**:
- **trace_id** (128-bit identifier): Trace ID to propagate
- **span_id** (64-bit identifier): Span ID of the current span (becomes parent_span_id for next service)
- **trace_flags** (8-bit flags): Sampling and other flags
  - Bit 0 (sampled): 1 if trace is sampled, 0 if not sampled
  - Bits 1-7: Reserved for future use
- **trace_state** (string, optional): Vendor-specific trace metadata (extensibility mechanism)

**W3C Trace Context Format** (propagated in gRPC metadata):
```
traceparent: 00-{trace_id}-{parent_span_id}-{trace_flags}
```

**Example**:
```
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
             ^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^  ^^
             |  trace_id (128-bit hex)             parent_span_id    flags
             version (00)                           (64-bit hex)      (01=sampled)
```

**Propagation Flow**:
1. **Service A (Server)**: Receives request with no traceparent → creates new trace_id and span_id
2. **Service A (Client)**: Makes outgoing RPC → injects traceparent with trace_id and current span_id
3. **Service B (Server)**: Receives request with traceparent → extracts trace_id and parent_span_id, creates child span

**Lifecycle**:
1. **Extract**: Read from incoming gRPC metadata (if present)
2. **Activate**: Set as current context in thread-local storage
3. **Use**: New spans automatically inherit trace_id and set parent_span_id
4. **Inject**: Write to outgoing gRPC metadata before RPC call

### 4. Resource

**Description**: Resource represents the entity producing telemetry. It identifies the service, host, and environment.

**Attributes** (attached to all spans from this service):
- **service.name** (string): Name of the service (e.g., "greeter_server", "greeter_client")
- **service.version** (string, optional): Version of the service (e.g., "1.0.0")
- **host.name** (string): Hostname where service is running
- **process.pid** (int): Process ID
- **deployment.environment** (string, optional): Environment (e.g., "dev", "staging", "prod")

**Example**:
```json
{
  "service.name": "greeter_server",
  "service.version": "1.0.0",
  "host.name": "macbook-pro.local",
  "process.pid": 12345,
  "deployment.environment": "dev"
}
```

**Usage**:
- Set once during TracerProvider initialization
- Automatically attached to all exported spans
- Used for filtering and grouping traces in observability backend

### 5. Tracer

**Description**: Tracer is the factory for creating Spans. Each instrumentation component (e.g., gRPC interceptor) gets its own Tracer.

**Attributes**:
- **name** (string): Name of the instrumentation library (e.g., "grpc-server-interceptor")
- **version** (string, optional): Version of the instrumentation library
- **schema_url** (string, optional): URL to semantic conventions schema

**Example**:
```cpp
auto tracer = tracer_provider->GetTracer(
  "grpc-server-interceptor",
  "1.0.0",
  "https://opentelemetry.io/schemas/1.21.0"
);
```

**Relationship**:
- A Tracer is obtained from the TracerProvider (singleton)
- A Tracer creates many Spans (1:N)

## Entity Relationships

```
TracerProvider (singleton)
    └── creates → Tracer (per instrumentation library)
            └── creates → Span (per operation)
                    └── belongs to → Trace (per request)
                            └── propagates → TraceContext (across services)
                                    └── describes → Resource (service metadata)
```

## Data Flow

```
1. Request arrives at gRPC Server
   ↓
2. Server Interceptor extracts TraceContext from metadata (or creates new Trace)
   ↓
3. Server Interceptor creates Server Span with trace_id and parent_span_id
   ↓
4. Span set as active in thread-local context
   ↓
5. Application code executes, optionally creates child Spans
   ↓
6. Application makes outgoing gRPC call
   ↓
7. Client Interceptor creates Client Span (inherits trace_id, parent=server span)
   ↓
8. Client Interceptor injects TraceContext into outgoing metadata
   ↓
9. Remote service receives request, creates child Server Span
   ↓
10. All Spans end, queued in BatchSpanProcessor
   ↓
11. BatchSpanProcessor exports Spans to OTLP Collector
   ↓
12. OTLP Collector forwards to Tempo backend
   ↓
13. Grafana queries Tempo to visualize complete Trace
```

## Implementation Mapping

| Entity | C++ Type | OpenTelemetry SDK Class |
|--------|----------|------------------------|
| Trace | Logical grouping | N/A (implicit via trace_id) |
| Span | Smart pointer | `opentelemetry::trace::Span` |
| Trace Context | Context object | `opentelemetry::trace::SpanContext` |
| Resource | Resource attributes | `opentelemetry::sdk::resource::Resource` |
| Tracer | Tracer instance | `opentelemetry::trace::Tracer` |
| TracerProvider | Singleton | `opentelemetry::sdk::trace::TracerProvider` |

## Validation Rules

**Trace ID**:
- MUST be 128-bit (16 bytes) non-zero value
- MUST be globally unique (use cryptographically secure random generator)
- MUST remain constant across all spans in a trace

**Span ID**:
- MUST be 64-bit (8 bytes) non-zero value
- MUST be unique within a trace
- MUST be generated for each span (no reuse)

**Parent-Child Relationships**:
- Root span MUST have null/empty parent_span_id
- Child span MUST have parent_span_id set to parent's span_id
- parent_span_id MUST reference a span in the same trace (same trace_id)

**Timing**:
- start_time MUST be before end_time
- end_time MUST be set when span ends
- Child span start_time SHOULD be after parent start_time
- Child span end_time SHOULD be before parent end_time

**Status**:
- Status MUST be set for all spans
- ERROR status MUST include error message
- gRPC errors (status != OK) MUST set span status to ERROR

## Example Trace Data (JSON Export Format)

```json
{
  "resourceSpans": [{
    "resource": {
      "attributes": [
        {"key": "service.name", "value": {"stringValue": "greeter_server"}},
        {"key": "host.name", "value": {"stringValue": "localhost"}}
      ]
    },
    "scopeSpans": [{
      "scope": {
        "name": "grpc-server-interceptor",
        "version": "1.0.0"
      },
      "spans": [{
        "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
        "spanId": "00f067aa0ba902b7",
        "parentSpanId": "",
        "name": "/helloworld.Greeter/SayHello",
        "kind": "SPAN_KIND_SERVER",
        "startTimeUnixNano": 1700000000000000000,
        "endTimeUnixNano": 1700000000050000000,
        "attributes": [
          {"key": "rpc.system", "value": {"stringValue": "grpc"}},
          {"key": "rpc.service", "value": {"stringValue": "helloworld.Greeter"}},
          {"key": "rpc.method", "value": {"stringValue": "SayHello"}},
          {"key": "rpc.grpc.status_code", "value": {"intValue": 0}}
        ],
        "status": {"code": "STATUS_CODE_OK"}
      }]
    }]
  }]
}
```

## References

- OpenTelemetry Trace Specification: https://opentelemetry.io/docs/specs/otel/trace/api/
- OpenTelemetry Semantic Conventions: https://opentelemetry.io/docs/specs/semconv/
- W3C Trace Context: https://www.w3.org/TR/trace-context/
- OTLP Protocol: https://opentelemetry.io/docs/specs/otlp/
