#include <iostream>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <chrono>
#include "helloworld.pb.h"
#include "helloworld.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service {
    public:
    explicit GreeterServiceImpl(std::shared_ptr<prometheus::Registry> registry)
        : request_counter_(
              &prometheus::BuildCounter()
                   .Name("grpc_requests_total")
                   .Help("Total number of gRPC requests")
                   .Register(*registry)
                   .Add({{"method", "SayHello"}})),
          duration_histogram_(
              &prometheus::BuildHistogram()
                   .Name("grpc_request_duration_seconds")
                   .Help("gRPC request duration in seconds")
                   .Register(*registry)
                   .Add({{"method", "SayHello"}},
                        std::vector<double>{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0})) {}

    Status SayHello(ServerContext* context, const HelloRequest* request,
                          HelloReply* reply) override {
        auto start_time = std::chrono::steady_clock::now();
        request_counter_->Increment();

        std::string prefix("Hello ");
        reply->set_message(prefix + request->name());

        auto end_time = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed_seconds = end_time - start_time;
        duration_histogram_->Observe(elapsed_seconds.count());
        return Status::OK;
    }
  private:
    prometheus::Counter* request_counter_;
    prometheus::Histogram* duration_histogram_;
};

void RunServer(uint16_t port) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    // Create Prometheus exposer on localhost:8124 and a registry
    auto exposer = std::make_unique<prometheus::Exposer>("127.0.0.1:8124");
    auto registry = std::make_shared<prometheus::Registry>();
    exposer->RegisterCollectable(registry);

    GreeterServiceImpl service(registry);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {
    RunServer(50051);
    return 0;
}