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
#include <cstdio>
#include <string>
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
// Not a config row (no census confirms it), so the process logs its effective value once, at the first stage.
bool PlacingOff() {
    static const bool s = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_DRILL_PLACE");
        const bool off = (v == "0");
        UE_LOGI("director: the walk stage is %s (VOTVCOOP_DRILL_PLACE=%s)", off ? "OFF" : "ON",
                v.empty() ? "unset" : v.c_str());
        return off;
    }();
    return s;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

bool Unset(const ue_wrap::FVector& v) { return v.X == 0.f && v.Y == 0.f && v.Z == 0.f; }

void* LivePawn() {
    void* const me = coop::players::Registry::Get().Local();
    return me && R::IsLive(me) ? me : nullptr;
}

// The walk this stage belongs to has ended: its session's walks were ended (EndWalks moved the epoch) or its
// background walk was cancelled. The stage then fails the goal and never moves the pawn, which by now belongs
// to the next session or to a drill that gave this walk up. Same two tests as ControlManager::Drive.
bool Stale(DirectorGoal& goal, uint32_t epoch) {
    const char* reason = nullptr;
    if (WalkEpoch() != epoch) reason = "walks_ended";
    else if (goal.cancel && goal.cancel->load(std::memory_order_acquire)) reason = "cancelled";
    if (!reason) return false;
    UE_LOGW("director: the stage stops, the walk is no longer wanted (%s) -- the pawn is not moved", reason);
    if (!goal.failed) { goal.failed = true; goal.failReason = reason; }
    return true;
}

// One game-thread hop. A faulted hop ends the stage with the goal failed, so the walk is never started
// from a place the stage could not read. A stale walk (Stale above) hops nowhere.
template <class F>
bool Hop(DirectorGoal& goal, uint32_t epoch, const char* what, F&& body) {
    if (Stale(goal, epoch)) return false;
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
    // A straight walk is not judged by a NavMesh route (the baked mesh may not know the opening it walks
    // through), so there is no route to shorten and the stage has nothing to compare.
    if (PlacingOff() || goal.walkFromHere || goal.straight) {
        UE_LOGI("director: walking from here for %s: %s", why,
                PlacingOff()        ? "VOTVCOOP_DRILL_PLACE=0"
                : goal.walkFromHere ? "this walk opts out (a carry, or a walk that is the measurement)"
                                    : "a straight walk has no route to shorten");
        return true;
    }
    const uint32_t epoch = goal.epoch.value_or(WalkEpoch());

    // Hop 1: where the walker stands, where the goal is, and the walker's route to it.
    bool pawnRead = false, goalRead = false, there = false, pawnRouted = false;
    ue_wrap::FVector from{}, to{};
    float pawnRoute = 0.f;
    if (!Hop(goal, epoch, "start", [&] {
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
    // An actor-only goal carries no position of its own: the log line says where the actor was read.
    char whyBuf[160];
    if (goalRead && Unset(goal.targetPos)) {
        std::snprintf(whyBuf, sizeof whyBuf, "%s (actor at %.0f,%.0f,%.0f)", why, to.X, to.Y, to.Z);
        why = whyBuf;
    }
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
    if (!Hop(goal, epoch, "bed", [&] {
            void* const me = LivePawn();
            if (!me) return;
            const SS::Target bed = SS::Choose(me);
            if (bed.kind == SS::Kind::Bed) anchors.push_back({"bed", {bed.x, bed.y, bed.z}});
        }))
        return false;
    anchors.push_back({"KPP", {PN::kKPPSpawnX, PN::kKPPSpawnY, PN::kKPPSpawnZ}});
    for (Anchor& a : anchors) {
        // An anchor already within reach of the goal is the goal itself: a route of nothing, which RouteInLegs does not
        // answer (it asks the NavMesh for a way, and there is none to take). Without this a walk TO the KPP gate never
        // placed at the KPP anchor and walked the whole map.
        if (Flat(a.at, to) <= goal.reachCm) {
            a.routed = true;
            a.route  = 0.f;
            continue;
        }
        if (!Hop(goal, epoch, "anchor route", [&] {
                void* const me = LivePawn();
                if (me) a.routed = RouteInLegs(me, a.at, to, goal.reachCm, &a.route);
            }))
            return false;
    }
    const Anchor* best = nullptr;
    for (const Anchor& a : anchors)
        if (a.routed && (!best || a.route < best->route)) best = &a;

    // A walker with no route (or one past the leg cap) is judged by its straight distance, a floor of its
    // true way: an anchor must still be both worth the hop and nearer than that line.
    const float walkerCm = pawnRouted ? pawnRoute : Flat(from, to);
    const bool place = best && walkerCm > kStageMinRouteCm && walkerCm - best->route > kStageSavingCm;
    if (!place) {
        if (pawnRouted) UE_LOGI("director: walking from here for %s: route %.0f m", why, pawnRoute / 100.f);
        else
            UE_LOGI("director: walking from here for %s: no route from the walker, %.0f m away, and %s", why,
                    walkerCm / 100.f,
                    !best                            ? "no anchor has a route"
                    : walkerCm <= kStageMinRouteCm   ? "the line is short"
                                                     : "no anchor's route is shorter by enough");
        return true;
    }

    bool teleported = false;
    if (!Hop(goal, epoch, "teleport", [&] {
            // The decision was taken on another thread: the walk may have ended since the hop was posted.
            if (Stale(goal, epoch)) return;
            teleported = coop::teleport_client::ApplyLocally({best->at.X, best->at.Y, best->at.Z, 0.f, 0.f, 0.f});
        }))
        return false;
    if (goal.failed) return false;
    float off = -1.f;
    for (int i = 0; teleported && i < kLandTries; ++i) {
        if (!Hop(goal, epoch, "landing", [&] {
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
        UE_LOGI("director: PLACED at the %s anchor for %s: route %.0f m instead of a straight %.0f m with no route",
                best->name, why, best->route / 100.f, walkerCm / 100.f);
    return true;
}

}  // namespace coop::director
