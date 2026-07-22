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

TEST(MessageLoggingClientInterceptor, HashesSensitiveRequestFields) {
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
    request.set_name("alice");
    request.set_password("hunter2");
    HelloReply reply;
    const Status status = stub->SayHello(&ctx, request, &reply);

    ASSERT_TRUE(status.ok()) << status.error_message();

    const std::string logs = capture.text();
    // printf '%s' hunter2 | shasum -a 256
    EXPECT_NE(
        logs.find("\"password\":\"f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb"
                  "29541f94bcf526a3f6c7\""),
        std::string::npos)
        << logs;
    EXPECT_EQ(logs.find("hunter2"), std::string::npos) << logs;
    EXPECT_NE(logs.find("\"name\":\"alice\""), std::string::npos) << logs;
}

}  // namespace
