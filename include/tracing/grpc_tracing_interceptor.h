// Copyright 2025 CppGrpcDb2 Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/trace/scope.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/trace/tracer.h"

namespace tracing {

/**
 * @brief Server-side gRPC interceptor for automatic distributed tracing
 *
 * This interceptor automatically:
 * - Extracts W3C trace context from incoming gRPC metadata
 * - Creates a new server span for each RPC call
 * - Sets span attributes (rpc.system, rpc.service, rpc.method, rpc.grpc.status_code)
 * - Handles missing/invalid trace context by creating a root span
 * - Propagates trace context through OpenTelemetry context
 *
 * Thread Safety: This interceptor is thread-safe and can handle concurrent RPC calls
 *
 * Usage:
 * @code
 *   auto server_builder = grpc::ServerBuilder();
 *
 *   // Create interceptor factory
 *   std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> creators;
 *   creators.push_back(std::make_unique<tracing::ServerTracingInterceptorFactory>());
 *
 *   // Register with server builder
 *   server_builder.experimental().SetInterceptorCreators(std::move(creators));
 *
 *   auto server = server_builder.BuildAndStart();
 * @endcode
 */
class ServerTracingInterceptor : public grpc::experimental::Interceptor {
public:
    explicit ServerTracingInterceptor(grpc::experimental::ServerRpcInfo* info);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    grpc::experimental::ServerRpcInfo* rpc_info_;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    opentelemetry::nostd::unique_ptr<opentelemetry::trace::Scope> scope_;

    /**
     * @brief Extract trace context from gRPC metadata and create server span
     */
    void StartServerSpan(grpc::experimental::InterceptorBatchMethods* methods);

    /**
     * @brief Finalize span with gRPC status code and end it
     */
    void EndServerSpan(const grpc::Status& status);

    /**
     * @brief Set common RPC span attributes
     */
    void SetRpcAttributes();

    /**
     * @brief Extract method name from full gRPC method path
     * Example: "/helloworld.Greeter/SayHello" -> "SayHello"
     */
    std::string ExtractMethodName(const std::string& full_method) const;

    /**
     * @brief Extract service name from full gRPC method path
     * Example: "/helloworld.Greeter/SayHello" -> "helloworld.Greeter"
     */
    std::string ExtractServiceName(const std::string& full_method) const;
};

/**
 * @brief Factory for creating ServerTracingInterceptor instances
 */
class ServerTracingInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new ServerTracingInterceptor(info);
    }
};

/**
 * @brief Client-side gRPC interceptor for automatic distributed tracing
 *
 * This interceptor automatically:
 * - Creates a new client span for each outgoing RPC call
 * - Injects W3C trace context into outgoing gRPC metadata
 * - Sets span attributes (rpc.system, rpc.service, rpc.method, rpc.grpc.status_code)
 * - Propagates trace context to downstream services
 *
 * Thread Safety: This interceptor is thread-safe and can handle concurrent RPC calls
 *
 * Usage:
 * @code
 *   auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
 *
 *   // Create client with tracing interceptor
 *   std::vector<std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>> creators;
 *   creators.push_back(std::make_unique<tracing::ClientTracingInterceptorFactory>());
 *
 *   auto client = Greeter::NewStub(
 *       grpc::experimental::CreateCustomChannelWithInterceptors(
 *           "localhost:50051",
 *           grpc::InsecureChannelCredentials(),
 *           grpc::ChannelArguments(),
 *           std::move(creators)
 *       )
 *   );
 * @endcode
 */
class ClientTracingInterceptor : public grpc::experimental::Interceptor {
public:
    explicit ClientTracingInterceptor(grpc::experimental::ClientRpcInfo* info);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    grpc::experimental::ClientRpcInfo* rpc_info_;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    opentelemetry::nostd::unique_ptr<opentelemetry::trace::Scope> scope_;

    /**
     * @brief Create client span and inject trace context into metadata
     */
    void StartClientSpan(grpc::experimental::InterceptorBatchMethods* methods);

    /**
     * @brief Finalize span with gRPC status code and end it
     */
    void EndClientSpan(const grpc::Status& status);

    /**
     * @brief Set common RPC span attributes
     */
    void SetRpcAttributes();

    /**
     * @brief Extract method name from full gRPC method path
     */
    std::string ExtractMethodName(const std::string& full_method) const;

    /**
     * @brief Extract service name from full gRPC method path
     */
    std::string ExtractServiceName(const std::string& full_method) const;
};

/**
 * @brief Factory for creating ClientTracingInterceptor instances
 */
class ClientTracingInterceptorFactory
    : public grpc::experimental::ClientInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateClientInterceptor(
        grpc::experimental::ClientRpcInfo* info) override {
        return new ClientTracingInterceptor(info);
    }
};

/**
 * @brief Helper to create a traced gRPC channel
 *
 * Convenience function that creates a gRPC channel with tracing interceptor already configured
 *
 * @param target Server address (e.g., "localhost:50051")
 * @param credentials Channel credentials (e.g., grpc::InsecureChannelCredentials())
 * @return Traced gRPC channel
 */
std::shared_ptr<grpc::Channel> CreateTracedChannel(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& credentials
);

}  // namespace tracing
