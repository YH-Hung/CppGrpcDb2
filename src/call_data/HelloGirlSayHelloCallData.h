#ifndef CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H
#define CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H
#include "CallData.h"
#include <hello_girl.grpc.pb.h>

class HelloGirlSayHelloCallData : public CallDataBase<hellogirl::GirlGreeter::AsyncService, hellogirl::HelloGirlRequest, hellogirl::HelloGirlReply>{
public:
    HelloGirlSayHelloCallData(hellogirl::GirlGreeter::AsyncService *service, grpc::ServerCompletionQueue *cq)
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


#endif //CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H

