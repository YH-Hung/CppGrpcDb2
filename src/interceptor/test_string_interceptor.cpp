#include "string_transform_interceptor.h"
#include "helloworld.grpc.pb.h"
#include <iostream>
#include <cassert>
#include <algorithm>

void TransformMessageStrings(google::protobuf::Message* message, 
                            const StringTransformFunction& transform) {
    if (!message || !transform) {
        return;
    }
    
    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    const google::protobuf::Reflection* reflection = message->GetReflection();
    
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* field = descriptor->field(i);
        
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
            if (field->is_repeated()) {
                int count = reflection->FieldSize(*message, field);
                for (int j = 0; j < count; ++j) {
                    std::string original_value = reflection->GetRepeatedString(*message, field, j);
                    std::string transformed_value = transform(original_value);
                    reflection->SetRepeatedString(message, field, j, transformed_value);
                }
            } else {
                std::string original_value = reflection->GetString(*message, field);
                std::string transformed_value = transform(original_value);
                reflection->SetString(message, field, transformed_value);
            }
        } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
            if (field->is_repeated()) {
                int count = reflection->FieldSize(*message, field);
                for (int j = 0; j < count; ++j) {
                    google::protobuf::Message* sub_message = 
                        reflection->MutableRepeatedMessage(message, field, j);
                    TransformMessageStrings(sub_message, transform);
                }
            } else if (reflection->HasField(*message, field)) {
                google::protobuf::Message* sub_message = 
                    reflection->MutableMessage(message, field);
                TransformMessageStrings(sub_message, transform);
            }
        }
    }
}

int main() {
    // Create transformation lambdas
    StringTransformFunction request_transform = [](const std::string& input) -> std::string {
        std::cout << "Request transform: Uppercasing '" << input << "'\n";
        std::string result = input;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    };
    
    StringTransformFunction response_transform = [](const std::string& input) -> std::string {
        std::cout << "Response transform: Adding prefix to '" << input << "'\n";
        return "[TRANSFORMED] " + input;
    };
    
    // Test request transformation
    helloworld::HelloRequest request;
    request.set_name("world");
    
    std::cout << "Original request name: " << request.name() << std::endl;
    TransformMessageStrings(&request, request_transform);
    std::cout << "Transformed request name: " << request.name() << std::endl;
    
    // Test response transformation  
    helloworld::HelloReply reply;
    reply.set_message("Hello WORLD");
    
    std::cout << "Original response message: " << reply.message() << std::endl;
    TransformMessageStrings(&reply, response_transform);
    std::cout << "Transformed response message: " << reply.message() << std::endl;
    
    // Verify transformations
    assert(request.name() == "WORLD");
    assert(reply.message() == "[TRANSFORMED] Hello WORLD");
    
    std::cout << "\nAll tests passed! String transformation interceptor is working correctly.\n";
    
    return 0;
}