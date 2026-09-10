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

// Spawn `actorClass` at `location` through the deferred GameplayStatics pair (AlwaysSpawn); the
// AActor* or nullptr. `inertPawn` zeroes AutoPossessPlayer, AutoPossessAI and AutoReceiveInput and
// sets bBlockInput in the deferred window, so the pawn never takes a PlayerController or the local
// input. Game thread.
void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn = false);

// The split form: BeginDeferredSpawn, write class-specific fields before Init and BeginPlay,
// FinishDeferredSpawn with the same transform (the engine re-applies it). A wire-spawned entity
// gets its variant state stamped in the window. Game thread.
void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation);

bool  FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation);

// Self-test of the world-context staleness guard: corrupts the cached GUObjectArray index, runs
// EnsureWorldContext and checks it re-resolved a live context. Behind an autotest env flag. Game
// thread.
bool DebugCheckWorldContextRecovery();

// AActor::K2_GetActorLocation; (0,0,0) if it cannot be called.
FVector GetActorLocation(void* actor);

// The checked read: false when the location could not be obtained. Use this wherever a wrong answer
// grants something: GetActorLocation returns (0,0,0), the world origin, on every failure. Game
// thread.
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

// The child actor's parent (the ParentComponent's Outer) and, in `outComponentName`, the
// UChildActorComponent's name: the half of a child actor's identity that is stable across processes
// (the `_CAT_<n>` tail of the actor's own name is a per-process counter). Null when not a child
// actor or the parent is gone. Raw reads; safe off the game thread.
void* ParentActorOf(void* actor, std::wstring* outComponentName = nullptr);

// AActor::SetActorScale3D (root relative scale); the trash proxy applies the host's per-form scale
// on every convert and spawn. Game thread.
bool SetActorScale3D(void* actor, const FVector& scale);

// UKismetSystemLibrary::CollectGarbage: a full purge at the end of the frame, paired with the
// adoption sweep's destruction burst so pending-kill garbage does not wait for UE's 61 s periodic
// purge. Game thread.
bool ForceGarbageCollection();

// AActor::GetActorForwardVector; (0,0,0) on failure.
FVector GetActorForwardVector(void* actor);

// AActor::K2_GetActorRotation (world); zero on failure. Game thread.
FRotator GetActorRotation(void* actor);

// AActor::GetVelocity (world, cm/s); zero on failure. Its horizontal magnitude is the remote
// locomotion blend's walk speed. Game thread.
FVector GetActorVelocity(void* actor);

// AActor::K2_SetActorLocation (bSweep=false, bTeleport=true: a snap). Game thread.
bool SetActorLocation(void* actor, const FVector& location);

// AActor::K2_SetActorRotation (bTeleportPhysics=true). Game thread.
bool SetActorRotation(void* actor, const FRotator& rotation);

// USceneComponent::K2_GetComponentRotation (world); zero on failure. Game thread.
FRotator GetComponentWorldRotation(void* component);

// USceneComponent::K2_SetWorldRotation (bSweep=false, bTeleport=true). Call after SetActorRotation:
// moving the root re-bases the child's world transform. Game thread.
bool SetComponentWorldRotation(void* component, const FRotator& rotation);

// AActor::SetActorTickEnabled; a remote pawn must not run the local player's per-frame BP
// EventTick, which re-applies view and post-process to the shared screen. Game thread.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorHiddenInGame: visual only. Collision is a separate flag with its own setter
// below. Game thread.
bool SetActorHiddenInGame(void* actor, bool hidden);

// AActor::SetActorEnableCollision; pair it with a hide so an invisible mirror is neither grabbable
// nor physics-active. Game thread.
bool SetActorEnableCollision(void* actor, bool enabled);

// K2_TeleportTo: the engine's own long-range teleport; K2_SetActorLocation can be silently reverted
// by Character constraints when the move is far. Game thread.
bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation);

// GetActorBounds: the world-space AABB from the rendered mesh, not the bone hierarchy (the puppet's
// lowest visible point is Origin.Z - BoxExtent.Z). Game thread.
bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent);

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
void* GetWorldContext();

// FRotator to FQuat, UE4.27's own formula (ZYX, left-handed): the body of FRotator::Quaternion() in
// Runtime/Core/Public/Math/Rotator.h. The negative Y term is UE4's convention, not a defect; a
// right-handed reference shows the opposite signs.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw);

}  // namespace ue_wrap::engine
