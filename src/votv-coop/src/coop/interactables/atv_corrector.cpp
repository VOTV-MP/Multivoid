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
// AT REST, DO NOT TOUCH IT AT ALL -- measured 2026-08-30, and this arm is the whole of A6.
//
// WHAT WAS MEASURED. The [ATVC] instrument logs the RECEIVED wire velocity at the instant the
// corrector acts. On a parked ATV the author reports |v| = 0.0 and holds one Z to the decimal,
// while the MIRROR falls: cur.z 6282.5 -> 6272.5 -> 6236.6 over about a second, which is free
// fall. It comes to rest 25-40 cm low and stays. Cutting it back to the author's pose lands
// (5405.2 -> 5430.5 the sample after) and it falls again within 500 ms, every time, in four
// driven runs. The only thing this lane does to a mirror between packets is write its root's
// physics velocity -- and writing velocity WAKES the body, every packet, for a rig that had
// settled.
//
// WHY A VELOCITY TEST HERE IS NOT THE ONE ARC 1 REJECTED. The comment on kStallShrinkFrac says
// the stall arm "needs no threshold on velocity, which is exactly the quantity that was lying
// about whether convergence was possible" -- and that stands: it is about whether a NUDGE can
// close a gap, where the mirror's own velocity says nothing. This asks a different question of
// a different value: is the AUTHOR moving? If it is not, there is no velocity to mirror, the
// corrective term is meaningless by 14.4 (a nudge cannot move a body at rest), and the write is
// the only thing left that can be causing the fall. The worry that the branch might not be
// TAKEN at the handoff -- because the author's own copy is settling and reports a downward
// velocity -- was measured and did not happen: |v| = 0.0 at exactly those packets.
//
// MTA SHAPE (RULE 2026-05-28). CUnoccupiedVehicleSync carries `bSyncVelocity : 1` and sets it
// only when the velocity is non-negligible (Client/.../CUnoccupiedVehicleSync.cpp:311, server
// clears it again at :315-321), and the receiver calls SetMoveSpeed ONLY under that flag (:194).
// MTA never writes velocity onto a resting mirrored vehicle either. DELIBERATE DIVERGENCE: they
// spend a wire bit, we test the received value -- our payload is fixed-layout so the bit costs
// more than the test, and a value cannot be lied about independently of the pose it comes with.
// Their threshold is NOT ported: MTA's 0.1 is in units the vendored tree establishes nowhere,
// and porting a bare constant across unit systems is this project's most recent lesson.
// Ours comes from our own logs: a parked author reports 0.0, a coasting one 27 / 8.2 / 2.6 /
// 1.0 / 0.2 cm/s, a driven one 780-1500.
constexpr float kRestLinCmS  = 5.f;
constexpr float kRestAngDegS = 5.f;
// How many times we will re-place a resting mirror before admitting the pose lane cannot hold
// it. Three is enough to distinguish "it settled" from "it cannot settle here"; past that the
// difference is under the vehicle, not in this lane, and saying so beats teleporting forever.
constexpr int   kRestMaxReplaces = 3;
// ...within THIS long. A rest episode is a stretch of packets in which the author does not move;
// counting consecutive ones cannot bound it, because a re-place puts the rig exactly on the
// author's pose and the next packet is therefore the one most likely to be in band, which resets
// the count. Measured 2026-08-30: "bounded at three re-places" gave up three times in 46 s, and
// in the mirror-image regime -- error re-crossing the deadband every other packet -- it would
// never reach three and the diagnostic would never be emitted while the teleports ran forever.
constexpr uint64_t kRestEpisodeMs = 10000;

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
uint64_t g_restPlaces = 0;   // ...and how often a PARKED author's pose had to be re-placed

// THE VALUE THE LANE ACTS ON, WHICH NOTHING HAS EVER RECORDED. Three driven runs measured a
// mirror sinking 23-40 cm the moment it stopped authoring, and the archive cannot say why,
// because no instrument sampled `p.linVel*` -- the velocity we write onto the mirror -- at the
// instant we write it. The probe samples each peer's OWN root velocity every 500 ms, which is a
// different quantity at a different time. A design that turns on whether the author's reported
// velocity is ~0 at the handoff cannot be decided from a log that never contains it.
// Rate-limited to ~1 Hz for the routine case; every CUT logs unconditionally, because the cut is
// the instant in question.
uint64_t g_lastSampleLogMs = 0;
constexpr uint64_t kSampleLogMs = 1000;

// THE ONE WRITE RULE, at every site that writes a velocity onto a mirror.
//
// The defect this lane shipped on 2026-08-30 was not "the corrector writes too hard", it was that
// assigning a LINEAR velocity to one body of a settled constraint rig wakes it and it sinks. So
// the rule is about the linear component alone, and it is applied HERE rather than at one branch:
// the first version gated a whole early-return on both components, which (a) let the warp arm
// above it keep writing a linear velocity onto a resting mirror, unbounded, and (b) was defeated
// entirely by a parked-but-ROCKING author, whose angular velocity exceeded the band and routed the
// packet onto the full write path -- measured in the very run that shipped it (wireLin |v|=4.63,
// mirror then gained +51 cm/s of Z). Two quantities, two gates, one place.
void WriteMirrorVelocity(void* actor, const FVector& lin, const FVector& ang, bool linAtRest) {
    if (linAtRest) {
        ue_wrap::engine::SetActorRootPhysicsAngularVelocity(actor, ang);
        return;
    }
    ue_wrap::engine::SetActorRootPhysicsVelocity(actor, lin, ang);
}

void LogWire(const char* what, const AtvEntry& e, float dist,
             const FVector& wireLin, const FVector& cur, const FVector& wirePos) {
    UE_LOGI("[ATVC] %s dist=%.1f cur.z=%.1f wire.z=%.1f wireLin=(%.1f,%.1f,%.1f) |v|=%.1f "
            "stall=%d", what, dist, cur.Z, wirePos.Z,
            wireLin.X, wireLin.Y, wireLin.Z, Len(wireLin), e.stallPackets);
}

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

    // The two gates, computed once and read by every write site below.
    const bool linAtRest = Len(wireLin) <= kRestLinCmS;
    const bool angAtRest = Len(wireAng) <= kRestAngDegS;

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
        // LOG AFTER THE TELEPORT SUCCEEDS. Logging before it claimed a warp per packet that
        // never happened whenever teleportVehicle was unresolved -- an instrument reporting an
        // action it did not take is worse than no instrument.
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        LogWire("WARP", e, dist, wireLin, cur, wirePos);
        WriteMirrorVelocity(e.actor, wireLin, wireAng, linAtRest);
        ++g_warps;
        return;
    }

    // THE AUTHOR IS PARKED: mirror its POSE, never its velocity, and then leave the rig alone.
    // Falling through to the code below would write a zero velocity onto a settled body every
    // packet, which is the measured cause of A6 (see kRestLinCmS above).
    if (linAtRest && angAtRest) {
        e.stallPackets = 0;
        e.lastErrCm    = -1.f;
        if (dist <= kCorrDeadbandCm) return;  // in band, nobody moving it: touch NOTHING. The
                                              // episode counter is NOT cleared here -- landing
                                              // in band is the expected RESULT of a re-place,
                                              // so clearing on it is what unbounded the bound.
        if (now - e.lastRestPlaceMs > kRestEpisodeMs) e.restReplaces = 0;  // a new episode
        if (e.restReplaces >= kRestMaxReplaces) return;   // already said our piece, below
        ++e.restReplaces;
        e.lastRestPlaceMs = now;
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        ++g_restPlaces;
        // NO velocity write after the teleport. Both existing cut paths write one immediately
        // after TeleportRig, so in four runs the rig was never once put down and left to rest --
        // which is why "it fell back, so the worlds differ" could never be told apart from "it
        // fell back because we pushed it". This branch is the experiment as well as the fix.
        if (e.restReplaces >= kRestMaxReplaces) {
            // NAMES THE CLASS, NOT A SUBSYSTEM. A verdict that blames the nearest lane sends
            // the next session to rewrite working code, and there is more than one thing that
            // can hold a mirrored ATV off the authority's pose. Terrain is one. A HOOK is
            // another and is entirely invisible to the other peer: a player can tie arbitrary
            // physics props to a vehicle with prop_hook_C, the deployed hook_C carries its own
            // A<->B PhysicsConstraint, and the hook lane has NO implementation in this tree at
            // all (docs/items/hook.md 2 -- every row is a GAP), so one peer's ATV can be coupled
            // to a crate the other peer does not know exists. Under that, this rig is not five
            // bodies but five plus however many somebody tied on.
            UE_LOGW("atv: a parked mirror would not stay on the authority's pose after %d "
                    "re-places (last error %.1f cm) -- something local to THIS peer is holding "
                    "the rig off it (terrain under the vehicle, or a constraint the other peer "
                    "cannot see, e.g. a hook). Not the pose stream", kRestMaxReplaces, dist);
        }
        return;
    }
    if (!linAtRest) e.restReplaces = 0;   // the episode ended because the author moved

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
        LogWire("CUT", e, dist, wireLin, cur, wirePos);
        WriteMirrorVelocity(e.actor, wireLin, wireAng, linAtRest);
        ++g_stallWarps;
        UE_LOGI("atv: correction stalled at %.1f cm -- cut to the authority's pose "
                "(a nudge cannot move a body at rest)", dist);
        return;
    }

    FVector lin = wireLin;
    // The corrective term is a LINEAR push and is therefore governed by the linear gate too: a
    // parked-but-rocking author reaches here, and pushing its resting mirror is the defect.
    if (dist > kCorrDeadbandCm && !linAtRest) {
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
    if (now - g_lastSampleLogMs >= kSampleLogMs) {
        g_lastSampleLogMs = now;
        LogWire(dist > kCorrDeadbandCm ? "NUDGE" : "INBAND", e, dist, wireLin, cur, wirePos);
    }
    WriteMirrorVelocity(e.actor, lin, wireAng, linAtRest);
    ++g_corrs;
}

Counters ReadCounters() {
    return Counters{ g_corrs, g_warps, g_stallWarps, g_restPlaces };
}

}  // namespace coop::atv_corrector
