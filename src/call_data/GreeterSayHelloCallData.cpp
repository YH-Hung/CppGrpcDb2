#include "GreeterSayHelloCallData.h"

void GreeterSayHelloCallData::RegisterRequest() {
    service_->RequestSayHello(&ctx_, &request_, &responder_, cq_, cq_, this);
}

void GreeterSayHelloCallData::HandleRpc() {
    std::string prefix("Hello ");
    reply_.set_message(prefix + request_.name());
}

void GreeterSayHelloCallData::SpawnNewHandler() {
    new GreeterSayHelloCallData(service_, cq_);
}
