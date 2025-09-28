#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <chrono>

#include <iostream>
#include <memory>
#include <string>
#include <algorithm>
#include "byte_logging.h"

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif
#include "spdlog/spdlog.h"
#include "string_transform_interceptor.h"

using grpc::CallbackServerContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerUnaryReactor;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class SimpleGreeterServiceImpl final : public Greeter::CallbackService {
 public:
  SimpleGreeterServiceImpl() = default;
      
  ServerUnaryReactor* SayHello(CallbackServerContext* context,
                               const HelloRequest* request,
                               HelloReply* reply) override {
    spdlog::info("Received request for name: {}", request->name());

    // Log the raw bytes of the name in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request->name(), "Name bytes (hex)");

    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());

    ServerUnaryReactor* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }
};

void RunServer(uint16_t port) {
  std::string server_address = "0.0.0.0:" + std::to_string(port);

  auto interceptor_factory = std::make_unique<StringTransformServerInterceptorFactory>();
  
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

  SimpleGreeterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  
  // Register the string transformation interceptor factory
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
  interceptors.push_back(std::move(interceptor_factory));
  builder.experimental().SetInterceptorCreators(std::move(interceptors));
  
  std::unique_ptr<Server> server(builder.BuildAndStart());
  spdlog::info("Server listening on {}", server_address);

  server->Wait();
}

int main(int argc, char** argv) {
  uint16_t port = 50051;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  RunServer(port);
  return 0;
}