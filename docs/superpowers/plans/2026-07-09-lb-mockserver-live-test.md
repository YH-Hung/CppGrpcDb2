# MockServer + Envoy Live Failover Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive the real `greeter_failover_client` binary through six live failover scenarios against three MockServer gRPC containers and an Envoy round-robin proxy, orchestrated by docker compose and asserted by a shell script.

**Architecture:** Three `mockserver/mockserver:7.4.0` containers each mock `helloworld.Greeter/SayHello` with a distinct static reply naming the instance; an Envoy proxy fronts all three on one host port to exercise the client's single-endpoint path (app-level failover degenerates to one attempt, gRPC's built-in service-config retry does the recovery). A bash driver (`run_live_test.sh`) generates the proto descriptor, brings the stack up, runs the client with different `GRPC_TARGET_ENDPOINTS` values, stops/starts containers, and asserts on stdout lines and exit codes.

**Tech Stack:** MockServer 7.4.0 (native gRPC mocking), Envoy v1.31, docker compose v2+, bash 3.2-compatible script (macOS default), host `protoc`, existing C++20 `lb::FailoverClient`.

**Spec:** `docs/superpowers/specs/2026-07-09-lb-mockserver-live-test-design.md`

**Repo conventions that matter here:**
- Commit messages are lowercase imperative, matching recent history (e.g. `add greeter_failover_client demo`, `docs(lb): spec for FailoverClient facade`). End every commit body with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Generated proto artifacts (including descriptor sets) are never checked in — `gen/` is created at runtime and gitignored (Task 5).
- All commands below run from the repo root `/Users/yinghanhung/Projects/Polyglot_gRPC/CppGrpcDb2` unless stated otherwise.
- The client's stdout line formats are load-bearing test interfaces — do not restyle them:
  - success: `Greeter received: <message> (via <host:port>)`
  - failure: `<int status code>: <error message>`

**Known risk (validate in Task 4, Step 3):** In scenario 3 (rejoin), the background client probes mock1 while its container is restarting. Two windows could produce a *non-retriable* error and fail the run: (a) Docker Desktop's port proxy accepting TCP before MockServer binds (hang → `DEADLINE_EXCEEDED`, not retriable), (b) MockServer serving gRPC before its expectation file loads (no match → non-`UNAVAILABLE` status). If scenario 3 proves flaky across 3 consecutive runs, the approved fallback is: keep the `via 127.0.0.1:50051` rejoin assertion, drop the exit-0 assertion, and note the relaxation in both the script comment and the spec.

---

**Deviation log (2026-07-10, during execution):**
- **Task 2 reworked:** MockServer 7.4.0 does not re-encode JSON response
  bodies to protobuf and only emits gRPC trailers when the expectation
  defines them (verified with grpc-c++ and grpcurl over h2c and TLS; also on
  the snapshot image). Static `expectations/*.json` were replaced by
  `generate_artifacts.sh`, which writes `gen/expectations/mock{1,2,3}.json`
  with the reply pre-encoded as gRPC wire bytes (BINARY base64 body +
  `content-type: application/grpc` header + explicit `grpc-status: 0`
  trailer) next to the descriptor. Compose mounts point into `gen/`.
  Prerequisites now include `python3`. The spec's expectation section was
  amended accordingly.
- **Task 4 adjustment:** the lb library's spdlog diagnostics went to stdout
  and polluted the asserted output, so the demo client now routes its default
  spdlog logger to stderr (results-on-stdout is the script's contract); the
  script captures stderr to a scratch file for failure diagnostics and
  delegates descriptor generation to `generate_artifacts.sh`.
- **Environment fix (pre-Task 1 verification):** Homebrew abseil upgrade had
  orphaned re2/grpc (dyld failure for every gRPC binary); fixed via
  `brew upgrade re2 grpc` (grpc 1.81.1 → 1.82.1) and a full rebuild.

### Task 1: Extend `greeter_failover_client` with `[count] [delay_ms]` args

The round-robin cursor is in-process state starting at endpoint 0, so a fresh process always hits endpoint 0 first. Rotation and cooldown-rejoin are only observable when one process makes several sequential calls.

**Files:**
- Modify: `src/greeter_failover_client.cpp` (whole file, currently 33 lines)

- [ ] **Step 1: Replace the file with the extended version**

Write `src/greeter_failover_client.cpp` with exactly this content:

```cpp
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "helloworld.grpc.pb.h"
#include "lb/failover_client.h"

using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

namespace {

// Parses a decimal integer argument >= min_value. Returns false on trailing
// garbage, out-of-range, or below-minimum values.
bool ParseIntArg(const char* arg, int min_value, int& out) {
    try {
        std::size_t pos = 0;
        const int value = std::stoi(arg, &pos);
        if (pos != std::string(arg).size() || value < min_value) return false;
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

// usage: greeter_failover_client [count] [delay_ms]
//   count    number of sequential calls, >= 1 (default 1)
//   delay_ms sleep between calls in milliseconds, >= 0 (default 0)
// One FailoverClient serves all calls, so round-robin rotation and cooldown
// recovery are observable across the run. Exit 0 iff every call succeeded.
int main(int argc, char** argv) {
    int count = 1;
    int delay_ms = 0;
    if ((argc > 1 && !ParseIntArg(argv[1], 1, count)) ||
        (argc > 2 && !ParseIntArg(argv[2], 0, delay_ms)) || argc > 3) {
        std::cerr << "usage: " << argv[0] << " [count] [delay_ms]" << std::endl;
        return 2;
    }

    lb::FailoverClient<Greeter> client =
        lb::FailoverClient<Greeter>::FromEnv();

    bool all_ok = true;
    for (int i = 0; i < count; ++i) {
        if (i > 0 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        HelloRequest request;
        request.set_name("賴柔瑤");
        HelloReply reply;

        const lb::CallResult result =
            client.Call(request, reply, &Greeter::Stub::SayHello);

        if (result.status.ok()) {
            std::cout << "Greeter received: " << reply.message()
                      << " (via " << result.served_by << ")" << std::endl;
        } else {
            std::cout << result.status.error_code() << ": "
                      << result.status.error_message() << std::endl;
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
```

- [ ] **Step 2: Build the target**

Run: `cmake --build build --target greeter_failover_client`
Expected: compiles and links with no warnings introduced by this change.

- [ ] **Step 3: Verify arg validation (no server needed)**

Run: `./build/greeter_failover_client abc; echo "rc=$?"`
Expected: `usage: ./build/greeter_failover_client [count] [delay_ms]` on stderr, `rc=2`.

Run: `./build/greeter_failover_client 0; echo "rc=$?"`
Expected: usage line, `rc=2` (count must be >= 1).

Run: `./build/greeter_failover_client 1 2 3; echo "rc=$?"`
Expected: usage line, `rc=2` (too many args).

- [ ] **Step 4: Verify default and multi-call behavior against no server**

Run: `GRPC_TARGET_ENDPOINTS="127.0.0.1:59999" ./build/greeter_failover_client; echo "rc=$?"`
Expected: exactly one line starting with `14:` (UNAVAILABLE — nothing listens on 59999), `rc=1`. This confirms the no-arg path still makes exactly one call.

Run: `GRPC_TARGET_ENDPOINTS="127.0.0.1:59999" ./build/greeter_failover_client 3; echo "rc=$?"`
Expected: exactly three lines starting with `14:`, `rc=1`.

- [ ] **Step 5: Run the existing lb test suite (regression guard)**

Run: `ctest --test-dir build --output-on-failure -R "lb_"`
Expected: all lb tests pass (the client binary isn't under test, but this confirms the build is coherent).

- [ ] **Step 6: Commit**

```bash
git add src/greeter_failover_client.cpp
git commit -m "extend greeter_failover_client with count/delay args for live LB demos

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: MockServer compose stack + expectations, validated live

This task retires the spec's main external risk early: the MockServer 7.4.0 gRPC expectation format. Validate with one container before writing the full script.

**Files:**
- Create: `tests/lb/mockserver/expectations/mock1.json`
- Create: `tests/lb/mockserver/expectations/mock2.json`
- Create: `tests/lb/mockserver/expectations/mock3.json`
- Create: `tests/lb/mockserver/docker-compose.yml`

- [ ] **Step 1: Create the three expectation files**

`tests/lb/mockserver/expectations/mock1.json`:

```json
[
  {
    "httpRequest": {
      "method": "POST",
      "path": "/helloworld.Greeter/SayHello"
    },
    "httpResponse": {
      "statusCode": 200,
      "headers": { "grpc-status": ["0"] },
      "body": "{\"message\": \"Hello from mock1 @50051\"}"
    }
  }
]
```

`tests/lb/mockserver/expectations/mock2.json` — identical except the body reads `Hello from mock2 @50052`:

```json
[
  {
    "httpRequest": {
      "method": "POST",
      "path": "/helloworld.Greeter/SayHello"
    },
    "httpResponse": {
      "statusCode": 200,
      "headers": { "grpc-status": ["0"] },
      "body": "{\"message\": \"Hello from mock2 @50052\"}"
    }
  }
]
```

`tests/lb/mockserver/expectations/mock3.json` — body reads `Hello from mock3 @50053`:

```json
[
  {
    "httpRequest": {
      "method": "POST",
      "path": "/helloworld.Greeter/SayHello"
    },
    "httpResponse": {
      "statusCode": 200,
      "headers": { "grpc-status": ["0"] },
      "body": "{\"message\": \"Hello from mock3 @50053\"}"
    }
  }
]
```

No body matcher on the request: any `HelloRequest.name` (including the client's default non-ASCII name) matches. The message text is a **test interface** — Task 4's script asserts on `Hello from mockN @5005N` verbatim.

- [ ] **Step 2: Create `tests/lb/mockserver/docker-compose.yml`**

The `envoy` service is included now but only validated in Task 3. `gen/` is populated at runtime by `protoc` (Step 3) and gitignored in Task 5.

```yaml
name: lb-mockserver-live

services:
  mock1:
    image: mockserver/mockserver:7.4.0
    environment:
      MOCKSERVER_GRPC_DESCRIPTOR_DIRECTORY: /descriptors
      MOCKSERVER_INITIALIZATION_JSON_PATH: /expectations/init.json
    volumes:
      - ./gen:/descriptors:ro
      - ./expectations/mock1.json:/expectations/init.json:ro
    ports:
      - "50051:1080"

  mock2:
    image: mockserver/mockserver:7.4.0
    environment:
      MOCKSERVER_GRPC_DESCRIPTOR_DIRECTORY: /descriptors
      MOCKSERVER_INITIALIZATION_JSON_PATH: /expectations/init.json
    volumes:
      - ./gen:/descriptors:ro
      - ./expectations/mock2.json:/expectations/init.json:ro
    ports:
      - "50052:1080"

  mock3:
    image: mockserver/mockserver:7.4.0
    environment:
      MOCKSERVER_GRPC_DESCRIPTOR_DIRECTORY: /descriptors
      MOCKSERVER_INITIALIZATION_JSON_PATH: /expectations/init.json
    volumes:
      - ./gen:/descriptors:ro
      - ./expectations/mock3.json:/expectations/init.json:ro
    ports:
      - "50053:1080"

  envoy:
    image: envoyproxy/envoy:v1.31-latest
    volumes:
      - ./envoy.yaml:/etc/envoy/envoy.yaml:ro
    ports:
      - "50050:50050"
      - "127.0.0.1:9901:9901"
    depends_on:
      - mock1
      - mock2
      - mock3
```

- [ ] **Step 3: Generate the descriptor set**

```bash
mkdir -p tests/lb/mockserver/gen
protoc --descriptor_set_out=tests/lb/mockserver/gen/helloworld.dsc \
       --include_imports -Iprotos protos/helloworld.proto
```

Expected: `tests/lb/mockserver/gen/helloworld.dsc` exists and is non-empty (`ls -l` shows a few hundred bytes).

- [ ] **Step 4: Start mock1 only and wait for readiness**

Note: `envoy.yaml` doesn't exist yet, so start only `mock1` (compose validates mounts lazily per service):

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml up -d mock1
for i in $(seq 1 60); do
  curl -sf -X PUT http://127.0.0.1:50051/mockserver/status >/dev/null && echo READY && break
  sleep 0.5
done
```

Expected: `READY` within ~15 s (first run adds image pull time).

- [ ] **Step 5: Validate the gRPC expectation with the real client**

```bash
GRPC_TARGET_ENDPOINTS="127.0.0.1:50051" ./build/greeter_failover_client; echo "rc=$?"
```

Expected stdout, exactly:

```
Greeter received: Hello from mock1 @50051 (via 127.0.0.1:50051)
rc=0
```

If this fails, debug before proceeding — the whole design hangs on this format:
- `docker compose -f tests/lb/mockserver/docker-compose.yml logs mock1` — look for descriptor-load or expectation-parse errors.
- `curl -s -X PUT "http://127.0.0.1:50051/mockserver/retrieve?type=active_expectations"` — confirm the expectation registered.
- If MockServer rejects the expectation shape, consult https://www.mock-server.com/mock_server/grpc_mocking.html for the 7.4.0 format and adjust all three JSON files identically (keeping the `Hello from mockN @5005N` message text unchanged).

- [ ] **Step 6: Validate mock2 and mock3 the same way**

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml up -d mock2 mock3
for p in 50052 50053; do
  for i in $(seq 1 60); do
    curl -sf -X PUT "http://127.0.0.1:${p}/mockserver/status" >/dev/null && echo "READY ${p}" && break
    sleep 0.5
  done
done
GRPC_TARGET_ENDPOINTS="127.0.0.1:50052" ./build/greeter_failover_client
GRPC_TARGET_ENDPOINTS="127.0.0.1:50053" ./build/greeter_failover_client
```

Expected:

```
Greeter received: Hello from mock2 @50052 (via 127.0.0.1:50052)
Greeter received: Hello from mock3 @50053 (via 127.0.0.1:50053)
```

- [ ] **Step 7: Sanity-check rotation across all three (preview of scenario 1)**

```bash
GRPC_TARGET_ENDPOINTS="127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053" \
  ./build/greeter_failover_client 6; echo "rc=$?"
```

Expected: six lines rotating mock1 → mock2 → mock3 → mock1 → mock2 → mock3, `rc=0`.

- [ ] **Step 8: Tear down and commit**

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml down --remove-orphans
git add tests/lb/mockserver/docker-compose.yml tests/lb/mockserver/expectations/
git commit -m "add MockServer compose stack mocking Greeter for lb live testing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

Note: `gen/helloworld.dsc` stays untracked (gitignore lands in Task 5 — until then just don't `git add` it).

---

### Task 3: Envoy round-robin proxy config, validated live

**Files:**
- Create: `tests/lb/mockserver/envoy.yaml`

- [ ] **Step 1: Create `tests/lb/mockserver/envoy.yaml`**

Two deliberate choices, both required by the spec: **no active health checks and no Envoy-side retries** (a stopped backend must surface `UNAVAILABLE` to the client so its channel-level retry does the recovery), and a **5-minute `dns_refresh_rate`** (with `STRICT_DNS`, Docker DNS drops a stopped container's name; a long refresh keeps the dead endpoint in rotation for the duration of a test run instead of Envoy silently routing around it).

```yaml
admin:
  address:
    socket_address: { address: 0.0.0.0, port_value: 9901 }

static_resources:
  listeners:
    - name: grpc_listener
      address:
        socket_address: { address: 0.0.0.0, port_value: 50050 }
      filter_chains:
        - filters:
            - name: envoy.filters.network.http_connection_manager
              typed_config:
                "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
                stat_prefix: grpc_proxy
                codec_type: AUTO
                route_config:
                  name: local_route
                  virtual_hosts:
                    - name: greeter
                      domains: ["*"]
                      routes:
                        - match: { prefix: "/" }
                          route:
                            cluster: greeter_mocks
                            timeout: 15s
                http_filters:
                  - name: envoy.filters.http.router
                    typed_config:
                      "@type": type.googleapis.com/envoy.extensions.filters.http.router.v3.Router

  clusters:
    - name: greeter_mocks
      connect_timeout: 1s
      type: STRICT_DNS
      dns_refresh_rate: 300s
      lb_policy: ROUND_ROBIN
      typed_extension_protocol_options:
        envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
          "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
          explicit_http_config:
            http2_protocol_options: {}
      load_assignment:
        cluster_name: greeter_mocks
        endpoints:
          - lb_endpoints:
              - endpoint:
                  address:
                    socket_address: { address: mock1, port_value: 1080 }
              - endpoint:
                  address:
                    socket_address: { address: mock2, port_value: 1080 }
              - endpoint:
                  address:
                    socket_address: { address: mock3, port_value: 1080 }
```

- [ ] **Step 2: Start the full stack and wait for readiness**

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml up -d
for i in $(seq 1 60); do
  curl -sf http://127.0.0.1:9901/ready >/dev/null && echo "ENVOY READY" && break
  sleep 0.5
done
```

Expected: `ENVOY READY`. If envoy restarts in a loop, check config parse errors with `docker compose -f tests/lb/mockserver/docker-compose.yml logs envoy`.

- [ ] **Step 3: Validate proxy round-robin with the real client**

```bash
GRPC_TARGET_ENDPOINTS="127.0.0.1:50050" ./build/greeter_failover_client 3; echo "rc=$?"
```

Expected: three lines, each ending `(via 127.0.0.1:50050)`, and the three messages naming three *different* mocks (order may start anywhere in the rotation), `rc=0`. Example:

```
Greeter received: Hello from mock2 @50052 (via 127.0.0.1:50050)
Greeter received: Hello from mock3 @50053 (via 127.0.0.1:50050)
Greeter received: Hello from mock1 @50051 (via 127.0.0.1:50050)
rc=0
```

- [ ] **Step 4: Preview the built-in-retry scenario manually**

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml stop mock2
GRPC_TARGET_ENDPOINTS="127.0.0.1:50050" ./build/greeter_failover_client 6; echo "rc=$?"
docker compose -f tests/lb/mockserver/docker-compose.yml start mock2
```

Expected: six lines, all `(via 127.0.0.1:50050)`, no line mentioning `mock2`, `rc=0`. This proves the channel's service-config retry (`maxAttempts=4`, retriable on `UNAVAILABLE`, active because a single endpoint means `multi_endpoint=false`) recovers Envoy's failed picks of the dead backend. If instead calls fail with `14:` here, check `docker compose ... logs envoy` for whether Envoy dropped mock2 from DNS (should not, within `dns_refresh_rate: 300s`).

- [ ] **Step 5: Tear down and commit**

```bash
docker compose -f tests/lb/mockserver/docker-compose.yml down --remove-orphans
git add tests/lb/mockserver/envoy.yaml
git commit -m "add envoy round-robin proxy for single-endpoint built-in LB testing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: The test driver `run_live_test.sh`

**Files:**
- Create: `tests/lb/mockserver/run_live_test.sh` (executable)

- [ ] **Step 1: Create the script**

Bash 3.2-compatible (macOS default): no `mapfile`, no associative arrays. No `set -e` — client exit codes are assertions, captured explicitly.

```bash
#!/usr/bin/env bash
# Live failover test for greeter_failover_client: three MockServer gRPC
# endpoints (direct scenarios) plus an Envoy round-robin proxy (single-
# endpoint built-in-retry scenarios), all managed by docker compose.
#
# Usage: run_live_test.sh [--keep] [--client PATH]
#   --keep    leave the compose stack running after the test
#   --client  path to greeter_failover_client (default: <repo>/build/...)
#
# Spec: docs/superpowers/specs/2026-07-09-lb-mockserver-live-test-design.md
set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

CLIENT="${REPO_ROOT}/build/greeter_failover_client"
KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --keep) KEEP=1; shift ;;
        --client) CLIENT="$2"; shift 2 ;;
        *) echo "usage: $0 [--keep] [--client PATH]" >&2; exit 2 ;;
    esac
done

compose() { docker compose -f "${SCRIPT_DIR}/docker-compose.yml" "$@"; }

DIRECT="127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053"
PROXY="127.0.0.1:50050"

PASSES=0
FAILURES=0
log()  { printf '\n=== %s\n' "$*"; }
pass() { printf 'PASS: %s\n' "$*"; PASSES=$((PASSES + 1)); }
fail() { printf 'FAIL: %s\n--- client output ---\n%s\n---------------------\n' \
                "$1" "${2-}"; FAILURES=$((FAILURES + 1)); }

# --- prerequisites -----------------------------------------------------------
command -v docker >/dev/null 2>&1 || { echo "docker not found" >&2; exit 2; }
docker compose version >/dev/null 2>&1 \
    || { echo "docker compose plugin not available" >&2; exit 2; }
command -v protoc >/dev/null 2>&1 || { echo "protoc not found on PATH" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "curl not found" >&2; exit 2; }
if [ ! -x "${CLIENT}" ]; then
    echo "client binary not found: ${CLIENT}" >&2
    echo "build it first: cmake --build build --target greeter_failover_client" >&2
    exit 2
fi

# --- descriptor generation (never checked in) --------------------------------
mkdir -p "${SCRIPT_DIR}/gen"
protoc --descriptor_set_out="${SCRIPT_DIR}/gen/helloworld.dsc" \
       --include_imports -I "${REPO_ROOT}/protos" \
       "${REPO_ROOT}/protos/helloworld.proto" || exit 2

# --- stack lifecycle ----------------------------------------------------------
teardown() {
    if [ "${KEEP}" -eq 1 ]; then
        echo "--keep set: stack left running; stop it with:"
        echo "  docker compose -f ${SCRIPT_DIR}/docker-compose.yml down"
    else
        compose down --remove-orphans >/dev/null 2>&1 || true
    fi
}
trap teardown EXIT

log "starting compose stack (3x MockServer + Envoy)"
compose up -d || exit 2

wait_http() {  # wait_http <service> <url> [extra curl args...]
    _name="$1"; _url="$2"; shift 2
    _n=0
    while [ ${_n} -lt 60 ]; do
        if curl -sf "$@" "${_url}" >/dev/null 2>&1; then return 0; fi
        sleep 0.5
        _n=$((_n + 1))
    done
    echo "timeout waiting for ${_name} (${_url})" >&2
    compose logs --tail 40 "${_name}" >&2 || true
    return 1
}

wait_http mock1 "http://127.0.0.1:50051/mockserver/status" -X PUT || exit 2
wait_http mock2 "http://127.0.0.1:50052/mockserver/status" -X PUT || exit 2
wait_http mock3 "http://127.0.0.1:50053/mockserver/status" -X PUT || exit 2
wait_http envoy "http://127.0.0.1:9901/ready" || exit 2

# --- helpers ------------------------------------------------------------------
OUT=""
RC=0
run_client() {  # run_client <endpoints> <count> <delay_ms>
    OUT="$(GRPC_TARGET_ENDPOINTS="$1" "${CLIENT}" "$2" "$3" 2>&1)"
    RC=$?
}

line_for() {  # line_for <n> — the exact success line for mock<n>
    echo "Greeter received: Hello from mock$1 @5005$1 (via 127.0.0.1:5005$1)"
}

# --- scenario 1: direct round-robin ------------------------------------------
log "scenario 1: direct round-robin across all 3 endpoints"
run_client "${DIRECT}" 6 0
EXPECTED="$(line_for 1)
$(line_for 2)
$(line_for 3)
$(line_for 1)
$(line_for 2)
$(line_for 3)"
if [ ${RC} -eq 0 ] && [ "${OUT}" = "${EXPECTED}" ]; then
    pass "strict rotation mock1→mock2→mock3, twice"
else
    fail "direct round-robin (rc=${RC})" "${OUT}"
fi

# --- scenario 2: failover skips a stopped container ---------------------------
log "scenario 2: failover skips stopped mock1"
compose stop mock1 >/dev/null 2>&1
run_client "${DIRECT}" 4 0
OK=1
[ ${RC} -eq 0 ] || OK=0
case "${OUT}" in *"50051"*) OK=0 ;; esac
N=0
while IFS= read -r LINE; do
    case "${LINE}" in
        "$(line_for 2)"|"$(line_for 3)") ;;
        *) OK=0 ;;
    esac
    N=$((N + 1))
done <<EOF
${OUT}
EOF
[ ${N} -eq 4 ] || OK=0
if [ ${OK} -eq 1 ]; then
    pass "4/4 calls served by mock2/mock3; mock1 never named"
else
    fail "failover (rc=${RC})" "${OUT}"
fi

# --- scenario 3: cooldown rejoin after restart --------------------------------
log "scenario 3: mock1 rejoins rotation after restart"
GRPC_TARGET_ENDPOINTS="${DIRECT}" "${CLIENT}" 30 500 \
    > "${SCRIPT_DIR}/gen/rejoin.out" 2>&1 &
BG_PID=$!
compose start mock1 >/dev/null 2>&1
wait_http mock1 "http://127.0.0.1:50051/mockserver/status" -X PUT || true
wait ${BG_PID}
BG_RC=$?
BG_OUT="$(cat "${SCRIPT_DIR}/gen/rejoin.out")"
OK=1
[ ${BG_RC} -eq 0 ] || OK=0
case "${BG_OUT}" in *"via 127.0.0.1:50051"*) ;; *) OK=0 ;; esac
if [ ${OK} -eq 1 ]; then
    pass "mock1 served calls again after restart"
else
    fail "cooldown rejoin (rc=${BG_RC})" "${BG_OUT}"
fi

# --- scenario 4: proxy round-robin behind a single endpoint -------------------
log "scenario 4: proxy round-robin (single endpoint, LB behind envoy)"
run_client "${PROXY}" 6 0
OK=1
[ ${RC} -eq 0 ] || OK=0
N=0
while IFS= read -r LINE; do
    case "${LINE}" in
        "Greeter received: Hello from mock"*" (via 127.0.0.1:50050)") ;;
        *) OK=0 ;;
    esac
    N=$((N + 1))
done <<EOF
${OUT}
EOF
[ ${N} -eq 6 ] || OK=0
for M in mock1 mock2 mock3; do
    case "${OUT}" in *"${M}"*) ;; *) OK=0 ;; esac
done
if [ ${OK} -eq 1 ]; then
    pass "6/6 via proxy; replies from all three mocks"
else
    fail "proxy round-robin (rc=${RC})" "${OUT}"
fi

# --- scenario 5: built-in retry recovers a dead backend via proxy -------------
log "scenario 5: built-in retry recovers stopped mock2 behind proxy"
compose stop mock2 >/dev/null 2>&1
run_client "${PROXY}" 6 0
OK=1
[ ${RC} -eq 0 ] || OK=0
case "${OUT}" in *"mock2"*) OK=0 ;; esac
case "${OUT}" in *"mock1"*) ;; *) OK=0 ;; esac
case "${OUT}" in *"mock3"*) ;; *) OK=0 ;; esac
if [ ${OK} -eq 1 ]; then
    pass "6/6 succeeded via service-config retry; only mock1/mock3 replied"
else
    fail "built-in retry via proxy (rc=${RC})" "${OUT}"
fi

# --- scenario 6: all direct endpoints down ------------------------------------
log "scenario 6: all endpoints down → UNAVAILABLE, exit 1"
compose stop mock1 mock3 >/dev/null 2>&1
run_client "${DIRECT}" 1 0
OK=1
[ ${RC} -eq 1 ] || OK=0
case "${OUT}" in "14:"*) ;; *) OK=0 ;; esac
if [ ${OK} -eq 1 ]; then
    pass "exit 1 with grpc status 14 (UNAVAILABLE)"
else
    fail "all-down (rc=${RC})" "${OUT}"
fi

# --- summary -------------------------------------------------------------------
log "summary: ${PASSES} passed, ${FAILURES} failed"
[ ${FAILURES} -eq 0 ] || exit 1
exit 0
```

- [ ] **Step 2: Make it executable and lint it**

```bash
chmod +x tests/lb/mockserver/run_live_test.sh
bash -n tests/lb/mockserver/run_live_test.sh && echo "syntax OK"
```

Expected: `syntax OK`. If `shellcheck` is installed, run it too and fix genuine findings (style-only findings may be ignored).

- [ ] **Step 3: Run the full suite three times (flakiness check)**

```bash
tests/lb/mockserver/run_live_test.sh; echo "run1 rc=$?"
tests/lb/mockserver/run_live_test.sh; echo "run2 rc=$?"
tests/lb/mockserver/run_live_test.sh; echo "run3 rc=$?"
```

Expected each run: six `PASS:` lines, `summary: 6 passed, 0 failed`, `rc=0`.

**If scenario 3 fails intermittently** (see "Known risk" in the header): inspect `tests/lb/mockserver/gen/rejoin.out`. If failures are non-retriable statuses (`4:` DEADLINE_EXCEEDED or an expectation-miss status) during mock1's boot window, apply the approved fallback: in scenario 3, delete the `[ ${BG_RC} -eq 0 ] || OK=0` line, add a comment explaining the restart window can produce non-retriable errors, and update the spec's scenario 3 paragraph to match. Then rerun the 3× check.

- [ ] **Step 4: Verify `--keep` and manual teardown**

```bash
tests/lb/mockserver/run_live_test.sh --keep
docker compose -f tests/lb/mockserver/docker-compose.yml ps
docker compose -f tests/lb/mockserver/docker-compose.yml down --remove-orphans
```

Expected: after `--keep`, `ps` still lists the (partially stopped) stack; `down` removes it.

- [ ] **Step 5: Commit**

```bash
git add tests/lb/mockserver/run_live_test.sh
git commit -m "add run_live_test.sh driving six live failover scenarios

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: gitignore, Readme, final verification

**Files:**
- Modify: `.gitignore` (append)
- Modify: `Readme.md` (append to the `## Client-side failover load balancing` section, which is the last section of the file)

- [ ] **Step 1: Gitignore the runtime-generated descriptor dir**

Append to `.gitignore`:

```
# MockServer live-test runtime artifacts (descriptor set, scratch output)
tests/lb/mockserver/gen/
```

Verify: `git status` no longer lists `tests/lb/mockserver/gen/` as untracked.

- [ ] **Step 2: Document the live test in `Readme.md`**

Append at the end of the file (still inside the `## Client-side failover load balancing` section):

````markdown
### Live testing with MockServer + Envoy (docker compose)

`tests/lb/mockserver/run_live_test.sh` drives the real
`greeter_failover_client` binary against real containers:

| Host port | What |
|---|---|
| 50051–50053 | three MockServer 7.x instances mocking `Greeter/SayHello`, each replying with its own identity (`Hello from mock1 @50051`, ...) |
| 50050 | Envoy round-robin proxy over the three mocks — the "one endpoint, many backends" shape |
| 9901 (localhost only) | Envoy admin, readiness checks |

```bash
cmake --build build --target greeter_failover_client
tests/lb/mockserver/run_live_test.sh          # --keep leaves the stack up
```

Six scenarios: direct round-robin; failover around a stopped container;
cooldown rejoin after restart; proxy round-robin behind a single endpoint
(the app-level loop makes one attempt — distribution happens in Envoy);
built-in service-config retry recovering a dead backend behind the proxy;
all-endpoints-down returning `UNAVAILABLE`.

The demo client accepts optional `[count] [delay_ms]` args to make several
sequential calls from one process (`./build/greeter_failover_client 6 0`).
Round-robin and cooldown state are per-process, so multi-call runs are
needed to observe rotation and rejoin.
````

Note: paste the section content only — the outer ````markdown```` fence above is plan formatting, not part of `Readme.md`.

- [ ] **Step 3: Final end-to-end verification**

```bash
tests/lb/mockserver/run_live_test.sh; echo "rc=$?"
ctest --test-dir build --output-on-failure -R "lb_"
git status
```

Expected: live script `6 passed, 0 failed`, `rc=0`; all lb ctest targets pass; `git status` shows only `.gitignore` and `Readme.md` as modified (no stray generated files).

- [ ] **Step 4: Commit**

```bash
git add .gitignore Readme.md
git commit -m "docs(lb): document MockServer+Envoy live testing; ignore descriptor output

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
