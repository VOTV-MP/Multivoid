// harness/autotest_eventforce.cpp -- event force-NOW smoke driver (coop/dev/event_force).
//
// HOST-ONLY (client observes via wire). Verifies the 2026-07-03 volume-gate feature end to end
// on the canonical row (obelisk -- the user's own repro event):
//   1. PRE  -- badge snapshot path: RequestRefresh + StatusFor until the box resolves; a fresh
//              save must read armed=0 shots=1 ([volume-gated] badge state).
//   2. FORCE -- ForceNow("obelisk"): HostFire arm (v95 EventFire broadcast -> the client log
//              must show its REPLAY line; obelisk is replay-allowlisted) + the posted overlap
//              dispatch with the local pawn (event_force logs "'TB_event_obelisk' FORCED").
//   3. POST -- snapshot again: shots must have dropped to 0 ([FIRED] badge state), proving the
//              native class filter + N decrement + collision-off ran in game bytecode.
// Greppable verdict: "eventforce_test: VERDICT PASS|FAIL".
//
// Gated by env VOTVCOOP_RUN_EVENTFORCE_TEST=1 (autonomous mp.py only; not an ini flag).

#include "harness/autotest.h"

#include "coop/dev/event_force.h"
#include "harness/config.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace EF = coop::dev::event_force;

// One refresh + settle + read. RequestRefresh is ~1 Hz-limited internally; the
// 2.5 s gap keeps every call effective.
EF::BoxStatus SnapshotOnce(const char* eventName) {
    EF::RequestRefresh();
    ::Sleep(2500);
    return EF::StatusFor(eventName);
}

}  // namespace

void RunAutonomousEventForceTest() {
    const std::string roleEnv = harness::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("eventforce_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    // Same settle reasoning as eventfire_test: host bind + client transport connect, so the
    // arm broadcast has a live peer to reach.
    UE_LOGI("eventforce_test: starting on host (waiting 55 s for world + client transport)");
    ::Sleep(55000);

    // PRE: wait for the box snapshot to resolve (world actors up), <= 60 s.
    EF::BoxStatus pre;
    for (int i = 0; i < 24; ++i) {
        pre = SnapshotOnce("obelisk");
        if (pre.resolved) break;
    }
    UE_LOGI("eventforce_test: PRE %s resolved=%d armed=%d shots=%d",
            pre.boxName, pre.resolved ? 1 : 0, pre.armed ? 1 : 0, pre.shots);
    if (!pre.resolved) {
        UE_LOGW("eventforce_test: VERDICT FAIL -- box never resolved (world up? name drift?)");
        return;
    }
    if (pre.armed || pre.shots != 1)
        UE_LOGW("eventforce_test: unexpected PRE state on a fresh save (armed=%d shots=%d) -- "
                "continuing; the force path is still exercised", pre.armed ? 1 : 0, pre.shots);

    if (!EF::ForceNow("obelisk")) {
        UE_LOGW("eventforce_test: VERDICT FAIL -- ForceNow refused (dev gate? row missing?)");
        return;
    }
    ::Sleep(4000);  // arm + force GT tasks + the native chain

    const EF::BoxStatus post = SnapshotOnce("obelisk");
    UE_LOGI("eventforce_test: POST %s resolved=%d armed=%d shots=%d",
            post.boxName, post.resolved ? 1 : 0, post.armed ? 1 : 0, post.shots);

    const bool pass = post.resolved && pre.shots >= 1 && post.shots == 0;
    if (pass)
        UE_LOGI("eventforce_test: VERDICT PASS -- shots %d -> 0 via the native overlap dispatch; "
                "expect host 'FORCED' line + client 'REPLAY' line for the obelisk arm", pre.shots);
    else
        UE_LOGW("eventforce_test: VERDICT FAIL -- post shots=%d (expected 0); read the "
                "event_force lines above for which step broke", post.shots);
}

DWORD WINAPI EventForceTestThread(LPVOID /*arg*/) {
    RunAutonomousEventForceTest();
    return 0;
}

}  // namespace harness::autotest
