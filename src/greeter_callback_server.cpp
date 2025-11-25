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

#include "db2/db2.hpp"
#include "resource/resource_pool.hpp"

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif
#include "spdlog/spdlog.h"
#include "string_transform_interceptor.h"
#include "metrics_interceptor.h"
#include "tracing/tracer_provider.h"
#include "tracing/grpc_tracing_interceptor.h"
#include "tracing/trace_log_formatter.h"

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
  using Db2Pool = resource::ResourcePool<db2::Connection>;

  explicit GreeterServiceImpl(std::shared_ptr<Db2Pool> pool)
      : pool_(std::move(pool)) {}

  ServerUnaryReactor* SayHello(CallbackServerContext* context,
                               const HelloRequest* request,
                               HelloReply* reply) override {
    spdlog::info("Received request for name: {}", request->name());

    ServerUnaryReactor* reactor = context->DefaultReactor();
    // Check cancellation before doing any reply work
    if (context->IsCancelled()) {
      spdlog::warn("Request was cancelled by client before processing.");
      reactor->Finish(Status(grpc::StatusCode::CANCELLED, "Request cancelled"));
      return reactor;
    }

    // Acquire a DB2 Connection from the shared pool just for demonstration
    // It will be returned to the pool automatically when it goes out of scope.
    std::shared_ptr<db2::Connection> conn;
    if (pool_) {
      try {
        conn = pool_->acquire();
        spdlog::info("Acquired DB2 resource from pool. in_use={}, idle={}",
                     pool_->in_use(), pool_->idle_size());
        // Demonstration only: no actual DB operations are performed.
      } catch (const std::exception& e) {
        spdlog::error("Failed to acquire DB2 resource: {}", e.what());
      }
    } else {
      spdlog::warn("DB2 pool not available; proceeding without DB resource.");
    }

    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());

    reactor->Finish(Status::OK);
    return reactor;
  }
 private:
  std::shared_ptr<Db2Pool> pool_;
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

  // Create a shared resource pool of db2::Connection objects.
  // This demonstrates pooling only; no actual DB connection is performed.
  using Db2Pool = resource::ResourcePool<db2::Connection>;
  auto db2_pool = Db2Pool::create(
      /*max_size=*/8,
      []() {
        // Construct a Connection object; do NOT call connect_* in this demo.
        return std::make_unique<db2::Connection>();
      }
      // No validator provided to avoid requiring a real DB connection.
  );

  GreeterServiceImpl service(db2_pool);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  
  // Register interceptor factories (tracing + string transformation + metrics)
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;

  // Add tracing interceptor (runs first for complete span coverage)
  interceptors.push_back(std::make_unique<tracing::ServerTracingInterceptorFactory>());

  // Add string transformation interceptor
  interceptors.push_back(std::move(interceptor_factory));

  // Add metrics interceptor
  auto metrics_factory = std::make_unique<MetricsServerInterceptorFactory>(registry);
  interceptors.push_back(std::move(metrics_factory));

  builder.experimental().SetInterceptorCreators(std::move(interceptors));
  
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  // Keep a long-lived span active for the server lifetime so logs after this point carry trace context
  auto tracer = tracing::TracerProvider::GetTracer("greeter-callback-server");
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> server_span;
  if (tracer) {
    server_span = tracer->StartSpan("GreeterCallbackServer.Run");
  }
  opentelemetry::nostd::unique_ptr<opentelemetry::trace::Scope> server_scope;
  if (server_span) {
    server_scope.reset(new opentelemetry::trace::Scope(server_span));
  }

  spdlog::info("Server listening on {}", server_address);

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();

  // End the long-lived server span after shutdown
  if (server_span) {
    server_span->End();
  }
}

int main(int argc, char** argv) {
  // Initialize OpenTelemetry tracing
  tracing::TracerProvider::Initialize();

  // Set up trace-aware logging (inject trace_id and span_id into logs)
  tracing::SetTraceLogging();

  // Run the gRPC server
  RunServer(50051);

  // Shutdown tracing and flush pending spans
  tracing::TracerProvider::Shutdown();

  return 0;
}