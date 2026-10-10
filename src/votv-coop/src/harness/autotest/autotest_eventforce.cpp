// harness/autotest/autotest_eventforce.cpp -- event force-NOW smoke driver (coop/dev/event_force).
//
// HOST-ONLY (client observes via wire). The volume-gate feature end to end on the canonical row
// (obelisk), each phase ended by readiness, never a clock: slot 1 world-ready (WaitPeerWorldReady),
// so the arm broadcast reaches a live peer; PRE, the badge snapshot until the box resolves (a fresh
// save reads armed=0 shots=1, the [volume-gated] badge); FORCE, ForceNow("obelisk") -- the HostFire
// arm (the client logs its NOT-replayed line; obelisk's prop spawns and punch are host-only) and the
// posted overlap dispatch ("'TB_event_obelisk' FORCED"); POST, snapshots until the shots drop to 0
// (the [FIRED] badge: the native class filter, N decrement and collision-off ran in game bytecode).
// Greppable verdict: "eventforce_test: VERDICT PASS|FAIL". Env VOTVCOOP_RUN_EVENTFORCE_TEST=1.

#include "harness/autotest.h"

#include "coop/dev/event_force.h"
#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace EF = coop::dev::event_force;

constexpr DWORD kReadyBudgetMs = 180'000;   // the client boots, joins, downloads and loads first
constexpr DWORD kPhaseBudgetMs = 60'000;    // a snapshot phase: the box resolves, the shots drop
constexpr DWORD kPollMs        = 250;

// Snapshots until `done` holds or the phase's budget runs out. RequestRefresh is about 1 Hz-limited
// internally, so asking each poll costs nothing extra; the refresh lands on the game thread and
// StatusFor reads what it last left.
template <class Done>
EF::BoxStatus SnapshotUntil(const char* eventName, Done done) {
    EF::BoxStatus st{};
    for (DWORD waited = 0; waited < kPhaseBudgetMs; waited += kPollMs) {
        EF::RequestRefresh();
        st = EF::StatusFor(eventName);
        if (done(st)) return st;
        ::Sleep(kPollMs);
    }
    return st;
}

}  // namespace

void RunAutonomousEventForceTest() {
    if (IsClientRole()) {
        UE_LOGI("eventforce_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    UE_LOGI("eventforce_test: starting on host (waiting for slot 1 to be world-ready)");
    if (!WaitPeerWorldReady(1, kReadyBudgetMs)) {
        UE_LOGW("eventforce_test: VERDICT FAIL -- no client was seated and world-ready in slot 1 within %lu s",
                static_cast<unsigned long>(kReadyBudgetMs / 1000));
        return;
    }

    // PRE: the box snapshot resolves (world actors up).
    const EF::BoxStatus pre = SnapshotUntil("obelisk", [](const EF::BoxStatus& s) { return s.resolved; });
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
    // POST: the arm and force game-thread tasks and the native chain have run once the shots drop.
    const EF::BoxStatus post =
        SnapshotUntil("obelisk", [](const EF::BoxStatus& s) { return s.resolved && s.shots == 0; });
    UE_LOGI("eventforce_test: POST %s resolved=%d armed=%d shots=%d",
            post.boxName, post.resolved ? 1 : 0, post.armed ? 1 : 0, post.shots);

    const bool pass = post.resolved && pre.shots >= 1 && post.shots == 0;
    if (pass)
        UE_LOGI("eventforce_test: VERDICT PASS -- shots %d -> 0 via the native overlap dispatch; "
                "expect host 'FORCED' line + client 'NOT replayed' line for the obelisk arm", pre.shots);
    else
        UE_LOGW("eventforce_test: VERDICT FAIL -- post shots=%d (expected 0); read the "
                "event_force lines above for which step broke", post.shots);
}

DWORD WINAPI EventForceTestThread(LPVOID /*arg*/) {
    RunAutonomousEventForceTest();
    return 0;
}

}  // namespace harness::autotest
