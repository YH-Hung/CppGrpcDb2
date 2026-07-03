#include "lb/failover_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lb/channel_factory.h"
#include "lb/endpoint_config.h"

namespace {

using grpc::Status;
using grpc::StatusCode;

struct FakeReq {
    std::string name;
};
struct FakeResp {
    std::string message;
};

// Fake gRPC service mirroring the generated convention: static NewStub
// returning unique_ptr<Stub>, nested Stub with a unary method whose signature
// matches the real gRPC shape: Status(ClientContext*, const Req&, Resp*).
// SayHello is virtual so per-test behavior subclasses can override it.
class FakeService {
public:
    class Stub {
    public:
        explicit Stub(std::shared_ptr<grpc::Channel>) {}
        virtual ~Stub() = default;
        virtual Status SayHello(grpc::ClientContext* /*ctx*/, const FakeReq& req,
                                FakeResp* resp) {
            resp->message = "hello " + req.name;
            return Status::OK;
        }
    };

    static std::unique_ptr<Stub> NewStub(std::shared_ptr<grpc::Channel> channel) {
        return std::make_unique<Stub>(channel);
    }
};

using FakeStub = FakeService::Stub;

// Stub subclass whose SayHello delegates to a per-instance callable. Used so
// the test can encode per-endpoint behavior (gRPC's real Stub is final, but
// our fake Stub is not, so this is fine).
class BehaviorStub : public FakeStub {
public:
    explicit BehaviorStub(std::shared_ptr<grpc::Channel> ch) : FakeStub(ch) {}
    Status SayHello(grpc::ClientContext* ctx, const FakeReq& req,
                    FakeResp* resp) override {
        if (on_call) return on_call(ctx, req, resp);
        return FakeStub::SayHello(ctx, req, resp);
    }
    std::function<Status(grpc::ClientContext*, const FakeReq&, FakeResp*)> on_call;
};

using Behavior = std::function<Status(grpc::ClientContext*, const FakeReq&, FakeResp*)>;

// Builds a three-endpoint FailoverClient where endpoint i uses behaviors[i].
struct FakeClient {
    std::unique_ptr<lb::FailoverClient<FakeService>> client;

    static FakeClient Make(std::vector<Behavior> behaviors) {
        lb::LbConfig config;
        config.endpoints = {{"svc-a.example.com", 50051},
                            {"svc-b.example.com", 50051},
                            {"svc-c.example.com", 50051}};
        config.max_attempts = static_cast<int>(config.endpoints.size());

        auto index = std::make_shared<std::size_t>(0);
        auto behaviors_ptr =
            std::make_shared<std::vector<Behavior>>(std::move(behaviors));
        auto stub_factory = [index, behaviors_ptr](
                                std::shared_ptr<grpc::Channel> ch) {
            auto stub = std::make_unique<BehaviorStub>(ch);
            std::size_t i = (*index)++;
            if (i < behaviors_ptr->size()) {
                stub->on_call = [behaviors_ptr, i](grpc::ClientContext* ctx,
                                                   const FakeReq& req,
                                                   FakeResp* resp) {
                    return (*behaviors_ptr)[i](ctx, req, resp);
                };
            }
            return stub;
        };
        auto channel_builder = [](const lb::Endpoint&, const grpc::ChannelArguments&) {
            return std::shared_ptr<grpc::Channel>{nullptr};
        };
        FakeClient out;
        out.client = std::make_unique<lb::FailoverClient<FakeService>>(
            std::move(config), stub_factory, channel_builder);
        return out;
    }
};

Behavior Unavailable() {
    return [](grpc::ClientContext*, const FakeReq&, FakeResp*) {
        return Status(StatusCode::UNAVAILABLE, "down");
    };
}
Behavior Ok() {
    return [](grpc::ClientContext*, const FakeReq& req, FakeResp* resp) {
        resp->message = "hello " + req.name;
        return Status::OK;
    };
}
Behavior AppError() {
    return [](grpc::ClientContext*, const FakeReq&, FakeResp*) {
        return Status(StatusCode::INVALID_ARGUMENT, "bad request");
    };
}

TEST(FailoverClient, ServedByPopulatedOnSuccess) {
    auto fc = FakeClient::Make({Unavailable(), Ok(), Ok()});
    FakeReq req{"world"};
    FakeResp resp;
    const lb::CallResult result =
        fc.client->Call(req, resp, &FakeStub::SayHello);
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(result.served_by, "svc-b.example.com:50051");
}

TEST(FailoverClient, ServedByEmptyOnTotalFailure) {
    auto fc = FakeClient::Make({Unavailable(), Unavailable(), Unavailable()});
    FakeReq req{"world"};
    FakeResp resp;
    const lb::CallResult result =
        fc.client->Call(req, resp, &FakeStub::SayHello);
    EXPECT_EQ(result.status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_TRUE(result.served_by.empty());
}

TEST(FailoverClient, DoesNotFailOverOnApplicationError) {
    int calls = 0;
    auto fc = FakeClient::Make(
        {[&calls](grpc::ClientContext*, const FakeReq&, FakeResp*) {
             ++calls;
             return Status(StatusCode::INVALID_ARGUMENT, "bad");
         },
         Ok(), Ok()});
    FakeReq req{"world"};
    FakeResp resp;
    const lb::CallResult result =
        fc.client->Call(req, resp, &FakeStub::SayHello);
    EXPECT_EQ(result.status.error_code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(result.served_by.empty());
}

TEST(FailoverClient, AttemptCapHonored) {
    lb::LbConfig config;
    config.endpoints = {{"a.example.com", 1}, {"b.example.com", 2}, {"c.example.com", 3}};
    config.max_attempts = 1;
    int calls = 0;
    auto stub_factory = [&calls](std::shared_ptr<grpc::Channel> ch) {
        auto stub = std::make_unique<BehaviorStub>(ch);
        stub->on_call = [&calls](grpc::ClientContext*, const FakeReq&, FakeResp*) {
            ++calls;
            return Status(StatusCode::UNAVAILABLE, "down");
        };
        return stub;
    };
    auto channel_builder = [](const lb::Endpoint&, const grpc::ChannelArguments&) {
        return std::shared_ptr<grpc::Channel>{nullptr};
    };
    lb::FailoverClient<FakeService> client(std::move(config), stub_factory,
                                           channel_builder);
    FakeReq req{"x"};
    FakeResp resp;
    const lb::CallResult result = client.Call(req, resp, &FakeStub::SayHello);
    EXPECT_EQ(result.status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_EQ(calls, 1);
}

TEST(FailoverClient, LambdaFormDelegatesToTypedStub) {
    auto fc = FakeClient::Make({Unavailable(), Ok(), Ok()});
    FakeReq req{"world"};
    FakeResp resp;
    int lambda_calls = 0;
    const lb::CallResult result = fc.client->Call(
        req, resp,
        [&lambda_calls](FakeStub& /*stub*/, grpc::ClientContext& /*ctx*/,
                        const FakeReq& /*r*/, FakeResp& /*rp*/) {
            ++lambda_calls;
            return Status::OK;
        });
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(lambda_calls, 1);
}

TEST(FailoverClient, EndpointCountAndIntrospection) {
    auto fc = FakeClient::Make({Ok(), Ok(), Ok()});
    EXPECT_EQ(fc.client->endpoint_count(), 3u);
    EXPECT_EQ(fc.client->endpoints()[1].Target(), "svc-b.example.com:50051");
}

TEST(FailoverClient, FailoverOptionsWiredFromConfig) {
    lb::LbConfig config;
    config.endpoints = {{"a.example.com", 1}, {"b.example.com", 2}};
    config.max_attempts = 2;
    config.attempt_timeout = std::chrono::milliseconds(750);
    auto stub_factory = [](std::shared_ptr<grpc::Channel> ch) {
        return std::make_unique<FakeStub>(ch);
    };
    auto channel_builder = [](const lb::Endpoint&, const grpc::ChannelArguments&) {
        return std::shared_ptr<grpc::Channel>{nullptr};
    };
    lb::FailoverClient<FakeService> client(std::move(config), stub_factory,
                                           channel_builder);
    EXPECT_EQ(client.failover_options().max_attempts, 2);
    EXPECT_EQ(client.failover_options().attempt_timeout,
              std::chrono::milliseconds(750));
}

TEST(FailoverClient, DefaultMaxAttemptsDoesNotDisableFailover) {
    // Explicit construction without setting max_attempts: the default (0) must
    // not silently cap to one attempt. CallWithFailover treats 0 as "all
    // endpoints", so failover is enabled with multiple endpoints.
    lb::LbConfig config;
    config.endpoints = {{"a.example.com", 1}, {"b.example.com", 2}, {"c.example.com", 3}};
    // max_attempts left at default (0)
    auto stub_factory = [](std::shared_ptr<grpc::Channel> ch) {
        return std::make_unique<FakeStub>(ch);
    };
    auto channel_builder = [](const lb::Endpoint&, const grpc::ChannelArguments&) {
        return std::shared_ptr<grpc::Channel>{nullptr};
    };
    lb::FailoverClient<FakeService> client(std::move(config), stub_factory,
                                           channel_builder);
    // 0 is passed through; CallWithFailover resolves it to endpoint_count.
    EXPECT_EQ(client.failover_options().max_attempts, 0);
    // Verify failover actually happens: first endpoint fails, second succeeds.
    int calls = 0;
    auto stub_factory2 = [&calls](std::shared_ptr<grpc::Channel> ch) {
        auto stub = std::make_unique<BehaviorStub>(ch);
        stub->on_call = [&calls](grpc::ClientContext*, const FakeReq&, FakeResp*) {
            ++calls;
            return calls == 1 ? Status(StatusCode::UNAVAILABLE, "down")
                              : Status::OK;
        };
        return stub;
    };
    lb::LbConfig config2;
    config2.endpoints = {{"a.example.com", 1}, {"b.example.com", 2}};
    lb::FailoverClient<FakeService> client2(std::move(config2), stub_factory2,
                                            channel_builder);
    FakeReq req{"x"};
    FakeResp resp;
    const lb::CallResult result = client2.Call(req, resp, &FakeStub::SayHello);
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(calls, 2);  // failed over to the second endpoint
}

}  // namespace
