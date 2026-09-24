// harness/autotest/autotest_hostthrow.cpp -- the host-throw scenario (VOTVCOOP_RUN_HOSTTHROW=1):
// the HOST walks to a nav-reachable pile with the bot director, grabs it through the input seam,
// carries it and throws it; the CLIENT samples what its own mirror of that clump does in the air.
// The counterpart of the grab-intent drill's hard throw, where the client throws. Nobody is
// teleported: the director walks (coop/dev/director/director.h says why).

#include "harness/autotest.h"

#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"            // ResolveMirrorEidByActor
#include "coop/props/prop_element_tracker.h"   // the thrown clump's eid, for the verdict
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"      // SetActorRootPhysicsVelocity
#include "ue_wrap/engine/engine_mainplayer.h"  // ReadMainPlayerGrabState, ReleaseMainPlayerGrabIfHolding

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;


// The host half: the director's own walked grab, then a throw of what it holds.
void RunHost() {
    UE_LOGI("hostthrow: HOST -- waiting for the client's puppet to go live, then walking to a pile");
    for (int waited = 0; waited < 180; ++waited) {
        const int r = GT::RunAndWait([](std::atomic<int>& d) {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            d.store(rp && rp->valid() ? 1 : 2);
        });
        if (r == 1) break;
        ::Sleep(1000);
    }
    ::Sleep(35000);   // the client expresses its proxies after its puppet appears

    auto player = std::make_shared<void*>(nullptr);
    auto goal   = std::make_shared<coop::director::DirectorGoal>();
    if (GT::RunAndWait([player, goal](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (!p || !R::IsLive(p) || !E::GetController(p)) { d.store(2); return; }
            *player = p;
            d.store(coop::director::PickReachablePile(p, 0.f, 5000.f, *goal) ? 1 : 2);
        }) != 1) {
        UE_LOGW("hostthrow: VERDICT host=NO-PILE -- no possessed player or no nav-reachable pile");
        return;
    }
    coop::director::ControlManager mgr;
    coop::director::AddWalkGrabProcesses(mgr, *goal);
    if (!mgr.Run(*goal, /*maxSeconds=*/225)) {
        UE_LOGW("hostthrow: VERDICT host=NO-GRAB -- the director did not grab (%s)", goal->failReason);
        return;
    }
    UE_LOGI("hostthrow: grabbed; carrying 3 s so the client's mirror follows the hand");
    ::Sleep(3000);

    GT::RunAndWait([player](std::atomic<int>& d) {
        ue_wrap::engine::MainPlayerGrabState gs{};
        void* clump = nullptr;
        if (E::ReadMainPlayerGrabState(*player, gs) && gs.grabbingActor &&
            ue_wrap::prop::IsGarbageClump(gs.grabbingActor))
            clump = gs.grabbingActor;
        if (!clump) { UE_LOGW("hostthrow: VERDICT host=NO-CLUMP -- nothing in the hand at the throw"); d.store(2); return; }
        const bool rel = E::ReleaseMainPlayerGrabIfHolding(*player, clump);
        // Along the facing, 6 m/s out and 6 m/s up: a second of arc over open floor.
        const ue_wrap::FVector fwd = E::GetActorForwardVector(*player);
        const ue_wrap::FVector lin{ fwd.X * 600.f, fwd.Y * 600.f, 600.f };
        const bool vel = E::SetActorRootPhysicsVelocity(clump, lin, ue_wrap::FVector{0.f, 0.f, 0.f});
        UE_LOGI("hostthrow: THROWN clump=%p eid=%u released=%d velocitySet=%d vel=(%.0f,%.0f,%.0f)",
                clump, static_cast<unsigned>(coop::prop_element_tracker::GetPropElementIdForActor(clump)),
                rel ? 1 : 0, vel ? 1 : 0, lin.X, lin.Y, lin.Z);
        d.store(1);
    });
    ::Sleep(7000);   // the flight, the impact, the re-pile and its settle
}

// The client half: sample EVERY live clump mirror it can name, by eid, at 20 Hz. The driver keeps
// the thrown eid's samples. Following one clump picked by nearness latched onto whatever else lay or
// was carried near the host -- a hand on a rig window is enough.
void RunClient() {
    UE_LOGI("hostthrow: CLIENT watcher -- sampling every named clump mirror at 20 Hz");
    const ULONGLONG t0   = ::GetTickCount64();
    const ULONGLONG tEnd = t0 + 360000;
    // Kept across ticks, so each pointer travels with its object-array slot: a clump dies at its
    // land, and IsLive on a freed pointer can read true.
    struct Seen { void* actor; int32_t idx; };
    auto clumps = std::make_shared<std::vector<Seen>>();
    // A clump whose read failed: its unread sample is logged once, and since a failed read of a live
    // actor faults again it is not read again. Serial-checked, so a new clump in the same slot is read;
    // a dead one leaves the list at the next walk.
    auto unreadable = std::make_shared<std::vector<ue_wrap::CachedObjRef>>();
    int pass = 0;
    while (::GetTickCount64() < tEnd) {
        const bool refresh = (pass++ % 5) == 0;   // the object-array walk at 4 Hz, the samples at 20
        GT::RunAndWait([clumps, unreadable, refresh, t0](std::atomic<int>& d) {
            if (refresh) {
                clumps->clear();
                for (void* o : R::FindObjectsByClass(L"prop_garbageClump_C"))
                    if (o) clumps->push_back(Seen{o, R::InternalIndexOf(o)});
                unreadable->erase(std::remove_if(unreadable->begin(), unreadable->end(),
                                                 [](const ue_wrap::CachedObjRef& u) { return !u.Get(); }),
                                  unreadable->end());
            }
            for (const Seen& c : *clumps) {
                void* o = c.actor;
                if (!R::IsLiveByIndex(o, c.idx) || !ue_wrap::prop::IsGarbageClump(o)) continue;
                bool skip = false;
                for (const ue_wrap::CachedObjRef& u : *unreadable) if (u.Is(o)) { skip = true; break; }
                if (skip) continue;
                const coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(o);
                if (eid == coop::element::kInvalidId || eid == 0) continue;
                ue_wrap::FVector at{};
                if (E::TryGetActorLocation(o, at)) {
                    UE_LOGI("hostthrow: WATCH-SAMPLE t=%llu ms eid=%u mirror=%p pos=(%.1f,%.1f,%.1f)",
                            ::GetTickCount64() - t0, static_cast<unsigned>(eid), o, at.X, at.Y, at.Z);
                } else {   // no numbers: the judge reads a sample's coordinates as a place
                    UE_LOGI("hostthrow: WATCH-SAMPLE t=%llu ms eid=%u mirror=%p pos=(unread)",
                            ::GetTickCount64() - t0, static_cast<unsigned>(eid), o);
                    unreadable->emplace_back();
                    unreadable->back().Set(o);
                }
            }
            d.store(1);
        });
        ::Sleep(50);
    }
}

}  // namespace

void RunHostThrowScenario() {
    if (IsClientRole()) { RunClient(); return; }
    RunHost();
    UE_LOGI("hostthrow: HOST finished");   // every exit of RunHost ends here: the driver waits on this line
}

DWORD WINAPI HostThrowThread(LPVOID /*arg*/) {
    RunHostThrowScenario();
    return 0;
}

}  // namespace harness::autotest
