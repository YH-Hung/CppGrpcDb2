#ifndef CPPGRPCDB2_CALLDATA_H
#define CPPGRPCDB2_CALLDATA_H
#include <grpcpp/completion_queue.h>
#include <grpcpp/impl/codegen/config_protobuf.h>
#include <grpcpp/server_context.h>

#include <chrono>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <spdlog/spdlog.h>

#include "calldata_metrics.h"
#include "otel_tracing.h"

// Helper function to serialize protobuf message to JSON format
template <typename MessageType>
static std::string MessageToJsonString(const MessageType &msg) {
  grpc::protobuf::json::JsonPrintOptions options;
  options.preserve_proto_field_names = true; // Use snake_case field names

  std::string json_str;
  auto status =
      grpc::protobuf::json::MessageToJsonString(msg, &json_str, options);

  if (status.ok()) {
    return json_str;
  } else {
    return "<JSON conversion failed: " + status.ToString() + ">";
  }
}

class CallData {
public:
  virtual ~CallData() = default;
  virtual void Proceed(bool ok) = 0;
};

template <typename ServiceType, typename RequestType, typename ReplyType>
class CallDataBase : public CallData {
public:
  ~CallDataBase() override = default;
  CallDataBase(ServiceType *service, grpc::ServerCompletionQueue *cq,
               CallDataMetrics* metrics = nullptr)
      : service_(service), cq_(cq), responder_(&ctx_),
        status_(CallStatus::CREATE), metrics_(metrics) {
    // IMPORTANT: Do NOT call virtual methods from constructors.
    // RegisterRequest() is virtual and is invoked inside Proceed().
    // Calling Proceed() here would dispatch a pure virtual call because
    // the most-derived object is not fully constructed yet.
    // The derived class should explicitly call Proceed(true) after
    // construction.
  }

  void Proceed(bool ok) override {
    if (ok) {
      switch (status_) {
      case CallStatus::CREATE:
        // Register to receive a new RPC by invoke service_->RequestXXX()
        status_ = CallStatus::PROCESS;
        RegisterRequest();
        break;

      case CallStatus::PROCESS:
        OnRequestReceived();
        HandleRpc();
        OnRequestProcessed();

        // Reply RPC (asynchronously). Expect one more completion for this tag.
        status_ = CallStatus::FINISH;
        responder_.Finish(reply_, grpc::Status::OK, this);
        break;

      case CallStatus::FINISH:
        OnRpcComplete();
        delete this;
        break;
      }
    } else {
      OnRpcCancelled();
      delete this;
    }
  }

protected:
  virtual void RegisterRequest() = 0;
  virtual void HandleRpc() = 0;
  virtual void SpawnNewHandler() = 0;
  virtual std::string GetMethodName() const = 0;

  // RPC Lifecycle Methods
  //
  // The Proceed() method orchestrates the RPC through these lifecycle stages:
  //   CREATE → PROCESS → FINISH (or error)
  //
  // Lifecycle hooks group operations by when they occur:
  //   OnRequestReceived()  - Setup when request arrives
  //   OnRequestProcessed() - Cleanup after business logic
  //   OnRpcComplete()      - Final metrics on success
  //   OnRpcCancelled()     - Track cancelled/failed RPCs
  //
  // To add new metrics/logging: Modify the appropriate lifecycle method
  // To customize behavior: Override in derived class (call base implementation!)

  void OnRequestReceived() {
    SpawnNewHandler();
    start_time_ = std::chrono::steady_clock::now();
    InitializeMetricsForMethod();
    trace_id_ = otel::TraceIdForServerContext(&ctx_);

    try {
      std::string request_json = MessageToJsonString(request_);
      spdlog::info("[CallData] [trace_id: {}] Request message (JSON): {}",
                  trace_id_, request_json);
    } catch (const std::exception &e) {
      spdlog::warn("[CallData] [trace_id: {}] Failed to log request message: {}",
                  trace_id_, e.what());
    }

    processing_start_ = std::chrono::steady_clock::now();
  }

  void OnRequestProcessed() {
    RecordProcessingDuration();

    try {
      std::string reply_json = MessageToJsonString(reply_);
      spdlog::info("[CallData] [trace_id: {}] Reply message (JSON): {}",
                  trace_id_, reply_json);
    } catch (const std::exception &e) {
      spdlog::warn("[CallData] [trace_id: {}] Failed to log reply message: {}",
                  trace_id_, e.what());
    }

  }

  void OnRpcComplete() {
    RecordTotalDuration();
    if (metrics_ && request_counter_) {
      request_counter_->Increment();
    }
  }

  void OnRpcCancelled() {
    RecordCancellation();
  }

  ServiceType *service_;
  grpc::ServerCompletionQueue *cq_;
  grpc::ServerContext ctx_;

  RequestType request_;
  ReplyType reply_;
  grpc::ServerAsyncResponseWriter<ReplyType> responder_;
  std::string trace_id_;

  // Metrics tracking
  CallDataMetrics* metrics_{nullptr};
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point processing_start_;
  std::string method_name_;

  // Per-request metric instances (created with method label)
  prometheus::Counter* request_counter_{nullptr};
  prometheus::Histogram* duration_histogram_{nullptr};
  prometheus::Summary* duration_summary_{nullptr};
  prometheus::Histogram* processing_histogram_{nullptr};

private:
  enum class CallStatus { CREATE, PROCESS, FINISH };
  CallStatus status_;

  // Metrics utility methods
  void InitializeMetricsForMethod() {
    if (!metrics_) return;

    method_name_ = GetMethodName();
    std::map<std::string, std::string> method_label = {{"method", method_name_}};

    // Define bucket vectors once as static
    static const std::vector<double> duration_buckets =
        {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0};
    static const prometheus::Summary::Quantiles duration_quantiles =
        {{0.5, 0.05}, {0.9, 0.01}, {0.99, 0.001}};

    request_counter_ = &metrics_->request_counter_family.Add(
        {{"method", method_name_}, {"status", "ok"}});
    duration_histogram_ = &metrics_->duration_histogram_family.Add(
        method_label, duration_buckets);
    duration_summary_ = &metrics_->duration_summary_family.Add(
        method_label, duration_quantiles);
    processing_histogram_ = &metrics_->processing_histogram_family.Add(
        method_label, duration_buckets);
  }

  void RecordProcessingDuration() {
    if (metrics_ && processing_histogram_) {
      auto processing_end = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed = processing_end - processing_start_;
      processing_histogram_->Observe(elapsed.count());
    }
  }

  void RecordTotalDuration() {
    if (metrics_ && (duration_histogram_ || duration_summary_)) {
      auto end_time = std::chrono::steady_clock::now();
      std::chrono::duration<double> total_elapsed = end_time - start_time_;
      const double seconds = total_elapsed.count();
      if (duration_histogram_) {
        duration_histogram_->Observe(seconds);
      }
      if (duration_summary_) {
        duration_summary_->Observe(seconds);
      }
    }
  }

  void RecordCancellation() {
    if (metrics_) {
      auto& error_counter = metrics_->request_counter_family.Add({
          {"method", method_name_.empty() ? "unknown" : method_name_},
          {"status", "cancelled"}
      });
      error_counter.Increment();
    }
  }

};

// Macros to reduce code duplication for SayHello CallData classes

// Macro to define a SayHello CallData class
// Note: Include guard must be defined separately before using this macro
// Parameters: CLASS_PREFIX, SERVICE_TYPE, REQUEST_TYPE, REPLY_TYPE
// The macro constructs the class name as: {CLASS_PREFIX}CallData
#define DEFINE_SAY_HELLO_CALLDATA_CLASS(CLASS_PREFIX, SERVICE_TYPE, REQUEST_TYPE, REPLY_TYPE) \
class CLASS_PREFIX##CallData : public CallDataBase<SERVICE_TYPE, REQUEST_TYPE, REPLY_TYPE> { \
public: \
    CLASS_PREFIX##CallData(SERVICE_TYPE *service, grpc::ServerCompletionQueue *cq, \
                           CallDataMetrics* metrics = nullptr) \
        : CallDataBase(service, cq, metrics) { \
        /* Kick off the initial request registration now that the most-derived object is fully constructed. */ \
        CallDataBase::Proceed(true); \
    } \
 \
protected: \
    void RegisterRequest() override; \
    void HandleRpc() override; \
    void SpawnNewHandler() override; \
    std::string GetMethodName() const override; \
};

// Macro to implement RegisterRequest(), SpawnNewHandler(), and GetMethodName() methods
// Parameters: CLASS_PREFIX, METHOD_NAME
#define IMPLEMENT_SAY_HELLO_CALLDATA_METHODS(CLASS_PREFIX, METHOD_NAME) \
void CLASS_PREFIX##CallData::RegisterRequest() { \
    service_->RequestSayHello(&ctx_, &request_, &responder_, cq_, cq_, this); \
} \
 \
void CLASS_PREFIX##CallData::SpawnNewHandler() { \
    new CLASS_PREFIX##CallData(service_, cq_, metrics_); \
} \
 \
std::string CLASS_PREFIX##CallData::GetMethodName() const { \
    return METHOD_NAME; \
}

#endif // CPPGRPCDB2_CALLDATA_H
