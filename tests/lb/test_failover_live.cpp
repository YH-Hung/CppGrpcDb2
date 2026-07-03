#include <grpcpp/grpcpp.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "helloworld.grpc.pb.h"
#include "lb/failover_client.h"

namespace {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

// Minimal Greeter service that stamps its own listening port into the reply,
// so the test can correlate CallResult.served_by with the server that actually
// handled the request. Tracks per-server call counts for round-robin checks.
// Supports configurable behavior: a per-call delay, an error status to
// return instead of OK (for deadline / non-retriable-error coverage), and
// a "fail the first N calls" counter (for built-in retry coverage).
class ConfigurableGreeter : public Greeter::Service {
public:
    Status SayHello(ServerContext* /*context*/, const HelloRequest* request,
                    HelloReply* reply) override {
        const int n = ++calls_;
        if (delay_ms_.load() > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delay_ms_.load()));
        }
        // "Fail first N" takes priority over the static error_code.
        if (n <= fail_first_n_.load()) {
            return Status(StatusCode::UNAVAILABLE, "injected transient failure");
        }
        if (error_code_.load() != 0) {
            return Status(static_cast<StatusCode>(error_code_.load()),
                          "injected error");
        }
        reply->set_message("Hello " + request->name() + " from " +
                           std::to_string(port_.load()));
        return Status::OK;
    }

    void set_port(int port) { port_ = port; }
    int port() const { return port_.load(); }
    int calls() const { return calls_.load(); }
    void set_delay_ms(int ms) { delay_ms_ = ms; }
    void set_error_code(int code) { error_code_ = code; }
    void set_fail_first_n(int n) { fail_first_n_ = n; }
    void reset_calls() { calls_ = 0; }

private:
    std::atomic<int> port_{0};
    std::atomic<int> calls_{0};
    std::atomic<int> delay_ms_{0};
    std::atomic<int> error_code_{0};
    std::atomic<int> fail_first_n_{0};
};

// RAII handle for one in-process gRPC server on an ephemeral port.
struct ServerHandle {
    std::unique_ptr<Server> server;
    std::unique_ptr<ConfigurableGreeter> service;
    int selected_port = 0;
    std::thread runner;

    ~ServerHandle() {
        if (server) server->Shutdown();
        if (runner.joinable()) runner.join();
    }
};

// Starts one server on an ephemeral loopback port (127.0.0.1:0) and returns a
// handle. Returns nullptr (silently — no ADD_FAILURE) if BuildAndStart fails
// or no port is selected. Bind denial in restricted environments is an
// expected condition, not a test bug; callers check for null and GTEST_SKIP.
std::unique_ptr<ServerHandle> StartServer() {
    auto handle = std::make_unique<ServerHandle>();
    handle->service = std::make_unique<ConfigurableGreeter>();

    ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(handle->service.get());
    handle->server = builder.BuildAndStart();
    if (!handle->server || selected_port == 0) return nullptr;
    handle->selected_port = selected_port;
    handle->service->set_port(selected_port);

    handle->runner = std::thread([s = handle->server.get()] { s->Wait(); });
    return handle;
}

// Starts (or restarts) a server on a specific previously-bound port.
// Returns nullptr silently on failure (same convention as StartServer).
std::unique_ptr<ServerHandle> StartServerOnPort(int port) {
    auto handle = std::make_unique<ServerHandle>();
    handle->service = std::make_unique<ConfigurableGreeter>();

    ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:" + std::to_string(port),
                             grpc::InsecureServerCredentials(), &selected_port);
    builder.RegisterService(handle->service.get());
    handle->server = builder.BuildAndStart();
    if (!handle->server) return nullptr;
    handle->selected_port = selected_port;
    handle->service->set_port(port);

    handle->runner = std::thread([s = handle->server.get()] { s->Wait(); });
    return handle;
}

std::string Target(int port) { return "127.0.0.1:" + std::to_string(port); }

// Stops a server in-place: shuts down, joins the runner thread, drops the
// server object. The handle remains in the vector (for port reuse on restart).
void StopServer(ServerHandle& h) {
    if (h.server) {
        h.server->Shutdown();
        h.runner.join();
        h.server.reset();
    }
}

class FailoverLiveTest : public ::testing::Test {
protected:
    static constexpr int kServerCount = 3;

    void SetUp() override {
        bool ok = true;
        for (int i = 0; i < kServerCount; ++i) {
            auto handle = StartServer();
            if (!handle) { ok = false; break; }
            handles_.push_back(std::move(handle));
        }
        if (!ok) {
            handles_.clear();  // tear down any partially-started servers
            GTEST_SKIP() << "could not bind ephemeral server port";
            return;
        }
        for (const auto& h : handles_) {
            targets_.push_back(Target(h->selected_port));
        }
    }

    void TearDown() override {
        handles_.clear();
    }

    // Builds an LbConfig from targets_. Fails the test (not throw) if SetUp
    // was skipped and targets_ is empty — a safety net in case GTEST_SKIP
    // from SetUp doesn't prevent the test body from running.
    lb::LbConfig MakeConfig() {
        lb::LbConfig config;
        config.endpoints.clear();
        for (const std::string& t : targets_) {
            const std::size_t colon = t.rfind(':');
            config.endpoints.push_back(
                {t.substr(0, colon), std::stoi(t.substr(colon + 1))});
        }
        return config;
    }

    lb::FailoverClient<Greeter> MakeClient() {
        auto config = MakeConfig();
        if (config.endpoints.empty()) {
            ADD_FAILURE() << "MakeClient called with no endpoints (SetUp skipped?)";
            config.endpoints.push_back({"127.0.0.1", 1});  // dummy to avoid throw
        }
        return lb::FailoverClient<Greeter>(std::move(config),
                                           [](std::shared_ptr<grpc::Channel> ch) {
                                               return Greeter::NewStub(ch);
                                           });
    }

    lb::FailoverClient<Greeter> MakeClientWithAttempts(int max_attempts) {
        auto config = MakeConfig();
        if (config.endpoints.empty()) {
            ADD_FAILURE() << "MakeClientWithAttempts called with no endpoints";
            config.endpoints.push_back({"127.0.0.1", 1});
        }
        config.max_attempts = max_attempts;
        return lb::FailoverClient<Greeter>(std::move(config),
                                           [](std::shared_ptr<grpc::Channel> ch) {
                                               return Greeter::NewStub(ch);
                                           });
    }

    lb::CallResult SayHello(lb::FailoverClient<Greeter>& client,
                            const std::string& name) {
        HelloRequest req;
        req.set_name(name);
        HelloReply reply;
        return client.Call(req, reply, &Greeter::Stub::SayHello);
    }

    std::vector<std::unique_ptr<ServerHandle>> handles_;
    std::vector<std::string> targets_;
};

// 1. Round-robin: 9 sequential calls should hit all 3 servers in strict
// rotation 0,1,2,0,1,2,... with exactly 3 calls per server.
TEST_F(FailoverLiveTest, RoundRobinDistributesAcrossAllServers) {
    auto client = MakeClient();

    std::vector<std::string> served_by;
    for (int i = 0; i < 9; ++i) {
        const lb::CallResult result = SayHello(client, "round-" + std::to_string(i));
        ASSERT_TRUE(result.status.ok()) << result.status.error_message();
        served_by.push_back(result.served_by);
    }

    // Exact rotation order: targets_[0], targets_[1], targets_[2], repeat.
    ASSERT_EQ(served_by.size(), 9u);
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(served_by[i], targets_[i % 3])
            << "call " << i << " served by " << served_by[i]
            << ", expected " << targets_[i % 3];
    }
    // Exact per-server counts.
    for (int s = 0; s < 3; ++s) {
        EXPECT_EQ(handles_[s]->service->calls(), 3)
            << "server " << s << " handled " << handles_[s]->service->calls()
            << " calls, expected 3";
    }
}

// 2. served_by correlates with the reply message port suffix.
TEST_F(FailoverLiveTest, ServedByMatchesReplyPort) {
    auto client = MakeClient();

    for (int i = 0; i < 6; ++i) {
        HelloRequest req;
        req.set_name("match-" + std::to_string(i));
        HelloReply reply;
        const lb::CallResult result =
            client.Call(req, reply, &Greeter::Stub::SayHello);
        ASSERT_TRUE(result.status.ok()) << result.status.error_message();
        const std::size_t colon = result.served_by.rfind(':');
        ASSERT_NE(colon, std::string::npos);
        const std::string port = result.served_by.substr(colon + 1);
        EXPECT_NE(reply.message().rfind("from " + port), std::string::npos)
            << "reply '" << reply.message() << "' does not mention served_by port "
            << port;
    }
}

// 3. Failover: shut down server 0; all subsequent calls succeed, never name the
// dead endpoint, and strictly alternate between the two survivors.
TEST_F(FailoverLiveTest, FailoverSkipsDeadServerAndBalancesSurvivors) {
    auto client = MakeClient();

    // Kill server 0.
    const std::string dead_target = targets_[0];
    StopServer(*handles_[0]);

    // Issue 6 calls; all should succeed via the 2 live endpoints.
    // The first call hits the dead endpoint 0 (UNAVAILABLE), fails over to
    // endpoint 1 (OK). After that the round-robin cursor is at 2, so the
    // remaining calls alternate 2, 1, 2, 1, 2 — strict rotation among the
    // two non-cooldown survivors.
    std::vector<std::string> served_by;
    for (int i = 0; i < 6; ++i) {
        const lb::CallResult result = SayHello(client, "failover-" + std::to_string(i));
        ASSERT_TRUE(result.status.ok()) << result.status.error_message();
        EXPECT_NE(result.served_by, dead_target)
            << "call " << i << " was served by the dead endpoint";
        served_by.push_back(result.served_by);
    }
    // Exact served order: endpoint 1 (after failover from 0), then alternating.
    const std::vector<std::string> expected = {
        targets_[1], targets_[2], targets_[1], targets_[2], targets_[1], targets_[2],
    };
    ASSERT_EQ(served_by.size(), expected.size());
    for (std::size_t i = 0; i < served_by.size(); ++i) {
        EXPECT_EQ(served_by[i], expected[i])
            << "call " << i << " served by " << served_by[i]
            << ", expected " << expected[i];
    }
    // Each survivor handled exactly 3 calls; dead server handled zero.
    EXPECT_EQ(handles_[1]->service->calls(), 3);
    EXPECT_EQ(handles_[2]->service->calls(), 3);
    EXPECT_EQ(handles_[0]->service->calls(), 0);
}

// 4. Cooldown recovery: after killing a server and triggering cooldown,
// verify via snapshot() that it entered cooldown, then restart it and verify
// it rejoins rotation once cooldown lapses. While in cooldown it must be
// skipped (no calls routed to the restarted server until cooldown expires).
TEST_F(FailoverLiveTest, DeadServerRejoinsAfterRestart) {
    auto client = MakeClient();

    // Kill server 0.
    const int dead_port = handles_[0]->selected_port;
    StopServer(*handles_[0]);

    // Issue calls until the dead endpoint is attempted, fails, and enters
    // cooldown. With 3 endpoints and round-robin, endpoint 0 is first.
    // The first call hits endpoint 0, gets UNAVAILABLE, fails over to 1, OK.
    const lb::CallResult first = SayHello(client, "probe-0");
    ASSERT_TRUE(first.status.ok()) << first.status.error_message();
    // Endpoint 0 must now be in cooldown with at least one failure.
    const lb::EndpointManager::Snapshot snap = client.snapshot(0);
    EXPECT_GE(snap.failures, 1u) << "dead endpoint was never attempted";
    EXPECT_TRUE(snap.in_cooldown) << "dead endpoint did not enter cooldown";

    // Restart the dead server on the same port.
    handles_[0] = StartServerOnPort(dead_port);
    if (!handles_[0]) {
        GTEST_SKIP() << "could not rebind port " << dead_port;
        return;
    }
    const int calls_before_cooldown_lapse = handles_[0]->service->calls();

    // While cooldown is active (base 1s), issue a few quick calls — the
    // restarted endpoint should be skipped because it's still in cooldown.
    for (int i = 0; i < 3; ++i) {
        const lb::CallResult result = SayHello(client, "cooldown-" + std::to_string(i));
        ASSERT_TRUE(result.status.ok()) << result.status.error_message();
        EXPECT_NE(result.served_by, Target(dead_port))
            << "call " << i << " was served by the restarted endpoint during cooldown";
    }
    // The restarted server must have handled zero calls while in cooldown.
    EXPECT_EQ(handles_[0]->service->calls(), calls_before_cooldown_lapse)
        << "restarted endpoint received traffic while still in cooldown";

    // Wait for the cooldown (default base 1s) to lapse, then issue enough calls
    // that round-robin revisits the restarted endpoint.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    bool served_by_restarted = false;
    for (int i = 0; i < 12 && !served_by_restarted; ++i) {
        const lb::CallResult result = SayHello(client, "rejoin-" + std::to_string(i));
        ASSERT_TRUE(result.status.ok()) << result.status.error_message();
        if (result.served_by == Target(dead_port)) {
            served_by_restarted = true;
        }
    }

    EXPECT_TRUE(served_by_restarted)
        << "restarted server " << Target(dead_port)
        << " never rejoined rotation after cooldown lapse";
    EXPECT_GT(handles_[0]->service->calls(), 0)
        << "restarted server handled zero calls after rejoining";
}

// 5. All endpoints down: final status is UNAVAILABLE and served_by is empty.
TEST_F(FailoverLiveTest, AllEndpointsDownReturnsUnavailableAndEmptyServedBy) {
    auto client = MakeClient();

    // Kill all servers.
    for (auto& h : handles_) {
        StopServer(*h);
    }

    const lb::CallResult result = SayHello(client, "all-down");
    EXPECT_EQ(result.status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_TRUE(result.served_by.empty());
}

// 6. Non-retriable application error does not fail over: inject
// INVALID_ARGUMENT on server 0; the call returns immediately without trying
// the other endpoints.
TEST_F(FailoverLiveTest, NonRetriableErrorDoesNotFailOver) {
    auto client = MakeClient();

    // Inject INVALID_ARGUMENT on server 0 (the first round-robin target).
    handles_[0]->service->set_error_code(static_cast<int>(StatusCode::INVALID_ARGUMENT));

    HelloRequest req;
    req.set_name("app-error");
    HelloReply reply;
    const lb::CallResult result =
        client.Call(req, reply, &Greeter::Stub::SayHello);

    EXPECT_EQ(result.status.error_code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(result.served_by.empty());
    // Server 0 was the only endpoint tried.
    EXPECT_EQ(handles_[0]->service->calls(), 1);
    // Servers 1 and 2 were NOT tried.
    EXPECT_EQ(handles_[1]->service->calls(), 0);
    EXPECT_EQ(handles_[2]->service->calls(), 0);
}

// 7. max_attempts < endpoint_count: with max_attempts=1, only one endpoint is
// tried even if it fails.
TEST_F(FailoverLiveTest, MaxAttemptsCapsRetriesOnRealChannels) {
    auto client = MakeClientWithAttempts(1);

    // Kill server 0 (the first round-robin target).
    StopServer(*handles_[0]);

    const lb::CallResult result = SayHello(client, "capped");
    // With max_attempts=1, the single attempt to the dead server fails and
    // there is no failover.
    EXPECT_EQ(result.status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_TRUE(result.served_by.empty());
    // Only server 0 was attempted (and failed).
    EXPECT_EQ(handles_[1]->service->calls(), 0);
    EXPECT_EQ(handles_[2]->service->calls(), 0);
}

// 8. Slow endpoint deadline: inject a delay exceeding attempt_timeout on
// server 0. The attempt times out with DEADLINE_EXCEEDED. Since
// DEADLINE_EXCEEDED is not in the default retriable set, the call returns
// immediately without failover.
TEST_F(FailoverLiveTest, SlowEndpointDeadlineExceededDoesNotFailOver) {
    lb::LbConfig config;
    config.endpoints.clear();
    for (const std::string& t : targets_) {
        const std::size_t colon = t.rfind(':');
        config.endpoints.push_back(
            {t.substr(0, colon), std::stoi(t.substr(colon + 1))});
    }
    config.attempt_timeout = std::chrono::milliseconds(200);
    auto client = lb::FailoverClient<Greeter>(std::move(config),
                                               [](std::shared_ptr<grpc::Channel> ch) {
                                                   return Greeter::NewStub(ch);
                                               });

    // Inject a 1s delay on server 0 — far exceeding the 200ms attempt_timeout.
    handles_[0]->service->set_delay_ms(1000);

    const auto t0 = std::chrono::steady_clock::now();
    const lb::CallResult result = SayHello(client, "slow");
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // DEADLINE_EXCEEDED is not retriable by default → no failover.
    EXPECT_EQ(result.status.error_code(), StatusCode::DEADLINE_EXCEEDED);
    EXPECT_TRUE(result.served_by.empty());
    // The call should return around the attempt_timeout, not after the 1s delay.
    EXPECT_LT(elapsed, std::chrono::milliseconds(800));
    // Servers 1 and 2 were NOT tried.
    EXPECT_EQ(handles_[1]->service->calls(), 0);
    EXPECT_EQ(handles_[2]->service->calls(), 0);
}

// 9. Concurrent calls through one FailoverClient: all succeed and every
// endpoint gets traffic.
TEST_F(FailoverLiveTest, ConcurrentCallsThroughOneClient) {
    auto client = MakeClient();

    constexpr int kThreads = 6;
    constexpr int kCallsPerThread = 6;
    std::atomic<int> ok_count{0};
    std::mutex mu;
    std::unordered_set<std::string> all_served_by;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int c = 0; c < kCallsPerThread; ++c) {
                const lb::CallResult result =
                    SayHello(client, "conc-" + std::to_string(t) + "-" +
                                         std::to_string(c));
                if (result.status.ok()) {
                    ++ok_count;
                    std::lock_guard<std::mutex> lock(mu);
                    all_served_by.insert(result.served_by);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(ok_count.load(), kThreads * kCallsPerThread);
    // All 3 endpoints should have served at least one concurrent call.
    EXPECT_EQ(all_served_by.size(), 3u);
    for (const auto& h : handles_) {
        EXPECT_GT(h->service->calls(), 0);
    }
}

// 10. Single-endpoint mode: one server, one channel, one attempt. The call
// succeeds without any app-level failover.
TEST_F(FailoverLiveTest, SingleEndpointSucceedsWithoutFailover) {
    // Tear down the default 3-server fixture; start a single fresh server.
    handles_.clear();
    targets_.clear();

    auto single = StartServer();
    if (!single) {
        GTEST_SKIP() << "could not bind ephemeral server port";
        return;
    }
    const std::string target = Target(single->selected_port);

    lb::LbConfig config;
    config.endpoints = {{"127.0.0.1", single->selected_port}};
    lb::FailoverClient<Greeter> client(std::move(config),
                                       [](std::shared_ptr<grpc::Channel> ch) {
                                           return Greeter::NewStub(ch);
                                       });

    const lb::CallResult result = SayHello(client, "single");
    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.served_by, target);
    EXPECT_EQ(single->service->calls(), 1);
}

// 11. Single-endpoint built-in gRPC retry: with one endpoint, the channel's
// service-config retryPolicy (maxAttempts=4, retriable on UNAVAILABLE) handles
// transient failures transparently — the app-level loop does exactly one
// attempt. Inject 2 transient UNAVAILABLEs on the server; the client should
// still get OK (via built-in retry within the channel), and the server should
// have seen 3 calls (2 failed + 1 succeeded). This proves built-in retry is
// active in single-endpoint mode without any app-level failover.
//
// Note: DNS multi-address round_robin (one FQDN resolving to multiple
// A/AAAA records, balanced by gRPC's round_robin LB policy) is not covered
// here — it requires DNS infrastructure unavailable to an in-process
// localhost test.
TEST_F(FailoverLiveTest, SingleEndpointBuiltInRetryHandlesTransientFailure) {
    handles_.clear();
    targets_.clear();

    auto single = StartServer();
    if (!single) {
        GTEST_SKIP() << "could not bind ephemeral server port";
        return;
    }
    const std::string target = Target(single->selected_port);

    // Fail the first 2 calls, then succeed.
    single->service->set_fail_first_n(2);

    lb::LbConfig config;
    config.endpoints = {{"127.0.0.1", single->selected_port}};
    // Single endpoint → ChannelFactoryOptions.multi_endpoint=false →
    // service-config retryPolicy maxAttempts=4.
    lb::FailoverClient<Greeter> client(std::move(config),
                                       [](std::shared_ptr<grpc::Channel> ch) {
                                           return Greeter::NewStub(ch);
                                       });

    const lb::CallResult result = SayHello(client, "retry");
    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.served_by, target);
    // Server saw 3 calls: 2 transient failures + 1 success. This proves
    // built-in retry happened within the channel — the app-level loop only
    // does 1 attempt with a single endpoint.
    EXPECT_GE(single->service->calls(), 3)
        << "expected >=3 server calls (2 transient + 1 success via built-in retry), "
        << "got " << single->service->calls();
}

}  // namespace
