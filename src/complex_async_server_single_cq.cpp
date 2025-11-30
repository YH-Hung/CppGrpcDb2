#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include <memory>
#include <vector>
#include <signal.h>
#include <pthread.h>

#include "spdlog/spdlog.h"
#include "helloworld.grpc.pb.h"
#include "health.pb.h"
#include "health.grpc.pb.h"
#include "call_data/GreeterSayHelloCallData.h"
#include "message_logging_interceptor.h"

// Ensure health.proto descriptors are linked into the binary so that
// server reflection can serve them and grpcurl can describe/invoke Health.
static inline void ForceLinkHealthProtoDescriptors() {
    (void)grpc::health::v1::HealthCheckRequest::default_instance();
    (void)grpc::health::v1::HealthCheckResponse::default_instance();
}

class SingleCqServer {
public:
    SingleCqServer() {
        greeter_service_ = std::make_unique<helloworld::Greeter::AsyncService>();
    }

    void Run(uint16_t port) {
        // Force-link health proto descriptors before server starts.
        ForceLinkHealthProtoDescriptors();

        std::string server_address = "0.0.0.0:" + std::to_string(port);
        
        // Enable default health check service
        grpc::EnableDefaultHealthCheckService(true);
        // Enable server reflection for grpcurl
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();
        
        grpc::ServerBuilder builder;

        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

        builder.RegisterService(greeter_service_.get());

        // Register message logging interceptor
        auto logging_factory = std::make_unique<MessageLoggingServerInterceptorFactory>();
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
        interceptors.push_back(std::move(logging_factory));
        builder.experimental().SetInterceptorCreators(std::move(interceptors));

        cq_ = builder.AddCompletionQueue();

        server_ = builder.BuildAndStart();
        spdlog::info("Server listening on {}", server_address);

        // Set health check service serving (overall and per-service)
        if (server_) {
            auto* health_service = server_->GetHealthCheckService();
            if (health_service) {
                health_service->SetServingStatus(true);
                health_service->SetServingStatus("helloworld.Greeter", true);
            }
        }

        SpwanHandlers();
        HandleRpcs();
    }

    // Spawn a new CallData instance to serve new clients.
    void SpwanHandlers() {
        new GreeterSayHelloCallData(greeter_service_.get(), cq_.get());
    }

    void HandleRpcs() {
        void* tag;  // uniquely identifies a request.
        bool ok;
        while (cq_->Next(&tag, &ok)) {
            // Block waiting to read the next event from the completion queue. The
            // event is uniquely identified by its tag, which in this case is the
            // memory address of a CallData instance.
            // The return value of Next should always be checked. This return value
            // tells us whether there is any kind of event or cq_ is shutting down.
            static_cast<CallData*>(tag)->Proceed(ok);
        }
    }

    void Shutdown() {
        spdlog::info("Shutting down server...");
        // Gracefully shut down — allow active RPCs to finish
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);

        // Server must shutdown BEFORE shutdown CQ
        if (server_) {
            server_->Shutdown(deadline);
        }

        if (cq_) {
            cq_->Shutdown();
        }

        spdlog::info("Server shut down.");
    }

private:
    std::unique_ptr<helloworld::Greeter::AsyncService> greeter_service_;
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    std::unique_ptr<grpc::Server> server_;
};

int main(int argc, char** argv) {
    uint16_t port = 50051;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // Block termination signals. The main thread will wait for them.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    auto server = std::make_unique<SingleCqServer>();

    // Run the server in a separate thread.
    std::thread server_thread([&server, port]() {
        server->Run(port);
    });

    // Wait for a termination signal.
    int sig;
    sigwait(&set, &sig);

    spdlog::warn("Received termination signal, shutting down gRPC server...");
    server->Shutdown();

    // Wait for the server thread to finish.
    server_thread.join();

    spdlog::info("Server stopped.");

    return 0;
}
