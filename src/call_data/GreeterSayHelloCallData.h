#ifndef CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H
#define CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H
#include "CallData.h"
#include <helloworld.grpc.pb.h>

DEFINE_SAY_HELLO_CALLDATA_CLASS(
    GreeterSayHello,
    helloworld::Greeter::AsyncService,
    helloworld::HelloRequest,
    helloworld::HelloReply
)

#endif //CPPGRPCDB2_GREETERSAYHELLOCALLDATA_H