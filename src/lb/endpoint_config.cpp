#include "lb/endpoint_config.h"

#include <algorithm>
#include <cstdlib>

namespace lb {
namespace {

std::string_view Trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

bool ParsePort(std::string_view s, int& out) {
    if (s.empty() || s.size() > 5) return false;
    int value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 65535) return false;
    out = value;
    return true;
}

int ParsePositiveIntEnv(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 1000000) return fallback;
    return static_cast<int>(parsed);
}

}  // namespace

ParseResult ParseEndpoints(std::string_view csv) {
    ParseResult result;
    std::size_t pos = 0;
    while (pos < csv.size()) {
        const std::size_t comma = std::min(csv.find(',', pos), csv.size());
        const std::string_view entry = Trim(csv.substr(pos, comma - pos));
        pos = comma + 1;

        if (entry.empty()) continue;  // trailing/duplicate commas are harmless
        const std::size_t colon = entry.rfind(':');
        int port = 0;
        if (colon == std::string_view::npos || colon == 0 ||
            !ParsePort(entry.substr(colon + 1), port)) {
            result.rejected.emplace_back(entry);
            continue;
        }
        result.endpoints.push_back({std::string(entry.substr(0, colon)), port});
    }
    return result;
}

LbConfig LoadLbConfigFromEnv() {
    LbConfig config;
    const char* csv = std::getenv("GRPC_TARGET_ENDPOINTS");
    ParseResult parsed =
        ParseEndpoints(csv == nullptr ? std::string_view{} : std::string_view{csv});
    config.endpoints = std::move(parsed.endpoints);
    config.rejected = std::move(parsed.rejected);
    if (config.endpoints.empty()) {
        config.endpoints.push_back({"localhost", 50051});
    }
    config.cooldown_base =
        std::chrono::milliseconds(ParsePositiveIntEnv("GRPC_LB_COOLDOWN_BASE_MS", 1000));
    config.attempt_timeout =
        std::chrono::milliseconds(ParsePositiveIntEnv("GRPC_LB_ATTEMPT_TIMEOUT_MS", 2000));
    const int endpoint_count = static_cast<int>(config.endpoints.size());
    config.max_attempts =
        std::min(ParsePositiveIntEnv("GRPC_LB_MAX_ATTEMPTS", endpoint_count), endpoint_count);
    return config;
}

}  // namespace lb
