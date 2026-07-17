// ue_wrap/atv.h -- standalone engine access for the VOTV ATV/quadbike (AATV_C).
// Principle-7 engine-wrapper layer (NO network/coop state). coop::atv_sync drives the
// kinematic pose-stream sync through here.
//
// AATV_C : APawn is a CUSTOM physics rig (NOT a UWheeledVehicleMovementComponent vehicle):
// the root Mesh@0x0570 simulates PhysX, the wheels are constraint-driven. There is no movement
// component to feed input into on a remote -- the body transform is the only ground truth -> we
// kinematic POSE-STREAM it (like the kerfur/NPC pose stream + held-prop PropPose), NOT input-
// replication. On the mirror the local physics + tick are disabled (PrepareMirror) so the ATV's
// own applyWheelTorque/gravity can't fight the streamed transform (the clump/NPC-mirror
// discipline). Identity = the save-placed Key@0x0618 (cross-peer stable).
//
// Mesh@0x0570 IS the actor ROOT component, so the actor transform == the physics body transform
// -- we read/drive at the ACTOR level (GetActorLocation/Rotation/Velocity, SetActorLocation/
// Rotation), reusing ue_wrap::engine; no component-transform plumbing needed.
//
// RE: research/findings/vehicles/votv-ATV-quadbike-RE-and-coop-sync-design-2026-06-08.md +
//     votv-ATV-Phase1-pose-stream-blueprint-2026-06-08.md.

#pragma once

#include "ue_wrap/core/types.h"  // FVector, FRotator

#include <string>

namespace ue_wrap::atv {

// Resolve AATV_C + the field offsets. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is ATV_C or a subclass. False if not yet resolved.
bool IsAtv(void* obj);

// The ATV's save-persistent Key@0x0618 as a wide string ("" on failure, L"None" if unkeyed).
std::wstring GetKeyString(void* atv);

// Root body world transform (loc + FULL rotation -- the ATV tips/flips). The pose-stream read.
// False on null/unresolved.
bool GetRootTransform(void* atv, FVector& loc, FRotator& rot);

// Root body linear velocity (cm/s). The mirror inherits it on a physics re-enable (release).
bool GetRootVelocity(void* atv, FVector& vel);

// The current driver AmainPlayer_C* (Player@0x05B0), or nullptr if unoccupied. The COOP layer
// (atv_sync) resolves this raw pointer to a peer slot -- ue_wrap owns no coop state (principle 7).
void* GetOccupantPlayer(void* atv);

// isDriven@0x05F7 -- TRUE while a player is seated (the master "occupied" flag).
bool IsDriven(void* atv);

// Phase-2 display state (cheap field reads; exposed now for the state payload). 0/false on failure.
float GetFuel(void* atv);    // fuel@0x05D4   (0..100)
float GetHealth(void* atv);  // health@0x05E4 (0..100)
bool  GetBrake(void* atv);   // Brake@0x05D9  (handbrake)

// CLIENT mirror: snap the root body to the streamed transform (kinematic -- physics is off via
// PrepareMirror). SetActorLocation(teleport) + SetActorRotation(teleportPhysics). False on failure.
bool DriveMirrorTransform(void* atv, const FVector& loc, const FRotator& rot);

// CLIENT mirror prep (call once when the mirror is first driven): disable the local physics sim +
// actor tick + root rigid-body-collision notify so the ATV's own rig can't fight the streamed
// transform (the same discipline the clump/NPC mirrors use). Game thread.
void PrepareMirror(void* atv);

// The inverse of PrepareMirror: re-enable physics sim + actor tick + rigid-body notify. Called
// when a peer that was mirroring an ATV BECOMES its authority (driver/grabber, so its own rig runs
// again), when an authority releases (the ATV un-freezes to an idle physics-on grabbable state),
// and on disconnect so a streamed ATV is not left frozen in single-player. Game thread.
void ReleaseMirror(void* atv);

// v77 purchased-ATV materialization: fresh-spawn an AATV_C-or-subclass (`className`) at `loc`/`rot`
// via GameplayStatics BeginDeferred + FinishSpawning, leaving physics ON -- the result is a NATIVE
// idle ATV the local player can grab/drive (NOT a frozen mirror). coop::atv_sync uses this when the
// host announces a purchased ATV (AtvSpawn) that this client has no save-twin of (the order economy
// delivers only on the host). `className` is validated to descend from ATV_C (trust boundary -- a
// peer could send any string). Returns the spawned actor, or nullptr on resolve/spawn failure.
// Game thread only.
void* SpawnMirror(const std::wstring& className, const FVector& loc, const FRotator& rot);

// v77: tear down a SpawnMirror'd purchased-ATV mirror (K2_DestroyActor on the actor) when the host
// announces it gone (AtvDestroy) or on disconnect. No-op-safe on null/already-dead. Game thread.
void DestroyMirror(void* atv);

}  // namespace ue_wrap::atv
