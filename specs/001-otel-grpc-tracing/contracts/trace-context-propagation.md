# Contract: Trace Context Propagation

**Feature**: 001-otel-grpc-tracing
**Date**: 2025-11-21
**Purpose**: Define the contract for propagating trace context across gRPC service boundaries

## Overview

This contract specifies how trace context (trace ID, span ID, and flags) must be propagated between gRPC clients and servers using W3C Trace Context standard in gRPC metadata.

## W3C Trace Context Standard

### traceparent Header Format

**Header Name**: `traceparent` (lowercase, case-insensitive)

**Format**:
```
traceparent: version-trace_id-span_id-trace_flags
```

**Field Specifications**:

| Field | Size | Format | Description | Example |
|-------|------|--------|-------------|---------|
| version | 2 hex chars | `00` | Version of trace context format (currently 00) | `00` |
| trace_id | 32 hex chars | [0-9a-f]{32} | 128-bit trace ID, lowercase hex, non-zero | `4bf92f3577b34da6a3ce929d0e0e4736` |
| span_id | 16 hex chars | [0-9a-f]{16} | 64-bit parent span ID, lowercase hex, non-zero | `00f067aa0ba902b7` |
| trace_flags | 2 hex chars | [0-9a-f]{2} | 8-bit flags (bit 0 = sampled) | `01` |

**Example**:
```
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
```

### tracestate Header (Optional)

**Header Name**: `tracestate` (lowercase, case-insensitive)

**Format**:
```
tracestate: vendor1=value1,vendor2=value2
```

**Usage**: Extended vendor-specific trace metadata (not used in initial implementation)

## gRPC Metadata Mapping

### Client-Side Injection (Outgoing Requests)

**When**: Before making outgoing gRPC call

**Action**: Client interceptor MUST inject `traceparent` into gRPC client context metadata

**Implementation** (pseudo-code):
```cpp
// 1. Get current span from context
auto span = opentelemetry::trace::Tracer::GetCurrentSpan(context);
auto span_context = span->GetContext();

// 2. Format traceparent value
std::string traceparent = FormatTraceParent(
    span_context.trace_id(),
    span_context.span_id(),
    span_context.trace_flags()
);

// 3. Inject into gRPC metadata
client_context->AddMetadata("traceparent", traceparent);
```

**Result**: gRPC request metadata contains:
```
traceparent: 00-{trace_id}-{span_id}-01
```

### Server-Side Extraction (Incoming Requests)

**When**: Upon receiving gRPC request

**Action**: Server interceptor MUST extract `traceparent` from gRPC server context metadata

**Implementation** (pseudo-code):
```cpp
// 1. Get traceparent from incoming metadata
auto metadata = server_context->client_metadata();
auto it = metadata.find("traceparent");

if (it != metadata.end()) {
    // 2. Parse traceparent value
    auto trace_context = ParseTraceParent(it->second);

    // 3. Create span with extracted trace_id and parent_span_id
    auto span = tracer->StartSpan(
        rpc_method,
        {
            .parent = trace_context,
            .kind = SpanKind::SERVER
        }
    );
} else {
    // 4. No traceparent found -> create root span (new trace)
    auto span = tracer->StartSpan(rpc_method);
}
```

## Contract Rules

### Client MUST

1. **Generate trace_id** if this is the first span in a new trace (root span)
   - Use cryptographically secure random 128-bit value
   - Ensure non-zero value
2. **Inject traceparent** into every outgoing gRPC request metadata
   - Format: `00-{trace_id}-{current_span_id}-{flags}`
   - `current_span_id` is the span ID of the client-side span
3. **Use lowercase hexadecimal** for trace_id and span_id
4. **Set trace_flags bit 0** to 1 if trace is sampled, 0 if not
5. **Create client-side span** before injection with:
   - `span.kind = CLIENT`
   - `parent_span_id` set to parent span (if exists)
   - `trace_id` inherited from parent trace

### Server MUST

1. **Check for traceparent** in incoming request metadata
2. **Extract trace context** if traceparent present:
   - Parse trace_id (32 hex chars)
   - Parse parent_span_id (16 hex chars)
   - Parse trace_flags (2 hex chars)
3. **Create server-side span** with:
   - `span.kind = SERVER`
   - `trace_id` inherited from extracted context
   - `parent_span_id` set to extracted span_id from traceparent
4. **Create root span** if no traceparent present:
   - Generate new trace_id
   - Set parent_span_id to null/empty
5. **Set as active span** in thread-local context for request duration
6. **NOT propagate** trace context to response metadata (one-way propagation)

### Both MUST

1. **Validate traceparent format** before parsing:
   - Exactly 3 hyphens
   - version = "00"
   - trace_id is 32 hex chars, non-zero
   - span_id is 16 hex chars, non-zero
   - trace_flags is 2 hex chars
2. **Handle invalid traceparent** gracefully:
   - Log warning
   - Create new root span (ignore invalid traceparent)
   - Do NOT propagate invalid traceparent
3. **Preserve trace_id** across entire trace (never modify)
4. **Generate new span_id** for each span (never reuse)

## Example Propagation Flow

### Scenario: Client → Server A → Server B

```
1. Client (no parent trace)
   ↓
   Creates root span
   trace_id: 4bf92f3577b34da6a3ce929d0e0e4736
   span_id:  00f067aa0ba902b7
   parent:   null
   ↓
   Injects traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01

2. Server A receives request
   ↓
   Extracts traceparent
   ↓
   Creates server span
   trace_id: 4bf92f3577b34da6a3ce929d0e0e4736  (same)
   span_id:  1234567890abcdef
   parent:   00f067aa0ba902b7  (from traceparent)
   ↓
   Makes outgoing call to Server B
   ↓
   Injects traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-1234567890abcdef-01

3. Server B receives request
   ↓
   Extracts traceparent
   ↓
   Creates server span
   trace_id: 4bf92f3577b34da6a3ce929d0e0e4736  (same)
   span_id:  fedcba0987654321
   parent:   1234567890abcdef  (from traceparent)
```

**Result**: All three spans have the same `trace_id`, forming a single trace with correct parent-child relationships.

## Contract Testing

### Test Cases

**TC-001: Client injects traceparent on outgoing call**
- **Given**: Client makes gRPC call with active span
- **When**: Request is sent
- **Then**: Request metadata contains `traceparent` header with correct format

**TC-002: Server extracts traceparent from incoming call**
- **Given**: Server receives request with `traceparent` in metadata
- **When**: Request is processed
- **Then**: Server creates span with same trace_id and correct parent_span_id

**TC-003: Server creates root span when no traceparent**
- **Given**: Server receives request without `traceparent` in metadata
- **When**: Request is processed
- **Then**: Server creates new root span with new trace_id and null parent

**TC-004: Invalid traceparent is rejected**
- **Given**: Server receives request with malformed `traceparent`
- **When**: Request is processed
- **Then**: Server logs warning and creates new root span (ignores invalid header)

**TC-005: Trace ID preserved across multiple hops**
- **Given**: Client → Server A → Server B chain
- **When**: Request flows through all services
- **Then**: All spans have same trace_id

**TC-006: Parent-child relationships maintained**
- **Given**: Client → Server A → Server B chain
- **When**: Request flows through all services
- **Then**: Server A span has Client span as parent, Server B span has Server A span as parent

## Implementation Checklist

- [ ] Client interceptor injects traceparent into metadata
- [ ] Server interceptor extracts traceparent from metadata
- [ ] traceparent format validation (version, lengths, non-zero)
- [ ] Graceful handling of missing traceparent (create root span)
- [ ] Graceful handling of invalid traceparent (log + create root span)
- [ ] Integration test: client→server propagation
- [ ] Integration test: client→server→server multi-hop propagation
- [ ] Integration test: trace ID consistency across hops
- [ ] Integration test: parent-child relationships correct in exported traces

## Error Handling

| Error Condition | Action | Rationale |
|----------------|--------|-----------|
| Missing traceparent | Create new root span | First entry point in trace |
| Invalid format (wrong delimiters) | Log warning, create root span | Cannot trust corrupted context |
| Invalid version (!= "00") | Log warning, create root span | Unsupported version |
| Invalid trace_id (wrong length) | Log warning, create root span | Cannot propagate invalid ID |
| Invalid span_id (wrong length) | Log warning, create root span | Cannot set invalid parent |
| Zero trace_id | Log warning, create root span | Violates W3C spec |
| Zero span_id | Log warning, create root span | Violates W3C spec |
| Metadata injection failure | Log error, continue without tracing | Tracing failure should not block request |
| Metadata extraction failure | Log error, create root span | Continue with best effort |

## OpenTelemetry SDK Usage

The OpenTelemetry C++ SDK provides built-in propagators for W3C Trace Context:

```cpp
#include <opentelemetry/trace/propagation/http_trace_context.h>

// Create propagator (reuse single instance)
auto propagator = opentelemetry::trace::propagation::HttpTraceContext();

// Inject (client-side)
propagator.Inject(carrier, context);

// Extract (server-side)
auto context = propagator.Extract(carrier, context);
```

**Carrier Adapter**: Custom gRPC metadata carrier must implement:
- `Set(key, value)` for injection
- `Get(key)` for extraction

## References

- W3C Trace Context Specification: https://www.w3.org/TR/trace-context/
- OpenTelemetry Propagators API: https://opentelemetry.io/docs/specs/otel/context/api-propagators/
- gRPC Metadata: https://grpc.io/docs/guides/metadata/
