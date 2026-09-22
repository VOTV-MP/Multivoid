// coop/dev/lookat_aim_drill.cpp -- see coop/dev/lookat_aim_drill.h.

#include "coop/dev/lookat_aim_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <iterator>
#include <string>

namespace coop::dev::lookat_aim_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// The whole drill: turn the camera through a fan of headings and take whatever the game's own trace
// resolves. Choosing an actor first and hoping the trace agrees was tried and removed -- in five
// pair runs it never once produced a held aim, while the sweep produced one on both peers, and each
// of its parts (an offset fan around the actor's origin, a refusal list, a walk to the chosen prop)
// was a workaround for the same question the sweep answers directly: which actor will the trace
// accept? Every pose is held for a few ticks, since the trace runs on the player's own tick and one
// frame at a heading is not an answer.
constexpr float kSweepYawStepDeg   = 15.f;
constexpr float kSweepPitchDeg[]   = {-45.f, -25.f, -8.f, 5.f};   // props sit on floors and tables
constexpr int   kSweepYawSteps     = 24;   // the whole circle: the room is not all in front of us
constexpr int   kSweepTicksPerPose = 6;

enum class Phase { Wait, Sweep, Hold, Done };

Phase        g_phase = Phase::Wait;
int          g_sweepPose = 0;
int          g_sweepTick = 0;
float        g_sweepBaseYaw = 0.f;
void*        g_target = nullptr;     // identity only, for the log; never dereferenced after this tick
std::wstring g_targetCls, g_targetKey;

// True once this peer's own readiness line has been said, so the drill acts on the milestone the rig
// waits on rather than on a number of ticks. For a client that line means the host's snapshot is
// applied and the world is the one it will measure; there is nothing left to settle for.
bool RoleIsReady(coop::net::Session& s) {
    if (coop::roster::LocalIsHost())
        return s.connectedPeerCount() > 0;   // a client is in: both peers measure the same window
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::lookat_aim_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session) return;
    // Hold is terminal for this module: the aim is where it belongs and the probe does the reading,
    // so the drill must not go on paying a reflected getter per tick to decide that again.
    if (g_phase == Phase::Hold || g_phase == Phase::Done) return;

    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player)) return;
    void* controller = E::GetController(player);
    if (!controller) return;

    if (g_phase == Phase::Wait) {
        if (!session->connected() || !RoleIsReady(*session)) return;
        g_sweepBaseYaw = E::GetCameraRotation().Yaw;
        g_phase = Phase::Sweep;
        return;
    }

    // Whatever the trace resolves IS resolvable, which is the one property a chosen actor cannot be
    // given. A prop is what the report is about; a door or a machine answers the trace too, and
    // taking one would measure the wrong thing.
    E::MainPlayerLookAt la{};
    if (E::ReadMainPlayerLookAt(player, la) && la.actor && R::IsLive(la.actor) &&
        ue_wrap::prop::IsDescendantOfProp(la.actor)) {
        g_target    = la.actor;
        g_targetCls = R::ClassNameOf(la.actor);
        g_targetKey = ue_wrap::prop::GetInteractableKeyString(la.actor);
        // The camera is not touched again: from here on every change the probe counts is the peer's,
        // not the drill's.
        UE_LOGI("[LOOKAT-AIM-DRILL] READY aimed '%ls' key='%ls' -- the trace took it at heading %d; "
                "holding", g_targetCls.c_str(), g_targetKey.c_str(), g_sweepPose);
        g_phase = Phase::Hold;
        return;
    }

    const int poses = kSweepYawSteps * static_cast<int>(std::size(kSweepPitchDeg));
    if (g_sweepPose >= poses) {
        UE_LOGW("[LOOKAT-AIM-DRILL] no prop the trace would take in %d headings from where this peer "
                "stands -- it has no reading, and the probe's verdict will say so", poses);
        g_phase = Phase::Done;
        return;
    }
    if (g_sweepTick == 0) {
        const int yawIdx = g_sweepPose % kSweepYawSteps;
        const int pitIdx = g_sweepPose / kSweepYawSteps;
        ue_wrap::FRotator r{};
        r.Yaw   = g_sweepBaseYaw + static_cast<float>(yawIdx) * kSweepYawStepDeg;
        r.Pitch = kSweepPitchDeg[pitIdx];
        r.Roll  = 0.f;
        E::SetControlRotation(controller, r);
    }
    if (++g_sweepTick >= kSweepTicksPerPose) { g_sweepTick = 0; ++g_sweepPose; }
}

void OnDisconnect() {
    g_phase = Phase::Wait;
    g_sweepPose = g_sweepTick = 0;
    g_target = nullptr;
    g_targetCls.clear();
    g_targetKey.clear();
}

}  // namespace coop::dev::lookat_aim_drill
