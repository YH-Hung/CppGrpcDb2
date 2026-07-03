# LB Facade Design — High-Level FailoverClient

Date: 2026-07-03
Status: Approved, implementing

## Problem

`src/lb/` implements adaptive, application-level client-side load balancing
across multiple FQDN endpoints. The pieces (`endpoint_config`, `channel_factory`,
`EndpointManager`, `CallWithFailover`) are correct and well-tested, but every
consumer must wire them together by hand. The current demo
(`src/greeter_failover_client.cpp`) performs six ceremonial steps that leak
internal details:

1. Manually log `LbConfig.rejected` entries and the endpoint list.
2. Compute `ChannelFactoryOptions{endpoints.size() > 1}` — the
   retry-amplification rule is an `lb` internal that the caller shouldn't need
   to know.
3. Build channels, then iterate to build stubs.
4. Construct `EndpointManager::Options{cooldown_base, 30000ms}` — the 30000ms
   cap is a magic literal every caller duplicates.
5. Forward `config.max_attempts` into `FailoverOptions` manually.
6. Write an index-based RPC lambda (`stubs[index]->SayHello(...)`) and capture a
   `served_by` index, then map it back to `endpoints[index].Target()` for
   logging.

The index-based `CallWithFailover` contract in particular forces callers to
hold a parallel `stubs` vector, do their own index→stub lookup, and resolve the
serving endpoint target themselves.

## Goal

Provide a higher-level facade that removes the wiring ceremony and the
index-based RPC contract, while leaving the existing low-level types intact for
tests and power users. This is an **additive** change — no broad refactor, per
AGENTS.md.

## Non-goals

- No change to `EndpointManager` selection/cooldown semantics.
- No change to `CallWithFailover` loop semantics — the facade delegates to it.
- No change to env-variable parsing or the service-config JSON shape.
- No TLS, callback/async adapter, active health checks, or Prometheus client
  metrics — those remain out of scope, same as v1.

## Architecture

Two new files, no new library target (added to existing `grpc_client_lb`):

| File | Contents | Depends on |
|---|---|---|
| `src/lb/failover_client.h` | class template `FailoverClient<StubT>`, `struct CallResult`, `StubFactory` alias, `FromEnv()` statics, `Call()` template (lambda + pointer-to-member overload). Header-only template, like `failover_call.h`. | `endpoint_config.h`, `channel_factory.h`, `endpoint_manager.h`, `failover_call.h`, grpcpp |
| `src/lb/failover_client.cpp` | non-template `FailoverClientBase` (internal detail, anonymous-namespace or file-local) owning the config→channels→manager wiring, the rejected/endpoint logging, the `multi_endpoint` inference, and the `FailoverOptions.max_attempts` passthrough. | `endpoint_config`, `channel_factory`, `endpoint_manager`, spdlog |

The facade reuses `CallWithFailover` rather than reimplementing the failover
loop. It translates the typed caller lambda into the index-based lambda
`CallWithFailover` expects, and resolves `served_by` from
`endpoints_[index].Target()` itself.

One small existing-file edit: add a named public default constant
`kDefaultCooldownCap{30000}` to `endpoint_manager.h` so the facade (and any
future caller) shares one source of truth instead of repeating the `30000`
literal. This is distinct from `channel_factory`'s service-config `maxBackoff =
1s`, which bounds gRPC's in-channel retry backoff — the two are different
numbers serving different purposes and are not unified.

## Public API

```cpp
namespace lb {

struct CallResult {
    grpc::Status status;
    std::string served_by;  // endpoint target that handled the call; empty on total failure
};

// Non-template base owning channels, the EndpointManager (held via unique_ptr
// because EndpointManager contains a mutex and is therefore immovable), and
// the pre-wired FailoverOptions. The template subclass adds typed stubs.
class FailoverClientBase {
public:
    FailoverClientBase(LbConfig config, ChannelBuilder channel_builder);
    // movable (manager_ is unique_ptr), non-copyable.
    template <typename Fn>
    CallResult CallIndexed(Fn&& rpc);  // runs CallWithFailover, resolves served_by
    std::size_t endpoint_count() const;
    const std::vector<Endpoint>& endpoints() const;
    EndpointManager::Snapshot snapshot(std::size_t index) const;
    const FailoverOptions& failover_options() const;
protected:
    std::vector<Endpoint> endpoints_;
    std::vector<std::shared_ptr<grpc::Channel>> channels_;
    std::unique_ptr<EndpointManager> manager_;
    FailoverOptions failover_options_;
};

// ServiceT is a gRPC generated service type (e.g. helloworld::Greeter) exposing
// static NewStub(std::shared_ptr<grpc::ChannelInterface>) and a nested Stub
// class — the universal gRPC convention. The stub type is derived as
// typename ServiceT::Stub.
template <typename ServiceT>
class FailoverClient : public FailoverClientBase {
public:
    using StubT = typename ServiceT::Stub;
    using StubFactory =
        std::function<std::unique_ptr<StubT>(std::shared_ptr<grpc::Channel>)>;

    // Default: env config, InsecureChannelCredentials, ServiceT::NewStub.
    static FailoverClient FromEnv();
    // Same, with custom stub factory and/or channel builder (TLS, fakes).
    static FailoverClient FromEnv(const StubFactory& stub_factory,
                                  ChannelBuilder channel_builder = nullptr);
    // Explicit config for tests / fixed setups.
    FailoverClient(LbConfig config, const StubFactory& stub_factory,
                   ChannelBuilder channel_builder = nullptr);

    // Lambda form: rpc(stub, context, request, response) -> grpc::Status.
    // Works for unary sync RPCs and for streams consumed entirely inside the
    // lambda. To return a live stream to the caller, use EndpointManager +
    // stubs() directly.
    template <typename Req, typename Resp, typename Fn>
    CallResult Call(const Req& request, Resp& response, Fn&& rpc);

    // Pointer-to-member convenience for unary sync RPCs. The unary stub method
    // signature is Status(ClientContext*, const Req&, Resp*).
    template <typename Req, typename Resp>
    CallResult Call(const Req& request, Resp& response,
                    grpc::Status (StubT::*method)(grpc::ClientContext*,
                                                  const Req&, Resp*));

    const std::vector<std::unique_ptr<StubT>>& stubs() const;

private:
    std::vector<std::unique_ptr<StubT>> stubs_;
};

}  // namespace lb
```

### Implementation notes

- The template parameter is the **service** type (`Greeter`), not the stub
  type, because gRPC's `NewStub` is a static on the service class, not on
  `Stub`. The stub type is derived as `typename ServiceT::Stub`. The demo
  becomes `FailoverClient<Greeter>::FromEnv()`.
- `FailoverClientBase` holds the `EndpointManager` via `unique_ptr` because
  `EndpointManager` contains a `std::mutex` and is therefore neither copyable
  nor movable. This keeps the facade movable so `FromEnv()` can return by
  value. `CallIndexed` dereferences `*manager_` when calling
  `CallWithFailover`.
- The unary pointer-to-member signature matches gRPC's actual generated stub
  methods: `Status(ClientContext*, const Req&, Resp*)` — request by const
  reference, response by pointer. The lambda form (`rpc(*stubs_[index], ctx,
  request, response)`) dereferences the `unique_ptr<StubT>`.

### `Call()` implementation

```cpp
template <typename Req, typename Resp, typename Fn>
CallResult Call(const Req& request, Resp& response, Fn&& rpc) {
    std::size_t served_index = 0;
    grpc::Status status = CallWithFailover(
        manager_, failover_options_,
        [&](grpc::ClientContext& ctx, std::size_t index) {
            served_index = index;
            return rpc(stubs_[index], ctx, request, response);
        });
    return {std::move(status),
            status.ok() ? endpoints_[served_index].Target() : std::string{}};
}
```

The pointer-to-member overload is a one-line inline wrapper that builds a
forwarding lambda and calls the lambda form.

`served_by` is empty on total failure so callers can't accidentally log a
stale target. `served_index` is always assigned because `CallWithFailover`
tries at least one endpoint (the `attempt_cap` is `min(max_attempts, endpoint_count)`
and `EndpointManager` rejects 0 endpoints, so `attempt_cap >= 1`).

## Internal wiring (`FailoverClientBase`)

Construction performs, in order, the six steps currently scattered across
`greeter_failover_client.cpp`:

1. Log each rejected entry at `warn` and each accepted endpoint at `info`.
2. `ChannelFactoryOptions{endpoints.size() > 1}` — computed internally.
3. `BuildChannels(endpoints, factory_options, channel_builder)`.
4. `EndpointManager::Options{cooldown_base, kDefaultCooldownCap}`.
5. `failover_options_.max_attempts = config.max_attempts`; `attempt_timeout`
   and `retriable_codes` keep `FailoverOptions` defaults.
6. Build stubs via the `StubFactory` (default `StubT::NewStub`).

`FromEnv()` overloads call `LoadLbConfigFromEnv()` then delegate to the
explicit constructor. The default `StubFactory` is defined in the template
header since it names `StubT`.

## Demo migration

`src/greeter_failover_client.cpp` shrinks from 66 lines to ~15:

```cpp
int main(int argc, char** argv) {
    lb::FailoverClient<Greeter> client =
        lb::FailoverClient<Greeter>::FromEnv();

    HelloRequest request;
    request.set_name("賴柔瑤");
    HelloReply reply;

    const lb::CallResult result =
        client.Call(request, reply, &Greeter::Stub::SayHello);

    if (result.status.ok()) {
        std::cout << "Greeter received: " << reply.message()
                  << " (via " << result.served_by << ")" << std::endl;
        return 0;
    }
    std::cout << result.status.error_code() << ": "
              << result.status.error_message() << std::endl;
    return 1;
}
```

Gone: the 4 `lb/*.h` includes (one `lb/failover_client.h` instead),
`LbConfig` plumbing, the rejected/endpoint logging loops, `ChannelFactoryOptions`
/`BuildChannels`, the stub-construction loop, the `EndpointManager::Options`
`30000` literal, `FailoverOptions` setup, the index-capturing lambda, and the
`served_by` int→target mapping.

## CMake

- Add `src/lb/failover_client.cpp` to the `grpc_client_lb` `add_library` source
  list (CMakeLists.txt:206-209).
- Add `lb_failover_client_tests` mirroring `lb_failover_call_tests`
  (CMakeLists.txt:444-451), linking `grpc_client_lb` + GTest.
- No rename of `grpc_client_lb` (AGENTS.md). Existing low-level test targets
  all stay.

## Tests

New `tests/lb/test_failover_client.cpp`, focused GTest:

- `FromEnv()` happy path with a fake `StubFactory` + fake `ChannelBuilder`
  (no real channels created).
- `Call()` populates `served_by` with the winning endpoint target on success.
- `Call()` leaves `served_by` empty on total failure (all endpoints
  `UNAVAILABLE`).
- Pointer-to-member overload delegates correctly to the lambda form.
- Attempt cap honored (`max_attempts = 1` → exactly one attempt).
- Non-retriable status returns immediately without failover.

The fake stub factory returns stubs whose methods are overridden by the test
via the lambda form (the pointer-to-member form requires a real `StubT`, so
that case uses a hand-rolled mini-stub class with a matching method signature).

## README

Rewrite the `## Client-side failover load balancing` section
(Readme.md:352-370) from a 19-line terse block into:

- A one-paragraph intro stating user-observable behavior.
- A three-row environment-variable table (`Variable` / `Default` / `Meaning`)
  where `Meaning` describes what the user sees, not internals. `COOLDOWN_BASE_MS`
  gets the concrete `1s → 2s → 4s … 30s` sequence and the reset-on-success
  behavior; `MAX_ATTEMPTS` gets the clamp rule and the `=1` disable use case.
- A "one vs. several endpoints" subsection explaining the retry-budget tradeoff
  in terms of attempts and latency, not the opaque `maxAttempts 2` token.
- A new "Using the library from your own client" code block showing facade
  usage — the actual point of this work — with a one-sentence pointer to the
  low-level escape hatch and the design doc.

## Verification

```bash
cmake --build build --target grpc_client_lb greeter_failover_client lb_failover_client_tests
ctest --test-dir build --output-on-failure \
  -R 'lb_failover_client_tests|lb_failover_call_tests|lb_endpoint|lb_channel_factory'
```

## Out of scope

Same as v1: TLS credentials, callback/async client adapters, active health
checking (gRPC health protocol), Prometheus client-side metrics,
priority/sticky endpoint selection mode.
