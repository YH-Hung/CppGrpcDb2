#pragma once

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lb/channel_factory.h"
#include "lb/endpoint_config.h"
#include "lb/endpoint_manager.h"
#include "lb/failover_call.h"

namespace lb {

struct CallResult {
    grpc::Status status;
    // Endpoint target that handled the call (e.g. "svc-a:50051"). Empty when
    // no endpoint succeeded, so callers can't accidentally log a stale target.
    std::string served_by;
};

// Non-template base owning the channels, EndpointManager, and pre-wired
// FailoverOptions. The template subclass adds typed stubs. The EndpointManager
// is held via unique_ptr because it contains a mutex and is therefore
// immovable; this keeps the facade movable so FromEnv() can return by value.
class FailoverClientBase {
public:
    // Logs rejected entries and the accepted endpoint list, builds channels
    // via BuildChannels (computing multi_endpoint internally), constructs the
    // EndpointManager with kDefaultCooldownCap, and forwards
    // config.max_attempts into failover_options_.
    FailoverClientBase(LbConfig config, ChannelBuilder channel_builder);
    FailoverClientBase(FailoverClientBase&&) noexcept = default;
    FailoverClientBase& operator=(FailoverClientBase&&) noexcept = default;
    FailoverClientBase(const FailoverClientBase&) = delete;
    FailoverClientBase& operator=(const FailoverClientBase&) = delete;
    ~FailoverClientBase() = default;

    // Runs CallWithFailover against the index-based rpc lambda and resolves
    // served_by from endpoints_[index].Target() on success.
    template <typename Fn>
    CallResult CallIndexed(Fn&& rpc) {
        std::size_t served_index = 0;
        grpc::Status status = CallWithFailover(
            *manager_, failover_options_,
            [&](grpc::ClientContext& ctx, std::size_t index) {
                served_index = index;
                return rpc(ctx, index);
            });
        return {std::move(status),
                status.ok() ? endpoints_[served_index].Target() : std::string{}};
    }

    std::size_t endpoint_count() const { return endpoints_.size(); }
    const std::vector<Endpoint>& endpoints() const { return endpoints_; }
    EndpointManager::Snapshot snapshot(std::size_t index) const {
        return manager_->GetSnapshot(index);
    }
    const FailoverOptions& failover_options() const { return failover_options_; }

protected:
    std::vector<Endpoint> endpoints_;
    std::vector<std::shared_ptr<grpc::Channel>> channels_;
    std::unique_ptr<EndpointManager> manager_;
    FailoverOptions failover_options_;
};

// High-level typed failover client. Owns one channel + stub per endpoint and
// an adaptive EndpointManager. Each Call() issues one logical RPC, failing
// over to the next endpoint on a retriable status.
//
// ServiceT is a gRPC generated service type (e.g. helloworld::Greeter) that
// exposes a static NewStub(std::shared_ptr<grpc::ChannelInterface>) factory
// and a nested Stub class — the universal gRPC convention.
template <typename ServiceT>
class FailoverClient : public FailoverClientBase {
public:
    using StubT = typename ServiceT::Stub;
    using StubFactory =
        std::function<std::unique_ptr<StubT>(std::shared_ptr<grpc::Channel>)>;

    // Default construction: reads GRPC_* env vars via LoadLbConfigFromEnv,
    // uses InsecureChannelCredentials and ServiceT::NewStub.
    static FailoverClient FromEnv() {
        return FromEnv(DefaultStubFactory(), nullptr);
    }

    // Same, with a custom stub factory and/or channel builder (TLS, fakes).
    static FailoverClient FromEnv(const StubFactory& stub_factory,
                                  ChannelBuilder channel_builder = nullptr) {
        return FailoverClient(LoadLbConfigFromEnv(), stub_factory,
                              std::move(channel_builder));
    }

    // Explicit construction for tests / fixed configs. Builds channels via
    // channel_builder (nullptr = InsecureChannelCredentials) and stubs via
    // stub_factory.
    FailoverClient(LbConfig config, const StubFactory& stub_factory,
                   ChannelBuilder channel_builder = nullptr)
        : FailoverClientBase(std::move(config), std::move(channel_builder)) {
        stubs_.reserve(channels_.size());
        for (const std::shared_ptr<grpc::Channel>& channel : channels_) {
            stubs_.push_back(stub_factory(channel));
        }
    }

    // Lambda form: rpc(stub, context, request, response) -> grpc::Status.
    // The ClientContext is created and owned inside the call, so this works for
    // unary sync RPCs and for streaming RPCs where the entire stream is
    // consumed inside the lambda. To return a live stream object to the caller,
    // use the low-level EndpointManager + stubs() directly instead.
    template <typename Req, typename Resp, typename Fn>
    CallResult Call(const Req& request, Resp& response, Fn&& rpc) {
        return CallIndexed([&](grpc::ClientContext& ctx, std::size_t index) {
            return rpc(*stubs_[index], ctx, request, response);
        });
    }

    // Pointer-to-member convenience for unary sync RPCs. The common case
    // collapses to: client.Call(req, reply, &Greeter::Stub::SayHello);
    template <typename Req, typename Resp>
    CallResult Call(const Req& request, Resp& response,
                    grpc::Status (StubT::*method)(grpc::ClientContext*,
                                                  const Req&, Resp*)) {
        return Call(request, response,
                    [method](StubT& stub, grpc::ClientContext& ctx,
                             const Req& req, Resp& resp) {
                        return (stub.*method)(&ctx, req, &resp);
                    });
    }

    const std::vector<std::unique_ptr<StubT>>& stubs() const { return stubs_; }

private:
    std::vector<std::unique_ptr<StubT>> stubs_;

    static StubFactory DefaultStubFactory() {
        return [](std::shared_ptr<grpc::Channel> channel) {
            return ServiceT::NewStub(std::move(channel));
        };
    }
};

}  // namespace lb
