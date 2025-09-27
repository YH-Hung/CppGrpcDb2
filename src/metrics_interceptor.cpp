#include "metrics_interceptor.h"

using HP = grpc::experimental::InterceptionHookPoints;

void MetricsServerInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    if (methods->QueryInterceptionHookPoint(HP::POST_RECV_INITIAL_METADATA)) {
        // RPC effectively started; mark the time
        start_time_ = std::chrono::steady_clock::now();
        // Increment requests on start to reflect attempted calls
        if (metrics_.request_counter) {
            metrics_.request_counter->Increment();
        }
    }

    if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_STATUS)) {
        if (metrics_.duration_histogram && start_time_.time_since_epoch().count() != 0) {
            auto end_time = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time_;
            metrics_.duration_histogram->Observe(elapsed.count());
        }
    }

    methods->Proceed();
}

MetricsServerInterceptorFactory::MetricsServerInterceptorFactory(const std::shared_ptr<prometheus::Registry>& registry) {
    // Create metric families and a single child each (no labels to keep it simple and low-cardinality)
    auto& counter_family = prometheus::BuildCounter()
        .Name("grpc_requests_total")
        .Help("Total number of gRPC requests")
        .Register(*registry);

    auto& hist_family = prometheus::BuildHistogram()
        .Name("grpc_request_duration_seconds")
        .Help("gRPC request duration in seconds")
        .Register(*registry);

    request_counter_ = &counter_family.Add({});
    duration_histogram_ = &hist_family.Add({}, std::vector<double>{
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0
    });
}

grpc::experimental::Interceptor* MetricsServerInterceptorFactory::CreateServerInterceptor(
    grpc::experimental::ServerRpcInfo* /*info*/) {
    MetricsServerInterceptor::SharedMetrics shared{request_counter_, duration_histogram_};
    return new MetricsServerInterceptor(shared);
}
