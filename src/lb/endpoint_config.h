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
    std::chrono::milliseconds attempt_timeout{2000};
    int max_attempts = 0;               // 0 = all endpoints; resolved by CallWithFailover.
                                        // LoadLbConfigFromEnv resolves to min(env, count).
    std::vector<std::string> rejected;  // for caller-side logging
};

// Reads GRPC_TARGET_ENDPOINTS, GRPC_LB_COOLDOWN_BASE_MS,
// GRPC_LB_MAX_ATTEMPTS, GRPC_LB_ATTEMPT_TIMEOUT_MS.
// Unset, empty, or fully rejected endpoint list falls back to localhost:50051.
LbConfig LoadLbConfigFromEnv();

}  // namespace lb
