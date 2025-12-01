#include "GreeterSayHelloCallData.h"

IMPLEMENT_SAY_HELLO_CALLDATA_METHODS(GreeterSayHello)

void GreeterSayHelloCallData::HandleRpc() {
    std::string prefix("Hello ");
    reply_.set_message(prefix + request_.name());
}
