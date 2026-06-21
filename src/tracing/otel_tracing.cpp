#include "otel_tracing.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/support/interceptor.h>
#include <opentelemetry/context/propagation/global_propagator.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/samplers/always_on.h>
#include <opentelemetry/sdk/trace/samplers/parent.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace otel {
namespace {

namespace context = opentelemetry::context;
namespace propagation = opentelemetry::context::propagation;
namespace nostd = opentelemetry::nostd;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;
namespace sdktrace = opentelemetry::sdk::trace;
namespace trace = opentelemetry::trace;

using HP = grpc::experimental::InterceptionHookPoints;

constexpr const char* kDefaultEndpoint = "http://localhost:4317";
constexpr const char* kDefaultServiceName = "CppGrpcDb2";
constexpr const char* kTracerName = "CppGrpcDb2.grpc";

std::mutex g_state_mutex;
bool g_initialized = false;
bool g_enabled = true;
std::shared_ptr<sdktrace::TracerProvider> g_sdk_provider;
std::unordered_map<const void*, std::string> g_server_context_trace_ids;
std::unordered_map<const void*, std::string> g_client_context_trace_ids;

std::string GetEnvOrDefault(const char* name, const std::string& default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    return value;
}

bool IsFalseValue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "0" || value == "false" || value == "off" || value == "no";
}

double ParseSamplerRatio(const std::string& value) {
    if (value.empty()) {
        return 1.0;
    }

    try {
        const double parsed = std::stod(value);
        if (parsed < 0.0) {
            return 0.0;
        }
        if (parsed > 1.0) {
            return 1.0;
        }
        return parsed;
    } catch (const std::exception&) {
        spdlog::warn("Invalid OTEL_TRACES_SAMPLER_ARG='{}'; using 1.0", value);
        return 1.0;
    }
}

std::unique_ptr<sdktrace::Sampler> MakeSampler() {
    std::string sampler = GetEnvOrDefault("OTEL_TRACES_SAMPLER", "parentbased_always_on");
    std::transform(sampler.begin(), sampler.end(), sampler.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (sampler == "always_off") {
        return std::make_unique<sdktrace::TraceIdRatioBasedSampler>(0.0);
    }

    if (sampler == "traceidratio" || sampler == "parentbased_traceidratio") {
        auto delegate = std::make_unique<sdktrace::TraceIdRatioBasedSampler>(
            ParseSamplerRatio(GetEnvOrDefault("OTEL_TRACES_SAMPLER_ARG", "1.0")));
        if (sampler == "parentbased_traceidratio") {
            return std::make_unique<sdktrace::ParentBasedSampler>(std::move(delegate));
        }
        return delegate;
    }

    auto delegate = std::make_unique<sdktrace::AlwaysOnSampler>();
    return std::make_unique<sdktrace::ParentBasedSampler>(std::move(delegate));
}

template <typename Id>
std::string IdToLowerHex(const Id& id) {
    if (!id.IsValid()) {
        return {};
    }

    std::array<char, Id::kSize * 2> buffer{};
    id.ToLowerBase16(buffer);
    return std::string(buffer.data(), buffer.size());
}

std::string TraceIdFromSpan(const nostd::shared_ptr<trace::Span>& span) {
    if (!span) {
        return {};
    }

    const auto span_context = span->GetContext();
    if (!span_context.IsValid()) {
        return {};
    }

    return IdToLowerHex(span_context.trace_id());
}

std::string SpanIdFromSpan(const nostd::shared_ptr<trace::Span>& span) {
    if (!span) {
        return {};
    }

    const auto span_context = span->GetContext();
    if (!span_context.IsValid()) {
        return {};
    }

    return IdToLowerHex(span_context.span_id());
}

void RememberServerContextTraceId(const grpc::ServerContextBase* context,
                                  const std::string& trace_id) {
    if (context == nullptr || trace_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_server_context_trace_ids[context] = trace_id;
}

void ForgetServerContextTraceId(const grpc::ServerContextBase* context) {
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_server_context_trace_ids.erase(context);
}

void RememberClientContextTraceId(const grpc::ClientContext* context,
                                  const std::string& trace_id) {
    if (context == nullptr || trace_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_client_context_trace_ids[context] = trace_id;
}

class ServerMetadataCarrier final : public propagation::TextMapCarrier {
public:
    explicit ServerMetadataCarrier(
        const std::multimap<grpc::string_ref, grpc::string_ref>* metadata)
        : metadata_(metadata) {}

    nostd::string_view Get(nostd::string_view key) const noexcept override {
        value_.clear();
        if (metadata_ == nullptr) {
            return nostd::string_view{};
        }

        const std::string lookup_key(key.data(), key.size());
        for (const auto& entry : *metadata_) {
            if (entry.first.size() == lookup_key.size() &&
                std::equal(entry.first.begin(), entry.first.end(), lookup_key.begin(),
                           [](char lhs, char rhs) {
                               return std::tolower(static_cast<unsigned char>(lhs)) ==
                                      std::tolower(static_cast<unsigned char>(rhs));
                           })) {
                value_.assign(entry.second.data(), entry.second.size());
                return nostd::string_view(value_.data(), value_.size());
            }
        }

        return nostd::string_view{};
    }

    void Set(nostd::string_view, nostd::string_view) noexcept override {}

private:
    const std::multimap<grpc::string_ref, grpc::string_ref>* metadata_;
    mutable std::string value_;
};

class ClientMetadataCarrier final : public propagation::TextMapCarrier {
public:
    explicit ClientMetadataCarrier(std::multimap<std::string, std::string>* metadata)
        : metadata_(metadata) {}

    nostd::string_view Get(nostd::string_view) const noexcept override {
        return nostd::string_view{};
    }

    void Set(nostd::string_view key, nostd::string_view value) noexcept override {
        if (metadata_ == nullptr) {
            return;
        }

        metadata_->emplace(std::string(key.data(), key.size()),
                           std::string(value.data(), value.size()));
    }

private:
    std::multimap<std::string, std::string>* metadata_;
};

class TracingServerInterceptor final : public grpc::experimental::Interceptor {
public:
    explicit TracingServerInterceptor(grpc::experimental::ServerRpcInfo* info)
        : info_(info), method_name_(info && info->method() ? info->method() : "unknown") {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        if (!IsTracingEnabled()) {
            methods->Proceed();
            return;
        }

        if (methods->QueryInterceptionHookPoint(HP::POST_RECV_INITIAL_METADATA)) {
            StartSpan(methods);
        }

        if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_STATUS)) {
            EndSpan(methods->GetSendStatus());
        } else if (methods->QueryInterceptionHookPoint(HP::POST_RECV_CLOSE)) {
            EndSpan(grpc::Status::CANCELLED);
        }

        methods->Proceed();
    }

private:
    void StartSpan(grpc::experimental::InterceptorBatchMethods* methods) {
        if (span_) {
            return;
        }

        ServerMetadataCarrier carrier(methods->GetRecvInitialMetadata());
        auto propagator = propagation::GlobalTextMapPropagator::GetGlobalPropagator();
        auto current_context = context::RuntimeContext::GetCurrent();
        auto parent_context = propagator->Extract(carrier, current_context);

        trace::StartSpanOptions options;
        options.kind = trace::SpanKind::kServer;
        options.parent = parent_context;

        span_ = GetTracer(kTracerName)->StartSpan(method_name_, options);
        scope_ = std::make_unique<trace::Scope>(span_);
        RememberServerContextTraceId(info_ ? info_->server_context() : nullptr,
                                     TraceIdFromSpan(span_));
    }

    void EndSpan(const grpc::Status& status) {
        if (!span_) {
            return;
        }

        if (status.ok()) {
            span_->SetStatus(trace::StatusCode::kOk);
        } else {
            span_->SetStatus(trace::StatusCode::kError, status.error_message());
        }

        span_->End();
        ForgetServerContextTraceId(info_ ? info_->server_context() : nullptr);
        scope_.reset();
        span_ = nullptr;
    }

    grpc::experimental::ServerRpcInfo* info_{nullptr};
    std::string method_name_;
    nostd::shared_ptr<trace::Span> span_;
    std::unique_ptr<trace::Scope> scope_;
};

class TracingClientInterceptor final : public grpc::experimental::Interceptor {
public:
    explicit TracingClientInterceptor(grpc::experimental::ClientRpcInfo* info)
        : info_(info), method_name_(info && info->method() ? info->method() : "unknown") {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        if (!IsTracingEnabled()) {
            methods->Proceed();
            return;
        }

        if (methods->QueryInterceptionHookPoint(HP::PRE_SEND_INITIAL_METADATA)) {
            StartSpan(methods);
        }

        if (methods->QueryInterceptionHookPoint(HP::POST_RECV_STATUS)) {
            EndSpan(methods->GetRecvStatus());
        }

        methods->Proceed();
    }

private:
    void StartSpan(grpc::experimental::InterceptorBatchMethods* methods) {
        if (span_) {
            return;
        }

        trace::StartSpanOptions options;
        options.kind = trace::SpanKind::kClient;
        span_ = GetTracer(kTracerName)->StartSpan(method_name_, options);
        scope_ = std::make_unique<trace::Scope>(span_);

        const std::string trace_id = TraceIdFromSpan(span_);
        RememberClientContextTraceId(info_ ? info_->client_context() : nullptr, trace_id);

        ClientMetadataCarrier carrier(methods->GetSendInitialMetadata());
        auto propagator = propagation::GlobalTextMapPropagator::GetGlobalPropagator();
        propagator->Inject(carrier, context::RuntimeContext::GetCurrent());

        spdlog::info("[trace_id: {}] Starting client RPC {}", trace_id, method_name_);
    }

    void EndSpan(grpc::Status* status) {
        if (!span_) {
            return;
        }

        if (status == nullptr || status->ok()) {
            span_->SetStatus(trace::StatusCode::kOk);
        } else {
            span_->SetStatus(trace::StatusCode::kError, status->error_message());
        }

        spdlog::info("[trace_id: {}] Finished client RPC {} with status {}",
                     TraceIdFromSpan(span_), method_name_,
                     status ? static_cast<int>(status->error_code()) : 0);

        span_->End();
        scope_.reset();
        span_ = nullptr;
    }

    grpc::experimental::ClientRpcInfo* info_{nullptr};
    std::string method_name_;
    nostd::shared_ptr<trace::Span> span_;
    std::unique_ptr<trace::Scope> scope_;
};

}  // namespace

void InitTracing(const TracingOptions& options) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_initialized) {
        return;
    }

    g_enabled = !IsFalseValue(GetEnvOrDefault("OTEL_TRACING_ENABLED", "true"));
    g_initialized = true;

    if (!g_enabled) {
        spdlog::info("OpenTelemetry tracing disabled by OTEL_TRACING_ENABLED");
        return;
    }

    const std::string service_name = GetEnvOrDefault(
        "OTEL_SERVICE_NAME",
        options.service_name.empty() ? kDefaultServiceName : options.service_name);
    const std::string endpoint = GetEnvOrDefault(
        "OTEL_EXPORTER_OTLP_ENDPOINT",
        options.exporter_otlp_endpoint.empty() ? kDefaultEndpoint
                                               : options.exporter_otlp_endpoint);

    otlp::OtlpGrpcExporterOptions exporter_options;
    exporter_options.endpoint = endpoint;

    auto exporter = std::make_unique<otlp::OtlpGrpcExporter>(exporter_options);
    sdktrace::BatchSpanProcessorOptions processor_options;
    auto processor = std::make_unique<sdktrace::BatchSpanProcessor>(
        std::move(exporter), processor_options);

    auto resource_attributes = resource::ResourceAttributes{
        {"service.name", service_name},
    };
    auto otel_resource = resource::Resource::Create(resource_attributes);

    g_sdk_provider = std::make_shared<sdktrace::TracerProvider>(
        std::move(processor), otel_resource, MakeSampler());
    std::shared_ptr<trace::TracerProvider> api_provider = g_sdk_provider;
    trace::Provider::SetTracerProvider(nostd::shared_ptr<trace::TracerProvider>(api_provider));

    auto propagator = nostd::shared_ptr<propagation::TextMapPropagator>(
        new trace::propagation::HttpTraceContext());
    propagation::GlobalTextMapPropagator::SetGlobalPropagator(propagator);

    spdlog::info("OpenTelemetry tracing initialized for service '{}' using OTLP endpoint '{}'",
                 service_name, endpoint);
}

void ShutdownTracing() {
    std::shared_ptr<sdktrace::TracerProvider> provider;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        provider = g_sdk_provider;
        g_sdk_provider.reset();
        g_server_context_trace_ids.clear();
        g_client_context_trace_ids.clear();
        g_initialized = false;
    }

    if (provider) {
        provider->ForceFlush();
        provider->Shutdown();
    }
}

bool IsTracingEnabled() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_initialized && g_enabled;
}

nostd::shared_ptr<trace::Tracer> GetTracer(const std::string& name) {
    return trace::Provider::GetTracerProvider()->GetTracer(name);
}

std::string CurrentTraceIdHex() {
    return TraceIdFromSpan(trace::GetSpan(context::RuntimeContext::GetCurrent()));
}

std::string CurrentSpanIdHex() {
    return SpanIdFromSpan(trace::GetSpan(context::RuntimeContext::GetCurrent()));
}

std::string TraceIdForServerContext(const grpc::ServerContextBase* context) {
    if (context != nullptr) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_server_context_trace_ids.find(context);
        if (it != g_server_context_trace_ids.end()) {
            return it->second;
        }
    }

    return CurrentTraceIdHex();
}

std::string TraceIdForClientContext(const grpc::ClientContext* context) {
    if (context != nullptr) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_client_context_trace_ids.find(context);
        if (it != g_client_context_trace_ids.end()) {
            return it->second;
        }
    }

    return CurrentTraceIdHex();
}

void ClearClientContextTraceId(const grpc::ClientContext* context) {
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_client_context_trace_ids.erase(context);
}

grpc::experimental::Interceptor*
TracingServerInterceptorFactory::CreateServerInterceptor(
    grpc::experimental::ServerRpcInfo* info) {
    if (!IsTracingEnabled()) {
        return nullptr;
    }
    return new TracingServerInterceptor(info);
}

grpc::experimental::Interceptor*
TracingClientInterceptorFactory::CreateClientInterceptor(
    grpc::experimental::ClientRpcInfo* info) {
    if (!IsTracingEnabled()) {
        return nullptr;
    }
    return new TracingClientInterceptor(info);
}

std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>
MakeTracingServerInterceptorFactory() {
    return std::make_unique<TracingServerInterceptorFactory>();
}

std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>
MakeTracingClientInterceptorFactory() {
    return std::make_unique<TracingClientInterceptorFactory>();
}

std::shared_ptr<grpc::Channel> CreateTracingChannel(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& credentials) {
    grpc::ChannelArguments args;
    return CreateTracingChannel(target, credentials, args);
}

std::shared_ptr<grpc::Channel> CreateTracingChannel(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& credentials,
    const grpc::ChannelArguments& args) {
    std::vector<std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
        interceptors;
    interceptors.push_back(MakeTracingClientInterceptorFactory());

    return grpc::experimental::CreateCustomChannelWithInterceptors(
        target, credentials, args, std::move(interceptors));
}

}  // namespace otel
