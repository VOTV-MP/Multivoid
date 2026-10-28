// coop/net/connection_tuning.cpp -- see coop/net/connection_tuning.h.

#include "connection_tuning.h"

#include "coop/config/config.h"           // ResolveInt / ResolveFlag, the wire knobs
#include "coop/config/config_registry.h"  // rows::net_sendbuf_kb / net_sendrate_kbs
#include "coop/net/send_rate_control.h"   // StartRateBps, the rate a measured link opens on
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>  // SteamNetworkingUtils(), the per-connection wire knobs
#pragma warning(pop)

namespace coop::net {

void TuneConnection(uint32_t hConn, bool rateControlled, long pinnedRateKbs) {
    const auto h = static_cast<HSteamNetConnection>(hConn);

    // The lanes. On failure reliable sends collapse to lane 0 -- functional, no priority routing.
    constexpr int kLaneCount = 3;  // matches Lane::Count in session_lanes.h
    // Three distinct priorities and no weights: weights act only within one priority
    // (isteamnetworkingsockets.h, ConfigureConnectionLanes), so the drain is strictly lane 0, 1, 2.
    const int priorities[kLaneCount] = { 0, 1, 2 };
    const EResult rc = SteamNetworkingSockets()->ConfigureConnectionLanes(
        h, kLaneCount, priorities, nullptr);
    if (rc != k_EResultOK) {
        UE_LOGW("net: ConfigureConnectionLanes(h=0x%08x) rc=%d",
                static_cast<unsigned>(hConn), static_cast<int>(rc));
    }

    auto* utils = SteamNetworkingUtils();
    if (!utils) return;   // nothing below can be written; the lanes above already are

    // The send buffer those lanes share. 0 on the knob means the mod's own default.
    const long bufKb = coop::config::ResolveInt(coop::config_registry::rows::net_sendbuf_kb);
    if (bufKb > 0) {
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendBufferSize,
                                             static_cast<int32>(bufKb) * 1024);
        UE_LOGW("net: send buffer PINNED to %ld KB for h=0x%08x (drill knob net.sendbuf_kb)",
                bufKb, static_cast<unsigned>(hConn));
    } else {
        // GNS's 512 KB default is smaller than a join burst (~740 KB of PropSpawns plus the connect
        // replay), so the backlog engaged on every join; 4 MB makes it the exception, a real slow
        // link. The backlog stays the correctness net either way.
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendBufferSize,
                                             kDefaultSendBufBytes);
    }

    // The rate the connection opens at. SendRateMin and Max are written to one value, which is the
    // GNS header's own way of saying the application owns this rate. The drill's pin wins outright;
    // otherwise the measured-rate controller's opening rate, written HERE rather than at its first
    // decision, because the admission exchange that runs before a slot exists is inside that
    // window. What the window holds is now GNS's own stock flat 256 KB/s, not its ping-derived
    // guess: with no global pin, `SendRateMin == SendRateMax` and `SNP_ClampSendRate` takes the
    // "application has disabled bandwidth estimation" branch (`snp.cpp:4249-4255`), so
    // `m_nCurrentSendRateEstimate` is never consulted. 1.77x our opening rate, and flat.
    //
    // With both off nothing is written and the link runs at GNS's stock 256 KB/s, fixed. That is
    // the control arm of an experiment and not a shipped path: there is no global pin behind this
    // any more, so these are the only two writers of an opening rate in the process.
    if (pinnedRateKbs > 0) {
        const int32 pinned = static_cast<int32>(pinnedRateKbs) * 1024;
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendRateMin, pinned);
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendRateMax, pinned);
        UE_LOGW("net: send rate PINNED to %ld KB/s for h=0x%08x (drill knob net.sendrate_kbs)",
                pinnedRateKbs, static_cast<unsigned>(hConn));
    } else if (rateControlled) {
        const int32 open = static_cast<int32>(SendRateControl::StartRateBps());
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendRateMin, open);
        utils->SetConnectionConfigValueInt32(h, k_ESteamNetworkingConfig_SendRateMax, open);
        UE_LOGI("net: send rate opens at %d B/s for h=0x%08x, measured from here (net.ratecontrol)",
                open, static_cast<unsigned>(hConn));
    }
}

}  // namespace coop::net
