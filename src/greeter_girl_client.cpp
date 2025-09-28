#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include "hello_girl.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using hellogirl::GirlGreeter;
using hellogirl::HelloGirlReply;
using hellogirl::HelloGirlRequest;

class GirlGreeterClient {
public:
    explicit GirlGreeterClient(std::shared_ptr<Channel> channel)
        : stub_(GirlGreeter::NewStub(channel)) {}

    // Sends a greeting request and returns the combined string output
    std::string SayHello(const std::string& name, const std::string& spouse, int first_round) {
        // Data we are sending to the server.
        HelloGirlRequest request;
        request.set_name(name);
        request.set_spouse(spouse);
        request.set_first_round(first_round);

        // Container for the data we expect from the server.
        HelloGirlReply reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SayHello(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            std::ostringstream oss;
            oss << "message='" << reply.message() << "'\n"
                << "marriage='" << reply.marriage() << "'\n"
                << "size=" << reply.size();
            return oss.str();
        } else {
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<GirlGreeter::Stub> stub_;
};

int main(int argc, char** argv) {
    std::string target_str = "localhost:50051";
    GirlGreeterClient client(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    // Example values similar to greeter_client.cpp style
    std::string name = "賴柔瑤";
    std::string spouse = "me 英國人";
    int first_round = 38;

    auto reply = client.SayHello(name, spouse, first_round);
    std::cout << "GirlGreeter received:\n" << reply << std::endl;

    return 0;
}
