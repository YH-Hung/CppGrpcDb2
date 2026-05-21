# OpenTelemetry Adoption Plan for CppGrpcDb2

This document proposes a concrete, phased plan to adopt
[OpenTelemetry C++](https://github.com/open-telemetry/opentelemetry-cpp) for
distributed tracing across **every gRPC server and client** in this repository,
and to **replace the locally generated request id** (the UUID logged as
`[ReqID: ...]`) with the **OpenTelemetry trace id**.

The goal is end-to-end, standards-based context propagation:

- Every inbound RPC on a server starts (or continues) a trace span.
- Every outbound RPC from a client starts a child span and injects the W3C
  `traceparent` metadata so the callee can continue the same trace.
- Log lines carry the active `trace_id` / `span_id` instead of an ad hoc UUID,
  so logs and traces correlate in any OTLP-compatible backend.

---

## 1. Current State (Baseline)

### 1.1 gRPC servers

| Target | Style | Logging today | Request id today |
|--------|-------|---------------|------------------|
| `greeter_server` | sync `Greeter::Service` | iostream / none in business logic | none |
| `greeter_callback_server` | callback API | `spdlog` (`"Received request for name: ..."`) | none |
| `greeter_callback_server_no_db2` | callback API | `spdlog` | none |
| `complex_proto_async` | async `CallData` state machine | `spdlog` with `[ReqID: {uuid}]` | **UUID v4** generated per call |

The only place a request id exists today is the async path:

- `src/call_data/CallData.h`
  - `GenerateUuid()` builds a UUID v4.
  - `CallDataBase::OnRequestReceived()` sets `request_id_ = GenerateUuid();`
    and logs `"[CallData] [ReqID: {}] Request message (JSON): {}"`.
  - `CallDataBase::OnRequestProcessed()` logs the reply with the same
    `request_id_`.
  - The id is stored in the member `std::string request_id_`.

### 1.2 gRPC clients

| Target | Style |
|--------|-------|
| `greeter_client` | sync unary |
| `greeter_async_client` | async / completion queue |
| `greeter_callback_client` | callback API |
| `greeter_girl_client` | unary against `hello_girl` service |

Clients today do not propagate any correlation id.

### 1.3 Cross-cutting infrastructure

- **Metrics interceptor**: `src/interceptor/metrics_interceptor.cpp` +
  `include/metrics_interceptor.h` already demonstrate the server interceptor
  pattern (`grpc::experimental::Interceptor` /
  `ServerInterceptorFactoryInterface`) and are wired in
  `greeter_server.cpp` via `builder.experimental().SetInterceptorCreators(...)`.
  This is the natural extension point for a tracing interceptor.
- **Prometheus**: `prometheus::Exposer` + `prometheus::Registry` are created in
  each server (`127.0.0.1:8124` for sync/callback, `127.0.0.1:8125` for async).
  OpenTelemetry tracing is complementary to these metrics and does not replace
  them.
- **CMake dependencies** are resolved by small modules under `cmake/`
  (e.g. `prometheus.cmake`, `grpc.cmake`, `spdlog.cmake`) using
  `find_package(<pkg> CONFIG REQUIRED)`, and linked through helper functions in
  `cmake/common.cmake` (`create_grpc_executable`, `add_interceptor_support`,
  `create_test_executable`).

---

## 2. Target Architecture

```
                 ┌─────────────────────────────────────────────┐
                 │            OpenTelemetry SDK                  │
                 │  TracerProvider → Processor → OTLP Exporter   │
                 └─────────────────────────────────────────────┘
                        ▲                              ▲
        server spans    │                              │   client spans
                        │                              │
   ┌────────────────────┴───────┐        ┌─────────────┴───────────────┐
   │  Server tracing interceptor │        │  Client tracing interceptor  │
   │  - extract traceparent      │        │  - start span                │
   │  - start server span        │        │  - inject traceparent        │
   │  - put trace_id in logs      │        │  - put trace_id in logs      │
   └─────────────────────────────┘        └──────────────────────────────┘
```

Key decisions:

1. **Instrument at the interceptor layer**, not in each service method, so all
   four servers and all four clients get tracing without touching business
   logic. This mirrors the existing `metrics_interceptor` pattern.
2. **W3C Trace Context** (`traceparent` / `tracestate`) is the propagation
   format, carried as gRPC metadata.
3. A single shared **tracing bootstrap library** (new `otel_tracing` target)
   initializes the `TracerProvider` and the OTLP exporter once per process and
   exposes helpers to create the server/client interceptors and to fetch the
   current `trace_id` for logging.
4. **`trace_id` replaces the UUID `request_id_`** in all log lines. The UUID
   generator is removed once nothing depends on it.

---

## 3. Dependency Plan

### 3.1 Add OpenTelemetry C++

- Pin a known-good release of `opentelemetry-cpp` (e.g. `v1.16.x`; confirm the
  latest stable at integration time). It requires C++14+ (the project is C++20,
  so this is satisfied) and CMake ≥ 3.14.
- Required components:
  - `opentelemetry-cpp::api`
  - `opentelemetry-cpp::sdk` (`trace` SDK)
  - An exporter: start with `opentelemetry-cpp::otlp_grpc_exporter`
    (reuses the gRPC stack already present) and optionally
    `opentelemetry-cpp::ostream_span_exporter` for local debugging.
- Build `opentelemetry-cpp` with `WITH_OTLP_GRPC=ON`. Note its dependency on
  `nlohmann_json` and (for OTLP) on protobuf/gRPC, which are already available.

### 3.2 New CMake module

Add `cmake/opentelemetry.cmake` mirroring `cmake/prometheus.cmake`:

```cmake
# Find opentelemetry-cpp via CMake config package.
# Provides imported targets such as:
#   opentelemetry-cpp::api, opentelemetry-cpp::sdk,
#   opentelemetry-cpp::otlp_grpc_exporter
find_package(opentelemetry-cpp CONFIG REQUIRED)
message(STATUS "Using opentelemetry-cpp")
```

Include it from the top-level `CMakeLists.txt` next to the other `include(cmake/*.cmake)` calls.

If the build host has restricted network access (as documented for
`prometheus-cpp` in `doc/prometheus-cpp-guide.md`), document a from-source build
of `opentelemetry-cpp` and its submodules in a companion note and point
`CMAKE_PREFIX_PATH` at the install location.

### 3.3 Linking helpers

Extend `cmake/common.cmake`:

- Add an `add_tracing_support(target_name)` helper that links the new
  `otel_tracing` library (and transitively the OTel imported targets), keeping
  per-target `target_link_libraries` blocks consistent with the existing
  `add_db2_support` / `add_interceptor_support` helpers.
- Apply `add_tracing_support(...)` to every server and client target.

---

## 4. New Components to Build

### 4.1 `otel_tracing` library (new target)

Files (proposed):

- `include/otel_tracing.h`
- `src/tracing/otel_tracing.cpp`

Responsibilities:

- `InitTracing(const TracingOptions&)`: build a `TracerProvider` with a
  `BatchSpanProcessor` + OTLP gRPC exporter; set the global provider and a
  global W3C `TraceContext` propagator. Options come from environment variables
  (`OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_SERVICE_NAME`, `OTEL_TRACES_SAMPLER`,
  etc.) with sane defaults.
- `ShutdownTracing()`: flush and shut down the provider on process exit.
- `GetTracer(name)`: thin accessor.
- `CurrentTraceIdHex()` / `CurrentSpanIdHex()`: read the active span from the
  current `context` and return lower-hex ids for logging (empty string when no
  span is active).

This library is the single place that depends on the OTel SDK headers; servers
and clients depend only on its narrow header.

### 4.2 Server tracing interceptor (new target / inside `otel_tracing`)

Modeled on `MetricsServerInterceptorFactory`
(`include/metrics_interceptor.h`, `src/interceptor/metrics_interceptor.cpp`):

- `TracingServerInterceptorFactory : ServerInterceptorFactoryInterface`.
- `TracingServerInterceptor : grpc::experimental::Interceptor`:
  - On `POST_RECV_INITIAL_METADATA`: build a carrier from
    `ServerRpcInfo` client metadata, **extract** the parent context with the
    W3C propagator, then **start a server span** named after the method
    (`info->method()`), kind `SERVER`. Store the span/scope on the interceptor.
  - On `PRE_SEND_STATUS`: set span status from the gRPC status code and `End()`
    the span.
- Register it alongside the existing metrics factory in the
  `SetInterceptorCreators(...)` block of each server (sync, both callback
  servers). The async server registers it on its `ServerBuilder` too.

### 4.3 Client tracing interceptor (new target / inside `otel_tracing`)

- `TracingClientInterceptorFactory : ClientInterceptorFactoryInterface`.
- `TracingClientInterceptor`:
  - On `PRE_SEND_INITIAL_METADATA`: start a client span (kind `CLIENT`) named
    after the method, then **inject** the W3C `traceparent` into the outgoing
    metadata via `methods->GetSendInitialMetadata()`.
  - On `POST_RECV_STATUS`: set status and `End()` the span.
- Attach via `grpc::experimental::CreateCustomChannelWithInterceptors(...)` (or
  the channel-args interceptor API) in all four clients.

---

## 5. Replacing the Request Id With the Trace Id

This is the explicit requirement: where a server/client already generates and
logs a request id, switch to the OpenTelemetry trace id.

### 5.1 Async `CallData` path (the only existing request id)

In `src/call_data/CallData.h`:

1. Remove the dependency on the locally generated UUID:
   - Delete or stop calling `GenerateUuid()`.
   - Replace the member `std::string request_id_;` semantics so it holds the
     active **trace id** (hex) for the call, or rename it to `trace_id_` for
     clarity.
2. In `OnRequestReceived()`:
   - Replace `request_id_ = GenerateUuid();` with
     `request_id_ = otel::CurrentTraceIdHex();` (populated by the server tracing
     interceptor, which has already started the span by the time the CallData
     processes the request).
   - Keep the existing log lines but relabel `[ReqID: {}]` as
     `[trace_id: {}]` (or `[ReqID: {}]` populated with the trace id if log
     parsers must stay stable — decide explicitly and document the choice).
3. In `OnRequestProcessed()`: same relabel, same `request_id_`/`trace_id_`
   value.
4. Once nothing references `GenerateUuid()`, remove it (and confirm
   `SanitizeUuid` in `src/util/string_util.*` is still used elsewhere before
   touching it — it is a separate utility and should be left as is).

### 5.2 Servers/clients that log without an id today

For `greeter_callback_server`, `greeter_callback_server_no_db2`,
`greeter_server`, and the clients, **add** the active trace id to log lines so
all logs become trace-correlated. Recommended approaches, in order of
preference:

1. Configure the `spdlog` pattern to include a custom field and inject the
   trace id via a per-call log context, or
2. Prefix existing messages with `[trace_id: {}]` using
   `otel::CurrentTraceIdHex()` read inside the RPC handler scope.

### 5.3 Log/trace correlation contract

Document the final field name once (recommended `trace_id`, lower-hex, 32
chars) and use it consistently across all targets so a single PromQL/LogQL or
backend query can pivot between logs, metrics, and traces.

---

## 6. Configuration

All tracing configuration is environment-driven (no code changes to retarget a
backend):

| Variable | Purpose | Default |
|----------|---------|---------|
| `OTEL_SERVICE_NAME` | Resource `service.name` per binary | target name |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | OTLP collector endpoint | `http://localhost:4317` |
| `OTEL_TRACES_SAMPLER` | Sampler (e.g. `parentbased_traceidratio`) | parent-based always-on |
| `OTEL_TRACES_SAMPLER_ARG` | Sampler ratio | `1.0` |
| `OTEL_TRACING_ENABLED` | Project switch to disable tracing entirely | `true` |

Each `main()` calls `otel::InitTracing(...)` early and `otel::ShutdownTracing()`
before exit. When disabled, interceptors become no-ops and logging falls back to
empty trace ids.

---

## 7. Phased Rollout

1. **Phase 0 — Dependency & scaffolding**
   - Add `cmake/opentelemetry.cmake`, `otel_tracing` library, and the
     `add_tracing_support` helper. Build green with the library linked but
     unused.
2. **Phase 1 — Server tracing**
   - Implement `TracingServerInterceptor[Factory]`; register it in
     `greeter_server`, both callback servers, and `complex_proto_async`.
   - Verify spans appear in the collector / ostream exporter.
3. **Phase 2 — Request id → trace id**
   - Switch `CallData` logging from UUID to trace id; relabel logs; remove
     `GenerateUuid()`.
   - Add trace id to the other servers' log lines.
4. **Phase 3 — Client tracing & propagation**
   - Implement `TracingClientInterceptor[Factory]`; attach to all four clients;
     confirm `traceparent` propagation produces a single connected trace from
     client → server.
5. **Phase 4 — Hardening**
   - Sampling, batch processor tuning, graceful shutdown, error/status mapping,
     and disabled-mode fallbacks.

---

## 8. Testing Strategy

Follow the repository's focused GTest conventions (narrow targets under
`tests/`):

- `tests/tracing/` (new):
  - **Propagation round-trip**: inject the W3C context on a synthetic client
    carrier, extract it on a synthetic server carrier, assert the same
    `trace_id` and correct parent/child relationship.
  - **Trace id helper**: assert `CurrentTraceIdHex()` returns a 32-char lower
    hex when a span is active and empty otherwise.
  - **In-memory span exporter**: use the OTel in-memory exporter to assert a
    server interceptor produces one span per RPC with the expected name and
    status mapping (mirrors how metrics tests inspect
    `prometheus::Registry::Collect()`).
- Keep tracing tests independent of any live OTLP backend (use the in-memory or
  ostream exporter), analogous to how DB2 tests are gated behind
  `BUILD_DB2_TESTS`.
- Optionally add `BUILD_OTEL_TESTS` if any test needs a collector.

---

## 9. Acceptance Criteria

- All four servers and all four clients link `otel_tracing` and emit spans.
- A client call produces a trace whose server span is a child of the client
  span (verified `traceparent` propagation).
- No code path generates or logs the old UUID request id; log lines carry
  `trace_id` instead, with `GenerateUuid()` removed.
- Existing Prometheus metrics endpoints and behavior are unchanged.
- `cmake --build build` and `ctest --test-dir build` pass, including the new
  `tests/tracing/` targets.

---

## 10. Risks & Notes

- **Build footprint**: `opentelemetry-cpp` + OTLP gRPC exporter pulls in
  protobuf/gRPC (already present) and `nlohmann_json`. On restricted networks,
  plan a from-source build like the existing `prometheus-cpp` guide.
- **Interceptor ordering**: ensure the tracing interceptor runs early enough to
  set context before metrics/business logic; register it first in the
  interceptor vector.
- **Async context**: in `complex_proto_async`, the span is started by the server
  interceptor; `CallData` only reads the current trace id. If a worker thread
  handles processing off the gRPC thread, propagate the `context::Context`
  explicitly to keep the trace id correct.
- **Keep changes targeted**: per `AGENTS.md`, do not rename existing targets
  (e.g. `calldata_metrics`, `metrics_interceptor`) or perform broad refactors
  while adding tracing.
