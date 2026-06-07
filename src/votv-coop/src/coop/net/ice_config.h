// coop/net/ice_config.h -- ICE (STUN/TURN) configuration for P2P connections.
//
// Co-located net-internal header (src tree, not include/ -- same as
// session_lanes.h). Used by session_start.cpp's P2P branch only. Keeps the GNS
// config-value enums out of session.h: callers describe ICE in our own
// vocabulary; the .cpp maps it to SteamNetworkingUtils()->SetGlobalConfigValue*.
//
// One session per process, so the config is applied GLOBALLY (the shape the
// working test_p2p example proves), not per-connection. The design doc's
// "opts array" idea is an equivalent alternative; global is simpler + proven.
// See research/findings/votv-zero-ports-connectivity-ladder-design-2026-06-05.md s3.6.

#pragma once

#include <string>

namespace coop::net {

// Which ICE candidate types to gather + share with the peer. Maps to the GNS
// k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_* bitmask in the .cpp.
enum class IceEnable {
    Default,    // leave the GNS user default untouched
    Disable,    // no ICE at all (no IP disclosure)
    RelayOnly,  // TURN relay candidates only (rung 3)
    All,        // private + public + relay (rungs 1-3) -- the normal choice
};

struct IceConfig {
    std::string stunList;   // "host:port,host2:port" -- "" disables STUN (rung 2)
    std::string turnList;   // "turn:host:port,..."   -- "" disables TURN (rung 3)
    std::string turnUser;   // parallel to turnList (coturn REST creds)
    std::string turnPass;   // parallel to turnList
    IceEnable   enable = IceEnable::All;
};

// Apply the ICE configuration to GNS as GLOBAL config values. Sets the
// STUN/TURN server lists + the ICE-enable bitmask. Idempotent. Call once after
// GameNetworkingSockets_Init and before CreateListenSocketP2P / Connect, on a
// thread where SteamNetworkingUtils() is valid (post-init).
void ApplyGlobalIceConfig(const IceConfig& ice);

}  // namespace coop::net
