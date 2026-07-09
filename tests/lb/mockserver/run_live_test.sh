#!/usr/bin/env bash
# Live failover test for greeter_failover_client: three MockServer gRPC
# endpoints (direct scenarios) plus an Envoy round-robin proxy (single-
# endpoint built-in-retry scenarios), all managed by docker compose.
#
# Usage: run_live_test.sh [--keep] [--client PATH]
#   --keep    leave the compose stack running after the test
#   --client  path to greeter_failover_client (default: <repo>/build/...)
#
# Assertions run against the client's stdout only — the lb library logs to
# stderr, which is captured to gen/*.err for failure diagnostics.
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
        --client)
            [ $# -ge 2 ] || { echo "usage: $0 [--keep] [--client PATH]" >&2; exit 2; }
            CLIENT="$2"; shift 2 ;;
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
fail() {
    printf 'FAIL: %s\n--- client stdout ---\n%s\n--- client stderr ---\n%s\n---------------------\n' \
           "$1" "${2-}" "$(cat "${SCRIPT_DIR}/gen/client_stderr.log" 2>/dev/null)"
    FAILURES=$((FAILURES + 1))
}

# --- prerequisites -----------------------------------------------------------
command -v docker >/dev/null 2>&1 || { echo "docker not found" >&2; exit 2; }
docker compose version >/dev/null 2>&1 \
    || { echo "docker compose plugin not available" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "curl not found" >&2; exit 2; }
if [ ! -x "${CLIENT}" ]; then
    echo "client binary not found: ${CLIENT}" >&2
    echo "build it first: cmake --build build --target greeter_failover_client" >&2
    exit 2
fi

# --- runtime artifacts (descriptor + expectations, never checked in) ---------
"${SCRIPT_DIR}/generate_artifacts.sh" || exit 2

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
    OUT="$(GRPC_TARGET_ENDPOINTS="$1" "${CLIENT}" "$2" "$3" \
           2>"${SCRIPT_DIR}/gen/client_stderr.log")"
    RC=$?
}

line_for() {  # line_for <n> — the exact success line for mock<n>
    echo "Greeter received: Hello from mock$1 @5005$1 (via 127.0.0.1:5005$1)"
}

envoy_connect_fails() {  # upstream connect failures counted by the proxy
    curl -s http://127.0.0.1:9901/stats 2>/dev/null \
      | awk -F': ' '/^cluster\.greeter_mocks\.upstream_cx_connect_fail:/ {print $2}'
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
    > "${SCRIPT_DIR}/gen/rejoin.out" 2>"${SCRIPT_DIR}/gen/rejoin.err" &
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
    fail "cooldown rejoin (rc=${BG_RC})" "${BG_OUT}
--- background stderr ---
$(cat "${SCRIPT_DIR}/gen/rejoin.err" 2>/dev/null)"
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
FAILS_BEFORE="$(envoy_connect_fails)"
run_client "${PROXY}" 6 0
FAILS_AFTER="$(envoy_connect_fails)"
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
case "${OUT}" in *"mock2"*) OK=0 ;; esac
case "${OUT}" in *"mock1"*) ;; *) OK=0 ;; esac
case "${OUT}" in *"mock3"*) ;; *) OK=0 ;; esac
# Recovery alone could also be explained by Envoy quietly dropping the dead
# endpoint (e.g. a DNS re-resolve). The proxy's connect-fail counter must
# have grown across the run: proof Envoy kept picking the stopped backend,
# each pick surfaced as UNAVAILABLE, and the client's service-config retry
# did the recovering.
DELTA=$(( ${FAILS_AFTER:-0} - ${FAILS_BEFORE:-0} ))
[ ${DELTA} -ge 1 ] || OK=0
if [ ${OK} -eq 1 ]; then
    pass "6/6 via proxy; retry absorbed ${DELTA} upstream connect failures; only mock1/mock3 replied"
else
    fail "built-in retry via proxy (rc=${RC}, connect_fail delta=${DELTA})" "${OUT}"
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
