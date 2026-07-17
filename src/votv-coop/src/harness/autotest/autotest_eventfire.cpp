// harness/autotest_eventfire.cpp -- v95 EventFire replay-channel smoke driver.
//
// HOST-ONLY (client observes via wire). After the join settles, fires three events through the
// SAME seam the F1 dev menu uses (coop::event_fire_sync::HostFire = native dispatch + EventFire
// broadcast), covering the receive-side policy three ways:
//   1. 'solar'       RunEvent      -> REPLAY allowlisted (cosmetic boom; client log: "client REPLAY")
//   2. 'arirGraff_0' SpecialEvent  -> REPLAY allowlisted (decal; the repeatable-special lane)
//   3. 'enasus'      RunEvent      -> NO-replay (prop lane owns the outputs; client log:
//                                     "'enasus' NOT replayed -- prop lane owns the outputs";
//                                     the dropped props themselves mirror via PropSpawn)
// The client log must ALSO show the structural lines this build adds: "client scheduler
// SUPPRESSED (allEvents N -> 0)" and the host log "host poll primed (passEvents baseline=N)".
// The scheduler-fire OBSERVATION path (settime append -> poll growth) needs a real clock-cross
// and is NOT exercised here -- stated honestly in the runbook.
//
// Gated by env VOTVCOOP_RUN_EVENTFIRE_TEST=1 (autonomous mp.py only; not an ini flag).

#include "harness/autotest.h"

#include "coop/world/event_fire_sync.h"
#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace efs = coop::event_fire_sync;

// NOTE (corrected 2026-07-05): HostFire returns false ONLY on the client-role refusal -- the
// fire itself resolves inside its posted game-thread task and logs its own outcome, so this
// loop never actually retries a still-loading world. Kept as a role-refusal guard; the real
// PASS evidence is the "dispatched" line in the host log (the smoke's log-diff step reads it).
bool FireWithRetry(efs::FireKind kind, const wchar_t* name, const char* label) {
    for (int i = 0; i < 120; ++i) {  // <= 60 s
        if (efs::HostFire(kind, name, L"None")) {
            UE_LOGI("eventfire_test: fired %s ('%ls')", label, name);
            return true;
        }
        ::Sleep(500);
    }
    UE_LOGW("eventfire_test: %s ('%ls') never resolved -- giving up", label, name);
    return false;
}

}  // namespace

void RunAutonomousEventFireTest() {
    const std::string roleEnv = coop::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("eventfire_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    // 55 s: host bind (~20 s) + client launch + transport connect (~15 s) + margin. A fire
    // landing while the client is still LOADING is fine (the receive side queues until the
    // eventer resolves -- that path is part of what this smoke proves); a fire before the
    // TRANSPORT connects would be lost, hence the long settle.
    UE_LOGI("eventfire_test: starting on host (waiting 55 s for world + client transport)");
    ::Sleep(55000);

    if (!FireWithRetry(efs::FireKind::RunEvent, L"solar", "RunEvent/replay-allowlisted")) return;
    ::Sleep(4000);
    FireWithRetry(efs::FireKind::SpecialEvent, L"arirGraff_0", "SpecialEvent/replay-allowlisted");
    ::Sleep(4000);
    FireWithRetry(efs::FireKind::RunEvent, L"enasus", "RunEvent/no-replay(prop lane)");
    ::Sleep(4000);

    UE_LOGI("eventfire_test: DONE -- expect client log: 2x 'client REPLAY' + 1x 'NOT replayed' "
            "+ 1x 'scheduler SUPPRESSED'; host log: 'host poll primed' + 3x 'broadcast'");
}

DWORD WINAPI EventFireTestThread(LPVOID /*arg*/) {
    RunAutonomousEventFireTest();
    return 0;
}

}  // namespace harness::autotest
