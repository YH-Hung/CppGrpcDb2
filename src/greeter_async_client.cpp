#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include "helloworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterClient {
public:
    GreeterClient(std::shared_ptr<Channel> channel)
        : stub_(Greeter::NewStub(channel)) {}

    std::string SayHello(const std::string& user) {
        HelloRequest request;
        request.set_name(user);

        HelloReply reply;
        ClientContext context;
        CompletionQueue cq;
        Status status;

        // Two-phase async call (PrepareAsync + StartCall)
        std::unique_ptr<ClientAsyncResponseReader<HelloReply>> rpc(
            stub_->PrepareAsyncSayHello(&context, request, &cq));
        rpc->StartCall();

        // Request the reply and final status, with tag (void*)1
        rpc->Finish(&reply, &status, (void*)1);

        // Wait for the completion queue to return the next result
        void* got_tag;
        bool ok = false;
        cq.Next(&got_tag, &ok);

        if (ok && got_tag == (void*)1 && status.ok()) {
            return reply.message();
        } else {
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<Greeter::Stub> stub_;
};

int main(int argc, char** argv) {
    std::string target_str = "localhost:50051";
    GreeterClient greeter(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));
    std::string user("賴柔瑤");
    std::string reply = greeter.SayHello(user);
    std::cout << "Greeter received: " << reply << std::endl;

    return 0;
}
