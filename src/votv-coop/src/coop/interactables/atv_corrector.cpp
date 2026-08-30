// coop/interactables/atv_corrector.cpp -- see header. The body below MOVED VERBATIM from
// atv_sync.cpp on 2026-08-30; equivalence was proven by a body-diff instrument against a frozen
// pre-extraction copy, with delete-a-line and change-a-constant mutants shown to FAIL it.

#include "coop/interactables/atv_corrector.h"

#include "coop/net/protocol.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>

namespace coop::atv_corrector {

namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::atv_sync::AtvEntry;
using coop::atv_sync::NowMs;
using coop::atv_sync::Len;

namespace {

// The corrector. Warp is speed-scaled after CClientVehicle.cpp:3901 (their 15 + 10*|v| is in GTA
// units); ours is sized off the measured rig -- the ATV is ~2 m long and its native suspension
// travel is 2-4 cm (docs/vehicles/ATV.md 13.1), so 2 m plus half a second of travel is "so far
// apart that closing it smoothly would look worse than a cut".
constexpr float kWarpBaseCm     = 200.f;
constexpr float kWarpPerSpeedS  = 0.5f;
constexpr float kWarpAngleDeg   = 45.f;   // orientation alone can justify a warp: a body can spin
                                          // in place without ever tripping the distance threshold
constexpr float kCorrDeadbandCm = 5.f;    // inside this, the wire velocity alone is the correction
constexpr float kCorrMaxCmS     = 400.f;  // bound the corrective term: it must nudge, never launch
// The corrective velocity is sized to close kCorrGain of the remaining error by the time the NEXT
// packet is due, using the MEASURED interval since the last one. A fixed window cannot do this:
// this lane has two cadences (50 ms authored, 200 ms idle), and `err / 0.1 s` held for 200 ms
// travels TWICE the error -- it overshoots to -err and then oscillates at constant amplitude
// forever, which looks exactly like the jitter this model exists to remove. A gain below 1 over
// the real interval converges geometrically at any cadence, including a bunched or delayed packet.
constexpr float kCorrGain     = 0.5f;
// THE CORRECTOR MUST CONVERGE OR CUT -- measured 2026-08-29, and this arm is why.
// A velocity nudge is the right correction for a MOVING body and is powerless against a resting
// one: a client that joined mid-settle had its ATV come to rest 40.5 cm BELOW the host's, both
// copies then perfectly still, and a 20 cm/s upward corrective velocity is erased by gravity in
// 20 ms. The error stood for the whole run. So the corrector watches itself: if the distance
// stops shrinking while it is still outside the deadband, the nudge is not working here and we
// stop pretending it will. This is cadence-independent by construction -- it counts PACKETS in
// which the error failed to shrink, not seconds -- and it needs no threshold on velocity, which
// is exactly the quantity that was lying about whether convergence was possible.
constexpr float kStallShrinkFrac  = 0.95f;  // "shrank" = at least 5% closer than last packet
constexpr int   kStallWarpPackets = 5;      // 250 ms on the drive lane, 1 s on the idle one
constexpr uint64_t kCorrMinDtMs = 20;    // a burst must not manufacture a huge corrective velocity
constexpr uint64_t kCorrMaxDtMs = 1000;  // a long gap must not manufacture a vanishing one

// Ceilings for a velocity that arrives over the wire. Sized well above anything the vehicle can
// legitimately reach (its own speed_turbo is 3200) so a real throw or a fall still lands intact.
constexpr float kMaxWireLinCmS  = 20000.f;
constexpr float kMaxWireAngDegS = 3600.f;

FVector ClampVelocity(const FVector& v, float maxMag) {
    const float m = Len(v);
    if (!(m > maxMag)) return v;   // inverted: a NaN cannot reach here (guarded upstream) but the
                                   // idiom keeps the property if that ever changes
    const float k = maxMag / m;
    return FVector{ v.X * k, v.Y * k, v.Z * k };
}

uint64_t g_warps = 0;        // diagnostic counters -- a corrector nobody can see is a corrector
uint64_t g_corrs = 0;        // nobody can falsify (the instrument-blindness lesson)
uint64_t g_stallWarps = 0;   // ...and specifically: how often the nudge had to give up

}  // namespace

// THE CORRECTOR. Called ONLY on packet arrival for an ATV this peer does not author -- there is no
// per-frame mirror work at all any more. The body simulates between packets; we bias its velocity
// so it converges, and cut to the authority's pose when it is too far gone to converge gracefully.
//
// MTA shape with one deliberate divergence (RULE 2026-05-28). CNetAPI::ReadVehiclePuresync writes
// the wire velocity HARD every packet -- we do that. CClientVehicle::UpdateTargetPosition:3896
// then nudges the TRANSFORM by a per-frame slice of the position error; we bias VELOCITY instead,
// because their vehicle is one rigid body and AATV_C is a five-body constraint rig: a per-frame
// root nudge stretches sus_*/ax_* by the slice every frame, which is the same mechanism as the
// defect this whole model exists to remove. Letting the solver keep the rig rigid and steering it
// by velocity is the only correction that leaves the suspension free to do its own job.
void ApplyCorrection(AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap) {
    FVector cur; FRotator curRot;
    if (!A::GetRootTransform(e.actor, cur, curRot)) return;

    const FVector wirePos{ p.x, p.y, p.z };
    const FRotator wireRot{ p.pitch, p.yaw, p.roll };
    // Finite is not the same as sane: these go straight into PhysX. event_dispatch_state rejects
    // NaN/Inf; this bounds the finite-but-absurd. CLIENT-SCOPED by the standing rule -- a symmetric
    // clamp would be the bug, because the host is allowed to be authoritative about physics.
    const FVector wireLin = ClampVelocity({ p.linVelX, p.linVelY, p.linVelZ }, kMaxWireLinCmS);
    const FVector wireAng = ClampVelocity({ p.angVelX, p.angVelY, p.angVelZ }, kMaxWireAngDegS);

    const FVector err{ wirePos.X - cur.X, wirePos.Y - cur.Y, wirePos.Z - cur.Z };
    const float dist  = Len(err);
    const float warpD = kWarpBaseCm + kWarpPerSpeedS * Len(wireLin);
    const float dPitch = std::fabs(ue_wrap::NormalizeAxis(wireRot.Pitch - curRot.Pitch));
    const float dYaw   = std::fabs(ue_wrap::NormalizeAxis(wireRot.Yaw   - curRot.Yaw));
    const float dRoll  = std::fabs(ue_wrap::NormalizeAxis(wireRot.Roll  - curRot.Roll));

    const uint64_t now = NowMs();
    const uint64_t rawDt = e.lastPktMs ? (now - e.lastPktMs) : kCorrMaxDtMs;
    const uint64_t dtMs = rawDt < kCorrMinDtMs ? kCorrMinDtMs
                                               : (rawDt > kCorrMaxDtMs ? kCorrMaxDtMs : rawDt);
    e.lastPktMs = now;

    // Every test is INVERTED on purpose, MTA's own idiom (CClientVehicle.cpp:3905's comment): a
    // comparison against NaN is false, so writing them this way makes a NaN WARP rather than feed
    // a corrective velocity computed from garbage. THE THREE ANGLES ARE TESTED SEPARATELY, and
    // that is not style: folding them through a max() first DESTROYS the property, because
    // `NaN > x` is false, so a nested-ternary max silently returns the finite operand and a
    // NaN pitch sails through a test written to catch it.
    if (snap || !(dist <= warpD) ||
        !(dPitch <= kWarpAngleDeg) || !(dYaw <= kWarpAngleDeg) || !(dRoll <= kWarpAngleDeg)) {
        // FAIL CLOSED: if the rig could not be re-placed (teleportVehicle unresolved after a game
        // update), do NOT then write the authority's velocity onto a body still sitting in the
        // wrong place -- that accelerates the error instead of cutting it.
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, wireLin, wireAng);
        ++g_warps;
        return;
    }

    // Is the correction actually working? Count packets where the error stayed outside the
    // deadband and refused to shrink; past the limit, cut instead of nudging.
    if (dist <= kCorrDeadbandCm) {
        e.stallPackets = 0;
    } else if (e.lastErrCm >= 0.f && dist >= e.lastErrCm * kStallShrinkFrac) {
        ++e.stallPackets;
    } else {
        e.stallPackets = 0;
    }
    e.lastErrCm = dist;
    if (e.stallPackets >= kStallWarpPackets) {
        e.stallPackets = 0;
        e.lastErrCm = -1.f;
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, wireLin, wireAng);
        ++g_stallWarps;
        UE_LOGI("atv: correction stalled at %.1f cm -- cut to the authority's pose "
                "(a nudge cannot move a body at rest)", dist);
        return;
    }

    FVector lin = wireLin;
    if (dist > kCorrDeadbandCm) {
        // Close kCorrGain of the error over the interval we actually observed, NOT over a fixed
        // window -- see the constant's comment for why a fixed one oscillates on the idle cadence.
        const float gain = kCorrGain * 1000.f / static_cast<float>(dtMs);
        FVector corr{ err.X * gain, err.Y * gain, err.Z * gain };
        const float mag = Len(corr);
        if (mag > kCorrMaxCmS) {
            const float k = kCorrMaxCmS / mag;
            corr.X *= k; corr.Y *= k; corr.Z *= k;
        }
        lin.X += corr.X; lin.Y += corr.Y; lin.Z += corr.Z;
    }
    // Rotation gets NO continuous corrective term in this commit: mapping a rotator delta onto an
    // angular-velocity vector is only exact for small aligned deltas, and an ATV on the ground
    // takes its orientation from the terrain it is standing on once its position and velocity
    // agree. Orientation divergence is caught by the kWarpAngleDeg arm above instead -- one
    // mechanism, measurable, rather than a term whose gain we would be guessing.
    ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, lin, wireAng);
    ++g_corrs;
}

Counters ReadCounters() {
    return Counters{ g_corrs, g_warps, g_stallWarps };
}

}  // namespace coop::atv_corrector
