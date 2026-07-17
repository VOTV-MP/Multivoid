// coop/element/npc.h -- the Npc Element subclass.
//
// First subclass of `coop::element::Element`. PoC for the MTA CClientEntity
// adoption per `research/findings/mta/votv-mta-cclientelement-audit-2026-05-28.md`
// section 4.5. NPCs were chosen as the PoC target because (a) lifecycle is the
// simplest (host spawns, host destroys, no per-frame ownership transfer); (b)
// the prior `g_nextNpcSessionId` atomic counter is the closest semantic match
// to MTA's ElementID free-list; (c) the smallest "before" footprint of the
// three replicated entity classes.
//
// Owned by `coop::npc_sync` (all three lifecycle hooks wired + runtime-validated
// -- npctest 2026-06-07: host POST-bound=1, cross-peer mirror at the host's spawn
// loc):
//   - host interceptor on BeginDeferredSpawnFromClass: allocates an Npc per
//     host-spawned NPC class + broadcasts EntitySpawn (payload.elementId).
//   - host POST observer on the same UFunction: binds the returned AActor* via
//     Element::SetActor + records actor->eid for destroy-tracking.
//   - host K2_DestroyActor PRE observer: destroys the Npc + broadcasts
//     EntityDestroy when the engine destroys the actor.

// POSE DRIVE (increment 1, 2026-06-07): the Npc gains a receiver-side interpolator +
// engine drive -- a SUBSET of RemotePlayer's (pos + yaw + speed + stateBits; NO
// pitch/headYawDelta/vitals/ragdoll/mesh-offset). The CLIENT mirror uses it: each
// EntityPose batch entry calls SetTargetNpcPose (advance-before-rebase, the proven
// interp-starvation fix) and net_pump::Tick calls Tick() every frame to drive the mirror's
// transform + CMC.Velocity so the NPC's OWN AnimBP animates (same native locomotion path as
// the player puppet). The HOST Npc never interpolates -- it READS the live actor (TickPoseStream).
// Implementation in src/coop/npc_pose_drive.cpp. The interp TIMING (alpha/dAlpha bookkeeping)
// is the shared coop::LerpWindow (RULE 2 / WP13 -- RemotePlayer owns one too, extracted
// 2026-06-07); this class applies the window's dAlpha to its OWN pos+yaw errors.

#pragma once

#include "coop/element/element.h"
#include "coop/element/lerp_window.h"
#include "ue_wrap/core/types.h"  // FVector

#include <cstdint>

namespace coop::net { struct EntityPoseSnapshot; }

namespace coop::element {

class Npc : public Element {
public:
    Npc() : Element(ElementType::Npc) {}

    // CLIENT mirror: a fresh EntityPose entry for this eid. Opens a LERP window toward the
    // new pose (or snaps on the first packet / a teleport). Game thread only.
    void SetTargetNpcPose(const coop::net::EntityPoseSnapshot& snap);

    // CLIENT mirror: every frame -- advance the interp + push the pose to the engine (skips the
    // engine write when frozen at target between packets). No-op until a pose arrives + the actor
    // is live. Game thread only.
    void Tick();

    // Wisp mirror (2026-07-03): the wisp_C fade-in fires at a tick-driven landing edge a
    // CMC-parked mirror can never compute (CurrentFloor stays stale) -- the pose drive replays
    // it (ue_wrap::wisp::DriveWispLanding) once the streamed pose reads grounded. Armed by
    // npc_mirror at materialization for exactly class wisp_C.
    void MarkWispMirror() { isWispMirror_ = true; }

private:
    void AdvanceInterp();   // advance the open window to now (mirrors RemotePlayer::AdvanceInterp)
    void ApplyToEngine();   // SetActorLocation + SetActorRotation + DriveCharacterMovement

    // Receiver-side interpolation state (game thread only; the engine path is single-threaded).
    ue_wrap::FVector curPos_{};
    float            curYaw_       = 0.f;
    float            curSpeed_     = 0.f;   // not interpolated -- the AnimBP blends locomotion
    uint8_t          curStateBits_ = 0;     // not interpolated -- snapped (bit 0 = in air)
    ue_wrap::FVector targetPos_{};
    float            targetYaw_      = 0.f;
    ue_wrap::FVector errorPos_{};            // cached (target - cur) at packet arrival; applied dAlpha/frame
    float            errorYaw_       = 0.f;
    float            curBodyYaw_     = 0.f;   // v40: interpolated VISIBLE-body (ACharacter::Mesh) world yaw
    float            targetBodyYaw_  = 0.f;   //      (driven onto the mirror mesh each frame after SetActorRotation)
    float            errorBodyYaw_   = 0.f;   //      cached (target-cur) at packet; applied dAlpha/frame (shortest-arc)
    coop::LerpWindow window_;                // shared interp timing (same one RemotePlayer owns)
    ue_wrap::FVector curLookAt_{};           // v39: streamed kerfur head-look WORLD target (NOT interpolated --
    bool             hasLookAt_      = false; //       the native FAnimNode_LookAt smooths it via its own InterpSpeed)
    bool             hasBodyYaw_     = false; // v40: bodyYaw valid (kerfur-family) -> drive the mesh world yaw
    uint8_t          kerfState_      = 0;     // v74: streamed kerfur command (enum_kerfurCommand) -- snapped, drives the
    bool             kerfSpooky_     = false; //      AnimBP state machine on the parked mirror; spooky = the kill/spooky flag
    bool             hasKerfState_   = false; //      valid iff the host sent it (kerfur-family only)
    bool             hasPose_        = false;  // first packet snaps
    bool             dirty_          = true;   // unapplied change to push to the engine
    bool             isWispMirror_   = false;  // wisp_C mirror: replay the landing edge (fade-in)
    bool             wispLanded_     = false;  //   ... one-shot latch (drive succeeded)
};

}  // namespace coop::element
