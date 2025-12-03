#ifndef CPPGRPCDB2_CALLDATA_H
#define CPPGRPCDB2_CALLDATA_H
#include <grpcpp/completion_queue.h>
#include <grpcpp/impl/codegen/config_protobuf.h>
#include <grpcpp/server_context.h>

#include <chrono>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <random>
#include <spdlog/spdlog.h>
#include <sstream>

#include "calldata_metrics.h"

// Helper function to generate a UUID v4
static std::string GenerateUuid() {
  std::stringstream ss;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);
  std::uniform_int_distribution<> dis2(8, 11);

  ss << std::hex;
  for (int i = 0; i < 8; i++)
    ss << dis(gen);
  ss << "-";
  for (int i = 0; i < 4; i++)
    ss << dis(gen);
  ss << "-4";
  for (int i = 0; i < 3; i++)
    ss << dis(gen);
  ss << "-";
  ss << dis2(gen);
  for (int i = 0; i < 3; i++)
    ss << dis(gen);
  ss << "-";
  for (int i = 0; i < 12; i++)
    ss << dis(gen);
  return ss.str();
}

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
               CallDataSharedMetrics* metrics = nullptr)
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
    request_id_ = GenerateUuid();

    try {
      std::string request_json = MessageToJsonString(request_);
      spdlog::info("[CallData] [ReqID: {}] Request message (JSON): {}",
                  request_id_, request_json);
    } catch (const std::exception &e) {
      spdlog::warn("[CallData] [ReqID: {}] Failed to log request message: {}",
                  request_id_, e.what());
    }

    RecordRequestMetrics();
    processing_start_ = std::chrono::steady_clock::now();
  }

  void OnRequestProcessed() {
    RecordProcessingDuration();

    try {
      std::string reply_json = MessageToJsonString(reply_);
      spdlog::info("[CallData] [ReqID: {}] Reply message (JSON): {}",
                  request_id_, reply_json);
    } catch (const std::exception &e) {
      spdlog::warn("[CallData] [ReqID: {}] Failed to log reply message: {}",
                  request_id_, e.what());
    }

    RecordResponseMetrics();
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
  std::string request_id_;

  // Metrics tracking
  CallDataSharedMetrics* metrics_{nullptr};
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point processing_start_;
  std::string method_name_;

  // Per-request metric instances (created with method label)
  prometheus::Counter* request_counter_{nullptr};
  prometheus::Histogram* duration_histogram_{nullptr};
  prometheus::Histogram* processing_histogram_{nullptr};
  prometheus::Histogram* request_size_histogram_{nullptr};
  prometheus::Histogram* response_size_histogram_{nullptr};

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
    static const std::vector<double> size_buckets =
        {64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};

    if (metrics_->request_counter_family) {
      request_counter_ = &metrics_->request_counter_family->Add(
          {{"method", method_name_}, {"status", "ok"}});
    }
    if (metrics_->duration_histogram_family) {
      duration_histogram_ = &metrics_->duration_histogram_family->Add(
          method_label, duration_buckets);
    }
    if (metrics_->processing_histogram_family) {
      processing_histogram_ = &metrics_->processing_histogram_family->Add(
          method_label, duration_buckets);
    }
    if (metrics_->request_size_histogram_family) {
      request_size_histogram_ = &metrics_->request_size_histogram_family->Add(
          method_label, size_buckets);
    }
    if (metrics_->response_size_histogram_family) {
      response_size_histogram_ = &metrics_->response_size_histogram_family->Add(
          method_label, size_buckets);
    }
  }

  void RecordRequestMetrics() {
    ObserveHistogram(request_size_histogram_, request_.ByteSizeLong());
  }

  void RecordProcessingDuration() {
    if (metrics_ && processing_histogram_) {
      auto processing_end = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed = processing_end - processing_start_;
      processing_histogram_->Observe(elapsed.count());
    }
  }

  void RecordResponseMetrics() {
    ObserveHistogram(response_size_histogram_, reply_.ByteSizeLong());
  }

  void RecordTotalDuration() {
    if (metrics_ && duration_histogram_) {
      auto end_time = std::chrono::steady_clock::now();
      std::chrono::duration<double> total_elapsed = end_time - start_time_;
      duration_histogram_->Observe(total_elapsed.count());
    }
  }

  void RecordCancellation() {
    if (metrics_ && metrics_->request_counter_family) {
      auto& error_counter = metrics_->request_counter_family->Add({
          {"method", method_name_.empty() ? "unknown" : method_name_},
          {"status", "cancelled"}
      });
      error_counter.Increment();
    }
  }

  // Generic histogram observer with null safety
  template<typename T>
  void ObserveHistogram(prometheus::Histogram* histogram, T value) {
    if (metrics_ && histogram) {
      histogram->Observe(static_cast<double>(value));
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
                           CallDataSharedMetrics* metrics = nullptr) \
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