#include "lb/endpoint_manager.h"

#include <algorithm>
#include <stdexcept>

namespace lb {

EndpointManager::EndpointManager(std::size_t endpoint_count)
    : EndpointManager(endpoint_count, Options{}) {}

EndpointManager::EndpointManager(std::size_t endpoint_count, const Options& options)
    : EndpointManager(endpoint_count, options, [] { return std::chrono::steady_clock::now(); }) {}

EndpointManager::EndpointManager(std::size_t endpoint_count, const Options& options, const Clock& clock)
    : options_(options),
      clock_(clock ? clock : [] { return std::chrono::steady_clock::now(); }),
      states_(endpoint_count) {
    if (endpoint_count == 0) {
        throw std::invalid_argument("EndpointManager requires at least one endpoint");
    }
}

std::size_t EndpointManager::Select() { return Select(std::vector<bool>{}); }

std::size_t EndpointManager::Select(const std::vector<bool>& already_tried) {
    std::lock_guard<std::mutex> lock(mu_);
    const TimePoint now = clock_();
    const auto tried = [&](std::size_t i) {
        return i < already_tried.size() && already_tried[i];
    };
    // Round-robin over endpoints that are neither already tried nor in cooldown.
    for (std::size_t k = 0; k < states_.size(); ++k) {
        const std::size_t index = (cursor_ + k) % states_.size();
        if (tried(index)) continue;
        if (states_[index].unavailable_until <= now) {
            cursor_ = (index + 1) % states_.size();
            return index;
        }
    }
    // Every still-untried endpoint is cooling down: pick the one recovering
    // soonest among them, rather than failing the call outright.
    std::size_t soonest = states_.size();
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (tried(i)) continue;
        if (soonest == states_.size() ||
            states_[i].unavailable_until < states_[soonest].unavailable_until) {
            soonest = i;
        }
    }
    if (soonest == states_.size()) {
        // Misuse: every endpoint was marked tried, so there is nothing to pick.
        throw std::invalid_argument(
            "EndpointManager::Select called with every endpoint marked tried");
    }
    return soonest;
}

bool EndpointManager::ReportSuccess(std::size_t index) {
    std::lock_guard<std::mutex> lock(mu_);
    State& state = states_.at(index);
    ++state.successes;
    const bool recovered = state.consecutive_failures > 0;
    state.consecutive_failures = 0;
    state.unavailable_until = TimePoint{};
    return recovered;
}

void EndpointManager::ReportFailure(std::size_t index) {
    std::lock_guard<std::mutex> lock(mu_);
    State& state = states_.at(index);
    ++state.failures;
    ++state.consecutive_failures;
    const int exponent = std::min(state.consecutive_failures - 1, 20);  // bound the shift
    auto cooldown = options_.cooldown_base * (1LL << exponent);
    cooldown = std::min<std::chrono::milliseconds>(cooldown, options_.cooldown_cap);
    state.unavailable_until = clock_() + cooldown;
}

EndpointManager::Snapshot EndpointManager::GetSnapshot(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    const State& state = states_.at(index);
    return {state.successes, state.failures, state.consecutive_failures,
            state.unavailable_until > clock_()};
}

}  // namespace lb
