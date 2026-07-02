#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace lb {

struct Endpoint {
    std::string host;
    int port = 0;

    std::string Target() const { return host + ":" + std::to_string(port); }
    bool operator==(const Endpoint&) const = default;
};

struct ParseResult {
    std::vector<Endpoint> endpoints;
    std::vector<std::string> rejected;  // non-empty entries that failed validation
};

// Splits a comma-separated "host:port" list. Entries empty after trimming are
// skipped silently. Pure: no logging, no env access — callers log `rejected`.
ParseResult ParseEndpoints(std::string_view csv);

struct LbConfig {
    std::vector<Endpoint> endpoints;
    std::chrono::milliseconds cooldown_base{1000};
    int max_attempts = 1;               // resolved: min(env value, endpoint count)
    std::vector<std::string> rejected;  // for caller-side logging
};

// Reads GRPC_TARGET_ENDPOINTS, GRPC_LB_COOLDOWN_BASE_MS, GRPC_LB_MAX_ATTEMPTS.
// Unset, empty, or fully rejected endpoint list falls back to localhost:50051.
LbConfig LoadLbConfigFromEnv();

}  // namespace lb
