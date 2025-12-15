#include "calldata_metrics.h"

CallDataMetrics::CallDataMetrics(const std::shared_ptr<prometheus::Registry>& registry)
    : request_counter_family(prometheus::BuildCounter()
                                 .Name("grpc_requests_total")
                                 .Help("Total number of gRPC requests")
                                 .Register(*registry)),
      duration_histogram_family(prometheus::BuildHistogram()
                                    .Name("grpc_request_duration_seconds")
                                    .Help("Total gRPC request duration in seconds")
                                    .Register(*registry)),
      processing_histogram_family(prometheus::BuildHistogram()
                                      .Name("grpc_processing_duration_seconds")
                                      .Help("Business logic processing duration in seconds")
                                      .Register(*registry)),
      request_size_histogram_family(prometheus::BuildHistogram()
                                        .Name("grpc_request_size_bytes")
                                        .Help("gRPC request size in bytes")
                                        .Register(*registry)),
      response_size_histogram_family(prometheus::BuildHistogram()
                                         .Name("grpc_response_size_bytes")
                                         .Help("gRPC response size in bytes")
                                         .Register(*registry)) {}
