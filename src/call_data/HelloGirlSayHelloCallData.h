#ifndef CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H
#define CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H
#include "CallData.h"
#include <hello_girl.grpc.pb.h>

DEFINE_SAY_HELLO_CALLDATA_CLASS(
    HelloGirlSayHello,
    hellogirl::GirlGreeter::AsyncService,
    hellogirl::HelloGirlRequest,
    hellogirl::HelloGirlReply
)

#endif //CPPGRPCDB2_HELLOGIRLSAYHELLOCALLDATA_H

