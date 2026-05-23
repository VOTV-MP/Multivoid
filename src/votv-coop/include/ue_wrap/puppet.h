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

// Read the AnimBP CLASS the local player's body actually runs, straight off its
// mesh_playerVisible component (AnimClass @0x6A8) -- the authoritative, always-
// present class pointer. (Looking the Blueprint-generated class up by leaf name
// fails; the live component is the source of truth.) Returns the UClass*, or null.
void* GetMeshPlayerVisibleAnimClass(void* mainPlayerPawn);

// Spawn a puppet (ASkeletalMeshActor) at `loc` (= the source actor's world
// location, NOT a derived feet-Z) wearing `skeletalMeshAsset` and running
// `animClass`. `srcMeshComp` is the LOCAL mainPlayer_C::mesh_playerVisible
// component -- the puppet's SkeletalMeshComponent copies its parent-relative
// transform (RelativeLocation + RelativeRotation) so the puppet's visual body
// sits exactly where the source's visual body sits (no float-above-ground, no
// 90 deg sideways pop). Both peers run mainPlayer_C so the local's mesh comp
// carries the same RelLoc/RelRot the source uses. Returns the spawned actor,
// or nullptr. Logs a local-vs-puppet AnimBP state diff for diagnosis.
void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass,
                  void* srcMeshComp);

// The puppet's USkeletalMeshComponent (first SkeletalMeshComponent child),
// cached per actor. nullptr if none.
void* GetSkeletalMeshComponent(void* puppetActor);

// Drive the puppet's pose from a network snapshot value:
//   * walkSpeed/spd (locomotion BlendSpace inputs; speed in cm/s, 0 = idle).
//   * headLookAt (FRotator): the AnimBP-exposed head-bone look direction.
//     - headPitch: streamed view pitch, degrees. Wire contract is the
//       canonical FRotator axis range (-180, 180]; live values from VOTV's
//       camera-clamped source sit well inside (-89, 89).
//     - headYawDelta: head yaw RELATIVE to body (degrees). Currently always
//       streamed as 0 -- the source actor's body yaw already lags the camera
//       inside VOTV's Character, so the natural "head leads body" effect
//       reproduces on the puppet for free when we stream actor yaw. Param
//       kept for a future enhancement where the head's yaw lead is sent
//       explicitly (e.g. free-look key without turning the body).
// No-op if the live AnimInstance isn't resolved yet.
void DriveAnimBP(void* puppetActor, float speed, float headPitch, float headYawDelta);

// DIAGNOSTIC (the "diff observable state" rule): read the live AnimInstance off a
// SkeletalMeshComponent and log its key pose-driver variables. Call on the LOCAL
// working body and on the puppet to see exactly which variable differs if the
// puppet still doesn't pose. No-op-safe on null.
void DumpAnimState(const wchar_t* label, void* skeletalMeshComponent);

// Bug 2 deep diagnostic: dump live FAnimNode memory regions (BlendSpacePlayer
// at +0x1180, both FAnimNode_StateMachines at +0x1AC0 and +0x1CC8 per the CXX
// dump). Logs non-trivial floats + ints to pinpoint:
//   * Local BlendSpacePlayer.X offset (the walking-speed sample coordinate;
//     non-zero on walking local -- gives us the byte offset within the node).
//   * Local State Machine current state index vs puppet's (whether the puppet
//     is stuck on idle while the local is in walk).
// Call on BOTH local and puppet to diff. No-op-safe on null.
void DumpAnimNodeRegions(const wchar_t* label, void* skeletalMeshComponent);

// Bug 2 root-cause fix (Plan B2, 2026-05-23):
//
// The AnimBP's BlueprintUpdateAnimation pulls velocity from
// Pawn->GetMovementComponent()->Velocity and uses it to populate spd,
// Movement, IK targets, and the bool packs the state machine reads. On a
// null-Pawn puppet, BUA short-circuits and the AnimInstance is vastly
// under-populated -> state machine stays in idle even with our spd writes.
//
// Earlier Plan B1 (intercept BUA via game_thread::SetInterceptor and skip
// the original) was retired because skipping BUA left ALL the other
// AnimInstance fields zero, not just spd. The full AnimBP_vars_all diff
// dump (2026-05-23) proved this empirically: 8 set fields on puppet vs ~26
// on local.
//
// Plan B2: RemotePlayer spawns a hidden, inert satellite ACharacter at
// puppet creation time, writes its CharacterMovementComponent.Velocity from
// the streamed pose each Tick, and points the puppet's AnimInstance.Pawn
// pointer at the satellite. BUA then runs naturally and pulls EVERY field
// it would on the local mainPlayer -- including the ones the state machine
// transitions read. The puppet animates the same way the local does.

}  // namespace ue_wrap::puppet
