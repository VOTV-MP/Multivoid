// harness/autotest/autotest_floodprobe.cpp -- the connection-cap drill, client side. Once this
// client holds its seat and has settled, it opens six more raw connections to the host from the
// same address, two seconds apart, and says nothing on them: under the default cap of four per
// thirty seconds the host parks at most four of them (this client's own join counts while its
// stamp is inside the window) and refuses the rest with the flood code, and the seated client
// is untouched. After the refusal lifts it opens one more, which the host must park again. The
// host's log and this client's are the evidence, read by the driver. Env
// VOTVCOOP_RUN_FLOOD_PROBE=1, client.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

constexpr DWORD kPollMs = 500;
constexpr DWORD kSeatWaitMs = 180'000;   // the client boots, joins and finishes its link first
constexpr DWORD kSettleMs = 25'000;      // the download and the world load finish before the flood
constexpr int   kJunkConnections = 6;    // cap 4: at most four park, at least two are refused
constexpr DWORD kJunkGapMs = 2000;
constexpr DWORD kRefusalWaitMs = 40'000;  // past the thirty-second window and refusal

}  // namespace

void RunFloodProbe() {
    if (!IsClientRole()) {
        UE_LOGI("flood_probe: host role -- the client opens the connections; this log is the "
                "evidence");
        return;
    }
    auto& s = harness::session_runtime::Session();
    DWORD waited = 0;
    while (!s.IsSlotReady(0)) {
        if (waited >= kSeatWaitMs) {
            UE_LOGW("flood_probe: INCONCLUSIVE -- this client never finished its link within "
                    "%lu s", static_cast<unsigned long>(kSeatWaitMs / 1000));
            return;
        }
        ::Sleep(kPollMs);
        waited += kPollMs;
    }
    UE_LOGI("flood_probe: seated -- settling %lu s before the flood",
            static_cast<unsigned long>(kSettleMs / 1000));
    ::Sleep(kSettleMs);

    const std::string peerIp =
        coop::config::ResolveString(coop::config_registry::rows::net_peer);
    const long port = coop::config::ResolveInt(coop::config_registry::rows::net_port);
    SteamNetworkingIPAddr addr{};
    addr.Clear();
    if (!addr.ParseString(peerIp.c_str())) {
        UE_LOGW("flood_probe: INCONCLUSIVE -- net.peer '%s' did not parse", peerIp.c_str());
        return;
    }
    addr.m_port = static_cast<uint16>(port);
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) {
        UE_LOGW("flood_probe: INCONCLUSIVE -- no transport");
        return;
    }
    // The junk connections say nothing and are never closed by this side: the host parks or
    // refuses each, and its close reaches the session's status callback, which logs the code
    // for a connection it never seated and releases the handle.
    for (int i = 1; i <= kJunkConnections; ++i) {
        const HSteamNetConnection h = sockets->ConnectByIPAddress(addr, 0, nullptr);
        UE_LOGI("flood_probe: junk connection %d of %d to %s:%ld -- h=0x%08x (says nothing)",
                i, kJunkConnections, peerIp.c_str(), port, static_cast<unsigned>(h));
        ::Sleep(kJunkGapMs);
    }
    UE_LOGI("flood_probe: waiting %lu s for the refusal to lift",
            static_cast<unsigned long>(kRefusalWaitMs / 1000));
    ::Sleep(kRefusalWaitMs);
    const HSteamNetConnection h = sockets->ConnectByIPAddress(addr, 0, nullptr);
    UE_LOGI("flood_probe: recovery connection -- h=0x%08x; the host must park it",
            static_cast<unsigned>(h));
    ::Sleep(5000);
    UE_LOGI("flood_probe: DONE -- the verdict is in the host and client logs");
}

DWORD WINAPI FloodProbeThread(LPVOID) {
    RunFloodProbe();
    return 0;
}

}  // namespace harness::autotest
