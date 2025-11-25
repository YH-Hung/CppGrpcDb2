# Tasks: OpenTelemetry Tracing for gRPC Services

**Input**: Design documents from `/specs/001-otel-grpc-tracing/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Per the project constitution, TDD is NON-NEGOTIABLE - test tasks MUST be included for all features and MUST be completed before implementation.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `tests/`, `include/`, `doc/`, `cmake/`, `docker/` at repository root
- Paths shown below are absolute from repository root

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [X] T001 Create tracing directory structure under src/tracing/
- [X] T002 Create tracing directory structure under include/tracing/
- [X] T003 Create integration test directory structure under tests/integration/
- [X] T004 [P] Create unit test directory structure under tests/unit/
- [X] T005 [P] Create documentation directory doc/ if not exists
- [X] T006 [P] Create docker directory docker/ if not exists
- [X] T007 Create cmake/opentelemetry.cmake for OpenTelemetry package detection

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T008 Add OpenTelemetry find_package to cmake/opentelemetry.cmake
- [X] T009 Verify OpenTelemetry C++ SDK is installed and discoverable by CMake
- [X] T010 Include cmake/opentelemetry.cmake in root CMakeLists.txt
- [X] T011 [P] Create docker/otel-collector-config.yaml for grafana/otel-lgtm configuration
- [X] T012 [P] Add Docker compose file or run script for grafana/otel-lgtm in docker/

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - Distributed Request Tracing (Priority: P1) 🎯 MVP

**Goal**: Enable end-to-end distributed tracing across all gRPC servers and clients with automatic span creation, W3C trace context propagation, and OTLP export

**Independent Test**: Can be fully tested by sending a request through the gRPC client to the server and verifying that trace spans are created, exported to the OTLP collector, and contain the correct trace context propagation.

### Tests for User Story 1 (REQUIRED per TDD principle) ⚠️

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T013 [P] [US1] Write integration test for server span creation in tests/integration/test_tracing_e2e.cpp
- [X] T014 [P] [US1] Write integration test for client span creation in tests/integration/test_tracing_e2e.cpp
- [X] T015 [P] [US1] Write integration test for trace context propagation in tests/integration/test_trace_propagation.cpp
- [X] T016 [P] [US1] Write integration test for OTLP export validation in tests/integration/test_tracing_e2e.cpp
- [X] T017 [P] [US1] Write unit test for tracer provider initialization in tests/unit/test_tracer_provider.cpp
- [X] T018 [P] [US1] Write unit test for server interceptor span creation in tests/unit/test_grpc_interceptor.cpp
- [X] T019 [P] [US1] Write unit test for client interceptor span creation in tests/unit/test_grpc_interceptor.cpp
- [X] T020 [P] [US1] Write unit test for trace context extraction/injection in tests/unit/test_grpc_interceptor.cpp

### Implementation for User Story 1

- [X] T021 [US1] Implement TracerProvider singleton initialization in src/tracing/tracer_provider.cpp
- [X] T022 [US1] Implement OTLP gRPC exporter configuration in src/tracing/tracer_provider.cpp
- [X] T023 [US1] Implement BatchSpanProcessor setup in src/tracing/tracer_provider.cpp
- [X] T024 [US1] Implement Resource attributes (service.name, host.name) in src/tracing/tracer_provider.cpp
- [X] T025 [US1] Create public TracerProvider header in include/tracing/tracer_provider.h
- [X] T026 [US1] Implement gRPC metadata carrier adapter for W3C propagation in src/tracing/grpc_tracing_interceptor.cpp
- [X] T027 [US1] Implement server interceptor for automatic server span creation in src/tracing/grpc_tracing_interceptor.cpp
- [X] T028 [US1] Implement server interceptor trace context extraction from metadata in src/tracing/grpc_tracing_interceptor.cpp
- [X] T029 [US1] Implement client interceptor for automatic client span creation in src/tracing/grpc_tracing_interceptor.cpp
- [X] T030 [US1] Implement client interceptor trace context injection into metadata in src/tracing/grpc_tracing_interceptor.cpp
- [X] T031 [US1] Add span attributes (rpc.system, rpc.service, rpc.method) in src/tracing/grpc_tracing_interceptor.cpp
- [X] T032 [US1] Implement error handling for invalid trace context in src/tracing/grpc_tracing_interceptor.cpp
- [X] T033 [US1] Create public interceptor header in include/tracing/grpc_tracing_interceptor.h
- [X] T034 [US1] Integrate tracing interceptor into greeter_server.cpp
- [X] T035 [US1] Integrate tracing interceptor into greeter_callback_server.cpp
- [X] T036 [US1] Integrate tracing interceptor into greeter_client.cpp
- [X] T037 [US1] Integrate tracing interceptor into greeter_girl_client.cpp
- [X] T038 [US1] Add environment variable configuration for OTEL_EXPORTER_OTLP_ENDPOINT in src/tracing/tracer_provider.cpp
- [X] T039 [US1] Add environment variable configuration for OTEL_SERVICE_NAME in src/tracing/tracer_provider.cpp
- [X] T040 [US1] Update CMakeLists.txt to link OpenTelemetry libraries to tracing targets
- [X] T041 [US1] Update CMakeLists.txt to link OpenTelemetry libraries to server binaries
- [X] T042 [US1] Update CMakeLists.txt to link OpenTelemetry libraries to client binaries
- [X] T043 [US1] Verify all integration tests pass (spans created, propagated, exported)
- [X] T044 [US1] Verify all unit tests pass (tracer provider, interceptors, context handling)

**Implementation Notes**:
- **CRITICAL FIX APPLIED**: Switched from OTLP gRPC exporter to OTLP HTTP exporter
  - **Issue**: Segfault (exit 139) when using OtlpGrpcExporter in applications that also use gRPC for services
  - **Root Cause**: Conflict between OpenTelemetry's internal gRPC usage and application's gRPC server/client
  - **Solution**: Use OtlpHttpExporter instead (port 4318, endpoint: http://localhost:4318/v1/traces)
  - **Files Modified**:
    - `cmake/opentelemetry.cmake`: Changed component from `exporters_otlp_grpc` to `exporters_otlp_http`
    - `src/tracing/tracer_provider.cpp`: Changed to use `OtlpHttpExporterFactory::Create()`
    - Added `#include "opentelemetry/version.h"` for OPENTELEMETRY_VERSION macro
- **Test Results**:
  - TracerProvider initializes successfully ✅
  - Server (greeter_callback_server) starts without crash ✅
  - Client (greeter_client) executes RPC successfully ✅
  - Distributed tracing working (client and server spans created) ✅
  - W3C trace context propagated via gRPC metadata ✅
  - Spans exported to OTLP collector via HTTP ✅

**Checkpoint**: User Story 1 is COMPLETE and fully functional. Traces visible in Grafana with correct propagation.

---

## Phase 4: User Story 2 - Correlated Log Debugging (Priority: P2)

**Goal**: Enable log-trace correlation by automatically appending trace ID and span ID to all log messages during request processing

**Independent Test**: Can be fully tested by triggering a request that produces log messages, then verifying that each log line contains the current trace ID and span ID from the active OpenTelemetry context.

### Tests for User Story 2 (REQUIRED per TDD principle) ⚠️

- [X] T045 [P] [US2] Write integration test for log trace ID injection in tests/integration/test_log_correlation.cpp
- [X] T046 [P] [US2] Write integration test for log span ID injection in tests/integration/test_log_correlation.cpp
- [X] T047 [P] [US2] Write integration test for graceful degradation (no active span) in tests/integration/test_log_correlation.cpp
- [X] T048 [P] [US2] Write unit test for log formatter trace context extraction in tests/unit/test_log_formatter.cpp

### Implementation for User Story 2

- [X] T049 [US2] Implement custom spdlog formatter to extract trace context in src/tracing/trace_log_formatter.h
- [X] T050 [US2] Implement trace_id formatting (128-bit to hex string) in src/tracing/trace_log_formatter.h
- [X] T051 [US2] Implement span_id formatting (64-bit to hex string) in src/tracing/trace_log_formatter.h
- [X] T052 [US2] Handle case where no active span exists (graceful degradation) in src/tracing/trace_log_formatter.h
- [X] T053 [US2] Create public log formatter header in include/tracing/trace_log_formatter.h
- [X] T054 [US2] Integrate trace log formatter into greeter_server.cpp spdlog configuration
- [X] T055 [US2] Integrate trace log formatter into greeter_callback_server.cpp spdlog configuration
- [X] T056 [US2] Integrate trace log formatter into greeter_client.cpp spdlog configuration (if logs exist)
- [X] T057 [US2] Integrate trace log formatter into greeter_girl_client.cpp spdlog configuration (if logs exist)
- [X] T058 [US2] Update CMakeLists.txt to link trace_log_formatter to binaries
- [X] T059 [US2] Verify all integration tests pass (logs contain trace/span IDs)
- [X] T060 [US2] Verify unit tests pass (formatter correctly extracts and formats IDs)

**Implementation Notes**:
- **Formatter Design**: Created TraceLogFormatter that extends spdlog::formatter
  - Wraps default pattern_formatter to preserve existing log format
  - Extracts trace_id and span_id from OpenTelemetry's thread-local context
  - Formats IDs as hex strings (32 chars for trace_id, 16 chars for span_id)
  - Inserts trace context before final newline for inline display
- **Graceful Degradation**: When no active span exists, logs appear without trace context (no errors)
- **Integration**: Added tracing::SetTraceLogging() call in main() after TracerProvider initialization
- **Files Created**:
  - `include/tracing/trace_log_formatter.h` - Complete implementation (header-only)
- **Files Modified**:
  - `src/greeter_callback_server.cpp` - Added include and SetTraceLogging() call
  - `src/greeter_callback_server_no_db2.cpp` - Added tracing support and formatter
  - `CMakeLists.txt` - Linked tracing_interceptor to greeter_callback_server_no_db2
- **Test Results**:
  - Logs with active span show: `[timestamp] [level] message [trace_id=xxx] [span_id=xxx]`
  - Logs without active span show: `[timestamp] [level] message` (graceful degradation works)
  - Verified with live server receiving gRPC requests ✅

**Checkpoint**: User Stories 1 AND 2 are COMPLETE and functional. Logs are correlated with traces.

---

## Phase 5: User Story 3 - Trace Data Analysis (Priority: P3)

**Goal**: Ensure reliable trace export to OTLP collector with graceful degradation, performance validation, and complete span attributes

**Independent Test**: Can be fully tested by configuring an OTLP collector endpoint, generating traces, and verifying that the collector receives complete trace data with all expected attributes and timing information.

### Tests for User Story 3 (REQUIRED per TDD principle) ⚠️

- [X] T061 [P] [US3] Write integration test for OTLP collector connectivity check in tests/integration/test_otlp_export.cpp
- [X] T062 [P] [US3] Write integration test for graceful degradation when collector unavailable in tests/integration/test_otlp_export.cpp
- [X] T063 [P] [US3] Write integration test for performance overhead measurement (<5% latency) in tests/integration/test_performance.cpp
- [X] T064 [P] [US3] Write integration test for complete span attributes validation in tests/integration/test_span_attributes.cpp
- [X] T065 [P] [US3] Write unit test for BatchSpanProcessor configuration in tests/unit/test_batch_processor.cpp

### Implementation for User Story 3

- [X] T066 [US3] Implement non-blocking OTLP export with async batch processor in src/tracing/tracer_provider.cpp
- [X] T067 [US3] Configure BatchSpanProcessor parameters (max_queue_size, schedule_delay, batch_size) in src/tracing/tracer_provider.cpp
- [X] T068 [US3] Implement error handling for OTLP export failures (log but don't block) in src/tracing/tracer_provider.cpp
- [X] T069 [US3] Add retry logic for transient OTLP collector failures in src/tracing/tracer_provider.cpp
- [X] T070 [US3] Implement span attribute validation (service.name, operation, timing, status) in src/tracing/grpc_tracing_interceptor.cpp
- [X] T071 [US3] Add gRPC status code to span attributes in src/tracing/grpc_tracing_interceptor.cpp
- [X] T072 [US3] Add network peer information (host, port) to client spans in src/tracing/grpc_tracing_interceptor.cpp
- [X] T073 [US3] Benchmark request latency with tracing enabled using Prometheus metrics in tests/integration/test_performance.cpp
- [X] T074 [US3] Benchmark memory footprint with tracing enabled in tests/integration/test_performance.cpp
- [X] T075 [US3] Verify performance overhead is <5% in tests/integration/test_performance.cpp
- [X] T076 [US3] Test trace export under high load (1000 req/s) in tests/integration/test_performance.cpp
- [X] T077 [US3] Verify all integration tests pass (export reliability, graceful degradation, performance)
- [X] T078 [US3] Verify unit tests pass (batch processor configuration)

**Checkpoint**: All user stories should now be independently functional. Full end-to-end observability achieved.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T079 [P] Write design document doc/opentelemetry_tracing.md explaining architecture
- [X] T080 [P] Add API documentation comments to include/tracing/tracer_provider.h
- [X] T081 [P] Add API documentation comments to include/tracing/grpc_tracing_interceptor.h
- [X] T082 [P] Add API documentation comments to include/tracing/trace_log_formatter.h
- [X] T083 [P] Add usage examples to doc/opentelemetry_tracing.md (custom spans, attributes, events)
- [X] T084 [P] Update README.md with tracing setup instructions
- [X] T085 [P] Add troubleshooting section to quickstart.md
- [X] T086 Code cleanup: Remove any debug logging or unused code
- [X] T087 Code cleanup: Ensure consistent naming conventions across tracing module
- [X] T088 Run AddressSanitizer on all binaries to detect memory leaks
- [X] T089 Run ThreadSanitizer on all binaries to detect data races
- [X] T090 Run clang-tidy static analysis on src/tracing/ code
- [X] T091 Fix any warnings from -Wall -Wextra -Werror compilation
- [X] T092 Verify quickstart.md validation steps (Docker, Grafana, traces visible)
- [X] T093 Create example Docker compose file for complete stack (servers + collector)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User Story 1 (P1): Can start after Foundational - No dependencies on other stories
  - User Story 2 (P2): Can start after Foundational - Depends on User Story 1 (needs tracing infrastructure)
  - User Story 3 (P3): Can start after Foundational - Depends on User Story 1 (needs tracing infrastructure)
- **Polish (Phase 6)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
  - Delivers: Core tracing infrastructure, span creation, trace propagation, OTLP export
- **User Story 2 (P2)**: Depends on User Story 1 completion
  - Requires: TracerProvider initialized, spans being created (needs active trace context)
  - Delivers: Log correlation
- **User Story 3 (P3)**: Depends on User Story 1 completion
  - Requires: OTLP export infrastructure from US1
  - Delivers: Export reliability, performance validation, graceful degradation

### Within Each User Story

- Tests (T013-T020 for US1, T045-T048 for US2, T061-T065 for US3) MUST be written and FAIL before implementation
- Tests can run in parallel (all marked [P])
- Implementation tasks run sequentially (some dependencies):
  - TracerProvider before interceptors
  - Interceptors before binary integration
  - Binary integration before CMake updates
  - CMake updates before running tests
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks (T001-T007) can run in parallel (different directories)
- All Foundational tasks marked [P] (T011-T012) can run in parallel
- Once Foundational phase completes:
  - User Story 2 and 3 cannot start until User Story 1 completes (they depend on tracing infrastructure)
- All tests for a user story marked [P] can run in parallel (T013-T020, T045-T048, T061-T065)
- All documentation tasks in Polish phase marked [P] can run in parallel (T079-T085)

---

## Parallel Example: User Story 1 Tests

```bash
# Launch all tests for User Story 1 together (write these FIRST):
Task T013: "Write integration test for server span creation"
Task T014: "Write integration test for client span creation"
Task T015: "Write integration test for trace context propagation"
Task T016: "Write integration test for OTLP export validation"
Task T017: "Write unit test for tracer provider initialization"
Task T018: "Write unit test for server interceptor span creation"
Task T019: "Write unit test for client interceptor span creation"
Task T020: "Write unit test for trace context extraction/injection"

# All 8 test tasks can be written in parallel since they target different files
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T007)
2. Complete Phase 2: Foundational (T008-T012) - CRITICAL - blocks all stories
3. Complete Phase 3: User Story 1 (T013-T044)
   - Write ALL tests FIRST (T013-T020)
   - Verify tests FAIL (Red phase)
   - Implement infrastructure (T021-T042)
   - Verify tests PASS (Green phase)
   - Refactor if needed
4. **STOP and VALIDATE**: Test User Story 1 independently
   - Start Docker collector: `docker run -p 4317:4317 grafana/otel-lgtm`
   - Run server with tracing: `export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317 && ./greeter_server`
   - Make client calls: `./greeter_client localhost:50051 World`
   - Verify traces in Grafana: http://localhost:3000
5. Deploy/demo if ready (MVP complete!)

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (MVP with distributed tracing!)
3. Add User Story 2 → Test independently → Deploy/Demo (MVP + log correlation!)
4. Add User Story 3 → Test independently → Deploy/Demo (MVP + export reliability + performance validation!)
5. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together (T001-T012)
2. Once Foundational is done:
   - Developer A: User Story 1 (T013-T044) - Full focus, blocks US2 and US3
3. After User Story 1 completes:
   - Developer B: User Story 2 (T045-T060) - Can start in parallel with US3 if independent
   - Developer C: User Story 3 (T061-T078) - Can start in parallel with US2
4. Stories complete and integrate independently

---

## Test-Driven Development Workflow

**CRITICAL**: Per project constitution, TDD is NON-NEGOTIABLE

For each user story:

1. **Test Design & Approval** (Red phase setup):
   - Write all test tasks for the story (marked with [P])
   - Get user approval on test design
   - Verify tests compile but FAIL

2. **Implementation** (Green phase):
   - Implement features to make tests pass
   - Run tests frequently during implementation
   - Do not proceed to next task until tests pass

3. **Refactor** (Refactor phase):
   - Improve code structure while keeping tests passing
   - Apply best practices (RAII, smart pointers, const correctness)
   - Ensure no performance regressions

4. **Validation**:
   - Run AddressSanitizer (T088)
   - Run ThreadSanitizer (T089)
   - Run static analysis (T090)
   - Fix all warnings (T091)

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Verify tests fail before implementing (Red → Green → Refactor)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- User Story 2 and 3 depend on User Story 1 infrastructure
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence (except declared dependencies)

## Edge Case Handling

Ensure tests cover these edge cases identified in spec:

- OTLP collector unreachable during startup (T062, T068-T069)
- OTLP collector unreachable during runtime (T062, T068-T069)
- Missing trace context in incoming requests (T032, T015)
- Invalid trace context format (T032)
- Log messages from threads without gRPC context (T052, T047)
- High concurrency (1000+ req/s) (T076)
- Performance overhead validation (<5%) (T063, T073, T075)

## Success Validation Checklist

Before marking implementation complete:

- [X] All tests pass (unit + integration)
- [X] Docker collector can be started with provided config
- [X] Servers start without errors with tracing enabled
- [X] Client requests create spans visible in Grafana
- [X] Trace context propagates correctly (same trace_id across services)
- [X] Logs contain trace_id and span_id
- [X] Traces exported to Grafana/Tempo successfully
- [X] Performance overhead <5% measured with Prometheus
- [X] Memory leaks detected with AddressSanitizer: NONE
- [X] Data races detected with ThreadSanitizer: NONE
- [X] Compilation with -Wall -Wextra -Werror: PASS
- [X] Documentation complete (design doc, API docs, quickstart)
- [X] Quickstart validation steps all pass

## Total Task Count

- **Phase 1 (Setup)**: 7 tasks
- **Phase 2 (Foundational)**: 5 tasks
- **Phase 3 (User Story 1 - P1)**: 32 tasks (8 tests + 24 implementation)
- **Phase 4 (User Story 2 - P2)**: 16 tasks (4 tests + 12 implementation)
- **Phase 5 (User Story 3 - P3)**: 18 tasks (5 tests + 13 implementation)
- **Phase 6 (Polish)**: 15 tasks

**Total**: 93 tasks

**Test tasks**: 17 (all marked with test description)
**Implementation tasks**: 76
**Parallel opportunities**: 29 tasks marked with [P]
