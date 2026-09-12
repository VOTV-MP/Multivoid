// harness/autotest/autotest_kickprobe.cpp -- the after-join end-reason drill, host side. Once a
// client holds slot 1 and has announced its world, hold ten seconds, then kick it with the
// moderation code. The client's log must carry the DISCONNECTED notice with that code: the
// notice a player sees when a host kicks, bans or quits, where that close used to be a log line
// and a flee. The client side is a log diff. Env VOTVCOOP_RUN_KICK_PROBE=1, host.

#include "harness/autotest.h"

#include "coop/net/end_reason.h"
#include "coop/net/session.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

namespace harness::autotest {
namespace {

constexpr DWORD kPollMs = 500;
constexpr DWORD kSeatWaitMs = 180'000;  // the client boots, joins, downloads and loads first
constexpr DWORD kSettleMs = 10'000;     // seated, in the world, and playing before the kick

}  // namespace

void RunKickProbe() {
    if (IsClientRole()) {
        UE_LOGI("kick_probe: client role -- the host runs the kick; this log is the evidence");
        return;
    }
    auto& s = harness::session_runtime::Session();
    DWORD waited = 0;
    while (!(s.IsSlotReady(1) && s.IsSlotWorldReady(1))) {
        if (waited >= kSeatWaitMs) {
            UE_LOGW("kick_probe: INCONCLUSIVE -- no client was seated and world-ready in slot 1 "
                    "within %lu s", static_cast<unsigned long>(kSeatWaitMs / 1000));
            return;
        }
        ::Sleep(kPollMs);
        waited += kPollMs;
    }
    UE_LOGI("kick_probe: slot 1 seated and world-ready -- kicking in %lu s",
            static_cast<unsigned long>(kSettleMs / 1000));
    ::Sleep(kSettleMs);
    const coop::net::EndReason code = coop::net::EndReason::KickedByHost;
    const bool kicked = s.Kick(1, code, "kicked by host");
    UE_LOGI("kick_probe: %s slot 1 [%s] -- the client log must carry the DISCONNECTED notice "
            "with this id", kicked ? "KICKED" : "the kick did nothing on",
            coop::net::Describe(code).id);
}

DWORD WINAPI KickProbeThread(LPVOID) {
    RunKickProbe();
    return 0;
}

}  // namespace harness::autotest
