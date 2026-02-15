#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <json/json.h>
#include "helloworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterClient {
public:
    GreeterClient(std::shared_ptr<Channel> channel)
        : stub_(Greeter::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    std::string SayHello(const std::string& user) {
        // Data we are sending to the server.
        HelloRequest request;
        request.set_name(user);

        // Container for the data we expect from the server.
        HelloReply reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SayHello(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
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

    // Populate service config for retry
    // {
    //     "methodConfig": [
    //       {
    //         "name": [
    //           {
    //             "service": "helloworld.Greeter",
    //             "method": "SayHello"
    //           }
    //         ],
    //         "retryPolicy": {
    //           "maxAttempts": 5,
    //           "initialBackoff": "0.5s",
    //           "maxBackoff": "30s",
    //           "backoffMultiplier": 2,
    //           "retryableStatusCodes": [
    //             "UNAVAILABLE"
    //           ]
    //         }
    //       }
    //     ]
    //   }
    Json::Value service_config;
    Json::Value method_config(Json::arrayValue);
    Json::Value name_array(Json::arrayValue);
    Json::Value service_name;
    service_name["service"] = "helloworld.Greeter";
    name_array.append(service_name);

    Json::Value retry_policy;
    retry_policy["maxAttempts"] = 4;
    retry_policy["initialBackoff"] = "0.1s";
    retry_policy["maxBackoff"] = "1s";
    retry_policy["backoffMultiplier"] = 2;
    Json::Value retryable_codes(Json::arrayValue);
    retryable_codes.append("UNAVAILABLE");
    retry_policy["retryableStatusCodes"] = retryable_codes;

    Json::Value method_entry;
    method_entry["name"] = name_array;
    method_entry["retryPolicy"] = retry_policy;
    method_config.append(method_entry);
    service_config["methodConfig"] = method_config;

    Json::StreamWriterBuilder writer_builder;
    writer_builder["commentStyle"] = "None";
    writer_builder["indentation"] = "";
    const std::string service_config_json = Json::writeString(writer_builder, service_config);

    grpc::ChannelArguments channel_args;
    channel_args.SetServiceConfigJSON(service_config_json);
    channel_args.SetInt(GRPC_ARG_ENABLE_RETRIES, 1);  // enable retry, default true

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    GreeterClient greeter(
        grpc::CreateCustomChannel(target_str, grpc::InsecureChannelCredentials(), channel_args));
    std::string user("賴柔瑤");
    // std::string user("黃美晴");
    std::string reply = greeter.SayHello(user);
    std::cout << "Greeter received: " << reply << std::endl;

    return 0;
}
