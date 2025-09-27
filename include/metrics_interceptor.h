#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <chrono>
#include <memory>

// A simple gRPC server interceptor that records a request counter and
// a request duration histogram for every RPC.
// Metrics are shared across all interceptor instances created by the factory.
class MetricsServerInterceptor : public grpc::experimental::Interceptor {
public:
    struct SharedMetrics {
        prometheus::Counter* request_counter{nullptr};
        prometheus::Histogram* duration_histogram{nullptr};
    };

    explicit MetricsServerInterceptor(SharedMetrics shared)
        : metrics_(shared) {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    SharedMetrics metrics_{};
    std::chrono::steady_clock::time_point start_time_{};
};

class MetricsServerInterceptorFactory : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    explicit MetricsServerInterceptorFactory(const std::shared_ptr<prometheus::Registry>& registry);

    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;

private:
    prometheus::Counter* request_counter_{nullptr};
    prometheus::Histogram* duration_histogram_{nullptr};
};
