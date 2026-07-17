// coop/net/ice_config.cpp -- see ice_config.h.

#include "ice_config.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>
#pragma warning(pop)

namespace coop::net {

namespace {

int IceEnableFlags(IceEnable e) {
    switch (e) {
        case IceEnable::Disable:
            return k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable;
        case IceEnable::RelayOnly:
            return k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay;
        case IceEnable::All:
            return k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All;
        case IceEnable::Default:
        default:
            return k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default;
    }
}

}  // namespace

void ApplyGlobalIceConfig(const IceConfig& ice) {
    auto* utils = SteamNetworkingUtils();
    if (!utils) {
        UE_LOGE("ice: SteamNetworkingUtils() null -- GNS not initialized");
        return;
    }

    // Which candidate types to gather/share. Leave GNS's user default in place
    // when the caller asks for Default (don't write a value).
    if (ice.enable != IceEnable::Default) {
        utils->SetGlobalConfigValueInt32(
            k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, IceEnableFlags(ice.enable));
    }

    // STUN server list (rung 2 hole-punch). An empty string is a MEANINGFUL
    // value to GNS: "NAT piercing will not be attempted" -- so set it
    // unconditionally to reflect the caller's intent exactly.
    utils->SetGlobalConfigValueString(
        k_ESteamNetworkingConfig_P2P_STUN_ServerList, ice.stunList.c_str());

    // TURN relay (rung 3, coturn). Only meaningful with all three parallel
    // lists; skip entirely when no TURN server is configured.
    if (!ice.turnList.empty()) {
        utils->SetGlobalConfigValueString(
            k_ESteamNetworkingConfig_P2P_TURN_ServerList, ice.turnList.c_str());
        utils->SetGlobalConfigValueString(
            k_ESteamNetworkingConfig_P2P_TURN_UserList, ice.turnUser.c_str());
        utils->SetGlobalConfigValueString(
            k_ESteamNetworkingConfig_P2P_TURN_PassList, ice.turnPass.c_str());
    }

    UE_LOGI("ice: applied enable=%d stun='%s' turn='%s'",
            static_cast<int>(ice.enable),
            ice.stunList.empty() ? "(none)" : ice.stunList.c_str(),
            ice.turnList.empty() ? "(none)" : ice.turnList.c_str());
}

}  // namespace coop::net
