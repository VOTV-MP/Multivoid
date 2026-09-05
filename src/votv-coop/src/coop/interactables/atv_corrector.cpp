// coop/interactables/atv_corrector.cpp -- see coop/interactables/atv_corrector.h. The corrector
// for an ATV this peer does not author: a velocity bias between packets, a cut when the error
// is too far to converge, and nothing at all while the author is parked.

#include "coop/interactables/atv_corrector.h"

#include "coop/config/config.h"
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

// The warp distance is speed-scaled, after MTA's CClientVehicle, but sized off the measured
// rig: the ATV is about 2 m long and its native suspension travel is a few centimetres, so 2 m
// plus half a second of travel is too far apart to close smoothly.
constexpr float kWarpBaseCm     = 200.f;
constexpr float kWarpPerSpeedS  = 0.5f;
constexpr float kWarpAngleDeg   = 45.f;   // orientation alone can justify a warp: a body can spin in place
constexpr float kCorrDeadbandCm = 5.f;    // inside this, the wire velocity alone is the correction
constexpr float kCorrMaxCmS     = 400.f;  // bound the corrective term: it must nudge, never launch
// The corrective velocity closes kCorrGain of the remaining error by the time the next packet
// is due, over the measured interval since the last one. A fixed window cannot: this lane has
// two cadences (50 ms authored, 200 ms idle), and an error over a fixed 0.1 s held for 200 ms
// travels twice the error, overshoots and oscillates at constant amplitude, which looks
// exactly like the jitter this exists to remove. A gain below 1 over the real interval
// converges geometrically at any cadence.
constexpr float kCorrGain     = 0.5f;
// The corrector must converge or cut. A velocity nudge corrects a moving body and is powerless
// against a resting one: a mirror that came to rest 40 cm below the host's, both still, sees
// a 20 cm/s upward nudge erased by gravity in 20 ms, and the error stands. So the corrector
// watches itself: if the distance stops shrinking while still outside the deadband, the nudge
// is not working and it stops pretending. Cadence-independent by construction, counting
// packets in which the error failed to shrink, with no threshold on velocity, the quantity
// that was lying about whether convergence was possible.
constexpr float kStallShrinkFrac  = 0.95f;  // "shrank" = at least 5% closer than last packet
constexpr int   kStallWarpPackets = 5;      // 250 ms on the drive lane, 1 s on the idle one
// At rest, do not touch it at all. On a parked ATV the author reports zero velocity and one
// steady height while the mirror falls, free fall over about a second, comes to rest a few
// tens of centimetres low and stays; cut back to the author's pose, it falls again within
// half a second, every time. The only thing this lane does to a mirror between packets is
// write its root's physics velocity, and writing a velocity wakes a settled body. This is not
// the velocity test the stall arm rejects: that one asks whether a nudge can close a gap,
// where the mirror's own velocity says nothing; this asks whether the author is moving, and
// if it is not there is no velocity to mirror and the write is the only thing left that can
// cause the fall. MTA's unoccupied-vehicle sync carries a sync-velocity bit set only when the
// velocity is non-negligible, and the receiver writes the speed only under it; the divergence
// is that they spend a wire bit and we test the received value, since our payload is
// fixed-layout and a value cannot be lied about independently of its pose. Their threshold is
// not ported (its units are established nowhere in the vendored tree); ours comes from our
// logs: a parked author reports 0, a coasting one a few cm/s, a driven one hundreds.
constexpr float kRestLinCmS  = 5.f;
constexpr float kRestAngDegS = 5.f;
// How many times a resting mirror is re-placed before admitting the pose lane cannot hold it:
// three distinguishes settled from cannot-settle-here, and past that the difference is under
// the vehicle, where saying so beats teleporting forever.
constexpr int   kRestMaxReplaces = 3;
// Within this long. A rest episode is a stretch of packets in which the author does not move,
// and counting consecutive ones cannot bound it: a re-place puts the rig exactly on the
// author's pose, so the next packet is the one most likely in band, which resets the count.
// Bounded per episode the diagnostic is reached; bounded by consecutive packets, an error
// re-crossing the deadband every other packet would teleport forever without it.
constexpr uint64_t kRestEpisodeMs = 10000;

constexpr uint64_t kCorrMinDtMs = 20;    // a burst must not manufacture a huge corrective velocity
constexpr uint64_t kCorrMaxDtMs = 1000;  // a long gap must not manufacture a vanishing one

// Ceilings for a velocity that arrives over the wire, well above anything the vehicle reaches
// (its own speed_turbo is 3200), so a real throw or fall lands intact.
constexpr float kMaxWireLinCmS  = 20000.f;
constexpr float kMaxWireAngDegS = 3600.f;

FVector ClampVelocity(const FVector& v, float maxMag) {
    const float m = Len(v);
    if (!(m > maxMag)) return v;   // inverted, so the property holds against a NaN
    const float k = maxMag / m;
    return FVector{ v.X * k, v.Y * k, v.Z * k };
}

uint64_t g_warps = 0;        // diagnostic counters: a corrector nobody can see is one nobody can falsify
uint64_t g_corrs = 0;
uint64_t g_stallWarps = 0;   // ...and specifically: how often the nudge had to give up
uint64_t g_restPlaces = 0;   // ...and how often a PARKED author's pose had to be re-placed

// The value the lane acts on, logged where it is written: the received wire velocity at the
// instant the corrector acts, since a mirror sinking the moment it stops authoring cannot be
// explained from a log that samples each peer's own root velocity at another time.
// Rate-limited to about 1 Hz for the routine case; every cut logs unconditionally, since the
// cut is the instant in question.
uint64_t g_lastSampleLogMs = 0;
constexpr uint64_t kSampleLogMs = 1000;

// The one write rule, at every site that writes a velocity onto a mirror: assigning a linear
// velocity to one body of a settled constraint rig wakes it and it sinks, so the rule is about
// the linear component alone and applied here rather than at one branch. A gate on a whole
// early return let the warp arm keep writing a linear velocity onto a resting mirror, and was
// defeated by a parked-but-rocking author whose angular velocity exceeded the band and routed
// the packet onto the full write path. Two quantities, two gates, one place.
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

// The corrector, called only on packet arrival for an ATV this peer does not author; there is
// no per-frame mirror work. The body simulates between packets; its velocity is biased so it
// converges, and it is cut to the authority's pose when too far gone to converge gracefully.
// MTA's shape with one divergence: their pure-sync read writes the wire velocity hard every
// packet, as this does, and then nudges the transform by a per-frame slice of the position
// error; this biases velocity instead, because their vehicle is one rigid body and the ATV is
// a five-body constraint rig, where a per-frame root nudge stretches the suspension and axle
// bodies by the slice every frame, the very defect this model removes. Steering by velocity
// leaves the solver free to keep the rig rigid.
void ApplyCorrection(AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap) {
    // The control arm, resolved once: a config read per packet on a lane at 20 Hz per vehicle is
    // not free, and the value cannot change mid-session. Off, this function is the only thing that
    // stops: the rig still simulates, still receives vitals, still runs its own tick, so the two
    // arms are a single-variable comparison.
    static const bool sEnabled =
        ::coop::config::ResolveFlag(::coop::config_registry::rows::atv_corrector);
    if (!sEnabled) return;

    FVector cur; FRotator curRot;
    if (!A::GetRootTransform(e.actor, cur, curRot)) return;

    const FVector wirePos{ p.x, p.y, p.z };
    const FRotator wireRot{ p.pitch, p.yaw, p.roll };
    // Finite is not the same as sane: these go straight into PhysX. The dispatch rejects NaN and
    // Inf; this bounds the finite-but-absurd. Client-scoped by the standing rule: a symmetric
    // clamp would be the bug, since the host is authoritative about physics.
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

    // Every test is inverted on purpose, MTA's own idiom: a comparison against NaN is false, so
    // written this way a NaN warps rather than feeding a corrective velocity computed from
    // garbage. The three angles are tested separately: folding them through a max first destroys
    // the property, since a nested-ternary max silently returns the finite operand.
    if (snap || !(dist <= warpD) ||
        !(dPitch <= kWarpAngleDeg) || !(dYaw <= kWarpAngleDeg) || !(dRoll <= kWarpAngleDeg)) {
        // Fail closed: if the rig could not be re-placed (the teleport unresolved after a game
        // update), do not write the authority's velocity onto a body still in the wrong place; that
        // accelerates the error. Logged after the teleport succeeds; an instrument reporting an
        // action it did not take is worse than none.
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        LogWire("WARP", e, dist, wireLin, cur, wirePos);
        WriteMirrorVelocity(e.actor, wireLin, wireAng, linAtRest);
        ++g_warps;
        return;
    }

    // The author is parked: mirror its pose, never its velocity, then leave the rig alone. Falling
    // through would write a zero velocity onto a settled body every packet, the measured cause of
    // the fall.
    if (linAtRest && angAtRest) {
        e.stallPackets = 0;
        e.lastErrCm    = -1.f;
        if (dist <= kCorrDeadbandCm) return;  // in band and nobody moving it: touch nothing. The episode counter is not cleared here, since landing in band is the expected result of a re-place
        if (now - e.lastRestPlaceMs > kRestEpisodeMs) e.restReplaces = 0;  // a new episode
        if (e.restReplaces >= kRestMaxReplaces) return;   // already said our piece, below
        ++e.restReplaces;
        e.lastRestPlaceMs = now;
        if (!A::TeleportRig(e.actor, wirePos, wireRot)) return;
        ++g_restPlaces;
        // No velocity write after the teleport. The two cut paths write one immediately after the
        // teleport, so the rig was never once put down and left to rest, and fell-back-because-the-
        // worlds-differ could not be told from fell-back-because-we-pushed-it. This branch is the
        // experiment as well as the fix.
        if (e.restReplaces >= kRestMaxReplaces) {
            // The line names the class, not a subsystem, and points at the cheapest discriminator
            // first: ride height, the body's Z above the mean of its own rig bodies in the probe
            // line, separates a rig of the wrong shape (ours, always; the last case was our own
            // collision guard cancelling the wheel hit delegates and taking the rig's shape with
            // them) from a rig in the wrong place (possibly the world's). A pose lane can only ever
            // fix the second, and a verdict that points outward when the cause is ours sends the
            // next session to measure the world.
            UE_LOGW("atv: a parked mirror would not stay on the authority's pose after %d "
                    "re-places (last error %.1f cm). CHECK THE RIG'S SHAPE BEFORE THE WORLD: "
                    "compare rideH in [ATVP] on both peers ([dev] atv_probe=1). If they differ, "
                    "the mirror is DEFORMED and no pose correction can fix it -- last time that "
                    "was our own collision guard. Only if the shapes agree is this about the "
                    "world (terrain, or a hook coupling the rig to a prop the author cannot see)",
                    kRestMaxReplaces, dist);
        }
        return;
    }
    if (!linAtRest) e.restReplaces = 0;   // the episode ended because the author moved

    // Is the correction working? Count packets where the error stayed outside the deadband and
    // refused to shrink; past the limit, cut instead of nudging.
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
    // The corrective term is a linear push, so the linear gate governs it too: a parked-but-rocking
    // author reaches here, and pushing its resting mirror is the defect.
    if (dist > kCorrDeadbandCm && !linAtRest) {
        // Close kCorrGain of the error over the observed interval, not a fixed window (see the
        // constant).
        const float gain = kCorrGain * 1000.f / static_cast<float>(dtMs);
        FVector corr{ err.X * gain, err.Y * gain, err.Z * gain };
        const float mag = Len(corr);
        if (mag > kCorrMaxCmS) {
            const float k = kCorrMaxCmS / mag;
            corr.X *= k; corr.Y *= k; corr.Z *= k;
        }
        lin.X += corr.X; lin.Y += corr.Y; lin.Z += corr.Z;
    }
    // Rotation gets no continuous corrective term: mapping a rotator delta onto an angular
    // velocity is only exact for small aligned deltas, and an ATV on the ground takes its
    // orientation from the terrain once its position and velocity agree. Orientation divergence
    // is caught by the angle warp arm instead: one measurable mechanism, not a guessed gain.
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
