// coop/net/ice_config.h -- ICE (STUN/TURN) configuration for P2P connections.
//
// Co-located net-internal header (src tree, not include/ -- same as
// session_lanes.h). Used by session_start.cpp's P2P branch only. Keeps the GNS
// config-value enums out of session.h: callers describe ICE in our own
// vocabulary; the .cpp maps it to SteamNetworkingUtils()->SetGlobalConfigValue*.
//
// One session per process, so the config is applied GLOBALLY (the shape the
// working test_p2p example proves), not per-connection. A per-connection "opts
// array" is an equivalent alternative; global is simpler + proven.

#pragma once

#include <string>

namespace coop::net {

struct IceConfig {
    std::string stunList;   // "host:port,host2:port" -- "" disables STUN (rung 2)
    std::string turnList;   // "turn:host:port,..."   -- "" disables TURN (rung 3)
    std::string turnUser;   // parallel to turnList (coturn REST creds)
    std::string turnPass;   // parallel to turnList
    // Which candidates to gather and share: every kind (private, public, relay: rungs 1-3), or the
    // TURN relay's alone, so the peer is shown the relay's address instead of ours.
    bool        relayOnly = false;
};

// Apply the ICE configuration to GNS as GLOBAL config values: the candidate policy and all three
// TURN lists, written every time, empty included, so nothing of a previous session in this process
// carries into the next. Idempotent. Call after GameNetworkingSockets_Init and before
// CreateListenSocketP2P / Connect, on a thread where SteamNetworkingUtils() is valid (post-init).
void ApplyGlobalIceConfig(const IceConfig& ice);

}  // namespace coop::net
