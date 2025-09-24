#pragma once

#include <functional>
#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>

using StringTransformFunction = std::function<std::string(const std::string&)>;

class StringTransformServerInterceptor : public grpc::experimental::Interceptor {
public:
    explicit StringTransformServerInterceptor(
        StringTransformFunction request_transform,
        StringTransformFunction response_transform);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    void TransformMessageStrings(google::protobuf::Message* message, 
                                const StringTransformFunction& transform);
    
    StringTransformFunction request_transform_;
    StringTransformFunction response_transform_;
};

class StringTransformServerInterceptorFactory 
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    StringTransformServerInterceptorFactory() = default;
    
    void SetRequestTransform(StringTransformFunction transform) {
        request_transform_ = std::move(transform);
    }
    
    void SetResponseTransform(StringTransformFunction transform) {
        response_transform_ = std::move(transform);
    }
    
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override;

private:
    StringTransformFunction request_transform_;
    StringTransformFunction response_transform_;
};