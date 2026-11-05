// harness/autotest/meet.cpp -- see meet.h.

#include "harness/autotest/meet.h"

#include "harness/autotest.h"

#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

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
    float line = 0.f, route = 0.f;
    int state = 0;   // 1: both places read, 2: no puppet yet or a place unread
    GT::RunAndWait([&](std::atomic<int>& done) {
        void* const me = coop::players::Registry::Get().Local();
        coop::RemotePlayer* const host = coop::players::Registry::Get().Puppet(0);
        void* const hostActor = host && host->valid() ? host->GetActor() : nullptr;
        ue_wrap::FVector mine{}, theirs{};
        if (me && hostActor && ue_wrap::reflection::IsLive(me) && ue_wrap::reflection::IsLive(hostActor) &&
            E::TryGetActorLocation(me, mine) && E::TryGetActorLocation(hostActor, theirs)) {
            goal.targetActor = hostActor;
            goal.targetPos = theirs;
            goal.epoch = DIR::WalkEpoch();
            line = Between(mine, theirs);
            // The walk is bounded by the NavMesh route, not the line: inside the base the line crosses
            // walls and the route goes round through the doors.
            std::vector<ue_wrap::FVector> pts;
            if (E::FindNavPath(me, mine, theirs, pts))
                for (size_t i = 1; i < pts.size(); ++i) route += Between(pts[i - 1], pts[i]);
            state = 1;
        } else {
            state = 2;
        }
        done.store(1);
    });
    if (state != 1) {
        UE_LOGW("%hs: VERDICT ABORT -- the host's puppet or a position is not there to walk to", logTag);
        return false;
    }
    if (line <= reachCm) {
        UE_LOGI("%hs: already beside the host", logTag);
        return true;
    }
    goal.reachCm = reachCm;
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
