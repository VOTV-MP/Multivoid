// coop/element/world_actor.h -- the WorldActor Element subclass (B3b, 2026-06-17).
//
// A peer of `Npc` under `coop::element::Element` (the intended sibling design --
// Element is the MTA CClientEntity adoption, and CClientStreamElement has several
// per-type siblings sharing the stream manager). Mirrors the ~14 NON-Character
// event actors (gray saucers, Rozital mothership, ariral ships, sky UFO, space
// jellyfish, firetank, ...) the Character-only NPC mirror can't replicate:
// `npc_pose_drive` drives pos + YAW-ONLY rotation + DriveCharacterMovement (CMC@0x288,
// ACharacter-only), so a raw AActor would lose pitch/roll AND the Character parking
// would misread the non-Character layout (design doc votv-b3b-worldactor-mirror-
// design-2026-06-17.md). So WorldActor is its OWN element: pos + FULL rotation
// (pitch/yaw/roll), NO CMC, NO kerfur, NO save-persist.
//
// Owned by `coop::world_actor_sync` (the npc_sync shape, simpler):
//   - host interceptor on BeginDeferredSpawnFromClass (a SECOND interceptor on the
//     same UFunction; the WorldActor allowlist is DISJOINT from npc_sync's, so the
//     two are conflict-free -- game_thread.h:115 multi-interceptor support):
//     allocates a WorldActor per host-spawned allowlisted actor + broadcasts
//     WorldActorSpawn.
//   - host POST observer on the same UFunction: binds the returned AActor*.
//   - host K2_DestroyActor PRE observer: broadcasts WorldActorDestroy + releases.
//
// CLIENT mirror drive: each WorldActorPose batch entry calls SetTargetPose (the
// advance-before-rebase interp, the proven interp-starvation fix), and the net-pump
// tick calls Tick() every frame to SetActorLocation + SetActorRotation the mirror.
// The mirror's actor tick is parked (world_actor_sync: SetActorTickEnabled(false)),
// so the streamed pose is authoritative (no integration fight). The HOST WorldActor
// never interpolates -- it READS the live actor (world_actor_sync::TickPoseStream).
// The interp TIMING is the shared coop::LerpWindow (RULE 2 / WP13 -- RemotePlayer +
// Npc own one too); this class applies its dAlpha to its OWN pos + 3-angle errors.

#pragma once

#include "coop/element/element.h"
#include "coop/element/lerp_window.h"
#include "ue_wrap/core/types.h"  // FVector

namespace coop::net { struct WorldActorPoseSnapshot; }

namespace coop::element {

class WorldActor : public Element {
public:
    WorldActor() : Element(ElementType::WorldActor) {}

    // CLIENT mirror: a fresh WorldActorPose entry for this eid. Opens a LERP window toward the new
    // pose (or snaps on the first packet / a teleport). Game thread only.
    void SetTargetPose(const coop::net::WorldActorPoseSnapshot& snap);

    // CLIENT mirror: every frame -- advance the interp + push the pose to the engine (skips the
    // engine write when frozen at target between packets). No-op until a pose arrives + the actor
    // is live. Game thread only.
    void Tick();

    // v100: the interp'd class-specific visible-heading yaw (WorldActorPoseSnapshot.auxYaw).
    // Consumed by class lanes (piramid_sync writes it to the heading ArrowComponents); the
    // generic ApplyToEngine ignores it. Valid once a pose arrived (hasPose()).
    float CurrentAuxYaw() const { return curAuxYaw_; }
    // v102: the latest class-specific auxiliary TARGET vector (WorldActorPoseSnapshot.auxX/Y/Z;
    // piramid2_C = relLook, the head's look target). NOT interpolated -- it is a target the
    // mirror's own native easing consumes, so the latest wire value IS the truth.
    void CurrentAuxVec(float& x, float& y, float& z) const { x = auxX_; y = auxY_; z = auxZ_; }
    // v104: the latest class-specific TARGET-IDENTITY eid (WorldActorPoseSnapshot.auxTargetEid;
    // piramid2_C = the host's wispTarget as its npc-lane eid; 0 = none). Latest-wins like the
    // aux vec -- an identity, nothing to interpolate.
    uint32_t CurrentAuxTargetEid() const { return auxTargetEid_; }
    bool  HasPose() const { return hasPose_; }

private:
    void AdvanceInterp();   // advance the open window to now (mirrors Npc::AdvanceInterp)
    void ApplyToEngine();   // SetActorLocation + SetActorRotation (FULL rotation; no CMC, no kerfur)

    // Receiver-side interpolation state (game thread only; the engine path is single-threaded).
    ue_wrap::FVector curPos_{};
    float            curPitch_ = 0.f;
    float            curYaw_   = 0.f;
    float            curRoll_  = 0.f;
    ue_wrap::FVector targetPos_{};
    float            targetPitch_ = 0.f;
    float            targetYaw_   = 0.f;
    float            targetRoll_  = 0.f;
    ue_wrap::FVector errorPos_{};     // cached (target - cur) at packet arrival; applied dAlpha/frame
    float            errorPitch_ = 0.f;  // shortest-arc deltas, applied dAlpha/frame
    float            errorYaw_   = 0.f;
    float            errorRoll_  = 0.f;
    float            curAuxYaw_    = 0.f;  // v100 class-specific heading (see CurrentAuxYaw)
    float            targetAuxYaw_ = 0.f;
    float            errorAuxYaw_  = 0.f;
    float            auxX_ = 0.f, auxY_ = 0.f, auxZ_ = 0.f;  // v102 aux target vec (latest wire value)
    uint32_t         auxTargetEid_ = 0;  // v104 aux target identity (latest wire value; 0 = none)
    coop::LerpWindow window_;          // shared interp timing (same one RemotePlayer / Npc own)
    bool             hasPose_ = false; // first packet snaps
    bool             dirty_   = true;  // unapplied change to push to the engine

    // [WA-TRACE client-drive] state (2026-07-05 0s-frozen-pyramid hunt): 1 Hz per-mirror step/state
    // log + the engine-write RESULTS (K2_SetActorLocation/Rotation CAN fail silently -- e.g. a
    // static-mobility root -- and the old ApplyToEngine discarded both returns).
    uint64_t dbgLastLogMs_   = 0;
    bool     lastApplyLocOk_ = true;
    bool     lastApplyRotOk_ = true;
};

}  // namespace coop::element
