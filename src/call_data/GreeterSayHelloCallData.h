#ifndef CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H
#define CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H
#include "CallData.h"
#include <helloworld.grpc.pb.h>

class GreeterSayHelloCallData : public CallDataBase<helloworld::Greeter::AsyncService, helloworld::HelloRequest, helloworld::HelloReply>{
public:
    GreeterSayHelloCallData(helloworld::Greeter::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataBase(service, cq) {
        // Kick off the initial request registration now that the most-derived
        // object is fully constructed.
        CallDataBase::Proceed(true);
    }

protected:
    void RegisterRequest() override;
    void HandleRpc() override;
    void SpawnNewHandler() override;
};


#endif //CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H