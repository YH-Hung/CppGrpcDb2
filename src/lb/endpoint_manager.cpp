#include "lb/endpoint_manager.h"

#include <algorithm>
#include <cassert>

namespace lb {

EndpointManager::EndpointManager(std::size_t endpoint_count)
    : EndpointManager(endpoint_count, Options{}) {}

EndpointManager::EndpointManager(std::size_t endpoint_count, const Options& options)
    : EndpointManager(endpoint_count, options, [] { return std::chrono::steady_clock::now(); }) {}

EndpointManager::EndpointManager(std::size_t endpoint_count, const Options& options, const Clock& clock)
    : options_(options),
      clock_(clock ? clock : [] { return std::chrono::steady_clock::now(); }),
      states_(endpoint_count) {
    assert(endpoint_count > 0);
}

std::size_t EndpointManager::Select() {
    std::lock_guard<std::mutex> lock(mu_);
    const TimePoint now = clock_();
    for (std::size_t k = 0; k < states_.size(); ++k) {
        const std::size_t index = (cursor_ + k) % states_.size();
        if (states_[index].unavailable_until <= now) {
            cursor_ = (index + 1) % states_.size();
            return index;
        }
    }
    // All endpoints cooling down: pick the one recovering soonest.
    const auto soonest = std::min_element(
        states_.begin(), states_.end(), [](const State& a, const State& b) {
            return a.unavailable_until < b.unavailable_until;
        });
    return static_cast<std::size_t>(soonest - states_.begin());
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
