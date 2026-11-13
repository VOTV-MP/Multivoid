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

#include "ue_wrap/core/reflection.h"  // FName (ModuleByName)
#include "ue_wrap/core/types.h"  // FVector, FRotator

#include <cstddef>
#include <cstdint>
#include <string>

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

// The seated driver's controls on this copy: the lights (`lights`) and the turbo (`turbo`), as the brake above. False on
// null, an unresolved ATV_C or a class without the property. Game thread.
bool  GetLights(void* atv);
bool  GetTurbo(void* atv);

// The ATV's own bodies for those three on a copy nobody drives, each a write of the field and then the game's function
// that acts on it: `lights` then `Upd Lights` (which clears it again on a flat battery), `Brake` then `setBrake`, and the
// turbo as the key sets it (`turbo` true, `Speed` := `speed_turbo`) or `unturbo` for off. False, and nothing written, on
// null, an unresolved field or a function that did not dispatch. Game thread.
bool  SetLights(void* atv, bool on);
bool  SetBrake(void* atv, bool on);
bool  SetTurbo(void* atv, bool on);

// The ATV's vitals as the game keeps them: `fuel` and `health` (floats), and the `empty` and `brokenn` marks (bool bits).
// The writers write the field alone: the game's own `fuelUp` and `toolboxFix` also clear the mark, so a caller that
// mirrors them clears it itself, and `CallUpdHealth` is the game's repaint of a health written (smoke, the -1 floor).
// False on null, an unresolved ATV_C or a class without the property (said once at the resolve). Game thread.
bool  SetFuel(void* atv, float v);
bool  SetHealth(void* atv, float v);
bool  GetEmpty(void* atv);
bool  SetEmpty(void* atv, bool v);
bool  GetBroken(void* atv);
bool  SetBroken(void* atv, bool v);
bool  CallUpdHealth(void* atv);

// The fitted modules: the live `Modules` array, one byte per module VALUE (the game's enum_physicalModules; each is the
// value `physModToActor` maps to the module's prop class). ReadModules copies at most `cap` into `out` and returns how
// many the ATV holds (it may exceed `cap`), or -1 when unreadable. WriteModules builds the array afresh in engine memory
// (the old buffer is left to the engine's own lifetime, as the save-record writers do) and then runs the ATV's own
// `updUpgrades`, which fits or takes off every part to match. ModuleByName maps a module's prop name, the FName
// `upgradeTake` is called with, to its value through the ATV's own `upgradesNames`, which parallels `Modules`; false when
// the ATV holds no part of that name. Game thread.
int   ReadModules(void* atv, uint8_t* out, int cap);
bool  WriteModules(void* atv, const uint8_t* mods, int count);
bool  ModuleByName(void* atv, const reflection::FName& name, uint8_t& value);

// The ATV's own horn on this copy: the honk key's press or release event, which also writes `input_alt` and, with the Fly
// module fitted, Fly's `fly` flag (measured on a copy nobody drives). Both are put back as they were in the same frame, so
// the replay sounds the horn and leaves the copy's own controls alone. False when the event did not dispatch. Game thread.
bool  ReplayHorn(void* atv, bool press);

// The ATV's own `checkIfRunout`: it calls `runout` when the tank or the health is down. The guard after fuel or health
// is lowered from outside the game's own tick. False on null, an unresolved function or a call that did not dispatch.
// Game thread.
bool  CheckIfRunout(void* atv);

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
    ClassNotFound,   // no class of that name is loaded or loadable
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

}  // namespace ue_wrap::atv
