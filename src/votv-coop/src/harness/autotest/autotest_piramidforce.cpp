// harness/autotest_piramidforce.cpp -- piramid mirror-lane e2e smoke driver.
//
// HOST-ONLY (the client observes via wire; assert its side by log diff). Exercises the whole
// v97 lane on the REAL native chain (docs/events/piramid.md section 5):
//   1. FORCE  -- event_force::ForceNow("piramid"): HostFire arm + the native TB_event_piramid
//               overlap dispatch -> piramidSpawner_2.runTrigger runs in game bytecode: 4x
//               killerwisp_C (npc lane) + piramid2_C at scale 2 (world_actor lane catches the
//               BeginDeferred -> WorldActorSpawn broadcast; client materializes the mirror).
//   2. ARM    -- poll piramid_sync::DebugHooksArmed(): the lane resolved piramid2_C's brain
//               members + registered the interceptors/observer (host side; the client's own
//               "piramid-brain: armed" line is the client-side assert).
//   3. GATHER -- the native wisps spawn 40-69 km out (a real gather would take hours), so the
//               probe re-pins every live killerwisp onto a 150 m ring around the pyramid every
//               5 s (host TeleportTo; the npc pose stream carries it to the client). The host
//               pyramid's OWN seeWisps/checkIfReached then acquire + arrive + gather natively.
//   4. VERDICT -- DebugHostRelayCount() >= 1 within the window = the gather COMMIT was edge-
//               detected and relayed. Client log must then show "piramid-gather[client]:
//               replay OK". Greppable: "piramidforce_test: VERDICT PASS|FAIL".
//
// Gated by env VOTVCOOP_RUN_PIRAMIDFORCE_TEST=1 (autonomous mp.py only; not an ini flag).
// Probe/diagnostic code -- RULE 2 exempt ([[feedback-rule2-exempts-probes-diagnostics-tools]]).

#include "harness/autotest.h"

#include "coop/creatures/piramid_sync.h"
#include "coop/dev/event_force.h"
#include "coop/dev/set_clock.h"
#include "coop/element/element.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/npc.h"
#include "coop/element/world_actor.h"
#include "harness/config.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace EF = coop::dev::event_force;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

// Game-thread task: teleport every live host killerwisp onto a ring around the live host
// pyramid (radius outside the native 10000 arrive threshold so the pyramid WALKS to it --
// the walk exercises the pose stream; 4 azimuths guarantee at least one wisp sits in the
// scan's front hemisphere).
void RepinWispsAroundPyramid() {
    void* pyramid = nullptr;
    {
        std::vector<coop::element::WorldActor*> snap;
        coop::element::WaMirrors().Snapshot(snap);
        for (auto* el : snap) {
            if (!el || el->GetTypeName() != "piramid2_C") continue;
            void* a = el->GetActor();
            if (a && R::IsLiveByIndex(a, el->GetInternalIdx())) { pyramid = a; break; }
        }
    }
    if (!pyramid) {
        UE_LOGW("piramidforce_test: repin -- no live host pyramid element (yet?)");
        return;
    }
    const auto pl = E::GetActorLocation(pyramid);
    std::vector<coop::element::Npc*> wisps;
    coop::element::NpcMirrors().Snapshot(wisps);
    constexpr float kRingRadius = 15000.0f;  // 150 m: > arrive(10000), a ~2 s march
    int pinned = 0;
    for (auto* el : wisps) {
        if (!el || el->GetTypeName() != "killerwisp_C") continue;
        void* w = el->GetActor();
        if (!w || !R::IsLiveByIndex(w, el->GetInternalIdx())) continue;
        const float az = static_cast<float>(pinned % 4) * 1.5707963f;  // 0/90/180/270 deg
        ue_wrap::FVector loc{pl.X + kRingRadius * std::cos(az),
                             pl.Y + kRingRadius * std::sin(az), pl.Z};
        E::TeleportTo(w, loc, ue_wrap::FRotator{0.f, 0.f, 0.f});
        ++pinned;
    }
    UE_LOGI("piramidforce_test: repin -- %d killerwisp(s) ringed at %.0f around pyramid (%.0f,%.0f,%.0f)",
            pinned, kRingRadius, pl.X, pl.Y, pl.Z);
}

}  // namespace

void RunAutonomousPiramidForceTest() {
    const std::string roleEnv = harness::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("piramidforce_test: not host -- host-only (client asserts via its own log lines)");
        return;
    }
    UE_LOGI("piramidforce_test: starting on host (waiting 55 s for world + client transport)");
    ::Sleep(55000);

    // PRE: wait for the box snapshot to resolve (the eventforce shape).
    EF::BoxStatus pre;
    for (int i = 0; i < 24; ++i) {
        EF::RequestRefresh();
        ::Sleep(2500);
        pre = EF::StatusFor("piramid");
        if (pre.resolved) break;
    }
    UE_LOGI("piramidforce_test: PRE %s resolved=%d armed=%d shots=%d",
            pre.boxName, pre.resolved ? 1 : 0, pre.armed ? 1 : 0, pre.shots);
    if (!pre.resolved) {
        UE_LOGW("piramidforce_test: VERDICT FAIL -- TB_event_piramid never resolved");
        return;
    }
    // Night sun BEFORE the wisps spawn: a daylight sun despawns wisps within ~2 min (the
    // wisplane test uses midday for exactly that kill) -- the bait must outlive the pyramid's
    // walk-in. Visual-only fraction; the named clock is untouched.
    coop::dev::set_clock::SetTimeFraction(0.95f);
    UE_LOGI("piramidforce_test: sun forced to night (frac 0.95) so the bait wisps survive");
    if (!EF::ForceNow("piramid")) {
        UE_LOGW("piramidforce_test: VERDICT FAIL -- ForceNow refused (dev gate? row missing?)");
        return;
    }

    // ARM: the native chain spawns piramid2_C; the lane must arm within ~a second of it.
    bool armed = false;
    for (int i = 0; i < 30 && !armed; ++i) {
        ::Sleep(1000);
        armed = coop::piramid_sync::DebugHooksArmed();
    }
    if (!armed) {
        UE_LOGW("piramidforce_test: VERDICT FAIL -- lane never armed (pyramid spawn missing? "
                "grep 'world-actor' + 'piramid-brain' above)");
        return;
    }
    UE_LOGI("piramidforce_test: lane armed -- starting wisp re-pin loop (gather bait)");

    // GATHER: re-pin the wisps every 5 s until the host relays a gather (or 180 s -- the
    // pyramid spawns ~2 km outside its +-700 m hunt box (isInside gate) and must march in
    // at ~30 m/s before scanWisps can acquire anything).
    int relays = 0;
    for (int i = 0; i < 36; ++i) {
        if ((relays = coop::piramid_sync::DebugHostRelayCount()) >= 1) break;
        ue_wrap::game_thread::Post([] { RepinWispsAroundPyramid(); });
        ::Sleep(5000);
    }
    relays = coop::piramid_sync::DebugHostRelayCount();

    if (relays >= 1)
        UE_LOGI("piramidforce_test: VERDICT PASS -- %d gather relay(s); expect client "
                "'piramid-gather[client]: replay OK' + 'piramid-brain[client]: mirror ... tick "
                "restored' + a mirrored piramid2_C world-actor", relays);
    else
        UE_LOGW("piramidforce_test: VERDICT FAIL -- no gather relay in the window (did the "
                "pyramid acquire? grep 'piramid-gather[host]' + 'repin' lines above)");
}

DWORD WINAPI PiramidForceTestThread(LPVOID /*arg*/) {
    RunAutonomousPiramidForceTest();
    return 0;
}

}  // namespace harness::autotest
