#pragma once

#include <grpc/impl/channel_arg_names.h>

// Both halves of the *aggressive* keepalive contract between this repo's lb
// clients and the in-repo servers. MakeChannelArguments leaves client
// keepalive disabled by default (the only setting safe against uncoordinated
// servers); a client opts into this contract via ChannelFactoryOptions
// (keepalive_time = kKeepaliveTimeMs, keepalive_permit_without_calls = true),
// and every in-repo server an lb client targets applies
// AddKeepaliveServerArgs — otherwise the server answers the opted-in ping
// rate with GOAWAY("too_many_pings") and the connection churns.

namespace lb {

// Client (opt-in): ping every 10s, even without active calls, so half-dead
// connections are detected in seconds instead of at the TCP timeout.
inline constexpr int kKeepaliveTimeMs = 10000;
inline constexpr int kKeepaliveTimeoutMs = 5000;

// Server: minimum interval between unsolicited client pings the server
// accepts. Half the client interval leaves headroom for scheduling jitter.
inline constexpr int kServerMinRecvPingIntervalMs = kKeepaliveTimeMs / 2;

// Applies the server half to a grpc::ServerBuilder before BuildAndStart().
// Templated so tests can record the arguments with a fake builder.
template <typename ServerBuilderT>
void AddKeepaliveServerArgs(ServerBuilderT& builder) {
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
                               kServerMinRecvPingIntervalMs);
}

}  // namespace lb
