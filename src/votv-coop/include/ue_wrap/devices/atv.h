// ue_wrap/atv.h -- standalone engine access for the VOTV ATV/quadbike (AATV_C).
// Principle-7 engine-wrapper layer (NO network/coop state). coop::atv_sync drives the
// kinematic pose-stream sync through here.
//
// AATV_C : APawn is a CUSTOM physics rig (NOT a UWheeledVehicleMovementComponent vehicle):
// the root Mesh@0x0570 simulates PhysX, the four wheels are SEPARATE simulating bodies held by
// UPhysicsConstraintComponents (sus_*/ax_*). Identity = the save-placed Key@0x0618 (cross-peer
// stable).
//
// MIRROR MODEL (arc 1, 2026-08-29 -- REPLACES the freeze/teleport model this file used to
// describe). A non-authoring peer runs the rig NATIVELY -- physics ON, tick ON -- and is CORRECTED
// toward the authority. The sync layer changes NOTHING about the actor itself; what a mirror may
// not do is author COLLISION damage, and that is cancelled at the seven UFunctions below rather
// than by parking anything. That inversion is not a preference: the rig's
// entire visible output IS suspension travel, and a kinematic root teleported 20x/s drags four
// constrained bodies behind it. Measured native travel is 2-4 cm (docs/vehicles/ATV.md 13); the
// shipped freeze model put the other peer's copy at 29.58 cm and 1.1 m away. So: no
// PrepareMirror/ReleaseMirror, no SetActorSimulatePhysics from the sync layer at all.
// MTA precedent: CClientVehicle::UpdateTargetPosition + CNetAPI::ReadVehiclePuresync.
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

// The current driver AmainPlayer_C* (Player@0x05B0), or nullptr if unoccupied. The COOP layer
// (atv_sync) resolves this raw pointer to a peer slot -- ue_wrap owns no coop state (principle 7).
void* GetOccupantPlayer(void* atv);

// isDriven@0x05F7 -- TRUE while a player is seated (the master "occupied" flag).
bool IsDriven(void* atv);

// Phase-2 display state (cheap field reads; exposed now for the state payload). 0/false on failure.
float GetFuel(void* atv);    // fuel@0x05D4   (0..100)
float GetHealth(void* atv);  // health@0x05E4 (0..100)
bool  GetBrake(void* atv);   // Brake@0x05D9  (handbrake)

// The game's OWN rig-consistent teleport: ATV_C::teleportVehicle(NewLocation, NewRotation) --
// K2_SetActorLocation(bTeleport=true) + K2_SetActorRotation(bTeleportPhysics=true), and THEN it
// re-places frontWheel_R onto ax_FR1, frontWheel_L onto ax_FL1 and backWheelRoot onto `back`
// [V, disasm ATV.teleportVehicle @0..393]. This is the WARP primitive and the only transform the
// sync layer ever writes: a bare SetActorLocation moves the ROOT only
// (engine_attach.cpp:74-82) and leaves the constrained wheel bodies behind -- which is exactly
// why the game ships two teleport forms. False on unresolved/null. Game thread.
bool TeleportRig(void* atv, const FVector& loc, const FRotator& rot);

// Resolve ATV_C's SEVEN ComponentHitSignature bound-event UFunctions (`mesh`, `car1_Capsule`,
// `car1_backWheel_R/L`, `car1_frontWheel_R`, `car1_frontWheelRoot`, `car1_backWheelRoot`) into
// `out`; returns how many resolved. These are the ONLY PE-visible seam for the ATV's
// collision-authored state: each BndEvt stub just copies its params and jumps into the ubergraph
// [V, disasm], and the work it reaches -- impulse() -> `health -= |NormalImpulse|/500000 * 2 *
// getBumperMult()` -> explode() at <=0, and processTire() -> tire durability -> ejectWheel -- is
// all EX_Local* and therefore invisible one level down. A non-owner that runs them explodes its
// own copy of a vehicle the authority still has. Game thread.
int ResolveHitDelegates(void** out, int max);

// v77 runtime-ATV materialization: fresh-spawn an AATV_C-or-subclass (`className`) at `loc`/`rot`
// via GameplayStatics BeginDeferred + FinishSpawning, leaving physics ON -- the result is a NATIVE
// idle ATV the local player can grab/drive (NOT a frozen mirror). coop::atv_sync uses this when the
// host announces a RUNTIME-SPAWNED ATV (AtvSpawn) that this client has no save-twin of. (Premise
// corrected 2026-08-29: the old text said "purchased ... the order economy delivers only on the
// host"; nothing sells an ATV. The real source is list_props row 'atv' -> spawnAsObject = ATV_C,
// reached from ui_spawnmenu -- docs/vehicles/ATV.md 11.4.) `className` is validated to descend from ATV_C (trust boundary -- a
// peer could send any string). Returns the spawned actor, or nullptr on resolve/spawn failure.
// Game thread only.
void* SpawnMirror(const std::wstring& className, const FVector& loc, const FRotator& rot);

// v77: tear down a SpawnMirror'd runtime-ATV mirror (K2_DestroyActor on the actor) when the host
// announces it gone (AtvDestroy) or on disconnect. No-op-safe on null/already-dead. Game thread.
void DestroyMirror(void* atv);

}  // namespace ue_wrap::atv
