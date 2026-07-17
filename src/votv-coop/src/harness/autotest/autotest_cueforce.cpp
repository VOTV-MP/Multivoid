// harness/autotest_cueforce.cpp -- starRain cue-force driver (event_cue join-snapshot e2e).
//
// HOST-ONLY. Posts runEvent('starRain') through the F1 seam (event_fire_sync::HostFire)
// DELIBERATELY BEFORE any client connects (unlike eventfire_test's 55 s client settle). The e2e
// orchestration launches a client AFTER the host's "runEvent('starRain'...) dispatched" line: the host
// cue poll starts at transport connect and broadcasts the live starRain PSC (dropped for the
// still-loading slot by the world-ready send gate), and the world-ready join re-send
// (event_cue_sync::QueueConnectBroadcastForSlot) must deliver exactly ONE copy. Expect:
//   host log:   "event_cue: connect-snapshot -- re-sent live 'starRain' (cue 0) to slot 1"
//   client log: exactly one "event_cue: replayed 'starRain' (cue 0)"
// The Meteor Shower emitter lives ~2 min -- the client's 30-60 s load window fits inside it.
//
// Gated by env VOTVCOOP_RUN_CUEFORCE_TEST=1 (autonomous mp.py only; not an ini flag).

#include "harness/autotest.h"

#include "coop/world/event_fire_sync.h"
#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {

void RunAutonomousCueForceTest() {
    const std::string roleEnv = coop::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("cueforce_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    // Short settle: only the HOST world needs to be up (the point is firing pre-client).
    UE_LOGI("cueforce_test: starting on host (waiting 20 s for the world, then firing pre-client)");
    ::Sleep(20000);

    // HostFire's return only reports the client-role refusal -- the fire itself (ResolvePass +
    // NativeFire) runs in its posted game-thread task and logs its OWN outcome. So no retry loop
    // here (it would never retry), and the marker below says POSTED, not fired: the orchestration
    // must gate the client launch on the authoritative host log line
    //   "event_fire: runEvent('starRain', special='None') dispatched"
    // (a world-not-up failure is a loud event_fire WARN instead; audit 2026-07-05).
    namespace efs = coop::event_fire_sync;
    if (!efs::HostFire(efs::FireKind::RunEvent, L"starRain", L"None")) {
        UE_LOGW("cueforce_test: HostFire refused (client role?) -- giving up");
        return;
    }
    UE_LOGI("cueforce_test: posted 'starRain' PRE-CLIENT -- gate the client launch on the "
            "\"runEvent('starRain', special='None') dispatched\" host line; then expect "
            "host 'connect-snapshot -- re-sent live' + exactly one client 'replayed'");
}

DWORD WINAPI CueForceTestThread(LPVOID /*arg*/) {
    RunAutonomousCueForceTest();
    return 0;
}

}  // namespace harness::autotest
