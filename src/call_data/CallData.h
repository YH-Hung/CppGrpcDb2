#ifndef CPPGRPCDB2_CALLDATA_H
#define CPPGRPCDB2_CALLDATA_H
#include <grpcpp/completion_queue.h>
#include <grpcpp/server_context.h>
#include <spdlog/spdlog.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>
#include <sstream>

// Helper function to serialize protobuf message preserving UTF-8 characters
template<typename MessageType>
static std::string MessageToString(const MessageType& msg) {
    std::ostringstream oss;
    const google::protobuf::Descriptor* descriptor = msg.GetDescriptor();
    const google::protobuf::Reflection* reflection = msg.GetReflection();
    
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* field = descriptor->field(i);
        if (i > 0) oss << " ";
        
        oss << field->name() << ": ";
        
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
            if (field->is_repeated()) {
                oss << "[";
                int count = reflection->FieldSize(msg, field);
                for (int j = 0; j < count; ++j) {
                    if (j > 0) oss << ", ";
                    oss << "\"" << reflection->GetRepeatedString(msg, field, j) << "\"";
                }
                oss << "]";
            } else {
                oss << "\"" << reflection->GetString(msg, field) << "\"";
            }
        } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT32) {
            if (field->is_repeated()) {
                oss << "[";
                int count = reflection->FieldSize(msg, field);
                for (int j = 0; j < count; ++j) {
                    if (j > 0) oss << ", ";
                    oss << reflection->GetRepeatedInt32(msg, field, j);
                }
                oss << "]";
            } else {
                oss << reflection->GetInt32(msg, field);
            }
        } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT64) {
            if (field->is_repeated()) {
                oss << "[";
                int count = reflection->FieldSize(msg, field);
                for (int j = 0; j < count; ++j) {
                    if (j > 0) oss << ", ";
                    oss << reflection->GetRepeatedInt64(msg, field, j);
                }
                oss << "]";
            } else {
                oss << reflection->GetInt64(msg, field);
            }
        } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
            if (field->is_repeated()) {
                oss << "[";
                int count = reflection->FieldSize(msg, field);
                for (int j = 0; j < count; ++j) {
                    if (j > 0) oss << ", ";
                    const google::protobuf::Message& sub_msg = reflection->GetRepeatedMessage(msg, field, j);
                    oss << "{" << MessageToString(sub_msg) << "}";
                }
                oss << "]";
            } else if (reflection->HasField(msg, field)) {
                const google::protobuf::Message& sub_msg = reflection->GetMessage(msg, field);
                oss << "{" << MessageToString(sub_msg) << "}";
            } else {
                oss << "<not set>";
            }
        } else {
            // For other types, fall back to DebugString for that field
            oss << "<" << field->type_name() << ">";
        }
    }
    return oss.str();
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
    CallDataBase(ServiceType* service, grpc::ServerCompletionQueue* cq)
    : service_(service), cq_(cq), responder_(&ctx_), status_(CallStatus::CREATE) {
        // IMPORTANT: Do NOT call virtual methods from constructors.
        // RegisterRequest() is virtual and is invoked inside Proceed().
        // Calling Proceed() here would dispatch a pure virtual call because
        // the most-derived object is not fully constructed yet.
        // The derived class should explicitly call Proceed(true) after construction.
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

                    // Log request message (before processing)
                    try {
                        std::string request_str = MessageToString(request_);
                        spdlog::info("[CallData] Request message: {}", request_str);
                    } catch (const std::exception& e) {
                        spdlog::warn("[CallData] Failed to log request message: {}", e.what());
                    }

                    // Invoke business logic
                    HandleRpc();

                    // Log reply message (after processing, before sending)
                    try {
                        std::string reply_str = MessageToString(reply_);
                        spdlog::info("[CallData] Reply message: {}", reply_str);
                    } catch (const std::exception& e) {
                        spdlog::warn("[CallData] Failed to log reply message: {}", e.what());
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
            // ok == false indicates that the operation associated with this tag did not
            // complete successfully (e.g., CQ shutdown or cancellation). Regardless of
            // our current state, no further events for this tag will arrive; it is safe
            // to destroy the call object.
            delete this;
        }
    }

protected:
    virtual void RegisterRequest() = 0;
    virtual void HandleRpc() = 0;
    virtual void SpawnNewHandler() = 0;

    ServiceType* service_;
    grpc::ServerCompletionQueue* cq_;
    grpc::ServerContext ctx_;

    RequestType request_;
    ReplyType reply_;
    grpc::ServerAsyncResponseWriter<ReplyType> responder_;

private:
    enum class CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;
};


#endif //CPPGRPCDB2_CALLDATA_H