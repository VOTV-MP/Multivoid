// coop/element/npc.h -- the Npc Element subclass. Host-spawn, host-destroy, no per-frame
// ownership transfer.
//
// `coop::npc_sync` owns the lifecycle through three seams: an interceptor on
// UGameplayStatics::BeginDeferredActorSpawnFromClass (the host branch allocates an Npc for an allowlisted
// class and broadcasts EntitySpawn; the client branch skips the original, leaving only the mirror),
// a POST observer on the same UFunction (binds the AActor* and records actor->eid), and a PRE
// observer on K2_DestroyActor (destroys the Npc, broadcasts EntityDestroy).

// The CLIENT mirror interpolates a SUBSET of RemotePlayer's pose (pos + yaw + speed + stateBits; no
// pitch/headYawDelta/vitals/ragdoll/mesh-offset): each EntityPose entry calls SetTargetNpcPose
// (advance-before-rebase, which keeps the interp from starving) and npc_mirror::TickClientNpcs
// calls Tick() every frame to drive the transform + CMC.Velocity so the NPC's own AnimBP animates.
// The HOST Npc never interpolates -- it reads the live actor (npc_sync::TickPoseStream). Drive in
// src/coop/creatures/npc_pose_drive.cpp; the alpha bookkeeping is the shared coop::LerpWindow
// RemotePlayer owns too.

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

    // CLIENT mirror: every frame -- advance the interp, and push the pose to the engine when it
    // differs from what was last written there. No-op until a pose arrives + the actor is live.
    // Game thread only.
    void Tick();

    // Wisp mirror: the wisp_C fade-in fires at a tick-driven landing edge a CMC-parked mirror can
    // never compute (CurrentFloor stays stale) -- the pose drive replays it
    // (ue_wrap::wisp::DriveWispLanding) once the streamed pose reads grounded. Armed by npc_mirror
    // at materialization for exactly class wisp_C.
    void MarkWispMirror() { isWispMirror_ = true; }

private:
    void AdvanceInterp();   // advance the open window to now (mirrors RemotePlayer::AdvanceInterp)
    bool ApplyToEngine();   // SetActorLocation + SetActorRotation + DriveCharacterMovement; false if a transform write failed

    // Receiver-side interpolation state (game thread only; the engine path is single-threaded).
    ue_wrap::FVector curPos_{};
    float            curYaw_       = 0.f;
    float            curSpeed_     = 0.f;   // not interpolated -- the AnimBP blends locomotion
    uint8_t          curStateBits_ = 0;     // not interpolated -- snapped (bit 0 = in air)
    ue_wrap::FVector targetPos_{};
    float            targetYaw_      = 0.f;
    ue_wrap::FVector errorPos_{};            // cached (target - cur) at packet arrival; applied dAlpha/frame
    float            errorYaw_       = 0.f;
    float            curBodyYaw_     = 0.f;   // interpolated VISIBLE-body (ACharacter::Mesh) world yaw
    float            targetBodyYaw_  = 0.f;   //   (driven onto the mirror mesh each frame after SetActorRotation)
    float            errorBodyYaw_   = 0.f;   //   cached (target-cur) at packet; applied dAlpha/frame (shortest-arc)
    coop::LerpWindow window_;                // shared interp timing (same one RemotePlayer owns)
    ue_wrap::FVector curLookAt_{};           // streamed kerfur head-look WORLD target (NOT interpolated --
    bool             hasLookAt_      = false; //   the native FAnimNode_LookAt smooths it via its own InterpSpeed)
    bool             hasBodyYaw_     = false; // bodyYaw valid (kerfur-family) -> drive the mesh world yaw
    uint8_t          kerfState_      = 0;     // streamed kerfur command (enum_kerfurCommand) -- snapped, drives the
    bool             kerfSpooky_     = false; //   AnimBP state machine on the parked mirror; spooky = the kill/spooky flag
    bool             hasKerfState_   = false; //   valid iff the host sent it (kerfur-family only)
    bool             hasPose_        = false;  // first packet snaps
    bool             dirty_          = true;   // unapplied change to push to the engine
    bool             isWispMirror_   = false;  // wisp_C mirror: replay the landing edge (fade-in)
    bool             wispLanded_     = false;  //   ... one-shot latch (drive succeeded)

    // What ApplyToEngine last wrote, and onto which actor, so a pose identical to it costs
    // nothing: the host's rotation re-sends a mirror that lies still (an eggvasion's 121 eggs) the
    // same pose, and each re-send reopened the interpolation window and re-teleported the actor
    // every frame. A mirror's tick and its CMC's are parked, so nothing else moves it between two
    // writes; a rebound actor differs, so it is written.
    struct Applied {
        void*            actor = nullptr;
        ue_wrap::FVector pos{}, lookAt{};
        float            yaw = 0.f, speed = 0.f, bodyYaw = 0.f;
        uint8_t          stateBits = 0, kerfState = 0;
        bool             hasLookAt = false, hasBodyYaw = false, hasKerfState = false, spooky = false;
    } applied_;
    bool SameAsApplied(void* actor) const;
    void RememberApplied(void* actor);
};

}  // namespace coop::element
