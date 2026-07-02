#pragma once

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "lb/endpoint_manager.h"

namespace lb {

struct FailoverOptions {
    // Deadline for a single endpoint attempt. This must exceed the channel's
    // built-in retry budget (maxBackoff is 1s, see channel_factory), or an
    // in-channel retry can be cut off before it completes. Total worst-case
    // wall-clock for a failed call is up to endpoints_tried * attempt_timeout.
    std::chrono::milliseconds attempt_timeout{2000};
    int max_attempts = 0;  // <= 0: one attempt per endpoint
    std::vector<grpc::StatusCode> retriable_codes{grpc::StatusCode::UNAVAILABLE};
};

// Runs `rpc` against endpoints chosen by `manager`, failing over to the next
// endpoint on a retriable status. `rpc` is invoked as
//   grpc::Status rpc(grpc::ClientContext& context, std::size_t endpoint_index)
// and must issue the RPC through the stub matching `endpoint_index`.
//
// Layering: gRPC's built-in retry (service config) handles transient blips
// within an endpoint; this loop handles endpoint-level failure by moving to
// the next FQDN. Each endpoint is tried at most once per logical call, each
// attempt on a fresh ClientContext (contexts are single-use) with its own
// deadline. Non-retriable statuses (application errors) return immediately —
// they can never succeed on another endpoint.
template <typename Fn>
grpc::Status CallWithFailover(EndpointManager& manager, const FailoverOptions& options,
                              Fn&& rpc) {
    const std::size_t endpoint_count = manager.size();
    const std::size_t attempt_cap =
        options.max_attempts > 0
            ? std::min<std::size_t>(static_cast<std::size_t>(options.max_attempts),
                                    endpoint_count)
            : endpoint_count;

    std::vector<bool> tried(endpoint_count, false);
    grpc::Status last_status(grpc::StatusCode::UNAVAILABLE, "no endpoint attempted");

    for (std::size_t attempt = 0; attempt < attempt_cap; ++attempt) {
        std::size_t index = manager.Select();
        if (tried[index]) {
            // Cooldowns can steer Select() back to a tried endpoint; probe forward.
            for (std::size_t k = 0; k < endpoint_count && tried[index]; ++k) {
                index = (index + 1) % endpoint_count;
            }
            if (tried[index]) break;  // every endpoint tried
        }
        tried[index] = true;

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + options.attempt_timeout);
        grpc::Status status = rpc(context, index);

        if (status.ok()) {
            if (manager.ReportSuccess(index)) {
                spdlog::info("lb: endpoint {} recovered", index);
            }
            return status;
        }
        const bool retriable =
            std::find(options.retriable_codes.begin(), options.retriable_codes.end(),
                      status.error_code()) != options.retriable_codes.end();
        if (!retriable) {
            return status;
        }
        manager.ReportFailure(index);
        spdlog::warn("lb: endpoint {} unavailable ({}), failing over", index,
                     status.error_message());
        last_status = std::move(status);
    }
    return last_status;
}

}  // namespace lb
