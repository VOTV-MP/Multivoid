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

// Two puppet UClass strategies (controlled by IsMainPlayerPuppetKind() env
// var below).
//
//   MainPlayer (DEFAULT, 2026-05-25 per
//   research/findings/votv-puppet-mainplayer-body-RE-2026-05-25.md):
//     spawn AmainPlayer_C with inertPawn=true (AutoPossessPlayer/AI=0,
//     bBlockInput=1). The class's built-in mesh_playerVisible carries the
//     player body skin + IK leg bones; we just neuter the per-screen
//     systems (DestroyComponent on both PostProcessComponents; hide FP
//     `arms` viewmodel) and let the existing satellite-ACharacter (Plan B2)
//     drive the AnimBP. useLegIK can stay TRUE (real ACharacter has the
//     floor-trace context the IK needs).
//
//   SkeletalMeshActor (BACKUP, retiring per RULE 2 once mainPlayer_C path
//   verified in hands-on test):
//     spawn ASkeletalMeshActor + SetSkeletalMesh + SetAnimClass with the
//     local player's skin asset + body AnimBP. Same satellite path. Bare-
//     actor IK doesn't work (no Character context), so useLegIK is FALSE.
//
// Both kinds: returns the actor or nullptr. Logs a local-vs-puppet AnimBP
// state diff for diagnosis. RemotePlayer's Z convention adapts via
// puppet::GetSpawnMeshOffsetZ(localPlayer) below.
void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass);

// Returns true iff the active puppet kind is mainPlayer_C. Reads
// VOTVCOOP_PUPPET_KIND env var ONCE on first call (cached). Values:
//   "mainplayer" (default) -> true (mainPlayer_C orphan puppet, IK on)
//   "skelmesh"             -> false (legacy SkeletalMeshActor puppet)
// Default chosen on the user directive 2026-05-25: "we need to create
// remote as a second puppet which local first person player sees when he
// bends his camera down (body, legs, ik legs). NOT KERFUR. (kerfur we
// leave as a backup)".
//
// Retirement (RULE 2): the SkelMesh path goes when mainPlayer_C path is
// hands-on-verified working (body visible to other peer with correct
// orientation + IK legs working + no enemy-targeting regression). At that
// point this function + the SkelMesh branch in SpawnPuppet + the
// SkeletalMeshActorClass name constant all go.
bool IsMainPlayerPuppetKind();

// Z offset to apply at spawn placement so the puppet's visible mesh world Z
// matches the source actor's mesh world Z. Differs by kind:
//   * MainPlayer: 0.f (the ACharacter's CapsuleComponent IS the root; the
//     mesh hangs at -halfH below via BP construction; puppet.actor.Z =
//     source.actor.Z directly).
//   * SkelMesh:  -halfH (the SkeletalMeshComponent IS the root; we
//     reconstruct the -halfH shim at the actor transform level).
// `localPlayer` is the local mainPlayer_C; used to read CapsuleHalfHeight.
// Returns 0 if kind == MainPlayer or if localPlayer is null/dead.
float GetSpawnMeshOffsetZ(void* localPlayer);

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

// Head-tracking diagnostic 2026-05-23 PM: dump the kerfur AnimBP's FAnimNode_LookAt
// instances + every FAnimNode_ModifyBone's BoneToModify FName + their Alpha. The
// "puppet head tracks local player" symptom is driven by these nodes; we need to
// identify which ModifyBone targets 'head' (so we can write its Rotation) and
// confirm the LookAt Alphas (so we can zero them to bypass the auto-track). No-op-
// safe on null. One-shot at puppet spawn.
void DumpKerfurHeadGraph(void* skeletalMeshComponent);

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
