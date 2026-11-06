// harness/autotest/meet.cpp -- see meet.h.

#include "harness/autotest/meet.h"

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/shelter_spot.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <windows.h>

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
// How near the pawn must read to an anchor for the teleport to count as landed, and how long it is polled.
constexpr float kLandedCm = 150.f;
constexpr int kLandTries = 50;
constexpr DWORD kLandPollMs = 100;

float Between(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// VOTVCOOP_MEET_PLACE=1: every drill that meets the host goes through the known-good anchor first.
bool PlaceInsteadOfWalking() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_MEET_PLACE") == "1");
    return s;
}

// A spot already proven safe from the out-of-bounds check, as the actor location to set there.
struct Anchor {
    const char* name;
    ue_wrap::FVector at;
};

// The director's walk to the host's puppet; the client check is the callers'.
bool WalkWithDirector(const char* logTag, float reachCm) {
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

}  // namespace

bool PlaceBesideHost(const char* logTag, float reachCm) {
    if (!IsClientRole()) return true;
    // 1: an anchor was chosen and the teleport dispatched, 2: the joiner is already nearer the host than
    // any anchor, 3: no puppet or a place unread, 4: the teleport call reported failure.
    int state = 0;
    Anchor chosen{};
    float anchorToHost = 0.f;
    GT::RunAndWait([&](std::atomic<int>& done) {
        void* const me = coop::players::Registry::Get().Local();
        coop::RemotePlayer* const host = coop::players::Registry::Get().Puppet(0);
        void* const hostActor = host && host->valid() ? host->GetActor() : nullptr;
        ue_wrap::FVector mine{}, theirs{};
        if (me && hostActor && ue_wrap::reflection::IsLive(me) && ue_wrap::reflection::IsLive(hostActor) &&
            E::TryGetActorLocation(me, mine) && E::TryGetActorLocation(hostActor, theirs)) {
            namespace SS = coop::shelter_spot;
            namespace PN = ue_wrap::profile::name;
            // The table: each spot is one the game's out-of-bounds check has already accepted.
            //   "bed": shelter_spot::Choose, the NavMesh floor beside the base bed (net_pump places a
            //          joiner there); it answers the KPP itself when no bed is found, which the KPP row
            //          below already holds.
            //   "KPP": the gate where a new game starts the player (PN::kKPPSpawn*).
            std::vector<Anchor> anchors;
            const SS::Target bed = SS::Choose(me);
            if (bed.kind == SS::Kind::Bed) anchors.push_back({"bed", {bed.x, bed.y, bed.z}});
            anchors.push_back({"KPP", {PN::kKPPSpawnX, PN::kKPPSpawnY, PN::kKPPSpawnZ}});
            size_t best = 0;
            for (size_t i = 1; i < anchors.size(); ++i)
                if (Between(anchors[i].at, theirs) < Between(anchors[best].at, theirs)) best = i;
            chosen = anchors[best];
            anchorToHost = Between(chosen.at, theirs);
            if (Between(mine, theirs) <= anchorToHost) {
                state = 2;
            } else {
                state = coop::teleport_client::ApplyLocally(
                            {chosen.at.X, chosen.at.Y, chosen.at.Z, 0.f, 0.f, 0.f}) ? 1 : 4;
            }
        } else {
            state = 3;
        }
        done.store(1);
    });
    if (state == 3) {
        UE_LOGW("%hs: VERDICT ABORT -- the host's puppet or a position is not there to place beside", logTag);
        return false;
    }
    if (state == 4) {
        UE_LOGW("%hs: VERDICT ABORT -- the teleport to the %hs anchor reported failure", logTag, chosen.name);
        return false;
    }
    if (state == 2) {
        UE_LOGI("%hs: already nearer the host than any anchor, walking the rest", logTag);
        return WalkWithDirector(logTag, reachCm);
    }
    // The game's own teleport call may land a tick later: poll the pawn's location, bounded.
    float off = -1.f;
    for (int i = 0; i < kLandTries; ++i) {
        GT::RunAndWait([&](std::atomic<int>& done) {
            void* const me = coop::players::Registry::Get().Local();
            ue_wrap::FVector now{};
            off = me && ue_wrap::reflection::IsLive(me) && E::TryGetActorLocation(me, now)
                      ? Between(now, chosen.at) : -1.f;
            done.store(1);
        });
        if (off >= 0.f && off <= kLandedCm) break;
        ::Sleep(kLandPollMs);
    }
    if (off < 0.f || off > kLandedCm) {
        UE_LOGW("%hs: VERDICT ABORT -- teleported to the %hs anchor but the pawn reads %.0f cm from it", logTag,
                chosen.name, off);
        return false;
    }
    UE_LOGI("%hs: placed at the %hs anchor (%.0f cm from the host), walking the rest", logTag, chosen.name,
            anchorToHost);
    return WalkWithDirector(logTag, reachCm);
}

bool WalkToHost(const char* logTag, float reachCm) {
    if (!IsClientRole()) return true;
    if (PlaceInsteadOfWalking()) {
        UE_LOGI("%hs: VOTVCOOP_MEET_PLACE=1 -- placing at the anchor nearest the host first", logTag);
        return PlaceBesideHost(logTag, reachCm);
    }
    return WalkWithDirector(logTag, reachCm);
}

}  // namespace harness::autotest::meet
