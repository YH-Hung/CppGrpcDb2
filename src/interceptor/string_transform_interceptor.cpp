#include "string_transform_interceptor.h"
#include <spdlog/spdlog.h>

StringTransformServerInterceptor::StringTransformServerInterceptor(
    StringTransformFunction request_transform,
    StringTransformFunction response_transform)
    : request_transform_(std::move(request_transform))
    , response_transform_(std::move(response_transform)) {
}

void StringTransformServerInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    if (methods->QueryInterceptionHookPoint(grpc::experimental::InterceptionHookPoints::POST_RECV_MESSAGE)) {
        if (request_transform_) {
            auto* req_msg = static_cast<google::protobuf::Message*>(methods->GetRecvMessage());
            if (req_msg) {
                spdlog::debug("Applying request string transformation");
                TransformMessageStrings(req_msg, request_transform_);
            }
        }
    }
    
    if (methods->QueryInterceptionHookPoint(grpc::experimental::InterceptionHookPoints::PRE_SEND_MESSAGE)) {
        if (response_transform_) {
            auto* resp_msg = static_cast<google::protobuf::Message*>(const_cast<void*>(methods->GetSendMessage()));
            if (resp_msg) {
                spdlog::debug("Applying response string transformation");
                TransformMessageStrings(resp_msg, response_transform_);
            }
        }
    }
    
    methods->Proceed();
}

void StringTransformServerInterceptor::TransformMessageStrings(google::protobuf::Message* message, 
                                                              const StringTransformFunction& transform) {
    if (!message || !transform) {
        return;
    }
    
    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    const google::protobuf::Reflection* reflection = message->GetReflection();
    
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* field = descriptor->field(i);

        // TYPE_STRING did NOT affect TYPE_BYTES fields
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
            if (field->is_repeated()) {
                int count = reflection->FieldSize(*message, field);
                for (int j = 0; j < count; ++j) {
                    std::string original_value = reflection->GetRepeatedString(*message, field, j);
                    std::string transformed_value = transform(original_value);
                    reflection->SetRepeatedString(message, field, j, transformed_value);
                    spdlog::debug("Transformed repeated string field '{}' [{}]: '{}' -> '{}'", 
                                field->name(), j, original_value, transformed_value);
                }
            } else {
                std::string original_value = reflection->GetString(*message, field);
                std::string transformed_value = transform(original_value);
                reflection->SetString(message, field, transformed_value);
                spdlog::debug("Transformed string field '{}': '{}' -> '{}'", 
                            field->name(), original_value, transformed_value);
            }
        } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
            spdlog::debug("Ignoring and keeping bytes field '{}' not touched", field->name());
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

grpc::experimental::Interceptor* StringTransformServerInterceptorFactory::CreateServerInterceptor(
    grpc::experimental::ServerRpcInfo* info) {
    
    if (request_transform_ || response_transform_) {
        return new StringTransformServerInterceptor(request_transform_, response_transform_);
    }
    
    return nullptr;
}