#ifndef CPPGRPCDB2_CALLDATA_H
#define CPPGRPCDB2_CALLDATA_H
#include <grpcpp/completion_queue.h>
#include <grpcpp/server_context.h>

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

                    // Invoke business logic
                    HandleRpc();

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