#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include <memory>
#include <vector>
#include <signal.h>
#include <pthread.h>
#include <atomic>

#include "spdlog/spdlog.h"
#include "helloworld.grpc.pb.h"
#include "hello_girl.grpc.pb.h"
#include "health.pb.h"
#include "health.grpc.pb.h"
#include "call_data/GreeterSayHelloCallData.h"
#include "call_data/HelloGirlSayHelloCallData.h"
#include "message_logging_interceptor.h"
#include "calldata_metrics.h"
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/gauge.h>

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
        girl_greeter_service_ = std::make_unique<hellogirl::GirlGreeter::AsyncService>();
    }

    void Run(uint16_t port) {
        // Force-link health proto descriptors before server starts.
        ForceLinkHealthProtoDescriptors();

        // Initialize Prometheus metrics
        metrics_registry_ = std::make_shared<prometheus::Registry>();
        metrics_exposer_ = std::make_unique<prometheus::Exposer>("127.0.0.1:8125");
        metrics_exposer_->RegisterCollectable(metrics_registry_);

        calldata_metrics_ = std::make_unique<CallDataMetrics>(metrics_registry_);
        shared_metrics_ = calldata_metrics_->GetSharedMetrics();

        // Gauge metric to indicate whether the single CQ worker thread is busy
        // (i.e., currently executing CallData::Proceed()). 1 = busy, 0 = idle.
        worker_busy_family_ = &prometheus::BuildGauge()
                                   .Name("grpc_cq_worker_busy")
                                   .Help("1 if the CQ worker thread is executing CallData::Proceed(), 0 if idle")
                                   .Register(*metrics_registry_);
        worker_busy_gauge_ = &worker_busy_family_->Add({});
        worker_busy_gauge_->Set(0.0);

        spdlog::info("Metrics endpoint: http://127.0.0.1:8125/metrics");

        std::string server_address = "0.0.0.0:" + std::to_string(port);
        
        // Enable default health check service
        grpc::EnableDefaultHealthCheckService(true);
        // Enable server reflection for grpcurl
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();
        
        grpc::ServerBuilder builder;

        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

        builder.RegisterService(greeter_service_.get());
        builder.RegisterService(girl_greeter_service_.get());

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
                health_service->SetServingStatus("hellogirl.GirlGreeter", true);
            }
        }

        SpwanHandlers();
        HandleRpcs();
    }

    // Spawn a new CallData instance to serve new clients.
    void SpwanHandlers() {
        new GreeterSayHelloCallData(greeter_service_.get(), cq_.get(), &shared_metrics_);
        new HelloGirlSayHelloCallData(girl_greeter_service_.get(), cq_.get(), &shared_metrics_);
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
            // Mark this worker thread as busy while executing Proceed().
            cq_worker_busy_.store(true, std::memory_order_relaxed);
            if (worker_busy_gauge_) worker_busy_gauge_->Set(1.0);
            static_cast<CallData*>(tag)->Proceed(ok);
            cq_worker_busy_.store(false, std::memory_order_relaxed);
            if (worker_busy_gauge_) worker_busy_gauge_->Set(0.0);
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
    std::unique_ptr<hellogirl::GirlGreeter::AsyncService> girl_greeter_service_;
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<prometheus::Exposer> metrics_exposer_;
    std::shared_ptr<prometheus::Registry> metrics_registry_;
    std::unique_ptr<CallDataMetrics> calldata_metrics_;
    CallDataSharedMetrics shared_metrics_;
    // Busy flag indicates whether the CQ worker is currently inside CallData::Proceed()
    std::atomic<bool> cq_worker_busy_{false};
    prometheus::Family<prometheus::Gauge>* worker_busy_family_{nullptr};
    prometheus::Gauge* worker_busy_gauge_{nullptr};
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
