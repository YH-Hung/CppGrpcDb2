#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <chrono>

#include <iostream>
#include <memory>
#include <string>
#include <algorithm>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif
#include "spdlog/spdlog.h"
#include "string_transform_interceptor.h"
#include "metrics_interceptor.h"

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

using grpc::CallbackServerContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerUnaryReactor;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::CallbackService {
 public:
  GreeterServiceImpl() = default;

  ServerUnaryReactor* SayHello(CallbackServerContext* context,
                               const HelloRequest* request,
                               HelloReply* reply) override {
    spdlog::info("Received request for name: {}", request->name());
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());

    ServerUnaryReactor* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }
};

void RunServer(uint16_t port) {
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
  // Create Prometheus exposer on localhost:8124 and a registry
  auto exposer = std::make_unique<prometheus::Exposer>("127.0.0.1:8124");
  auto registry = std::make_shared<prometheus::Registry>();
  exposer->RegisterCollectable(registry);

  // Create string transformation interceptor factory
  auto interceptor_factory = std::make_unique<StringTransformServerInterceptorFactory>();
  
  // Set example transformation lambdas
  interceptor_factory->SetRequestTransform([](const std::string& input) -> std::string {
    spdlog::info("Request transform: Uppercasing '{}'", input);
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
  });
  
  interceptor_factory->SetResponseTransform([](const std::string& input) -> std::string {
    spdlog::info("Response transform: Adding prefix to '{}'", input);
    return "[TRANSFORMED] " + input;
  });

  GreeterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  
  // Register the string transformation interceptor factory and metrics factory
  auto metrics_factory = std::make_unique<MetricsServerInterceptorFactory>(registry);
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
  interceptors.push_back(std::move(interceptor_factory));
  interceptors.push_back(std::move(metrics_factory));
  builder.experimental().SetInterceptorCreators(std::move(interceptors));
  
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  spdlog::info("Server listening on {}", server_address);

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer(50051);
  return 0;
}