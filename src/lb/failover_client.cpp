#include "lb/failover_client.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <utility>

namespace lb {

FailoverClientBase::FailoverClientBase(LbConfig config, ChannelBuilder channel_builder)
    : endpoints_(std::move(config.endpoints)),
      manager_(std::make_unique<EndpointManager>(
          endpoints_.size(),
          EndpointManager::Options{config.cooldown_base, kDefaultCooldownCap})) {
    for (const std::string& entry : config.rejected) {
        spdlog::warn("lb: ignoring malformed endpoint entry '{}'", entry);
    }
    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        spdlog::info("lb: endpoint {} = {}", i, endpoints_[i].Target());
    }

    const ChannelFactoryOptions factory_options{endpoints_.size() > 1};
    channels_ = BuildChannels(endpoints_, factory_options, std::move(channel_builder));

    failover_options_.max_attempts = config.max_attempts;
    failover_options_.attempt_timeout = config.attempt_timeout;
}

}  // namespace lb
