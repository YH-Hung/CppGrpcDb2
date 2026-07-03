# Client-Side gRPC Failover Load Balancing — Design

Date: 2026-07-02
Status: Draft, pending review

## Goal

Robust, adaptive client-side load balancing across multiple **FQDN** endpoints for the
gRPC C++ clients in this repo:

- Endpoints are configured via an environment variable.
- When the current endpoint is unavailable, the client retries the RPC on the next endpoint.
- When only a single endpoint is configured, the client falls back entirely to gRPC's
  built-in channel load balancing (`round_robin` over DNS-resolved addresses) and built-in
  retry mechanism (service-config `retryPolicy`), with no app-level retry loop.

## Why an app-level layer

One gRPC channel resolves exactly one target name. The built-in `round_robin` policy
balances across the many IPs a *single* FQDN resolves to, but there is no stable public
C++ API to feed *multiple distinct FQDNs* into one channel:

- `ipv4:addr1,addr2` multi-address targets accept literal IPs only.
- The custom-resolver API lives in gRPC's internal `grpc_core` namespace (unstable).

### Approaches considered

1. **App-level channel pool with failover (chosen).** One channel per FQDN plus a small
   adaptive manager that picks a channel per RPC and rotates away from failing endpoints.
   Public API only, unit-testable, and the single-endpoint case degenerates naturally into
   the required fallback.
2. **Custom resolver plugin** returning the union of all FQDNs' addresses, letting built-in
   `round_robin` + retries do everything. Most "native", but requires gRPC-internal headers
   and is hard to test. Rejected.
3. **Out-of-client:** one DNS name with multiple A records, or Envoy/xDS. Little or no
   code, but needs infrastructure control and is not client-side. Rejected here; worth
   remembering for production.

## Architecture

New unit `src/lb/`, built as a compiled static library `grpc_client_lb` (same pattern as
the interceptor libraries in the root `CMakeLists.txt`). Only the RPC wrapper template
stays header-only, because it must be.

| File | Contents | Depends on |
|---|---|---|
| `src/lb/endpoint_config.h` / `.cpp` | `Endpoint` struct, `ParseEndpoints()`, `LoadLbConfigFromEnv()` | nothing (pure) |
| `src/lb/endpoint_manager.h` / `.cpp` | `EndpointManager`: selection + cooldown state machine | nothing (clock injected) |
| `src/lb/channel_factory.h` / `.cpp` | builds one channel per endpoint with shared `ChannelArguments` | grpcpp, jsoncpp, spdlog |
| `src/lb/failover_call.h` | `CallWithFailover()` function template (header-only by necessity) | grpcpp, `EndpointManager` |

### 1. Configuration (`endpoint_config`)

Environment contract:

```
GRPC_TARGET_ENDPOINTS="svc-a.corp.example.com:50051, svc-b.corp.example.com:50051"
GRPC_LB_COOLDOWN_BASE_MS=1000        # optional, default 1000
GRPC_LB_MAX_ATTEMPTS=<n>             # optional, default = endpoint count
GRPC_LB_ATTEMPT_TIMEOUT_MS=2000      # optional, default 2000 (per-attempt deadline)
```

- `ParseEndpoints(std::string_view)` → `std::vector<Endpoint>`: splits on commas, trims
  whitespace, rejects empty or malformed entries (missing/non-numeric port). Every entry
  must be `host:port`; there is no implicit default port.
- Unset or empty `GRPC_TARGET_ENDPOINTS` → single endpoint `localhost:50051`
  (current demo behavior).
- Parsing is pure and does no logging: `ParseEndpoints` returns the accepted endpoints
  plus the list of rejected raw entries, and the caller (demo binary / channel factory)
  logs a spdlog warning per rejected entry. If all entries are rejected, fall back to the
  default as above.

### 2. Adaptive endpoint selection (`EndpointManager`)

Per-endpoint state: consecutive-failure count, `unavailable_until` steady-clock timestamp,
cumulative success/failure counters (for logging).

- **Selection:** round-robin cursor over endpoints *not in cooldown*. State is
  mutex-protected; RPCs may arrive from many threads.
- **Adaptive backoff:** `ReportFailure(i)` sets cooldown to
  `cooldown_base × 2^(consecutive_failures − 1)`, capped at 30 s.
  `ReportSuccess(i)` resets the failure count. Endpoints rejoin automatically when their
  cooldown lapses — no manual reset.
- **Never give up:** if *all* endpoints are in cooldown, return the one whose cooldown
  expires soonest rather than failing the RPC outright.
- The clock is injected (`std::function<TimePoint()>`, defaults to
  `std::chrono::steady_clock::now`), so cooldown logic is unit-testable without sleeping.
- No active health checking in v1; health is inferred from RPC outcomes (passive).

Selection policy is round-robin (true balancing). Priority mode (first-listed preferred,
others as failover) is a possible later extension, not in v1.

### 3. Channel construction (`channel_factory`)

One channel per FQDN, all sharing `ChannelArguments`:

- Service-config JSON with `retryPolicy` (same shape already demonstrated in
  `src/greeter_client.cpp`), built with jsoncpp. In multi-endpoint mode `maxAttempts` is
  lowered to 2 to bound worst-case latency — app-level attempts multiply with built-in
  attempts (retry amplification). In single-endpoint mode `maxAttempts` stays 4.
- `grpc.lb_policy_name = round_robin` so a single FQDN resolving to multiple A/AAAA
  records is still balanced by gRPC itself.
- Keepalive args (`GRPC_ARG_KEEPALIVE_TIME_MS = 10000`,
  `GRPC_ARG_KEEPALIVE_TIMEOUT_MS = 5000`,
  `GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS = 1`) so half-dead connections are detected in
  seconds instead of at the TCP timeout.
- The factory is injectable (`std::function<std::shared_ptr<Channel>(const Endpoint&,
  const ChannelArguments&)>`) so tests can supply fakes.
- Credentials: `InsecureChannelCredentials()` in this playground, isolated in the factory
  so swapping to TLS is a one-line change.

### 4. Failover loop (`CallWithFailover`)

```cpp
// Fn: (grpc::ClientContext&, int endpoint_index) -> grpc::Status
template <typename Fn>
grpc::Status CallWithFailover(EndpointManager& mgr, const FailoverOptions& opts, Fn rpc);
```

Per logical call:

1. Ask `EndpointManager` for an endpoint; skip any endpoint already tried this call.
2. Create a **fresh `grpc::ClientContext`** per attempt (contexts are single-use;
   reuse across retries is undefined behavior) with a per-attempt deadline
   (`opts.attempt_timeout`, default 2 s).
3. Invoke the RPC. On `OK` → `ReportSuccess`, return.
4. On a retriable status → `ReportFailure`, log the failover at `warn`, continue to the
   next endpoint. The retriable set is a `FailoverOptions` field defaulting to
   `{UNAVAILABLE}` — the code produced by connection-refused, endpoint-down, and
   DNS-resolution failure alike.
5. On any other status (`INVALID_ARGUMENT`, `PERMISSION_DENIED`, ...) → return
   immediately. Failing over on application errors would hammer every endpoint with a
   request that can never succeed.
6. Stop after `min(endpoint_count, GRPC_LB_MAX_ATTEMPTS)` attempts — at most one attempt
   per distinct endpoint per logical call; return the last status.

**Layering:** gRPC's built-in retry handles transient blips *within* an endpoint;
this loop handles *endpoint-level* failure by moving to the next FQDN. With one endpoint
configured the loop is a single attempt and everything is delegated to built-in LB +
retry — the required fallback, with no special-casing.

**Scope:** v1 wraps the sync stub API (the caller's lambda captures its stub). The
`EndpointManager` is API-agnostic; a callback/async adapter can be added later without
redesign.

## Demo binary

New `src/greeter_failover_client.cpp` (existing demos untouched, matching the repo's
one-binary-per-pattern style):

- Loads config from env, builds the channel pool, issues `SayHello` via
  `CallWithFailover`, prints the reply and which endpoint served it.
- Wired via `create_grpc_executable(greeter_failover_client ...)` +
  `target_link_libraries(greeter_failover_client PRIVATE grpc_client_lb)`.
  No DB2 support needed.

## CMake wiring

```cmake
add_library(grpc_client_lb
    src/lb/endpoint_config.cpp
    src/lb/endpoint_manager.cpp
    src/lb/channel_factory.cpp
)
target_include_directories(grpc_client_lb PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(grpc_client_lb
    PUBLIC ${_GRPC_GRPCPP}
    PUBLIC ${SPDLOG_TARGET}
    PRIVATE ${JSONCPP_TARGET}
)
target_compile_features(grpc_client_lb PUBLIC cxx_std_20)
```

Public headers stay next to the sources under `src/lb/` (implementation-scoped, like
`src/worker/WorkerPool.h`); `src/` is already on the include path globally.

## Observability

- spdlog: `warn` on failover and on an endpoint entering cooldown; `info` when an
  endpoint recovers (first success after failures).
- Prometheus client-side metrics (`grpc_client_failovers_total`, per-endpoint health
  gauge) fit this repo's theme but are deferred as follow-up work, not v1.

## Testing

GTest targets via `create_test_executable`, one per unit:

- `tests/lb/test_endpoint_config.cpp` — compiles `src/lb/endpoint_config.cpp` directly
  (repo convention: tests stay narrow; no gRPC dependency). Cases: single, many,
  whitespace, malformed entries, all-malformed → default, unset → default.
- `tests/lb/test_endpoint_manager.cpp` — compiles `src/lb/endpoint_manager.cpp` directly
  with a fake injected clock; no sleeps. Cases: rotation order, cooldown skip,
  exponential growth and 30 s cap, all-down → soonest-recovery pick, reset on success,
  concurrent selection smoke test.
- `tests/lb/test_failover_call.cpp` — links `grpc_client_lb` (needs `grpc::Status`).
  Scripted fake RPC lambdas verify: failover on `UNAVAILABLE`, stop on `OK`, no failover
  on `INVALID_ARGUMENT`, attempt cap respected, fresh context per attempt (counted),
  single-endpoint → exactly one attempt.
- Optional (deferred): integration test spinning two in-process `ServerBuilder` servers
  on localhost ports, stopping one, asserting the client lands on the survivor.

## Out of scope for v1

- Active health checking (gRPC health protocol) — passive failure detection only.
- Callback/async client adapters.
- Priority/sticky endpoint selection mode.
- Prometheus client metrics.
- TLS credentials (isolated in the factory for later).
