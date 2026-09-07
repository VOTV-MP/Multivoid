// coop/element/world_actor.h -- the WorldActor Element subclass, a peer of `Npc` under
// `coop::element::Element`. Element is the MTA CClientEntity adoption, and CClientStreamElement has
// several per-type siblings sharing one stream manager; this is that shape. The lane that owns
// these elements, and the wire they ride, is `coop::world_actor_sync`.
//
// It mirrors the 18 allowlisted NON-Character actors that the Character-only NPC mirror cannot
// replicate: the event bodies (gray saucers, the Rozital mothership, ariral ships, the sky UFO,
// the space jellyfish, the firetank) plus the sell-gun coin and the sandbox sub-pawn.
// `npc_pose_drive` drives position, YAW-ONLY rotation and DriveCharacterMovement -- the last
// ACharacter-only -- so a raw AActor would lose pitch and roll AND the Character parking would
// misread the non-Character layout. WorldActor is therefore its own element: position plus FULL
// rotation, no movement component, no kerfur, no save persistence. The HOST WorldActor never
// interpolates, since it READS the live actor; on a client the interp TIMING is the shared
// coop::LerpWindow that RemotePlayer and Npc own too, and this class applies its dAlpha to its own
// position and its three angle errors.

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

    // CLIENT mirror: every frame -- advance the interp and push the pose to the engine,
    // skipping the engine write while frozen at target between packets. No-op until a pose
    // has arrived and the actor is live. The mirror's own actor tick is parked, so the
    // streamed pose is authoritative and no integration fights it. Game thread only.
    void Tick();

    // The interpolated class-specific visible-heading yaw (WorldActorPoseSnapshot.auxYaw).
    // Consumed by class lanes -- piramid_sync writes it to the heading ArrowComponents -- while
    // the generic ApplyToEngine ignores it. Valid once a pose arrived (hasPose()).
    float CurrentAuxYaw() const { return curAuxYaw_; }
    // The latest class-specific auxiliary TARGET vector (WorldActorPoseSnapshot.auxX/Y/Z; for
    // piramid2_C it is relLook, the head's look target). NOT interpolated: it is a target the
    // mirror's own native easing consumes, so the latest wire value IS the truth.
    void CurrentAuxVec(float& x, float& y, float& z) const { x = auxX_; y = auxY_; z = auxZ_; }
    // The latest class-specific TARGET-IDENTITY eid (WorldActorPoseSnapshot.auxTargetEid; for
    // piramid2_C the host's wispTarget as its npc-lane eid, 0 for none). Latest-wins like the aux
    // vec -- an identity has nothing to interpolate.
    uint32_t CurrentAuxTargetEid() const { return auxTargetEid_; }
    bool  HasPose() const { return hasPose_; }

    // TRANSFORM-DELTA GATE, host send side. True when this actor's transform differs from the one
    // we last batched, recording the new value when it does. Self-re-arming: anything that moves a
    // resting actor is a change again -- a collector's capsule shoving a coin's r=15 physics body
    // sits OUTSIDE its r=10 pickup trigger. The epsilon is coarse on purpose, since this gates a
    // MIRROR's visual pose and not a physics result.
    //
    // SCOPE: this saves WIRE BYTES, not batch slots. The caller MUST test the batch cap BEFORE
    // calling, because reading the transform costs two ProcessEvent dispatches and two heap
    // allocations, and gating on delta ahead of the cap made the walk scale with the whole live
    // population instead of with 28. It follows that a resting sell-gun coin still OCCUPIES its
    // slot in iteration order. Starving the shipped event actors out of a 28-entry batch stays an
    // open concern once coins accumulate, and the fix for that is fairness in the walk -- a
    // rotating start -- not moving this call.
    bool PoseChangedSinceLastSend(const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

private:
    // last transform actually batched (host send side; see PoseChangedSinceLastSend)
    bool  sentAny_    = false;
    float sentX_ = 0.f, sentY_ = 0.f, sentZ_ = 0.f;
    float sentPitch_ = 0.f, sentYaw_ = 0.f, sentRoll_ = 0.f;

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
    float            curAuxYaw_    = 0.f;  // class-specific heading (see CurrentAuxYaw)
    float            targetAuxYaw_ = 0.f;
    float            errorAuxYaw_  = 0.f;
    float            auxX_ = 0.f, auxY_ = 0.f, auxZ_ = 0.f;  // aux target vec (latest wire value)
    uint32_t         auxTargetEid_ = 0;  // aux target identity (latest wire value; 0 = none)
    coop::LerpWindow window_;          // shared interp timing (same one RemotePlayer / Npc own)
    bool             hasPose_ = false; // first packet snaps
    bool             dirty_   = true;  // unapplied change to push to the engine

    // [WA-TRACE client-drive] state: a 1 Hz per-mirror step and state log, plus the engine-write
    // RESULTS. K2_SetActorLocation and K2_SetActorRotation CAN fail silently -- a static-mobility
    // root does exactly that -- and an earlier ApplyToEngine discarded both returns.
    uint64_t dbgLastLogMs_   = 0;
    bool     lastApplyLocOk_ = true;
    bool     lastApplyRotOk_ = true;
};

}  // namespace coop::element
