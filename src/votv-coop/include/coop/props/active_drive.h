// coop/active_drive.h -- the FIXED-DELAY SNAPSHOT INTERPOLATION primitive.
//
// Extracted verbatim from remote_prop.cpp (2026-06-22) at the RULE 2026-05-25 800-LOC soft
// cap, when the host-authoritative trash-clump carry/flight pose stream
// (coop/trash_clump_pose_stream) needed to reuse the SAME proven interp rather than duplicate
// it (RULE 2 -- one implementation of one concept). ZERO behaviour change vs the in-place
// version: this IS the carry-jank fix (render the proxy BEHIND the newest pose by the measured
// inter-pose interval, advancing on an INDEPENDENT render clock -- MTA CClientVehicle::
// UpdateTargetPosition / fAlpha = Unlerp(start, now, finish)). [[feedback-follow-mta-architecture]]
//
// TWO consumers, same primitive:
//   * remote_prop.cpp  -- a std::array<ActiveDrive, kMaxPeers> g_drives, ONE per peer slot (each
//                         client kinematically drives its own held prop independently).
//   * trash_clump_pose_stream.cpp -- a std::unordered_map<eid, ActiveDrive>, ONE per host-driven
//                         trash clump (N simultaneously-carried client-grabbed clumps).
// Both feed BeginLerpToPose (record a new target) + AdvanceLerp (advance one tick, EVERY tick).
// GAME-THREAD only (SetActorLocation/Rotation are UFunction dispatches).

#pragma once

#include "ue_wrap/engine.h"      // SetActorLocation / SetActorRotation
#include "ue_wrap/reflection.h"  // IsLive
#include "ue_wrap/types.h"       // FVector / FRotator / NormalizeAxis

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::active_drive {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// State of one driven actor. `actor` is the LOCAL actor pointer (a mirror / proxy in this
// process). lastKey/lastEid cache the identity the per-slot receiver resolved (so a same-Key
// PropPose skips the GUObjectArray walk); the per-eid carry stream uses lastEid only.
struct ActiveDrive {
    void*        actor = nullptr;
    void*        mesh = nullptr;
    std::string  lastKey;        // ASCII (the Aprop_C save UUID format); empty for a clump
    uint32_t     lastEid = 0;    // Prop Element id identity for the non-keyable clump
    uint64_t     lastApplyMs = 0;
    // Host-authoritative trash proxy: NO stream-stop timeout-release. A network gap mid-carry
    // must FREEZE the proxy at its last pose, never drop it to physics -- the carry ends ONLY on
    // an explicit reliable edge (OnRelease throw / OnConvert ToPile / disconnect). A non-proxy
    // Aprop_C held item keeps the 500 ms timeout (no such reliable end-of-carry guarantee).
    bool         isProxy = false;
    // FIXED-DELAY SNAPSHOT INTERP (scoped to the trash proxy; a non-proxy Aprop_C is EXEMPT and
    // keeps its proven teleport-to-latest snap). Render between the two MOST RECENT timestamped
    // poses a small fixed delay (the measured inter-pose interval) BEHIND the newest, so the
    // render clock (nowMs) advances INDEPENDENTLY of pose arrival. First pose primes (snap), a
    // far jump re-primes (snap), else interpolate prev->last. On a stream STOP the render clock
    // advances past `last`'s timestamp -> alpha clamps to 1 -> the proxy reaches the last pose
    // and FREEZES (no extrapolation; control released at the reliable edge).
    //
    // RETIRED 2026-06-22 (the carry JANK root, code-proven): the prior scheme lerped
    // renderedLoc->latest over the measured interval, resetting lerpStartMs = nowMs on every pose
    // -- and AdvanceLerp sampled the SAME nowMs that tick => alpha = 0 => ZERO movement on every
    // new-pose tick. At vsync-60 (pose rate ~= tick rate) nearly every tick was a new-pose tick.
    bool              lerpSeeded   = false;  // identity primed (first pose snapped)
    bool              haveTwoSnaps = false;  // >=2 buffered poses -> interpolate (else sit at the single snap)
    ue_wrap::FVector  prevLoc{}, lastLoc{}, renderedLoc{};
    ue_wrap::FRotator prevRot{}, lastRot{}, renderedRot{};
    uint64_t          prevPoseMs = 0;  // arrival time (ms) of the OLDER buffered pose
    uint64_t          lastPoseMs = 0;  // arrival time (ms) of the NEWEST buffered pose
};

// Interpolation tuning. The pose interval is measured per-stream and clamped: below kLerpMinMs a
// tick-fast stream effectively snaps; above kLerpMaxMs a slow/gappy stream caps the lerp window
// so a long stall doesn't produce a multi-second crawl. A jump larger than kSnapDist (cm) is a
// teleport / post-gap catch-up -> snap, don't crawl across.
constexpr uint64_t kLerpMinMs   = 16;     // ~one 60 fps frame
constexpr uint64_t kLerpMaxMs   = 200;    // ~5 Hz floor (a gappy stream still converges)
constexpr float    kSnapDist    = 500.f;  // 5 m: beyond this, snap instead of interpolate
constexpr float    kSnapDistSq  = kSnapDist * kSnapDist;
// Per-tick dirty-gate epsilons: at 125 Hz the interp step is often sub-visible -- a held-but-still
// clump, or a sub-cm creep. Skip the engine write below these (the skipped delta accumulates until
// it crosses). Conservative so a slow drift still steps invisibly (sub-pixel at gameplay scale).
constexpr float    kLerpEpsLocSq = 0.25f * 0.25f;  // 0.25 cm
constexpr float    kLerpEpsRotDeg = 0.1f;          // summed |axis delta|, degrees

inline uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Shortest-path angular interpolation (handles the +/-180 wrap so a yaw crossing 179 -> -179
// lerps 2 deg, not 358).
inline float LerpAngle(float a, float b, float t) {
    return a + ue_wrap::NormalizeAxis(b - a) * t;
}

// Reset a drive to idle. ONE implementation -- every clear site funnels here, so the lerp + proxy
// state can never be left half-cleared (RULE 2). Does NOT touch physics; the caller decides
// whether to re-enable simulation (a release) or leave it off (a stick / a freeze).
inline void ResetDriveState(ActiveDrive& d) {
    d.actor      = nullptr;
    d.mesh       = nullptr;
    d.lastKey.clear();
    d.lastEid    = 0;
    d.isProxy    = false;
    d.lerpSeeded   = false;
    d.haveTwoSnaps = false;   // drop the interpolation buffer -> a re-acquire re-primes (snaps) cleanly
}

// Record a new host pose (world loc + rot) as the lerp TARGET. First pose for an identity (or a
// far jump) snaps; otherwise interpolate from the CURRENT rendered transform over the measured
// pose interval. Game thread. (Decoupled from any wire struct so BOTH the PropPose receiver and
// the TrashCarryPose receiver feed it.)
inline void BeginLerpToPose(ActiveDrive& d, const ue_wrap::FVector& nloc,
                            const ue_wrap::FRotator& nrot, uint64_t nowMs) {
    // Distance from the LAST RECEIVED pose (not the rendered position): detects a teleport / post-gap
    // catch-up that should snap rather than crawl across the map. (Comparing to renderedLoc would mistake
    // a normal render-delay lag for a teleport.)
    const float dx = nloc.X - d.lastLoc.X, dy = nloc.Y - d.lastLoc.Y, dz = nloc.Z - d.lastLoc.Z;
    // A non-proxy Aprop_C held item ALWAYS snaps (its proven teleport-to-latest behavior). A proxy snaps
    // only to PRIME (first pose) or on a far jump (gap / teleport -- crawling across the map looks worse).
    if (!d.isProxy || !d.lerpSeeded || (dx * dx + dy * dy + dz * dz) > kSnapDistSq) {
        // Snap + prime BOTH buffer slots at the pose, so the next pose has a valid `prev` to interpolate from.
        d.prevLoc = d.lastLoc = d.renderedLoc = nloc;
        d.prevRot = d.lastRot = d.renderedRot = nrot;
        d.prevPoseMs = d.lastPoseMs = nowMs;
        d.haveTwoSnaps = false;     // only one distinct sample -> sit at it until the next pose
        d.lerpSeeded   = true;
        if (d.actor && R::IsLive(d.actor)) {
            E::SetActorLocation(d.actor, nloc);
            E::SetActorRotation(d.actor, nrot);
        }
        return;
    }
    // Proxy steady state: shift the newest sample to `prev` and append this pose as `last`. Both carry their
    // REAL arrival timestamps; AdvanceLerp interpolates prev->last by the render clock (now - span).
    d.prevLoc = d.lastLoc; d.prevRot = d.lastRot; d.prevPoseMs = d.lastPoseMs;
    d.lastLoc = nloc;      d.lastRot = nrot;      d.lastPoseMs = nowMs;
    d.haveTwoSnaps = true;
}

// Advance an in-progress lerp one tick (called EVERY tick, not only on a new pose, so the follow is
// smooth and a stream gap FREEZES at the target rather than dropping). No-op when no lerp is active
// (frozen) or the actor died. Game thread.
inline void AdvanceLerp(ActiveDrive& d, uint64_t nowMs) {
    if (!d.actor) return;
    if (!R::IsLive(d.actor)) { d.haveTwoSnaps = false; return; }
    if (!d.isProxy || !d.haveTwoSnaps) return;   // non-proxy snapped in BeginLerp; <2 samples -> sit at the pose
    // Render `span` BEHIND the newest pose. span = the REAL interval between the two buffered poses (clamped).
    uint64_t span = (d.lastPoseMs > d.prevPoseMs) ? (d.lastPoseMs - d.prevPoseMs) : kLerpMinMs;
    if (span < kLerpMinMs) span = kLerpMinMs;
    if (span > kLerpMaxMs) span = kLerpMaxMs;
    const uint64_t renderTime = (nowMs > span) ? (nowMs - span) : 0;
    // alpha by REAL timestamps. renderTime >= last (stream STOP / caught up) clamps to 1 -> reach `last` and
    // FREEZE (no extrapolation -- there is no velocity term; control is released at the reliable edge).
    float alpha;
    if (renderTime <= d.prevPoseMs)      alpha = 0.f;
    else if (renderTime >= d.lastPoseMs) alpha = 1.f;
    else alpha = static_cast<float>(renderTime - d.prevPoseMs) /
                 static_cast<float>(d.lastPoseMs - d.prevPoseMs);
    const ue_wrap::FVector loc{
        d.prevLoc.X + (d.lastLoc.X - d.prevLoc.X) * alpha,
        d.prevLoc.Y + (d.lastLoc.Y - d.prevLoc.Y) * alpha,
        d.prevLoc.Z + (d.lastLoc.Z - d.prevLoc.Z) * alpha };
    const ue_wrap::FRotator rot{
        LerpAngle(d.prevRot.Pitch, d.lastRot.Pitch, alpha),
        LerpAngle(d.prevRot.Yaw,   d.lastRot.Yaw,   alpha),
        LerpAngle(d.prevRot.Roll,  d.lastRot.Roll,  alpha) };
    // Dirty-gate: skip the SetActorLocation/Rotation (each a ParamFrame heap alloc + ProcessEvent) when
    // neither position nor any axis moved beyond an epsilon since the LAST WRITTEN transform.
    const float dl = (loc.X - d.renderedLoc.X) * (loc.X - d.renderedLoc.X)
                   + (loc.Y - d.renderedLoc.Y) * (loc.Y - d.renderedLoc.Y)
                   + (loc.Z - d.renderedLoc.Z) * (loc.Z - d.renderedLoc.Z);
    const float dr = std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - d.renderedRot.Pitch))
                   + std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - d.renderedRot.Yaw))
                   + std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - d.renderedRot.Roll));
    if (dl < kLerpEpsLocSq && dr < kLerpEpsRotDeg) return;   // within epsilon -> no redundant write
    E::SetActorLocation(d.actor, loc);
    E::SetActorRotation(d.actor, rot);
    d.renderedLoc = loc;
    d.renderedRot = rot;
}

}  // namespace coop::active_drive
