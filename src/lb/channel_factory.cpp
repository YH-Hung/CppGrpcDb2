#include "lb/channel_factory.h"

#include <json/json.h>

namespace lb {

namespace {

// proto3 Duration JSON form: decimal seconds with an "s" suffix, e.g. "0.25s".
std::string FormatSeconds(std::chrono::milliseconds duration) {
    const auto ms = duration.count();
    std::string out = std::to_string(ms / 1000);
    if (ms % 1000 != 0) {
        std::string frac = std::to_string(ms % 1000);
        frac.insert(0, 3 - frac.size(), '0');
        while (frac.back() == '0') frac.pop_back();
        out += "." + frac;
    }
    return out + "s";
}

}  // namespace

std::string BuildServiceConfigJson(const ChannelFactoryOptions& options) {
    Json::Value retry_policy;
    retry_policy["maxAttempts"] =
        options.multi_endpoint ? 2 : options.max_attempts.value_or(4);
    retry_policy["initialBackoff"] = FormatSeconds(options.initial_backoff);
    retry_policy["maxBackoff"] = FormatSeconds(options.max_backoff);
    retry_policy["backoffMultiplier"] = 2;
    Json::Value retryable_codes(Json::arrayValue);
    retryable_codes.append("UNAVAILABLE");
    retry_policy["retryableStatusCodes"] = retryable_codes;

    Json::Value method_entry;
    // An empty name entry matches every service and method on the channel.
    Json::Value name_array(Json::arrayValue);
    name_array.append(Json::Value(Json::objectValue));
    method_entry["name"] = name_array;
    method_entry["retryPolicy"] = retry_policy;

    Json::Value method_config(Json::arrayValue);
    method_config.append(method_entry);
    Json::Value service_config;
    service_config["methodConfig"] = method_config;

    Json::StreamWriterBuilder writer_builder;
    writer_builder["commentStyle"] = "None";
    writer_builder["indentation"] = "";
    return Json::writeString(writer_builder, service_config);
}

grpc::ChannelArguments MakeChannelArguments(const ChannelFactoryOptions& options) {
    grpc::ChannelArguments args;
    args.SetServiceConfigJSON(BuildServiceConfigJson(options));
    args.SetInt(GRPC_ARG_ENABLE_RETRIES, 1);
    // Only takes effect when a single endpoint's FQDN resolves to multiple
    // A/AAAA records; balances gRPC's own subchannels across them.
    args.SetLoadBalancingPolicyName("round_robin");
    // Detect half-dead connections in seconds instead of at the TCP timeout.
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    return args;
}

std::vector<std::shared_ptr<grpc::Channel>> BuildChannels(
    const std::vector<Endpoint>& endpoints, const ChannelFactoryOptions& options,
    ChannelBuilder builder) {
    if (!builder) {
        builder = [](const Endpoint& endpoint, const grpc::ChannelArguments& args) {
            return grpc::CreateCustomChannel(endpoint.Target(),
                                             grpc::InsecureChannelCredentials(), args);
        };
    }
    const grpc::ChannelArguments args = MakeChannelArguments(options);
    std::vector<std::shared_ptr<grpc::Channel>> channels;
    channels.reserve(endpoints.size());
    for (const Endpoint& endpoint : endpoints) {
        channels.push_back(builder(endpoint, args));
    }
    return channels;
}

}  // namespace lb
