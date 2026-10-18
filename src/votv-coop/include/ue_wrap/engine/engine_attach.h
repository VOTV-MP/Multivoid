// ue_wrap/engine/engine_attach.h -- attaching and detaching actors, and the root-body physics that
// follows from it. Engine-wrapper layer (principle 7): each call marshals one UFunction call or
// one reflected field access, with no gameplay, network or coop state. Game thread unless a
// declaration says otherwise. Implementation: src/ue_wrap/engine/engine_attach.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::engine {

// Attach `actor` to `component` at a socket, SnapToTarget (the Killer Wisp grab-hold);
// DetachActorFromParent reverses it. Game thread.
bool AttachActorToComponentSocket(void* actor, void* component, const wchar_t* socket);

bool DetachActorFromParent(void* actor);

// AActor::GetAttachParentActor: the actor `actor`'s root is attached to, or null in `parent` when it is attached to
// none. False when the read did not run (`parent` is null then too). Game thread.
bool GetAttachParentActor(void* actor, void*& parent);

// A Character attached to an actor: the Character, its CharacterMovementComponent, and the component that
// movement moves (UpdatedComponent, its capsule). A field that does not read is null.
struct AttachedCharacter {
    void* character = nullptr;
    void* movement = nullptr;
    void* updated = nullptr;
};

// Up to `max` Characters attached to `actor`, into `out`; returns how many. Found as AActor::GetAttachedActors
// finds attached actors: down the AttachChildren of `actor`'s own components from its root, where a child
// another actor owns names that actor, whose own attachments are not walked. Game thread.
int AttachedCharactersOf(void* actor, AttachedCharacter* out, int max);

// ---- Generic actor root-physics substrate ----
// The root component through K2_GetRootComponent, never the Aprop_C mesh offset, so they work on the
// non-Aprop_C trash clump. Each body call answers only a live primitive root and refuses any other (false,
// zero, null), as engine_physics does; SetActorRootMovable takes any root. Game thread.

// SetSimulatePhysics on the root primitive: freeze a held mirror, thaw it on release.
bool SetActorSimulatePhysics(void* actor, bool simulate);

// Force the root component Movable: a Static root silently ignores SetActorLocation (the call still
// returns true), and a save-loaded chipPile rests at Static.
bool SetActorRootMovable(void* actor);

// SetCollisionEnabled on the root (0 None, 1 QueryOnly, 2 PhysicsOnly, 3 QueryAndPhysics); a thrown
// mirror needs 3 to land.
bool SetActorRootCollisionEnabled(void* actor, uint8_t collisionType);

// SetNotifyRigidBodyCollision on the root; false keeps the collision but silences the
// OnComponentHit BP events (a mirror clump's landing handler would spawn a second pile).
bool SetActorRootNotifyRigidBodyCollision(void* actor, bool notify);

// The root's linear (cm/s) and angular (deg/s) velocity: the throw-energy transfer on release.
bool GetActorRootPhysicsVelocity(void* actor, FVector& outLin, FVector& outAng);

bool SetActorRootPhysicsVelocity(void* actor, const FVector& lin, const FVector& ang);

// Angular only, for a turning author: writing the linear component of a resting rig wakes it and it
// sinks.
bool SetActorRootPhysicsAngularVelocity(void* actor, const FVector& ang);

// The root's mass in kg (GetMass); 0 on failure. The native throw speed scales as
// 15000/max(mass,10).
float GetActorRootMass(void* actor);

// The root surface's UPhysicalMaterial (GetMaterial(0), then GetPhysicalMaterial()), the input
// lib_C::physSound takes; equivalent to the trace's answer for the single-material clump and pile.
// Null on failure.
void* GetActorRootPhysicalMaterial(void* actor);

// True only when the root body is positively confirmed asleep or not simulating; false on any
// failure. The host stamps kAtRest from it.
bool IsActorRootBodyAtRest(void* actor);

}  // namespace ue_wrap::engine
