// ue_wrap/devices/atv.h -- standalone engine access for the VOTV ATV/quadbike (AATV_C).
// Principle-7 engine-wrapper layer (NO network/coop state); coop::atv_sync drives the sync.
//
// AATV_C : APawn is a CUSTOM physics rig (NOT a UWheeledVehicleMovementComponent vehicle):
// the root Mesh simulates PhysX, the four wheels are SEPARATE simulating bodies held by
// UPhysicsConstraintComponents (sus_*/ax_*). Identity = the save-placed Key (cross-peer
// stable). Mesh IS the actor ROOT component, so the actor transform == the physics body
// transform: we read/drive at the ACTOR level, reusing ue_wrap::engine.
//
// A non-authoring peer runs the rig NATIVELY -- physics ON, tick ON -- and is CORRECTED toward
// the authority: the sync layer changes NOTHING about the actor itself, because the rig's entire
// visible output IS suspension travel and a kinematic root teleported 20x/s drags four
// constrained bodies behind it. What a mirror may not do is author COLLISION damage, cancelled
// at the seven UFunctions below. The model: docs/vehicles.md. MTA: CClientVehicle::Update-
// TargetPosition + CNetAPI::ReadVehiclePuresync.

#pragma once

#include "ue_wrap/core/types.h"  // FVector, FRotator

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::atv {

// Resolve AATV_C + the field offsets. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is ATV_C or a subclass. False while ATV_C is not loaded, or still loading.
// ATV_C loads with the main menu and stays one object for the process (measured across a rejoin),
// so the object scan hub's one judgement per class holds. Game thread.
bool IsAtv(void* obj);

// Every live ATV in the lineage (ATV_C and each subclass with an instance, the witch car included), one call per
// actor with its object-array slot. Skips the class default objects and the objects a load has not handed to the
// game thread yet or the engine is destroying; the caller still asks its own world and key questions. Walks the
// object index, never the object array. The callback must not create or destroy objects. Returns how many instances
// it skipped as not yet handed to the game thread (an ATV still loading; one the engine is destroying is not
// counted, it is leaving). Game thread.
size_t ForEachAtv(void (*fn)(void* ctx, void* actor, int32_t idx), void* ctx);

// The ATV's save-persistent Key as a wide string ("" on failure, L"None" if unkeyed).
std::wstring GetKeyString(void* atv);

// Root body world transform (loc + FULL rotation -- the ATV tips/flips). The pose-stream read.
// False on null/unresolved or an unreadable location or rotation.
bool GetRootTransform(void* atv, FVector& loc, FRotator& rot);

// The current driver AmainPlayer_C* (the Player field), or nullptr if unoccupied. The COOP layer
// (atv_sync) resolves this raw pointer to a peer slot -- ue_wrap owns no coop state (principle 7).
void* GetOccupantPlayer(void* atv);

// isDriven -- TRUE while a player is seated (the master "occupied" flag).
bool IsDriven(void* atv);

// Display state (cheap field reads, resolved by name). 0 or false on failure.
float GetFuel(void* atv);    // fuel, 0..100
float GetHealth(void* atv);  // health, 0..100
bool  GetBrake(void* atv);   // the handbrake

// The story event's flag: while it is set the ATV's own tick explodes it (health to 0, then `explode`) once its driver
// has moved. Resolved by name with no fallback; both return false, and touch nothing, on null, an unresolved ATV_C or a
// class without the property. Game thread.
bool ReadTrap(void* atv, bool& out);
bool WriteTrap(void* atv, bool v);

// Where the ATV's own `explode` places its blast: the world location of its `tp` billboard component. Resolved by name
// with no fallback; false, with `out` the origin, on null, an unresolved ATV_C or `tp`, a null component or a call
// that does not dispatch. Game thread.
bool ReadTpLocation(void* atv, FVector& out);

// The game's OWN rig-consistent teleport: ATV_C::teleportVehicle(NewLocation, NewRotation) --
// K2_SetActorLocation(bTeleport=true) + K2_SetActorRotation(bTeleportPhysics=true), and THEN it
// re-places frontWheel_R onto ax_FR1, frontWheel_L onto ax_FL1 and backWheelRoot onto `back`
// [V, disasm ATV.teleportVehicle @0..393]. This is the WARP primitive and the only transform the
// sync layer ever writes: a bare SetActorLocation moves the ROOT only
// (engine_attach.cpp:74-82) and leaves the constrained wheel bodies behind -- which is exactly
// why the game ships two teleport forms. False on unresolved/null. Game thread.
bool TeleportRig(void* atv, const FVector& loc, const FRotator& rot);

// The step at which SpawnMirror failed. SpawnMirror reports it and logs nothing for it: the caller
// (coop) says it once per key, since a replay asks again.
enum class SpawnFail : uint8_t {
    None,
    Unresolved,      // the ATV wrapper, the spawn functions or the world context are not available
    NoKey,           // the class name or the key is empty
    ClassNotFound,   // no class of that name is loaded
    NotAnAtv,        // the class does not descend from ATV_C
    NoOffset,        // the class has no key property
    NameNone,        // the key did not convert to a name
    BeginFailed,     // BeginDeferredActorSpawnFromClass did not dispatch or returned null
    FinishFailed,    // FinishSpawningActor could not be built or did not dispatch
    KeyMismatch,     // the finished actor stands under another key; it was destroyed
};

// The failed step as a short phrase for a log line.
const char* SpawnFailText(SpawnFail why);

// ATV mirror materialization: fresh-spawn an AATV_C-or-subclass (`className`) carrying `key` at
// `loc`/`rot` via GameplayStatics BeginDeferred + FinishSpawning, leaving physics ON -- the result is
// a NATIVE idle ATV the local player can grab/drive (NOT a frozen mirror). The key is written before
// the finish, so the actor is born with the host's Key and never mints one of its own. coop::atv_sync
// uses this when the host announces an ATV (AtvSpawn) that this peer does not hold.
// `className` is validated to descend from ATV_C (trust boundary -- a peer could send any string).
// Returns the spawned actor, or nullptr on resolve/spawn failure; nullptr WITHOUT spawning anything
// when `key` is empty, the class has no key property, or the key does not convert to a name (the
// string library not yet resolved included: ask fname_utils::Resolved() first to tell "not yet"). The
// key is read back after the finish, exactly (the index and the prune file key case-sensitively): a
// mirror born under another key is destroyed and nullptr returned. `why` (optional) names the failed
// step; `bornKey` (optional) receives the key the read-back found on a KeyMismatch. Game thread only.
void* SpawnMirror(const std::wstring& className, const FVector& loc, const FRotator& rot,
                  const std::wstring& key, SpawnFail* why = nullptr, std::wstring* bornKey = nullptr);

// Tear down an ATV -- K2_DestroyActor on the actor -- a SpawnMirror'd one or a native twin, when the
// host announces it gone (AtvDestroy), or a mirror on disconnect. False, and nothing done, on a null or
// already-dead actor or a destroy that did not dispatch. Game thread.
bool DestroyMirror(void* atv);

// The ATV's own `dismount()`: the seated player is handed back (re-possessed, shown) and `isDriven` clears. False,
// and nothing done, on a null or dead actor, an unresolved function or a call that did not dispatch. Game thread.
bool Dismount(void* atv);

// Where a seated driver's body goes: behind the handlebars, 100 cm ahead of the driver's `Camera` along the ATV's
// facing and 25 cm above `sitarrow` (where a riding kerfur's pivot goes, its AnimBP's seat pose built for it), so it
// rides on the seat; turned as `sitarrow` is. The ATV's own sit puts its player at
// `playerHit` instead, 140 cm higher, where only its hidden capsule stands. False on null, an unresolved ATV_C or
// component, or an unreadable transform. Game thread.
bool GetSeatTransform(void* atv, FVector& loc, FRotator& rot);

// The ATV whose root is nearest `at` within `reachCm`, or nullptr. Game thread.
void* NearestAtv(const FVector& at, float reachCm);

// The ATV's rider (`sittingKerfuro`) cleared when it is `kerfur`: a kerfur's own getOffCar leaves it set, and the ATV's
// dismount is what clears it. Game thread.
void ForgetSittingKerfur(void* atv, void* kerfur);

// What the ATV's own sit and dismount do for the kerfurs, run on this copy with no player of its own in the seat.
// InviteKerfurs puts `kerfurs` in `allKerfuros` and asks each to come (its own tryToOccupyCar): the first to reach the
// ATV sits and turns the rest back. ReleaseKerfurs gets the rider off (its own getOffCar), turns back the rest
// (failCar) and empties both. False when a field does not resolve. Game thread.
bool InviteKerfurs(void* atv, const std::vector<void*>& kerfurs);
bool ReleaseKerfurs(void* atv);

}  // namespace ue_wrap::atv
