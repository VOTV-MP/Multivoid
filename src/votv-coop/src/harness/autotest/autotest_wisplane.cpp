// harness/autotest_wisplane.cpp -- wisp mirror-lane e2e smoke driver (2026-07-03).
//
// HOST-ONLY (client observes via wire). Drives the full event-swarm wisp lifecycle:
//   1. T+55s  ForceNow("wisps") -- arm via HostFire + drive TB_event_wispSwarm's own overlap
//             (the events NOW! seam, verified 2026-07-03) -> trigger_wispSwarm_2's ubergraph
//             spawns up to 32x wisp_C via EX_CallMath BeginDeferred, one per 0.25-1.0 s.
//             Each spawn must be CAUGHT by the Func-thunk (host log "npc-sync[ex-spawn]:
//             enrolled 'wisp_C' eid=N") and MIRRORED (client log "materialized mirror ...
//             class='wisp_C'"). Pose streams while they descend/wander; the client's
//             fade-in fires via the lane-driven landing edge (DriveWispLanding).
//   2. T+130s SetTimeFraction(0.5) -- midday sun. A LANDED wisp's next tick fails the
//             night-band check -> dir(false) -> 3s fade -> EX_VirtualFunction self-destroy,
//             which the K2 PRE cannot see: the pose-walk dead-retire must broadcast it
//             (host log "npc-sync[pose dead-retire]: Npc eid=N"; client OnDestroy lines).
//             (If the fresh save starts in daytime the despawn leg simply runs early --
//             the log evidence is the same, just earlier.)
//   3. T+165s DONE marker. The assert is the LOG DIFF: enrolled count == materialized count
//             > 0, dead-retire count == client destroy count, zero errors.
//
// Gated by env VOTVCOOP_RUN_WISPLANE_TEST=1 (autonomous mp.py only; not an ini flag).

#include "harness/autotest.h"

#include "coop/dev/event_force.h"
#include "coop/dev/set_clock.h"
#include "coop/config/config.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {

void RunAutonomousWispLaneTest() {
    const std::string roleEnv = coop::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("wisplane_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    UE_LOGI("wisplane_test: starting on host (waiting 55 s for world + client transport)");
    ::Sleep(55000);

    if (!coop::dev::event_force::ForceNow("wisps")) {
        UE_LOGW("wisplane_test: VERDICT FAIL -- ForceNow('wisps') refused");
        return;
    }
    UE_LOGI("wisplane_test: wisps FORCED -- swarm spawn window open (~32 wisp_C over <=32 s); "
            "expect host [ex-spawn] enrolled lines + client materialized wisp_C mirrors");
    ::Sleep(75000);  // 32 spawns x <=1 s + enroll drains + landing margin

    UE_LOGI("wisplane_test: spawn window over -- forcing MIDDAY sun (despawn leg: landed wisps "
            "fail the night-band check -> fade-out -> PE-invisible self-destroy -> pose-walk "
            "dead-retire must broadcast EntityDestroy)");
    coop::dev::set_clock::SetTimeFraction(0.5f);
    ::Sleep(35000);  // fade (~3 s) + destroy + retire + client teardown margin

    // NOTE: keep this marker free of the grep tokens the assert counts (enrolled / dead-retire /
    // error) -- a self-matching marker inflated the first run's counts by one.
    UE_LOGI("wisplane_test: DONE -- assert via log diff (see autotest_wisplane.cpp header)");
}

DWORD WINAPI WispLaneTestThread(LPVOID /*arg*/) {
    RunAutonomousWispLaneTest();
    return 0;
}

}  // namespace harness::autotest
