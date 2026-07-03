#include <grpcpp/grpcpp.h>

#include <iostream>
#include <string>

#include "helloworld.grpc.pb.h"
#include "lb/failover_client.h"

using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

int main(int argc, char** argv) {
    lb::FailoverClient<Greeter> client =
        lb::FailoverClient<Greeter>::FromEnv();

    HelloRequest request;
    request.set_name("賴柔瑤");
    HelloReply reply;

    const lb::CallResult result =
        client.Call(request, reply, &Greeter::Stub::SayHello);

    if (result.status.ok()) {
        std::cout << "Greeter received: " << reply.message()
                  << " (via " << result.served_by << ")" << std::endl;
        return 0;
    }
    std::cout << result.status.error_code() << ": "
              << result.status.error_message() << std::endl;
    return 1;
}
