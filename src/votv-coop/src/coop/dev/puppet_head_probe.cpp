// puppet_head_probe -- see header. Observer-side, ini-gated, ~1 Hz per puppet.
#include "coop/dev/puppet_head_probe.h"

#include <chrono>
#include <cmath>
#include <unordered_map>

#include "coop/ini_config.h"
#include "ue_wrap/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"           // PuppetHeadLookProbe + ReadPuppetHeadLookProbe
#include "ue_wrap/reflection.h"       // IsLive
#include "ue_wrap/types.h"            // NormalizeAxis

namespace coop::puppet_head_probe {

namespace {

namespace Pup = ue_wrap::puppet;
namespace R = ue_wrap::reflection;
using SteadyClock = std::chrono::steady_clock;

// Per-actor state (game thread only -> no lock). Entries for recycled puppets linger
// harmlessly (a few bytes); the probe is dev-only and off by default. The baseline is
// the 'head'/'neck' bone world-yaw-minus-bodyYaw measured when the look is ~straight
// ahead (|desired| small) -- it cancels the bone's local-axis convention (~-90deg, the
// rig's 'head' forward axis) so the REPORTED twist is the look-induced deviation,
// directly comparable to the desired offset and the clamp.
struct ActorState {
    SteadyClock::time_point nextLog{};
    float baselineHead = 0.f;
    float baselineNeck = 0.f;
    bool  haveBaseline = false;
};
std::unordered_map<void*, ActorState> g_state;

}  // namespace

void Tick(void* puppetActor, float bodyYawDeg, float desiredLookYawDeg, float desiredPitchDeg) {
    UE_ASSERT_GAME_THREAD("puppet_head_probe::Tick");
    if (!puppetActor || !R::IsLive(puppetActor)) return;

    // Cheap per-tick gate: throttle FIRST (map lookup + compare); only touch the ini at
    // the ~1 Hz boundary so steady-state cost is one map probe per tick.
    const auto now = SteadyClock::now();
    ActorState& st = g_state[puppetActor];
    if (st.nextLog != SteadyClock::time_point{} && now < st.nextLog) return;
    if (!coop::ini_config::IsIniKeyTrue("puppet_head_probe")) {
        st.nextLog = now + std::chrono::seconds(2);  // back off while disabled
        return;
    }
    st.nextLog = now + std::chrono::milliseconds(1000);

    Pup::PuppetHeadLookProbe p{};
    if (!Pup::ReadPuppetHeadLookProbe(puppetActor, p)) {
        UE_LOGW("[HEAD-PROBE] actor=%p: ReadPuppetHeadLookProbe failed (mesh/anim not ready)",
                puppetActor);
        return;
    }

    // DESIRED head twist = where the replicated look-input points, relative to the body.
    const float desiredOffset = ue_wrap::NormalizeAxis(desiredLookYawDeg - bodyYawDeg);
    // RAW bone world yaw minus body yaw -- carries the rig's constant local-axis bias.
    const float rawHead = p.haveHead ? ue_wrap::NormalizeAxis(p.headWorldYaw - bodyYawDeg) : 0.f;
    const float rawNeck = p.haveNeck ? ue_wrap::NormalizeAxis(p.neckWorldYaw - bodyYawDeg) : 0.f;
    // Calibrate the baseline when looking ~straight ahead (cancels the ~-90deg rig axis).
    if (std::fabs(desiredOffset) < 5.f && p.haveHead) {
        st.baselineHead = rawHead;
        st.baselineNeck = rawNeck;
        st.haveBaseline = true;
    }
    // Look-INDUCED twist = how far the head/neck actually swung from straight-ahead.
    const float headTwist = (st.haveBaseline && p.haveHead) ? ue_wrap::NormalizeAxis(rawHead - st.baselineHead) : 0.f;
    const float neckTwist = (st.haveBaseline && p.haveNeck) ? ue_wrap::NormalizeAxis(rawNeck - st.baselineNeck) : 0.f;
    // Reach = head clamp + half-alpha neck clamp ~ 67 deg at the class-default 45/45.
    const float clampReach = p.headClampDeg + 0.5f * p.neckClampDeg;
    const bool exceedsClamp = std::fabs(desiredOffset) > (clampReach + 3.f);
    const bool headShort = st.haveBaseline && (std::fabs(headTwist) + 8.f < std::fabs(desiredOffset));
    const char* verdict =
        !st.haveBaseline            ? "no baseline yet (look straight ahead once to calibrate)"
        : (exceedsClamp && headShort) ? "PINNED -- twist short of desired beyond clamp => CLAMP CONFIRMED"
        : exceedsClamp              ? "desired > clamp but twist not yet short (sweep further)"
                                    : "within clamp (head tracking the input)";

    UE_LOGW("[HEAD-PROBE] actor=%p bodyYaw=%.1f lookYaw=%.1f | DESIRED off=%.1f deg | "
            "TWIST head=%.1f neck=%.1f (raw head=%.1f base=%.1f) | clamp head=%.1f neck=%.1f reach~%.1f | "
            "pitch=%.1f | %s",
            puppetActor, bodyYawDeg, desiredLookYawDeg, desiredOffset,
            headTwist, neckTwist, rawHead, st.baselineHead,
            p.headClampDeg, p.neckClampDeg, clampReach, desiredPitchDeg, verdict);
}

}  // namespace coop::puppet_head_probe
