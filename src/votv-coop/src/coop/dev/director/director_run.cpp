// coop/dev/director/director_run.cpp -- the bot-director scenario entry: WALKED GRAB.
// Resolves the possessed player, picks an OPEN chipPile (straightest NavMesh route in a
// distance band), sets the goal, then runs the brain (ControlManager + the walked-grab
// process set) to grab it. Env-gated (VOTVCOOP_RUN_DIRECTOR_WALKGRAB=1), role=Host / solo.
// Greppable "director: VERDICT".

#include "coop/dev/director/director.h"

#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace coop::director {
namespace {
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

template <class Fn>
int RunGT(Fn&& body) {   // bounded: a stalled game thread returns 0 instead of hanging forever
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    int waited = 0;
    while (done->load() == 0) {
        ::Sleep(5);
        waited += 5;
        if (waited >= 4000) return 0;
    }
    return done->load();
}

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

constexpr float kMinPileCm     = 250.f;
constexpr float kMaxPileCm     = 5000.f;
constexpr int   kMaxCandidates = 10;
}  // namespace

void RunWalkGrabDirector() {
    UE_LOGI("director: walked-grab scenario -- +20 s settle for the world to load");
    ::Sleep(20000);

    // Resolve a possessed local player.
    struct Rsv { void* player = nullptr; };
    auto rsv = std::make_shared<Rsv>();
    {
        int waited = 0;
        while (waited < 60) {
            const int r = RunGT([rsv](std::atomic<int>& d) {
                void* p = coop::players::Registry::Get().Local();
                if (p && R::IsLive(p) && E::GetController(p)) { rsv->player = p; d.store(1); }
                else d.store(2);
            });
            if (r == 1) break;
            ::Sleep(1000); ++waited;
        }
        if (!rsv->player) { UE_LOGW("director: no possessed local player -- aborting"); return; }
    }

    // Pick an OPEN chipPile: straightest NavMesh route within a distance band.
    DirectorGoal goal;
    if (RunGT([rsv, &goal](std::atomic<int>& d) {
            PT::ReSeedKnownKeyedProps(nullptr);   // eid-bind like chippile
            const ue_wrap::FVector at = E::GetActorLocation(rsv->player);
            struct Cand { void* pile; ue_wrap::FVector pos; float dist; };
            std::vector<Cand> cands;
            const int32_t n = R::NumObjects();
            for (int32_t i = 0; i < n; ++i) {
                void* o = R::ObjectAt(i);
                if (!o || !R::IsLive(o) || !ue_wrap::prop::IsChipPile(o)) continue;
                if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
                const ue_wrap::FVector p = E::GetActorLocation(o);
                const float dist = HorizDist(p, at);
                if (dist < kMinPileCm || dist > kMaxPileCm) continue;
                cands.push_back({ o, p, dist });
            }
            if (cands.empty()) { UE_LOGW("director: no chipPile in the %.0f-%.0fcm band -- aborting", kMinPileCm, kMaxPileCm); d.store(2); return; }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
            if (cands.size() > kMaxCandidates) cands.resize(kMaxCandidates);
            // Score by TOTAL ROUTE LENGTH (not point count): a pile in genuinely open space has a route ~=
            // its straight distance, while a pile behind a wall winds far around (measured 2026-07-23: a
            // 553cm-straight pile had a 24m NavMesh detour that straight-steer could not traverse). Prefer
            // the shortest actual walk, and REJECT a pile whose route detours > 3x its straight distance.
            float bestLen = 1e30f; int bestPts = -1;
            for (const Cand& c : cands) {
                std::vector<ue_wrap::FVector> p;
                if (!E::FindNavPath(rsv->player, at, c.pos, p)) continue;   // unreachable
                // GRAB-REACHABILITY: the route must END within grab distance of the pile. A pile up on a
                // desk/shelf sits far off the navmesh -- the bot reaches the route-end but never the pile
                // (measured 2026-07-23: a pile 833cm out whose route ended 500cm short -> ungrabbable).
                if (HorizDist(p.back(), c.pos) > 160.f) continue;
                float len = 0.f;
                for (size_t i = 1; i < p.size(); ++i) len += HorizDist(p[i - 1], p[i]);
                // Among grab-reachable piles, prefer the SHORTEST actual walk (doors are fine -- Goto opens
                // them + re-paths through).
                if (len < bestLen) { bestLen = len; bestPts = static_cast<int>(p.size());
                                     goal.targetActor = c.pile; goal.targetPos = c.pos; }
            }
            if (!goal.targetActor) {   // no grab-reachable pile -- honest: this save has none in the open
                UE_LOGW("director: NO grab-reachable chipPile (all sit off the navmesh / behind locked doors) "
                        "-- aborting; needs a pile in open reachable space (spawn one / cleaner save)");
                d.store(2); return;
            }
            UE_LOGI("director: goal pile=%p pos=(%.0f,%.0f,%.0f) straight=%.0fcm routeLen=%.0fcm routePts=%d",
                    goal.targetActor, goal.targetPos.X, goal.targetPos.Y, goal.targetPos.Z,
                    HorizDist(goal.targetPos, at), bestLen >= 1e30f ? -1.f : bestLen, bestPts);
            d.store(1);
        }) != 1) { return; }

    // Build the brain + run it.
    ControlManager mgr;
    AddWalkGrabProcesses(mgr, goal);
    const bool ok = mgr.Run(goal, /*maxSeconds=*/60);   // never-give-up: grind the boxes for the full window
    UE_LOGI("director: VERDICT walkgrab grabbed=%d (%s) reason=%s | drop-input-seam-faithful=%d",
            goal.grabbed ? 1 : 0, ok ? "DONE" : "FAILED", goal.grabbed ? "grabbed" : goal.failReason,
            DidClearHandUseEffectFallback() ? 0 : 1);
}

DWORD WINAPI WalkGrabDirectorThread(LPVOID /*arg*/) {
    RunWalkGrabDirector();
    return 0;
}

}  // namespace coop::director
