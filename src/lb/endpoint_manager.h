#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lb {

// Default cooldown ceiling shared by the facade and any caller that doesn't
// want to pick its own. Distinct from the gRPC service-config maxBackoff
// (which bounds in-channel retry backoff, not app-level cooldown).
inline constexpr std::chrono::milliseconds kDefaultCooldownCap{30000};

// Round-robin endpoint selection with exponential cooldown for failing
// endpoints. Indices refer to a caller-owned parallel container of
// channels/stubs. Thread-safe. gRPC-free; the clock is injectable so cooldown
// logic is testable without sleeping.
class EndpointManager {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;

    struct Options {
        std::chrono::milliseconds cooldown_base{1000};
        std::chrono::milliseconds cooldown_cap{30000};
    };

    struct Snapshot {
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
        int consecutive_failures = 0;
        bool in_cooldown = false;
    };

    explicit EndpointManager(std::size_t endpoint_count);
    EndpointManager(std::size_t endpoint_count, const Options& options);
    EndpointManager(std::size_t endpoint_count, const Options& options, const Clock& clock);

    // Next endpoint to try: round-robin over endpoints not in cooldown. If all
    // are cooling down, returns the one whose cooldown expires soonest rather
    // than failing the call outright.
    std::size_t Select();

    // As above, but skips endpoints the caller has already tried this logical
    // call (`already_tried[i] == true`). Among the remaining endpoints the same
    // policy applies: round-robin over those not in cooldown, else the one
    // recovering soonest. The failover loop uses this so an all-cooldown
    // fallback lands on the untried endpoint recovering soonest instead of
    // probing to an arbitrary, possibly worse, endpoint by index.
    // Throws std::invalid_argument if every endpoint is marked tried.
    std::size_t Select(const std::vector<bool>& already_tried);

    // Returns true if this success ended a failure streak (endpoint recovered).
    bool ReportSuccess(std::size_t index);

    void ReportFailure(std::size_t index);

    Snapshot GetSnapshot(std::size_t index) const;

    std::size_t size() const { return states_.size(); }

private:
    struct State {
        int consecutive_failures = 0;
        TimePoint unavailable_until{};
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
    };

    Options options_;
    Clock clock_;
    mutable std::mutex mu_;
    std::vector<State> states_;
    std::size_t cursor_ = 0;
};

}  // namespace lb
