// ue_wrap/engine/engine.h -- the world and the actors in it: spawning, transforms, the console, the
// pause. Engine operations built on reflection, where each call marshals one UFunction call or one
// reflected field access. No gameplay, network or coop state. Nearly every call must run on the
// game thread (ue_wrap::game_thread::Post); the raw reads that are safe elsewhere say so.
//
// The rest of the engine wrapper is one header per domain, each paired with the engine_*.cpp that
// implements it, and this header includes them all. So `#include "ue_wrap/engine/engine.h"` still
// reaches every engine call and nothing has to know which domain a function lives in; include a
// single part directly when you want the narrower dependency. Read the part, not this file: each
// is short and about one thing.

#pragma once

#include "ue_wrap/core/types.h"

#include "ue_wrap/engine/engine_attach.h"        // attach, detach, root-body physics
#include "ue_wrap/engine/engine_audio.h"         // a sound at a world location
#include "ue_wrap/engine/engine_bones.h"         // skeletal-mesh bones by name
#include "ue_wrap/engine/engine_component.h"     // an actor's components
#include "ue_wrap/engine/engine_mainplayer.h"    // the local player: grab, flashlight, ragdoll, damage
#include "ue_wrap/engine/engine_nav.h"           // nav paths and movement input
#include "ue_wrap/engine/engine_pawn.h"          // controller, control rotation, view target, camera
#include "ue_wrap/engine/engine_physics.h"       // a primitive component's rigid body
#include "ue_wrap/engine/engine_playerragdoll.h" // the plushie ragdoll body
#include "ue_wrap/engine/engine_save.h"          // save slots and travel
#include "ue_wrap/engine/engine_widget.h"        // runtime UMG

#include <string>
#include <vector>

namespace ue_wrap::engine {

// UKismetSystemLibrary::ExecuteConsoleCommand; the CDO, the UFunction and a world context resolve
// on first use. False on a resolution miss. Game thread.
bool ExecuteConsoleCommand(const wchar_t* command);

// UGameplayStatics::IsGamePaused / SetGamePaused on the cached world context; a paused peer stops
// ticking its world, so a session keeps every peer unpaused. IsGamePaused is false on a miss. Game
// thread.
bool IsGamePaused();

bool SetGamePaused(bool paused);

// UGameplayStatics::GetTimeSeconds on the cached world context: the world's own clock in seconds, which
// stops while the game is paused and runs at the world's time dilation. False on a resolution miss or a
// failed call. Game thread.
bool TryGetWorldTimeSeconds(float& seconds);

// Spawn `actorClass` at `location` through the deferred GameplayStatics pair (AlwaysSpawn); the
// AActor* or nullptr. `inertPawn` zeroes AutoPossessPlayer, AutoPossessAI and AutoReceiveInput and
// sets bBlockInput in the deferred window, so the pawn never takes a PlayerController or the local
// input. `beforeFinish`, when set, runs once on the deferred actor after those writes and before
// FinishSpawning, so its BeginPlay and the overlaps its colliders report as they register already see
// what the callback set; it is not called on a path that returns null before the actor exists. `ctx` is
// the caller's and need live only for the call; the callback runs on the game thread inside
// SpawnActor and must not re-enter it. A failed FinishSpawning call destroys the begun actor and
// returns nullptr (the callback has run by then). Game thread.
using BeforeFinishFn = void (*)(void* ctx, void* actor);
void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn = false,
                 BeforeFinishFn beforeFinish = nullptr, void* ctx = nullptr);

// The split form: BeginDeferredSpawn, write class-specific fields before Init and BeginPlay,
// FinishDeferredSpawn with the same transform (the engine re-applies it). A wire-spawned entity
// gets its variant state stamped in the window. Game thread.
void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation);

bool  FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation);

// AActor::K2_GetActorLocation, the one location read: false when the location could not be
// obtained, a read that faulted inside the engine and was absorbed included
// (reflection::CallFunction reports it). `out` is then the world origin, an ordinary position, so
// every caller decides what an unread location means. Game thread.
bool TryGetActorLocation(void* actor, FVector& out);

// AActor::GetActorScale3D (root world scale); unit scale on failure, since callers stamp it into
// spawn transforms. Game thread.
FVector GetActorScale3D(void* actor);

// True iff `actor` was spawned by a UChildActorComponent (a kerfur's eye camera, a console screen
// child): the parent's construction script spawns and destroys these on every peer (Aprop_C's
// ignoreSave = ignoreSav || IsChildActor()), so they carry no cross-peer identity. A raw read of
// the reflected ParentComponent weak pointer, safe off the game thread; a set pointer means child
// actor even mid-teardown.
bool IsChildActor(void* actor);

// The same read, saying whether it could be made: `readable` is false when the Actor class is not loaded yet or
// its ParentComponent member is missing (the answer is then false, which is no evidence the actor is not a
// child). A reader that must fail closed uses this form.
bool IsChildActor(void* actor, bool& readable);

// The child actor's parent (the ParentComponent's Outer) and, in `outComponentName`, the
// UChildActorComponent's name: the half of a child actor's identity that is stable across processes
// (the `_CAT_<n>` tail of the actor's own name is a per-process counter). Null when not a child
// actor or the parent is gone. Raw reads; safe off the game thread.
void* ParentActorOf(void* actor, std::wstring* outComponentName = nullptr);

// AActor::SetActorScale3D (root relative scale); the trash proxy applies the host's per-form scale
// on every convert and spawn. Game thread.
bool SetActorScale3D(void* actor, const FVector& scale);

// AActor::SetLifeSpan: the actor destroys itself `seconds` from now, by the engine's own lifespan expiry (the end an
// InitialLifeSpan default gives). False on a resolution miss or a failed call. Game thread.
bool SetActorLifeSpan(void* actor, float seconds);

// UKismetSystemLibrary::CollectGarbage: a full purge at the end of the frame, paired with the
// adoption sweep's destruction burst so pending-kill garbage does not wait for UE's 61 s periodic
// purge. Game thread.
bool ForceGarbageCollection();

// AActor::GetActorForwardVector; (0,0,0) on failure.
FVector GetActorForwardVector(void* actor);

// AActor::GetActorUpVector; (0,0,0) on failure (a unit vector is never zero, so a zero is an unread one).
FVector GetActorUpVector(void* actor);

// AActor::K2_GetActorRotation (world), the one rotation read: false when the rotation could not be
// obtained, a read that faulted inside the engine and was absorbed included. `out` is then the zero
// rotator, an ordinary facing, so every caller decides what an unread rotation means. Game thread.
bool TryGetActorRotation(void* actor, FRotator& out);

// AActor::GetVelocity (world, cm/s); zero on failure. Its horizontal magnitude is the remote
// locomotion blend's walk speed. Game thread.
FVector GetActorVelocity(void* actor);


// AActor::K2_SetActorLocation (bSweep=false, bTeleport=true: a snap). Game thread.
bool SetActorLocation(void* actor, const FVector& location);

// AActor::K2_SetActorRotation (bTeleportPhysics=true). Game thread.
bool SetActorRotation(void* actor, const FRotator& rotation);

// USceneComponent::K2_GetComponentRotation (world); zero on failure. Game thread.
FRotator GetComponentWorldRotation(void* component);

// The same read, false (and `out` untouched) when the component is dead, the function unresolved or the call failed:
// for a reader that must tell a failed read from a zero rotation. Game thread.
bool TryGetComponentWorldRotation(void* component, FRotator& out);

// USceneComponent::K2_SetWorldRotation (bSweep=false, bTeleport=true). Call after SetActorRotation:
// moving the root re-bases the child's world transform. Game thread.
bool SetComponentWorldRotation(void* component, const FRotator& rotation);

// AActor::SetActorTickEnabled; a remote pawn must not run the local player's per-frame BP
// EventTick, which re-applies view and post-process to the shared screen. Game thread.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorHiddenInGame: visual only. Collision is a separate flag with its own setter
// below. Game thread.
bool SetActorHiddenInGame(void* actor, bool hidden);

// AActor::SetActorEnableCollision: the whole actor's collision flag, query and physics. A mirror that
// must be neither grabbable nor physics-active while hidden takes it (mirror_defer's Off mode); one that
// must still land on the floor keeps its physics through SetActorRootCollisionEnabled. Game thread.
bool SetActorEnableCollision(void* actor, bool enabled);

// AActor::CustomTimeDilation, the actor's own multiplier on its tick delta (the world's global dilation
// still applies on top). A native property with no setter function: the offset is resolved from the
// actor's class and kept per class, and the float is stored raw. False, and one warning per class,
// when the property does not resolve. `actor` is live in this game-thread task. Game thread.
bool SetCustomTimeDilation(void* actor, float value);

// K2_TeleportTo: the engine's own long-range teleport; K2_SetActorLocation can be silently reverted
// by Character constraints when the move is far. Game thread.
bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation);

// GetActorBounds: the world-space AABB from the rendered mesh, not the bone hierarchy (the puppet's
// lowest visible point is Origin.Z - BoxExtent.Z). Game thread.
bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent);

// The sphere a reach is measured to: the centre of the actor's colliding bounds and half their
// diagonal, or its location with no radius when it has no colliding component. False when neither
// can be read, never a point at the world origin. Game thread.
bool ActorReachSphere(void* actor, FVector& outCenter, float& outRadiusUU);

// APlayerController::ProjectWorldLocationToScreen: a world point to viewport pixels through the
// local camera; false, `outScreen` untouched, behind the camera. The nameplates project here and
// snapshot the result for the render thread. Game thread.
bool ProjectWorldToScreen(void* playerController, const FVector& world,
                          FVector2D& outScreen, bool viewportRelative = false);

// Direct write of a UObject* slot at `byteOffset` inside `target`. Game thread.
void WriteObjectField(void* target, size_t byteOffset, void* value);

// Diagnostic: log every FProperty (name, offset, size) of a UClass, its own properties only. Game
// thread.
void LogClassProperties(const wchar_t* className);

// ACharacter's capsule half-height (UCapsuleComponent::CapsuleHalfHeight); 0.f if none. Game
// thread.
float GetActorCharacterHalfHeight(void* mainPlayerPawn);

// A long-lived WorldContextObject for the deferred-spawn pair: the GameInstance, else the World.
// One lookup when cached; the objects are the world singleton's. Game thread.
void* GetWorldContext();

// FRotator to FQuat, UE4.27's own formula (ZYX, left-handed): the body of FRotator::Quaternion() in
// Runtime/Core/Public/Math/Rotator.h. The negative Y term is UE4's convention, not a defect; a
// right-handed reference shows the opposite signs.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw);

// The inverse, FQuat to FRotator in UE4.27's convention (the pitch singularity handled as the
// engine does, at +-90 degrees). The quaternion is taken as a unit one.
FRotator QuatToRotator(float qx, float qy, float qz, float qw);

// The angle between two orientations, degrees, 0 to 180: 2*acos(|dot|) of their quaternions, computed in double
// on quaternions re-normalised there, so two reads of one orientation give 0 and not the float rounding of a
// near-1 dot (about 0.05 degrees). Rotators near pitch +-90 degrees are not unique per orientation, so this is
// the comparison to use, never a difference of pitch, yaw and roll.
float RotatorAngleDeg(const FRotator& a, const FRotator& b);

}  // namespace ue_wrap::engine
