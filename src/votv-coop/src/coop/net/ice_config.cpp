// coop/net/ice_config.cpp -- see ice_config.h.

#include "ice_config.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>
#pragma warning(pop)

namespace coop::net {

bool ApplyGlobalIceConfig(const IceConfig& ice) {
    auto* utils = SteamNetworkingUtils();
    if (!utils) {
        UE_LOGE("ice: SteamNetworkingUtils() null -- GNS not initialized");
        return false;
    }

    // Every value is written, because these are process-global and a session must not run on its
    // predecessor's: a previous session's relay-only policy, or a TURN credential minted for a
    // lobby that ended, stays in effect until something overwrites it. An empty STUN list is
    // meaningful to GNS ("NAT piercing will not be attempted"), and an empty TURN list offers no
    // relay candidate. Each write is checked by name; the first refusal ends the chain and the
    // start, since the values after it are still the previous session's.
    const char* refused = nullptr;
    if (!utils->SetGlobalConfigValueInt32(
            k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
            ice.relayOnly ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay
                          : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All))
        refused = "the candidate policy";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_STUN_ServerList, ice.stunList.c_str()))
        refused = "the STUN list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_ServerList, ice.turnList.c_str()))
        refused = "the TURN list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_UserList, ice.turnUser.c_str()))
        refused = "the TURN user list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_PassList, ice.turnPass.c_str()))
        refused = "the TURN password list";
    if (refused) {
        UE_LOGE("ice: GNS refused %s -- not applied (policy=%s)", refused,
                ice.relayOnly ? "relay" : "all");
        return false;
    }

    UE_LOGI("ice: applied policy=%s stun='%s' turn='%s'",
            ice.relayOnly ? "relay" : "all",
            ice.stunList.empty() ? "(none)" : ice.stunList.c_str(),
            ice.turnList.empty() ? "(none)" : ice.turnList.c_str());
    return true;
}

}  // namespace coop::net
