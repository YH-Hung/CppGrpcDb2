#include "calldata_metrics.h"

CallDataMetrics::CallDataMetrics(const std::shared_ptr<prometheus::Registry>& registry) {
    // Create metric families with labels
    auto& counter_family = prometheus::BuildCounter()
        .Name("grpc_requests_total")
        .Help("Total number of gRPC requests")
        .Register(*registry);

    auto& duration_family = prometheus::BuildHistogram()
        .Name("grpc_request_duration_seconds")
        .Help("Total gRPC request duration in seconds")
        .Register(*registry);

    auto& processing_family = prometheus::BuildHistogram()
        .Name("grpc_processing_duration_seconds")
        .Help("Business logic processing duration in seconds")
        .Register(*registry);

    auto& request_size_family = prometheus::BuildHistogram()
        .Name("grpc_request_size_bytes")
        .Help("gRPC request size in bytes")
        .Register(*registry);

    auto& response_size_family = prometheus::BuildHistogram()
        .Name("grpc_response_size_bytes")
        .Help("gRPC response size in bytes")
        .Register(*registry);

    // Store family pointers in shared metrics struct
    shared_metrics_.request_counter_family = &counter_family;
    shared_metrics_.duration_histogram_family = &duration_family;
    shared_metrics_.processing_histogram_family = &processing_family;
    shared_metrics_.request_size_histogram_family = &request_size_family;
    shared_metrics_.response_size_histogram_family = &response_size_family;
}
