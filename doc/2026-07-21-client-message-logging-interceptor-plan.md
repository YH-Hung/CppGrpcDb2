# Client Message Logging Interceptor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a client-side gRPC message logging interceptor (JSON request/response via spdlog) and wire it into `greeter_failover_client` through a custom `ChannelBuilder`, leaving `grpc_client_lb` untouched.

**Architecture:** A `MessageLoggingClientInterceptor` + factory pair mirroring the existing server-side `MessageLoggingServerInterceptor`, added to the existing `message_logging_interceptor` CMake target. The demo binary attaches it per-channel via `grpc::experimental::CreateCustomChannelWithInterceptors` inside a `lb::ChannelBuilder` lambda. Spec: `doc/2026-07-21-client-message-logging-interceptor-design.md`.

**Tech Stack:** C++20, gRPC experimental interceptor API, protobuf JSON util, spdlog, GTest, CMake.

## Global Constraints

- All targets pin `cxx_std_20` (repo-wide rule; don't drop lower).
- Generated `.pb.*` files must never be checked in.
- No otel trace-id in the client interceptor's log lines (spec decision), so it must NOT include `otel_tracing.h`.
- Log request/response only — no per-attempt status logging (spec decision).
- Interceptor never throws and always calls `methods->Proceed()`.
- `greeter_failover_client` stdout is a stable interface asserted by the live test script; interceptor output goes through spdlog (already routed to stderr in that binary).
- Build/test commands: `cmake --build build` and `ctest --test-dir build --output-on-failure` (build dir already configured with `-DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local`).

---

### Task 1: MessageLoggingClientInterceptor + GTest

**Files:**
- Create: `include/message_logging_client_interceptor.h`
- Create: `src/interceptor/message_logging_client_interceptor.cpp`
- Test: `tests/interceptor/test_message_logging_client_interceptor.cpp`
- Modify: `CMakeLists.txt` (add source to `message_logging_interceptor` target at line ~149; add test target after `lb_failover_live_tests` block ending at line ~480)

**Interfaces:**
- Consumes: nothing new — gRPC experimental client interceptor API, `helloworld_proto` generated stubs.
- Produces: `MessageLoggingClientInterceptorFactory` (default-constructible, subclass of `grpc::experimental::ClientInterceptorFactoryInterface`) declared in `include/message_logging_client_interceptor.h`. Task 2 constructs it via `std::make_unique<MessageLoggingClientInterceptorFactory>()`.

- [x] **Step 1: Write the failing test**

Create `tests/interceptor/test_message_logging_client_interceptor.cpp`:

```cpp
#include "message_logging_client_interceptor.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "helloworld.grpc.pb.h"

namespace {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

// Minimal echo Greeter for exercising one real unary RPC in-process.
class EchoGreeter : public Greeter::Service {
public:
    Status SayHello(ServerContext* /*context*/, const HelloRequest* request,
                    HelloReply* reply) override {
        reply->set_message("Hello " + request->name());
        return Status::OK;
    }
};

// RAII handle for one in-process gRPC server on an ephemeral port.
struct ServerHandle {
    std::unique_ptr<Server> server;
    std::unique_ptr<EchoGreeter> service;
    int selected_port = 0;
    std::thread runner;

    ~ServerHandle() {
        if (server) server->Shutdown();
        if (runner.joinable()) runner.join();
    }
};

// Starts one server on 127.0.0.1:0. Returns nullptr silently if the bind
// fails — expected in restricted sandboxes, callers GTEST_SKIP (same
// convention as tests/lb/test_failover_live.cpp).
std::unique_ptr<ServerHandle> StartServer() {
    auto handle = std::make_unique<ServerHandle>();
    handle->service = std::make_unique<EchoGreeter>();

    ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(handle->service.get());
    handle->server = builder.BuildAndStart();
    if (!handle->server || selected_port == 0) return nullptr;
    handle->selected_port = selected_port;

    handle->runner = std::thread([s = handle->server.get()] { s->Wait(); });
    return handle;
}

// Swaps the spdlog default logger for an in-memory capture sink; restores the
// previous logger on destruction so other tests are unaffected.
class SpdlogCapture {
public:
    SpdlogCapture()
        : previous_(spdlog::default_logger()),
          stream_(std::make_shared<std::ostringstream>()) {
        auto sink =
            std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream_);
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("capture", std::move(sink)));
    }
    ~SpdlogCapture() { spdlog::set_default_logger(previous_); }
    std::string text() const { return stream_->str(); }

private:
    std::shared_ptr<spdlog::logger> previous_;
    std::shared_ptr<std::ostringstream> stream_;
};

TEST(MessageLoggingClientInterceptor, LogsRequestAndResponseAsJson) {
    auto server = StartServer();
    if (!server) GTEST_SKIP() << "cannot bind a loopback port";

    SpdlogCapture capture;

    std::vector<
        std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
        creators;
    creators.push_back(
        std::make_unique<MessageLoggingClientInterceptorFactory>());
    auto channel = grpc::experimental::CreateCustomChannelWithInterceptors(
        "127.0.0.1:" + std::to_string(server->selected_port),
        grpc::InsecureChannelCredentials(), grpc::ChannelArguments(),
        std::move(creators));
    auto stub = Greeter::NewStub(channel);

    grpc::ClientContext ctx;
    HelloRequest request;
    request.set_name("interceptor-test");
    HelloReply reply;
    const Status status = stub->SayHello(&ctx, request, &reply);

    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(reply.message(), "Hello interceptor-test");

    const std::string logs = capture.text();
    // Method name from ClientRpcInfo::method().
    EXPECT_NE(logs.find("/helloworld.Greeter/SayHello"), std::string::npos)
        << logs;
    // Compact protobuf JSON with preserved (snake_case) field names.
    EXPECT_NE(logs.find("Request message (JSON)"), std::string::npos) << logs;
    EXPECT_NE(logs.find("\"name\":\"interceptor-test\""), std::string::npos)
        << logs;
    EXPECT_NE(logs.find("Response message (JSON)"), std::string::npos) << logs;
    EXPECT_NE(logs.find("\"message\":\"Hello interceptor-test\""),
              std::string::npos)
        << logs;
}

}  // namespace
```

- [x] **Step 2: Add the CMake test target and try to build (expect failure)**

In `CMakeLists.txt`, immediately after the `lb_failover_live_tests` block (which ends with `target_compile_features(lb_failover_live_tests PRIVATE cxx_std_20)` around line 480), add:

```cmake
# Client message logging interceptor test (in-process server, real channel)
add_executable(message_logging_client_interceptor_tests
    tests/interceptor/test_message_logging_client_interceptor.cpp)
target_link_libraries(message_logging_client_interceptor_tests
    PRIVATE GTest::gtest
    PRIVATE GTest::gtest_main
    PRIVATE message_logging_interceptor
    PRIVATE helloworld_proto
)
add_test(NAME message_logging_client_interceptor_tests
         COMMAND message_logging_client_interceptor_tests)
target_compile_features(message_logging_client_interceptor_tests PRIVATE cxx_std_20)
```

(`message_logging_interceptor` already propagates `include/`, `${generated_dir}`, gRPC, protobuf, and spdlog as PUBLIC, so no extra include dirs are needed.)

Run: `cmake --build build --target message_logging_client_interceptor_tests`
Expected: FAIL — `'message_logging_client_interceptor.h' file not found`.

- [x] **Step 3: Write the header**

Create `include/message_logging_client_interceptor.h`:

```cpp
#pragma once

#include <string>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_interceptor.h>
#include <google/protobuf/message.h>

// A gRPC client interceptor that logs the content of unary request and
// response messages for each RPC as compact JSON (snake_case field names).
// Client-side counterpart of MessageLoggingServerInterceptor; unlike the
// server variant it logs no otel trace id — clients using it (e.g.
// greeter_failover_client) set up no tracer, so the id would be all zeros.
class MessageLoggingClientInterceptor : public grpc::experimental::Interceptor {
public:
    explicit MessageLoggingClientInterceptor(const std::string& method_name);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    std::string method_name_;
};

class MessageLoggingClientInterceptorFactory
    : public grpc::experimental::ClientInterceptorFactoryInterface {
public:
    MessageLoggingClientInterceptorFactory() = default;

    grpc::experimental::Interceptor* CreateClientInterceptor(
        grpc::experimental::ClientRpcInfo* info) override;
};
```

- [x] **Step 4: Write the implementation**

Create `src/interceptor/message_logging_client_interceptor.cpp`:

```cpp
#include "message_logging_client_interceptor.h"

#include <grpcpp/impl/codegen/config_protobuf.h>
#include <spdlog/spdlog.h>

using HP = grpc::experimental::InterceptionHookPoints;

namespace {

// Serializes msg as compact JSON with snake_case field names and logs it at
// info; a conversion failure logs at warn instead. Never throws past callers.
void LogMessageJson(const std::string& method_name, const char* direction,
                    const google::protobuf::Message& msg) {
    grpc::protobuf::json::JsonPrintOptions options;
    options.preserve_proto_field_names = true;  // Use snake_case field names

    std::string json_str;
    auto status = grpc::protobuf::json::MessageToJsonString(msg, &json_str, options);
    if (status.ok()) {
        spdlog::info("[{}] {} message (JSON): {}", method_name, direction, json_str);
    } else {
        spdlog::warn("[{}] Failed to convert {} to JSON: {}", method_name,
                     direction, status.ToString());
    }
}

}  // namespace

MessageLoggingClientInterceptor::MessageLoggingClientInterceptor(
    const std::string& method_name)
    : method_name_(method_name) {
}

void MessageLoggingClientInterceptor::Intercept(
    grpc::experimental::InterceptorBatchMethods* methods) {
    // Log the outgoing request before it is sent.
    if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_MESSAGE)) {
        const void* send_msg_ptr = methods->GetSendMessage();
        if (send_msg_ptr) {
            try {
                LogMessageJson(
                    method_name_, "Request",
                    *static_cast<const google::protobuf::Message*>(send_msg_ptr));
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log request message: {}",
                             method_name_, e.what());
            }
        }
    }

    // Log the incoming response after it is received.
    if (methods->QueryInterceptionHookPoint(HP::POST_RECV_MESSAGE)) {
        void* recv_msg_ptr = methods->GetRecvMessage();
        if (recv_msg_ptr) {
            try {
                LogMessageJson(
                    method_name_, "Response",
                    *static_cast<google::protobuf::Message*>(recv_msg_ptr));
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Failed to log response message: {}",
                             method_name_, e.what());
            }
        }
    }

    methods->Proceed();
}

grpc::experimental::Interceptor*
MessageLoggingClientInterceptorFactory::CreateClientInterceptor(
    grpc::experimental::ClientRpcInfo* info) {
    if (!info || !info->method()) {
        return nullptr;
    }
    return new MessageLoggingClientInterceptor(info->method());
}
```

- [x] **Step 5: Add the source to the library target**

In `CMakeLists.txt`, extend the `message_logging_interceptor` library (currently at line ~149):

```cmake
# Message logging interceptor library
add_library(message_logging_interceptor
        src/interceptor/message_logging_interceptor.cpp
        src/interceptor/message_logging_client_interceptor.cpp
)
```

(Leave the rest of the target — include dirs, links, `cxx_std_20` — unchanged.)

- [x] **Step 6: Build and run the test**

Run:
```bash
cmake --build build --target message_logging_client_interceptor_tests
./build/message_logging_client_interceptor_tests
```
Expected: `[  PASSED  ] 1 test.` (or SKIPPED in a sandbox that denies loopback binds — in that case verify the pass via manual inspection is deferred to Task 2's demo run).

- [x] **Step 7: Run the full suite to check for regressions**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (Db2-gated tests skip as usual).

- [x] **Step 8: Commit**

```bash
git add include/message_logging_client_interceptor.h \
        src/interceptor/message_logging_client_interceptor.cpp \
        tests/interceptor/test_message_logging_client_interceptor.cpp \
        CMakeLists.txt
git commit -m "feat: add client-side message logging interceptor

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Wire the interceptor into greeter_failover_client

**Files:**
- Modify: `src/greeter_failover_client.cpp` (includes at lines 1-11; client construction at lines 54-55)
- Modify: `CMakeLists.txt` (the `greeter_failover_client` link block at lines ~289-292)

**Interfaces:**
- Consumes: `MessageLoggingClientInterceptorFactory` from Task 1 (`include/message_logging_client_interceptor.h`); `lb::ChannelBuilder` = `std::function<std::shared_ptr<grpc::Channel>(const lb::Endpoint&, const grpc::ChannelArguments&)>` and `lb::FailoverClient<Greeter>::FromEnv(const StubFactory&, ChannelBuilder)` from `src/lb/failover_client.h`; `lb::Endpoint::Target()` from `src/lb/endpoint_config.h`.
- Produces: nothing consumed by later tasks (final task).

- [x] **Step 1: Link the interceptor library**

In `CMakeLists.txt`, extend the existing block:

```cmake
# Failover LB demo client (no DB2)
create_grpc_executable(greeter_failover_client "src/greeter_failover_client.cpp")
target_link_libraries(greeter_failover_client
    PRIVATE grpc_client_lb
    PRIVATE message_logging_interceptor
)
```

- [x] **Step 2: Attach the interceptor via a custom ChannelBuilder**

In `src/greeter_failover_client.cpp`, add to the includes (after `#include <grpcpp/grpcpp.h>` and before the spdlog includes keep alphabetical grouping as-is):

```cpp
#include <memory>
#include <utility>
#include <vector>

#include "message_logging_client_interceptor.h"
```

(`<memory>`, `<utility>`, `<vector>` go in the existing std includes block at lines 5-8; the project include goes next to `"lb/failover_client.h"`.)

Replace the client construction (lines 54-55):

```cpp
    lb::FailoverClient<Greeter> client =
        lb::FailoverClient<Greeter>::FromEnv();
```

with:

```cpp
    // Same target/credentials/args as the default builder, plus a per-RPC
    // message logging interceptor (request/response JSON on stderr).
    lb::ChannelBuilder logging_channel_builder =
        [](const lb::Endpoint& endpoint, const grpc::ChannelArguments& args) {
            std::vector<std::unique_ptr<
                grpc::experimental::ClientInterceptorFactoryInterface>>
                creators;
            creators.push_back(
                std::make_unique<MessageLoggingClientInterceptorFactory>());
            return grpc::experimental::CreateCustomChannelWithInterceptors(
                endpoint.Target(), grpc::InsecureChannelCredentials(), args,
                std::move(creators));
        };

    lb::FailoverClient<Greeter> client = lb::FailoverClient<Greeter>::FromEnv(
        [](std::shared_ptr<grpc::Channel> channel) {
            return Greeter::NewStub(std::move(channel));
        },
        std::move(logging_channel_builder));
```

- [x] **Step 3: Build**

Run: `cmake --build build --target greeter_failover_client`
Expected: builds without warnings about the new code.

- [x] **Step 4: Manual verification against a live server**

```bash
./build/greeter_callback_server_no_db2 &> /tmp/greeter_server.log &
sleep 1
GRPC_TARGET_ENDPOINTS=127.0.0.1:50051 ./build/greeter_failover_client 2 100 \
    2> /tmp/failover_client.err
echo "exit=$?"
grep "Request message (JSON)" /tmp/failover_client.err
grep "Response message (JSON)" /tmp/failover_client.err
kill %1
```

Expected: `exit=0`; stdout shows two `Greeter received: ...` lines; stderr contains, per call, one `[/helloworld.Greeter/SayHello] Request message (JSON): {"name":"賴柔瑤"}` line and one `Response message (JSON)` line. Stdout must contain no interceptor lines (stable interface for the live test script).

Note: `greeter_callback_server_no_db2` applies a string-transform interceptor, so the reply text may differ from a plain echo — only the presence of the request/response JSON log lines and exit 0 matter here.

- [x] **Step 5: Run the full suite once more**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (Db2-gated tests skip as usual).

- [x] **Step 6: Commit**

```bash
git add src/greeter_failover_client.cpp CMakeLists.txt
git commit -m "feat: log request/response JSON in greeter_failover_client

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
