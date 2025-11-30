#ifndef CPPGRPCDB2_CALLDATA_H
#define CPPGRPCDB2_CALLDATA_H
#include <grpcpp/completion_queue.h>
#include <grpcpp/impl/codegen/config_protobuf.h>
#include <grpcpp/server_context.h>

#include <random>
#include <spdlog/spdlog.h>
#include <sstream>

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
  CallDataBase(ServiceType *service, grpc::ServerCompletionQueue *cq)
      : service_(service), cq_(cq), responder_(&ctx_),
        status_(CallStatus::CREATE) {
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
        // Spawn new handler to accept next call
        SpawnNewHandler();

        // Generate Request ID
        request_id_ = GenerateUuid();

        // Log request message (before processing)
        try {
          std::string request_json = MessageToJsonString(request_);
          spdlog::info("[CallData] [ReqID: {}] Request message (JSON): {}",
                       request_id_, request_json);
        } catch (const std::exception &e) {
          spdlog::warn(
              "[CallData] [ReqID: {}] Failed to log request message: {}",
              request_id_, e.what());
        }

        // Invoke business logic
        HandleRpc();

        // Log reply message (after processing, before sending)
        try {
          std::string reply_json = MessageToJsonString(reply_);
          spdlog::info("[CallData] [ReqID: {}] Reply message (JSON): {}",
                       request_id_, reply_json);
        } catch (const std::exception &e) {
          spdlog::warn("[CallData] [ReqID: {}] Failed to log reply message: {}",
                       request_id_, e.what());
        }

        // Reply RPC (asynchronously). Expect one more completion for this tag.
        status_ = CallStatus::FINISH;
        responder_.Finish(reply_, grpc::Status::OK, this);
        break;
      case CallStatus::FINISH:
        // Release memory
        delete this;
        break;
      }
    } else {
      // ok == false indicates that the operation associated with this tag did
      // not complete successfully (e.g., CQ shutdown or cancellation).
      // Regardless of our current state, no further events for this tag will
      // arrive; it is safe to destroy the call object.
      delete this;
    }
  }

protected:
  virtual void RegisterRequest() = 0;
  virtual void HandleRpc() = 0;
  virtual void SpawnNewHandler() = 0;

  ServiceType *service_;
  grpc::ServerCompletionQueue *cq_;
  grpc::ServerContext ctx_;

  RequestType request_;
  ReplyType reply_;
  grpc::ServerAsyncResponseWriter<ReplyType> responder_;
  std::string request_id_;

private:
  enum class CallStatus { CREATE, PROCESS, FINISH };
  CallStatus status_;
};

#endif // CPPGRPCDB2_CALLDATA_H