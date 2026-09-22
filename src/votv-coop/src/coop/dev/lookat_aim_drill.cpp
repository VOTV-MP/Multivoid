// coop/dev/lookat_aim_drill.cpp -- see coop/dev/lookat_aim_drill.h.

#include "coop/dev/lookat_aim_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <iterator>
#include <memory>
#include <string>

namespace coop::dev::lookat_aim_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// A prop this close is already in front of the player: turning the camera is the whole approach,
// and walking a metre would only push it away. Anything farther is walked to.
constexpr float kAimOnlyCm = 260.f;
constexpr float kReachCm   = 150.f;   // inside the interaction verb's reach, not at the edge of it
constexpr int   kWalkSecs  = 25;   // a route that has not arrived by now ends in the camera sweep

// The settle each role waits out once its own readiness line is true. It is not a wait FOR
// readiness -- that is the gate above it -- but the world's own last few frames of spawning, after
// which the nearest prop is the one that will still be nearest a second later.
constexpr int kSettleTicks = 120;

// How long the aim is given to resolve before the drill says the trace never took it. Ticks of the
// pump, not seconds: the camera is set on the first of them and the game re-traces on each.
constexpr int kAimTicks = 180;   // nine aim points, twenty ticks each

// A prop the trace will not take is not always a bad world -- it can be one the player is carrying,
// or one behind a wall the walk stopped short of. The drill moves on to the next candidate rather
// than ending the run with no reading, and stops after this many so it cannot walk the base forever.
constexpr size_t kMaxTries = 4;

// The reported case, and the one a food answers differently: Aprop_food_C::lookAt returns true
// whatever the prop is doing and puts its own `uses` in the byte the trace compares, where a plain
// Aprop_C returns a constant zero and a false that keeps the rebuild path from running at all. So
// a world with food in it is aimed at food, and anything else is the fallback.
constexpr const wchar_t* kPreferredLineage = L"prop_food_C";

// The finder answers with the nearest of the WHOLE world, so a save whose only food is a mushroom
// in the forest offers one 684 m away. Past this it is not a candidate: the walk would eat the run
// and arrive somewhere the other peer is not.
constexpr float kMaxTargetCm = 2000.f;

// The fallback, and the one that cannot fail on a world with anything in it: instead of choosing an
// actor and hoping the game's two-stage trace agrees, turn the camera through a fan of headings and
// take whatever the trace itself resolves. Every pose is held for a few ticks, since the trace runs
// on the player's own tick and one frame at a heading is not an answer.
constexpr float kSweepYawStepDeg   = 15.f;
constexpr float kSweepPitchDeg[]   = {-45.f, -25.f, -8.f, 5.f};   // props sit on floors and tables
constexpr int   kSweepYawSteps     = 24;   // the whole circle: the room is not all in front of us
constexpr int   kSweepTicksPerPose = 6;

// Where to point WITHIN the chosen prop. An actor's origin is not its visible middle -- a cup's is
// at its base, inside the counter it stands on -- and the game's second trace is a 10 cm sphere at
// the first one's hit point, so an aim a hand's width off answers with the furniture behind. The
// aim walks these offsets rather than betting the run on the origin.
constexpr ue_wrap::FVector kAimOffsets[] = {
    {0, 0, 0}, {0, 0, 10}, {0, 0, -10}, {0, 0, 20},
    {10, 0, 5}, {-10, 0, 5}, {0, 10, 5}, {0, -10, 5}, {0, 0, 30},
};
constexpr int kAimTicksPerOffset = 20;

enum class Phase { Wait, Pick, Walk, Aim, Sweep, Hold, Done };

Phase       g_phase = Phase::Wait;
int         g_settle = 0;
int         g_aimTicks = 0;
void*       g_target = nullptr;     // identity only; re-validated through the object array before use
std::wstring g_targetCls, g_targetKey;
void*       g_tried[kMaxTries] = {};   // candidates the trace refused, so the next scan walks past them
size_t      g_triedN = 0;
int         g_sweepPose = 0;           // which heading of the fan is being held
int         g_sweepTick = 0;
float       g_sweepBaseYaw = 0.f;
// The director's Run blocks, so the walk lives on a worker thread; the game thread reads this
// one flag to know the player is its own again.
std::atomic<bool> g_walkDone{false};

ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

// Every engine touch in here is posted back to the game thread by the director itself.
DWORD WINAPI WalkThread(LPVOID /*arg*/) {
    namespace D = coop::director;
    auto goal = std::make_shared<D::DirectorGoal>();
    goal->targetActor = g_target;
    goal->reachCm     = kReachCm;
    auto ready = std::make_shared<std::atomic<int>>(0);
    ue_wrap::game_thread::Post([goal, ready] {
        if (goal->targetActor && R::IsLive(goal->targetActor)) {
            goal->targetPos = E::GetActorLocation(goal->targetActor);
            ready->store(1);
        } else {
            ready->store(-1);
        }
    });
    for (int waited = 0; ready->load() == 0 && waited < 4000; waited += 5) ::Sleep(5);
    if (ready->load() == 1) {
        D::ControlManager mgr;
        D::AddWalkToProcesses(mgr, *goal);
        mgr.Run(*goal, kWalkSecs);
        UE_LOGI("[LOOKAT-AIM-DRILL] walk finished: %hs",
                goal->reached ? "reached the prop" : goal->failReason);
    } else {
        UE_LOGW("[LOOKAT-AIM-DRILL] the target was gone before the walk started");
    }
    g_walkDone.store(true);
    return 0;
}

// True once this peer's own readiness line has been said, so the drill acts on the same milestone
// the rig waits on rather than on a number of seconds.
bool RoleIsReady(coop::net::Session& s) {
    if (coop::roster::LocalIsHost())
        return s.connectedPeerCount() > 0;   // a client is in: both peers measure the same window
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The nearest prop worth aiming at, and how far off it is. One object-array walk per attempt.
bool PickTarget(void* player, float& distCm) {
    const ue_wrap::FVector me = E::GetActorLocation(player);
    const ue_wrap::prop::NearbyExclusion skip{g_tried, g_triedN};
    ue_wrap::prop::ScanStats stats{};
    ue_wrap::prop::NearestResult found =
        ue_wrap::prop::FindNearest(me, /*wantHeavy=*/false, &stats, kPreferredLineage, skip);
    if (!found.prop)
        found = ue_wrap::prop::FindNearest(me, /*wantHeavy=*/false, &stats, nullptr, skip);
    if (found.prop && found.dist > kMaxTargetCm) {
        UE_LOGI("[LOOKAT-AIM-DRILL] the nearest candidate '%ls' is %.0f cm away -- too far to be "
                "this peer's target; sweeping the camera instead", found.className.c_str(), found.dist);
        return false;
    }
    if (!found.prop) {
        UE_LOGW("[LOOKAT-AIM-DRILL] no prop left to aim at (%d objects walked, %d candidates, %zu "
                "already refused by the trace)", stats.totalScanned, stats.candidates, g_triedN);
        return false;
    }
    g_target    = found.prop;
    g_targetCls = found.className;
    g_targetKey = found.keyString;
    distCm      = found.dist;
    UE_LOGI("[LOOKAT-AIM-DRILL] target '%ls' key='%ls' at %.0f cm (static=%d frozen=%d) -- %hs",
            g_targetCls.c_str(), g_targetKey.c_str(), found.dist, found.isStatic ? 1 : 0,
            found.isFrozen ? 1 : 0, found.dist > kAimOnlyCm ? "walking to it" : "already in reach");
    return true;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::lookat_aim_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session) return;
    if (g_phase == Phase::Done) return;

    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !E::GetController(player)) return;

    switch (g_phase) {
        case Phase::Wait: {
            if (!session->connected() || !RoleIsReady(*session)) { g_settle = 0; return; }
            if (++g_settle < kSettleTicks) return;
            // The sweep first, and the walk only if it finds nothing. It costs no walking, it
            // cannot choose a prop the trace will refuse, and it leaves the peer standing where
            // its own save put it -- where the props it was reported about are.
            g_sweepBaseYaw = E::GetCameraRotation().Yaw;
            g_phase = Phase::Sweep;
            return;
        }
        case Phase::Pick: {
            float distCm = 0.f;
            if (!PickTarget(player, distCm)) { g_phase = Phase::Done; return; }
            if (distCm > kAimOnlyCm) {
                g_walkDone.store(false);
                if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(t);
                    g_phase = Phase::Walk;
                    return;
                }
                // No worker: aim from where we stand, which still reads the churn, only farther off.
                UE_LOGW("[LOOKAT-AIM-DRILL] no thread for the walk -- aiming from here");
            }
            g_phase = Phase::Aim;
            return;
        }
        case Phase::Walk: {
            if (!g_walkDone.load()) return;   // the worker still owns the player
            g_phase = Phase::Aim;
            return;
        }
        case Phase::Aim: {
            if (!g_target || !R::IsLive(g_target)) {
                UE_LOGW("[LOOKAT-AIM-DRILL] the target is gone -- nothing to hold an aim on");
                g_phase = Phase::Done;
                return;
            }
            E::MainPlayerLookAt la{};
            if (E::ReadMainPlayerLookAt(player, la) && la.actor == g_target) {
                // The game's OWN trace resolved it. The camera is not touched again: from here on
                // every change the probe counts is the peer's, not the drill's.
                UE_LOGI("[LOOKAT-AIM-DRILL] READY aimed '%ls' key='%ls' -- the trace resolved it "
                        "after %d tick(s); holding", g_targetCls.c_str(), g_targetKey.c_str(),
                        g_aimTicks);
                g_phase = Phase::Hold;
                return;
            }
            // Turn the camera onto the prop, moving the aim point through the offsets while the
            // trace has not taken it -- a prop that settled after the walk is not where the first
            // rotation pointed either.
            const size_t off = static_cast<size_t>(g_aimTicks / kAimTicksPerOffset) %
                               std::size(kAimOffsets);
            ue_wrap::FVector at = E::GetActorLocation(g_target);
            at.X += kAimOffsets[off].X;
            at.Y += kAimOffsets[off].Y;
            at.Z += kAimOffsets[off].Z;
            E::SetControlRotation(E::GetController(player), LookAt(E::GetCameraLocation(), at));
            if (++g_aimTicks < kAimTicks) return;
            UE_LOGW("[LOOKAT-AIM-DRILL] the trace never resolved '%ls' in %d ticks -- something is "
                    "between the camera and it, or the player is carrying it",
                    g_targetCls.c_str(), kAimTicks);
            if (g_triedN < kMaxTries) {
                g_tried[g_triedN++] = g_target;
                g_target = nullptr;
                g_aimTicks = 0;
                g_phase = Phase::Pick;   // the next candidate, with this one walked past
                return;
            }
            g_phase = Phase::Done;
            return;
        }
        case Phase::Sweep: {
            // Whatever the trace resolves IS resolvable, which is the one property the chosen-actor
            // path cannot guarantee. A prop is what the report is about; a door or a machine would
            // answer the trace too, and taking one would measure the wrong thing.
            E::MainPlayerLookAt la{};
            if (E::ReadMainPlayerLookAt(player, la) && la.actor &&
                ue_wrap::prop::IsDescendantOfProp(la.actor)) {
                g_target    = la.actor;
                g_targetCls = R::ClassNameOf(la.actor);
                g_targetKey = ue_wrap::prop::GetInteractableKeyString(la.actor);
                UE_LOGI("[LOOKAT-AIM-DRILL] READY aimed '%ls' key='%ls' -- found by sweeping the "
                        "camera (pose %d); holding", g_targetCls.c_str(), g_targetKey.c_str(),
                        g_sweepPose);
                g_phase = Phase::Hold;
                return;
            }
            const int poses = kSweepYawSteps * static_cast<int>(std::size(kSweepPitchDeg));
            if (g_sweepPose >= poses) {
                UE_LOGI("[LOOKAT-AIM-DRILL] the sweep found no prop the trace would take in %d "
                        "headings -- walking to one instead", poses);
                g_phase = Phase::Pick;
                return;
            }
            if (g_sweepTick == 0) {
                const int yawIdx = g_sweepPose % kSweepYawSteps;
                const int pitIdx = g_sweepPose / kSweepYawSteps;
                ue_wrap::FRotator r{};
                r.Yaw   = g_sweepBaseYaw + (static_cast<float>(yawIdx) -
                                            (kSweepYawSteps - 1) * 0.5f) * kSweepYawStepDeg;
                r.Pitch = kSweepPitchDeg[pitIdx];
                r.Roll  = 0.f;
                E::SetControlRotation(E::GetController(player), r);
            }
            if (++g_sweepTick >= kSweepTicksPerPose) { g_sweepTick = 0; ++g_sweepPose; }
            return;
        }
        case Phase::Hold:
        case Phase::Done:
            return;
    }
}

void OnDisconnect() {
    g_phase = Phase::Wait;
    g_settle = 0;
    g_aimTicks = 0;
    g_target = nullptr;
    g_targetCls.clear();
    g_targetKey.clear();
    g_triedN = 0;
    g_sweepPose = g_sweepTick = 0;
    g_walkDone.store(false);
}

}  // namespace coop::dev::lookat_aim_drill
