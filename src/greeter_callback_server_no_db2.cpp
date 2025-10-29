#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <chrono>
#include <thread>
#include <signal.h>
#include <pthread.h>

#include <iostream>
#include <memory>
#include <string>
#include <algorithm>
#include "byte_logging.h"

#ifdef BAZEL_BUILD
#include "examples/protos/hello_girl.grpc.pb.h"
#else
#include "hello_girl.grpc.pb.h"
#endif
#include "spdlog/spdlog.h"
#include "string_transform_interceptor.h"
#include "utf8ansi.h"

using grpc::CallbackServerContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerUnaryReactor;
using grpc::Status;
using hellogirl::GirlGreeter;
using hellogirl::HelloGirlReply;
using hellogirl::HelloGirlRequest;

class SimpleGreeterServiceImpl final : public GirlGreeter::CallbackService {
 public:
  SimpleGreeterServiceImpl() = default;
      
  ServerUnaryReactor* SayHello(CallbackServerContext* context,
                               const HelloGirlRequest* request,
                               HelloGirlReply* reply) override {
    spdlog::info("Received request for name: {}", request->name());

    ServerUnaryReactor* reactor = context->DefaultReactor();
    // Check cancellation before doing any reply work
    if (context->IsCancelled()) {
      spdlog::warn("Request was cancelled by client before processing.");
      reactor->Finish(Status(grpc::StatusCode::CANCELLED, "Request cancelled"));
      return reactor;
    }

    // Log the raw bytes of the name in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request->name(), "Name bytes (hex)");
    // Also log the raw bytes of the spouse in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request->spouse(), "Spouse bytes (hex)");

    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());

    // Compose additional fields as requested
    std::string marriage = request->name() + " is married with " + request->spouse();
    reply->set_marriage(marriage);
    reply->set_size(request->first_round() + 1);

    reactor->Finish(Status::OK);
    return reactor;
  }
};

void RunServer(uint16_t port) {
  std::string server_address = "0.0.0.0:" + std::to_string(port);

  auto interceptor_factory = std::make_unique<StringTransformServerInterceptorFactory>();
  
  interceptor_factory->SetRequestTransform([](const std::string& input) -> std::string {
    spdlog::info("Request transform: utf8 -> big5 for '{}'", input);
    try {
      return utf8ansi::utf8_to_big5(input);
    } catch (const std::exception& e) {
      spdlog::error("utf8_to_big5 failed: {}", e.what());
      return input; // fallback to original
    }
  });
  
  interceptor_factory->SetResponseTransform([](const std::string& input) -> std::string {
    spdlog::info("Response transform: big5 -> utf8");
    try {
      return utf8ansi::big5_to_utf8(input);
    } catch (const std::exception& e) {
      spdlog::error("big5_to_utf8 failed: {}", e.what());
      return input; // fallback to original
    }
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

  // Block SIGINT/SIGTERM in this thread and spawn a waiter thread
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  int mask_rc = pthread_sigmask(SIG_BLOCK, &set, nullptr);
  if (mask_rc != 0) {
    spdlog::warn("pthread_sigmask failed with code {}", mask_rc);
  }

  std::thread signal_thread([&server, set]() mutable {
    int sig = 0;
    int ret = sigwait(&set, &sig);
    if (ret == 0) {
      spdlog::info("Signal {} received. Initiating graceful shutdown...", sig);
      auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);
      server->Shutdown(deadline); // allow in-flight RPCs up to 10s to finish
    } else {
      spdlog::error("sigwait failed with code {}", ret);
    }
  });

  // Wait until shutdown is requested and all RPCs finish or deadline passes
  server->Wait();
  signal_thread.join();
  spdlog::info("Server stopped.");
}

int main(int argc, char** argv) {
  uint16_t port = 50051;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  RunServer(port);
  return 0;
}

// #include <grpcpp/grpcpp.h>
// #include <csignal>
// #include <thread>
// #include <atomic>
// #include <iostream>
//
// std::atomic<bool> stop_requested{false};
//
// void signal_waiter(grpc::Server* server) {
//   sigset_t sigset;
//   sigemptyset(&sigset);
//   sigaddset(&sigset, SIGINT);
//   sigaddset(&sigset, SIGTERM);
//
//   int sig;
//   // Wait until a signal is received
//   sigwait(&sigset, &sig);
//
//   std::cout << "Received termination signal, shutting down gRPC server..." << std::endl;
//
//   // Gracefully shut down — allow active RPCs to finish
//   auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
//   server->Shutdown(deadline);
//   stop_requested = true;
// }
//
// int main() {
//   // Block SIGINT/SIGTERM in main thread, will handle them in signal_waiter
//   sigset_t sigset;
//   sigemptyset(&sigset);
//   sigaddset(&sigset, SIGINT);
//   sigaddset(&sigset, SIGTERM);
//   pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
//
//   grpc::ServerBuilder builder;
//   builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
//   // builder.RegisterService(&service); // your service
//
//   std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
//   std::cout << "Server started, waiting for SIGTERM..." << std::endl;
//
//   // Thread dedicated to signal waiting
//   std::thread watcher(signal_waiter, server.get());
//
//   // Main thread blocks until Shutdown() is called
//   server->Wait();
//
//   watcher.join();
//   std::cout << "Server exited cleanly." << std::endl;
//   return 0;
// }