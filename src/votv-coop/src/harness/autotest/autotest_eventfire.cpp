// harness/autotest/autotest_eventfire.cpp -- the EventFire replay-channel smoke driver.
//
// HOST-ONLY; the client observes over the wire. After the join settles it fires three events
// through the SAME seam the F1 dev menu uses (coop::event_fire_sync::HostFire, a native dispatch
// plus an EventFire broadcast), covering the receive-side policy three ways:
//   1. 'solar'       RunEvent     -> replay allowlisted (cosmetic boom)
//   2. 'arirGraff_0' SpecialEvent -> replay allowlisted (a decal; the repeatable-special lane)
//   3. 'enasus'      RunEvent     -> NOT replayed; the prop lane owns the outputs, and the
//                                    dropped props themselves mirror over PropSpawn
// The client log must ALSO show the structural lines: the client scheduler reporting itself
// suppressed, and the host log reporting its poll primed against a passEvents baseline. The
// scheduler-fire OBSERVATION path (a settime append followed by poll growth) needs a real
// clock-cross and is NOT exercised here, which the runbook says outright.
//
// Gated by the environment variable VOTVCOOP_RUN_EVENTFIRE_TEST=1, not by an ini flag.

#include "harness/autotest.h"

#include "coop/world/event_fire_sync.h"
#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace efs = coop::event_fire_sync;

// HostFire returns false ONLY on the client-role refusal: the fire itself resolves inside its
// posted game-thread task and logs its own outcome, so this loop never retries a still-loading
// world. It is kept as a role-refusal guard; the PASS evidence is the "dispatched" line in the
// host log, which the smoke's log-diff step reads.
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
    if (IsClientRole()) {
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
