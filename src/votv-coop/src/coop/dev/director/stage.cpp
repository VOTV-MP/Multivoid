// coop/dev/director/stage.cpp -- where a walk starts; see stage.h.

#include "coop/dev/director/stage.h"

#include "coop/config/config.h"
#include "coop/dev/director/routes.h"
#include "coop/player/players_registry.h"
#include "coop/player/shelter_spot.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace coop::director {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace SS = coop::shelter_spot;
namespace PN = ue_wrap::profile::name;

constexpr float kStageMinRouteCm = 10000.f;   // a walk this short is walked
constexpr float kStageSavingCm   = 5000.f;    // an anchor must beat the walker's route by more than this
// How near the pawn must read to the anchor for the teleport to count as landed, and how long it is polled:
// the game's teleport call may land a tick later.
constexpr float kLandedCm  = 150.f;
constexpr int   kLandTries = 50;
constexpr DWORD kLandPollMs = 100;

// VOTVCOOP_DRILL_PLACE=0: every walk starts where the walker stands, for a run that measures the walk itself.
bool PlacingOff() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DRILL_PLACE") == "0");
    return s;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

bool Unset(const ue_wrap::FVector& v) { return v.X == 0.f && v.Y == 0.f && v.Z == 0.f; }

void* LivePawn() {
    void* const me = coop::players::Registry::Get().Local();
    return me && R::IsLive(me) ? me : nullptr;
}

// One game-thread hop. A faulted hop ends the stage with the goal failed, so the walk is never started
// from a place the stage could not read.
template <class F>
bool Hop(DirectorGoal& goal, const char* what, F&& body) {
    const int r = GT::RunAndWait([&body](std::atomic<int>& d) { body(); d.store(1); });
    if (r != GT::kTaskFaulted) return true;
    UE_LOGW("director: the stage's %s task faulted -- the walk is not started", what);
    if (!goal.failed) { goal.failed = true; goal.failReason = "gt_task_faulted"; }
    return false;
}

struct Anchor {
    const char*      name;
    ue_wrap::FVector at;
    bool             routed = false;
    float            route  = 0.f;
};

}  // namespace

bool StageWalk(DirectorGoal& goal, const char* why) {
    if (goal.staged) return true;
    goal.staged = true;
    if (PlacingOff() || goal.walkFromHere) {
        UE_LOGI("director: walking from here for %s: %s", why,
                goal.walkFromHere ? "this walk is the measurement" : "VOTVCOOP_DRILL_PLACE=0");
        return true;
    }

    // Hop 1: where the walker stands, where the goal is, and the walker's route to it.
    bool pawnRead = false, goalRead = false, there = false, pawnRouted = false;
    ue_wrap::FVector from{}, to{};
    float pawnRoute = 0.f;
    if (!Hop(goal, "start", [&] {
            void* const me = LivePawn();
            pawnRead = me && E::TryGetActorLocation(me, from);
            if (!pawnRead) return;
            to = goal.targetPos;
            if (Unset(to) && goal.targetActor)   // an actor-only goal: where the actor stands
                goalRead = R::IsLive(goal.targetActor) && E::TryGetActorLocation(goal.targetActor, to);
            else
                goalRead = !Unset(to);
            if (!goalRead) return;
            there = Flat(from, to) <= goal.reachCm;
            if (there) return;
            pawnRouted = RouteInLegs(me, from, to, goal.reachCm, &pawnRoute);
        }))
        return false;
    if (!pawnRead || !goalRead || there) {
        UE_LOGI("director: walking from here for %s: %s", why,
                !pawnRead ? "the walker's place does not read"
                : !goalRead ? "the goal's place does not read" : "already within reach");
        return true;
    }
    if (pawnRouted && pawnRoute <= kStageMinRouteCm) {
        UE_LOGI("director: walking from here for %s: route %.0f m", why, pawnRoute / 100.f);
        return true;
    }

    // The anchors: the bed spot where the base has one, and the KPP gate.
    std::vector<Anchor> anchors;
    if (!Hop(goal, "bed", [&] {
            void* const me = LivePawn();
            if (!me) return;
            const SS::Target bed = SS::Choose(me);
            if (bed.kind == SS::Kind::Bed) anchors.push_back({"bed", {bed.x, bed.y, bed.z}});
        }))
        return false;
    anchors.push_back({"KPP", {PN::kKPPSpawnX, PN::kKPPSpawnY, PN::kKPPSpawnZ}});
    for (Anchor& a : anchors) {
        if (!Hop(goal, "anchor route", [&] {
                void* const me = LivePawn();
                if (me) a.routed = RouteInLegs(me, a.at, to, goal.reachCm, &a.route);
            }))
            return false;
    }
    const Anchor* best = nullptr;
    for (const Anchor& a : anchors)
        if (a.routed && (!best || a.route < best->route)) best = &a;

    const bool place = best && (!pawnRouted || pawnRoute - best->route > kStageSavingCm);
    if (!place) {
        if (pawnRouted) UE_LOGI("director: walking from here for %s: route %.0f m", why, pawnRoute / 100.f);
        else UE_LOGI("director: walking from here for %s: no route from the walker or from an anchor", why);
        return true;
    }

    bool teleported = false;
    if (!Hop(goal, "teleport", [&] {
            teleported = coop::teleport_client::ApplyLocally({best->at.X, best->at.Y, best->at.Z, 0.f, 0.f, 0.f});
        }))
        return false;
    float off = -1.f;
    for (int i = 0; teleported && i < kLandTries; ++i) {
        if (!Hop(goal, "landing", [&] {
                void* const me = LivePawn();
                ue_wrap::FVector now{};
                off = me && E::TryGetActorLocation(me, now) ? Flat(now, best->at) : -1.f;
            }))
            return false;
        if (off >= 0.f && off <= kLandedCm) break;
        ::Sleep(kLandPollMs);
    }
    if (!teleported || off < 0.f || off > kLandedCm) {
        UE_LOGW("director: the stage's teleport to the %s anchor for %s %s", best->name, why,
                teleported ? "did not land: the pawn does not read at it" : "reported failure");
        goal.failed = true;
        goal.failReason = "the stage's teleport did not land";
        return false;
    }
    if (pawnRouted)
        UE_LOGI("director: PLACED at the %s anchor for %s: route %.0f m instead of %.0f m", best->name, why,
                best->route / 100.f, pawnRoute / 100.f);
    else
        UE_LOGI("director: PLACED at the %s anchor for %s: route %.0f m instead of no route", best->name, why,
                best->route / 100.f);
    return true;
}

}  // namespace coop::director
