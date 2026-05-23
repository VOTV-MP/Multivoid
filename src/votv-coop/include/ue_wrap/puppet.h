// ue_wrap/puppet.h -- the remote-player visible body, as a "skin-puppet".
//
// Engine-wrapper layer (principle 7). A puppet is a BARE actor (ASkeletalMeshActor)
// that wears the LOCAL player's exact skin (mesh_playerVisible's USkeletalMesh) and
// runs the same body AnimBP (UAnimBlueprint_kerfurOmega_regular_C), but is owned and
// driven entirely by us -- no pawn, no controller, no CharacterMovement.
//
// Root cause this replaces: a 2nd mainPlayer_C orphan posed as a "stick" because
// its AnimBP samples idle/zero owner-movement state with no controller, and it
// dragged the protagonist class's whole single-player tick surface (hijack/gamma
// cascade) along with it. Two converging agents + the CXX dump established the
// AnimBP poses purely from its own public variables and does NOT require a
// possessing controller (the SAME AnimBP is shared by NPC classes), so the right
// shape is OUR object driven by OUR data (RULE 1 root-cause; RULE 3 parallel class
// hierarchy). The AIController-poses-the-body path is retired (RULE 2).
//
// Every function calls a UFunction or touches engine memory -> game thread only.

#pragma once

#include "ue_wrap/types.h"

namespace ue_wrap::puppet {

// Read the local player's third-person skin asset (mainPlayer_C::mesh_playerVisible
// -> USkeletalMesh*). `mainPlayerPawn` is a live mainPlayer_C. Returns the skin
// USkeletalMesh*, or nullptr if not resolvable.
void* GetMeshPlayerVisibleAsset(void* mainPlayerPawn);

// The local player's mesh_playerVisible component (the skin/anim source). Returns
// the component, or null.
void* GetMeshPlayerVisibleComponent(void* mainPlayerPawn);

// The local player's capsule half-height (ACharacter). The pawn's actor origin is
// the capsule CENTRE; subtract this to get the FEET Z (where a puppet's mesh root
// must sit so it stands on the ground instead of floating). 0 if unreadable.
float GetCapsuleHalfHeight(void* mainPlayerPawn);

// Read the AnimBP CLASS the local player's body actually runs, straight off its
// mesh_playerVisible component (AnimClass @0x6A8) -- the authoritative, always-
// present class pointer. (Looking the Blueprint-generated class up by leaf name
// fails; the live component is the source of truth.) Returns the UClass*, or null.
void* GetMeshPlayerVisibleAnimClass(void* mainPlayerPawn);

// Spawn a puppet (ASkeletalMeshActor) at `loc` wearing `skeletalMeshAsset` and
// running `animClass` (both copied from the local player's working body), set to
// always-tick its pose and visible. Returns the actor, or nullptr. Logs a
// local-vs-puppet AnimBP state diff for diagnosis.
void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass);

// The puppet's USkeletalMeshComponent (first SkeletalMeshComponent child),
// cached per actor. nullptr if none.
void* GetSkeletalMeshComponent(void* puppetActor);

// Drive the puppet's pose from a network snapshot value:
//   * walkSpeed/spd (locomotion BlendSpace inputs; speed in cm/s, 0 = idle).
//   * headLookAt (FRotator): the AnimBP-exposed head-bone look direction.
//     headPitch is the streamed view pitch in degrees (-90..90). Body yaw is
//     applied via SetActorRotation in RemotePlayer::ApplyToEngine, so head
//     yaw delta inside this rotator stays 0 (head stays aligned with body in
//     the common case). When the user can strafe-look in future, head yaw
//     delta = view_yaw - body_yaw goes here too.
// No-op if the live AnimInstance isn't resolved yet.
void DriveAnimBP(void* puppetActor, float speed, float headPitch);

// DIAGNOSTIC (the "diff observable state" rule): read the live AnimInstance off a
// SkeletalMeshComponent and log its key pose-driver variables. Call on the LOCAL
// working body and on the puppet to see exactly which variable differs if the
// puppet still doesn't pose. No-op-safe on null.
void DumpAnimState(const wchar_t* label, void* skeletalMeshComponent);

}  // namespace ue_wrap::puppet
