// harness/autotest/meet.cpp -- see meet.h.

#include "harness/autotest/meet.h"

#include "harness/autotest.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/director/stage.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>

namespace harness::autotest::meet {
namespace {

namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace DIR = coop::director;

constexpr float kPaceCmPerSec = 300.f;   // a walk's pace
// What a route costs beyond its length at pace: the director's stuck episodes and the doors it presses on
// the way (a closed base door held it about 18 s on the rig before its press).
constexpr int kRouteMarginSec = 60;

float Between(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool WalkToHost(const char* logTag, float reachCm) {
    if (!IsClientRole()) return true;
    DIR::DirectorGoal goal;
    ue_wrap::FVector theirs{};
    bool read = false;   // false: no puppet yet or a place unread
    GT::RunAndWait([&](std::atomic<int>& done) {
        void* const me = coop::players::Registry::Get().Local();
        coop::RemotePlayer* const host = coop::players::Registry::Get().Puppet(0);
        void* const hostActor = host && host->valid() ? host->GetActor() : nullptr;
        ue_wrap::FVector mine{};
        if (me && hostActor && ue_wrap::reflection::IsLive(me) && ue_wrap::reflection::IsLive(hostActor) &&
            E::TryGetActorLocation(me, mine) && E::TryGetActorLocation(hostActor, theirs)) {
            goal.targetActor = hostActor;
            goal.targetPos = theirs;
            goal.epoch = DIR::WalkEpoch();
            read = true;
        }
        done.store(1);
    });
    if (!read) {
        UE_LOGW("%hs: VERDICT ABORT -- the host's puppet or a position is not there to walk to", logTag);
        return false;
    }
    goal.reachCm = reachCm;
    // The director stages the walk (stage.h): the joiner may start at the known-good anchor nearest the host.
    // The budget is taken from what remains after that, so a placed walk carries no 770 m budget.
    if (!DIR::StageWalk(goal, logTag)) {
        UE_LOGW("%hs: VERDICT ABORT -- could not start the walk to the host (%hs)", logTag, goal.failReason);
        return false;
    }
    float line = 0.f, route = 0.f;
    bool pawnRead = false;   // false: the walker's place does not read
    GT::RunAndWait([&](std::atomic<int>& done) {
        void* const me = coop::players::Registry::Get().Local();
        ue_wrap::FVector mine{};
        if (me && ue_wrap::reflection::IsLive(me) && E::TryGetActorLocation(me, mine)) {
            line = Between(mine, theirs);
            // The walk is bounded by the NavMesh route, not the line: inside the base the line crosses walls
            // and the route goes round through the doors. Asked in legs, since one path query is a partial
            // route on a long walk and under-counts it.
            if (line > reachCm && !DIR::RouteInLegs(me, mine, theirs, reachCm, &route)) route = 0.f;
            pawnRead = true;
        }
        done.store(1);
    });
    if (!pawnRead) {
        UE_LOGW("%hs: VERDICT ABORT -- the pawn's position is not there to walk from", logTag);
        return false;
    }
    if (line <= reachCm) {
        UE_LOGI("%hs: already beside the host", logTag);
        return true;
    }
    // Twice a walk's pace over the route (the line when no route was found), plus the margin.
    const float way = route > line ? route : line;
    const int budget = static_cast<int>(way / kPaceCmPerSec * 2.f) + kRouteMarginSec;
    UE_LOGI("%hs: walking to the host, %.0fcm away, route %.0fcm, %d s budget", logTag, line, route, budget);
    DIR::ControlManager mgr;
    DIR::AddWalkToProcesses(mgr, goal);
    mgr.Run(goal, budget);
    if (!goal.reached) {
        UE_LOGW("%hs: VERDICT ABORT -- could not walk to the host (%hs)", logTag, goal.failReason);
        return false;
    }
    UE_LOGI("%hs: met the host after a %.0fcm walk", logTag, line);
    return true;
}

}  // namespace harness::autotest::meet
