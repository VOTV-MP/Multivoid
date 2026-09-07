// ue_wrap/puppet.h -- the remote player's visible body: an unpossessed mainPlayer_C spawned
// inert (no controller, input blocked, its per-screen systems neutered), wearing the local
// player's skin and AnimBP, and driven by us: its CharacterMovement velocity and mode from the
// streamed pose, its head from the streamed look target. The same AnimBP serves the kerfur NPCs,
// so the kerfur head and body drives live here too. Every function is game thread only.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::puppet {

// The local player's third-person skin asset (mainPlayer_C::mesh_playerVisible's USkeletalMesh);
// nullptr if unresolvable.
void* GetMeshPlayerVisibleAsset(void* mainPlayerPawn);

// The local player's mesh_playerVisible component (the skin and anim source); nullptr if none.
void* GetMeshPlayerVisibleComponent(void* mainPlayerPawn);

// The AnimBP class the local player's body runs, read off its mesh_playerVisible component
// (its AnimClass): the live component is the source of truth, and the generated class does
// not resolve by leaf name. nullptr if none.
void* GetMeshPlayerVisibleAnimClass(void* mainPlayerPawn);

// The inherited ACharacter::Mesh slot, mesh_playerVisible's AttachParent: on
// mainPlayer_C it carries its own kel body overlapping mesh_playerVisible 1:1, so a custom skin
// on the latter stays covered unless this one is hidden. nullptr if none.
void* GetNativeBodyMeshComponent(void* mainPlayerActor);

// The USkeletalMesh asset a skinned component holds (a raw field read); null-safe.
void* GetComponentSkeletalMeshAsset(void* skinnedComponent);

// Spawn the puppet: AmainPlayer_C with inertPawn (AutoPossessPlayer and AutoPossessAI zero,
// input blocked), the local player's skin and AnimClass copied onto its mesh_playerVisible (the
// class default may carry none; the game applies the skin at save load), its GameMode pointer
// nulled and the gamemode's mainPlayer restored after BeginPlay, the post-process components
// destroyed and the first-person arms hidden. The actor, or nullptr; logs a local-vs-puppet
// AnimBP state diff.
void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass);

// The puppet's USkeletalMeshComponent (the first such child), cached per actor; nullptr if none.
void* GetSkeletalMeshComponent(void* puppetActor);

// Drive the puppet's head to a world look point, so it shows where the remote player is looking:
// the kerfur AnimBP's native lookAt/customLookAt path on the puppet's own AnimInstances, written
// on both rendered bodies (mesh_playerVisible and the ACharacter::Mesh slot each carry one);
// customLookAt stops the AnimBP re-aiming at the local camera each tick. The caller reconstructs
// the target from the streamed camera yaw and pitch. No-op until an AnimInstance resolves.
void DriveHeadLookAtWorld(void* puppetActor, const FVector& worldTarget);

// A read-only probe of the puppet's head-look state: the LookAt clamps and alphas, the AnimBP
// gate flags, and the world rotation of the head and neck bones. The LookAt nodes live inside
// the AnimBP state `lookAtPlayer`, exited when lookingAtPlayer flips false, so with the
// state-gate hook installed it must read true on every sample. Game thread.
struct PuppetHeadLookProbe {
    float headClampDeg = 0.f;    // FAnimNode_LookAt head node LookAtClamp (class-default 45)
    float neckClampDeg = 0.f;    // neck node LookAtClamp (class-default 45, Alpha 0.5)
    float headWorldYaw = 0.f;    // 'head' bone world yaw (deg)
    float headWorldPitch = 0.f;  // 'head' bone world pitch (deg)
    float neckWorldYaw = 0.f;    // 'neck' bone world yaw (deg)
    // The gate flags: why the head-look turns off.
    float headAlpha = -1.f;      // head LookAt node SkelCtl_Alpha (1.0 = active; 0 = look bypassed)
    float neckAlpha = -1.f;      // neck LookAt node SkelCtl_Alpha (0.5 native)
    bool  lookingAtPlayer = false;  // AnimBP lookingAtPlayer (dot-product gate: observer in front?)
    bool  customLookAt = false;     // our drive still pinned? (true = our lookAt wins; false = BUA reclaimed)
    bool  haveClamp = false;
    bool  haveHead  = false;
    bool  haveNeck  = false;
    bool  haveGates = false;     // alpha + lookingAtPlayer + customLookAt read off the AnimInstance
};
bool ReadPuppetHeadLookProbe(void* puppetActor, PuppetHeadLookProbe& out);

// Diagnostic: log the live AnimInstance's key pose-driver variables off a SkeletalMeshComponent,
// on the local body and on the puppet, to see which differs. Null-safe.
void DumpAnimState(const wchar_t* label, void* skeletalMeshComponent);

// Diagnostic: dump the kerfur AnimBP's FAnimNode_LookAt instances and every FAnimNode_ModifyBone's
// bone and alpha, the nodes that drive the head. Null-safe; once at puppet spawn.
void DumpKerfurHeadGraph(void* skeletalMeshComponent);

// Write the puppet's CharacterMovement state (Velocity and MovementMode) from a streamed pose:
// the AnimBP's update pulls velocity from the pawn's movement component for the locomotion
// blend, the foot-IK alpha and the falling state, so a puppet with no velocity written stays in
// idle. A direct memory write, no dispatch on the per-tick interp. `inAir` selects MOVE_Falling
// or MOVE_Walking. No-op on a null or dead actor or CMC.
void DriveCharacterMovement(void* puppetActor,
                            const FVector& worldVelocity,
                            bool inAir);

// The player puppet's sprint knob: MaxWalkSpeed at the class default (walking) or twice it
// (sprinting), mirroring mainPlayer's native updateSpeed; the footstep volume reads MaxWalkSpeed,
// not Velocity. Separate from DriveCharacterMovement, which NPC mirrors share with their own
// MaxWalkSpeed.
void DriveSprintWalkSpeed(void* puppetActor, bool sprinting);

// True iff `actor` is an ACharacter whose CMC reports MOVE_Falling (the wire's in-air bit); the
// same raw read as DriveCharacterMovement, so gameplay code never touches the offset. False on a
// null or dead actor or CMC.
bool ReadCharacterIsFalling(void* actor);

// ---- kerfur head-look ----
// The kerfur AnimBP (shared by the NPCs and the player puppets) aims the head and neck at its
// `lookAt` member (a world FVector), which its update recomputes from the local camera
// each tick unless `customLookAt` is set, so head-look is per peer by default; the NPC
// sync streams the host's resolved target. Both functions are gated on the kerfur-family AnimBP
// class, so a non-kerfur actor is a safe no-op.

// Host read: a live kerfur NPC's resolved head-look world target (its AnimBP lookAt), off the
// ACharacter::Mesh AnimInstance; false if not a live kerfur or unresolved.
bool ReadKerfurLookAt(void* npcActor, FVector& outWorldTarget);

// Client drive: point a kerfur-family mirror's head at a streamed world target (writes lookAt
// and sets customLookAt, so the mirror's own update stops overwriting it). No-op unless a live
// kerfur.
void DriveKerfurLookAt(void* npcActor, const FVector& worldTarget);

// The kerfur body's facing: the actor BP rotates its Mesh component's world yaw toward the local
// player, decoupled from the root, and a mirror's actor tick is off, so the host reads the
// resolved yaw and the client drives it. Kerfur-family only; DriveKerfurBodyYaw after
// SetActorRotation.
bool ReadKerfurBodyYaw(void* npcActor, float& outYaw);
void DriveKerfurBodyYaw(void* npcActor, float yaw);

// Park an ACharacter-derived puppet so the network SetActorLocation drive is authoritative: the
// CharacterMovement tick off (no gravity or velocity integration fighting the drive) and the
// actor tick off (no BP ReceiveTick; for an NPC mirror that is its AI). The AnimBP still ticks on
// the mesh and reads the CMC velocity we write. Applied to NPC mirrors; the player puppet's own
// spawn parks its ticks directly (puppet_spawn.cpp). No-op on a null or dead actor.
void DisableCharacterTicks(void* actor);

// The CMC-only park: the movement tick off, the actor tick left on, for a mirror whose
// ReceiveTick is per-viewer cosmetic (wisp_C fade, bob, shy despawn) while the pose lane owns
// position. The CMC-computed landing gate (CurrentFloor) then stays stale, so the pose lane
// drives that edge itself (ue_wrap::wisp::DriveWispLanding). No-op on a null or dead actor.
void DisableMovementTick(void* actor);

}  // namespace ue_wrap::puppet
