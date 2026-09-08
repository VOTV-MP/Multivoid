// coop/dev/spawn_order_probe.h -- the spawn-order correlation probe (read-only, dev only).
//
// THE QUESTION: can a client bind each save-loaded KEYLESS native (a chipPile, an off-prop
// kerfur) to the host's index->eid map entry by SPAWN ORDER -- the client's k-th keyless spawn
// being the host's k-th? Both peers load the same blob, and the game's loadObjects spawns per
// index in a synchronous loop through BeginDeferredActorSpawnFromClass, which this mod already
// native-hooks below the blueprint VM, so the order follows from the bytecode IF the thunk
// catches every keyless load spawn. COVERAGE is the one unproven part, and this probe measures
// it.
//
// THE PROBE: during the client join, record every keyless-family BeginDeferred spawn the thunk
// sees, by actor pointer; at load quiescence, walk the GUObjectArray for the surviving keyless
// natives and check each was recorded. CAUGHT-ALL (no survivor missed) makes spawn order the
// deterministic primary; MISSES sends the bind to the exact-transform bijection instead. Nothing
// is spawned, bound or mutated. Ini-gated [dev] spawn_order_probe=1; game thread only.
#pragma once

#include <cstdint>

namespace coop::dev::spawn_order_probe {

// The families the index->eid map covers. Keyed forms bind by key and are not probed here.
enum class Family : uint8_t { ChipPile = 0, KerfurOff = 1 };

// True iff [dev] spawn_order_probe=1 (latched once). When false, every call below is a cheap no-op.
bool IsEnabled();

// CLIENT join edge: arm the probe and clear the recorded set. Called from BeginClaimTracking,
// the same client-join seam that resets the census. No-op on the host and when disabled.
void ArmForJoin();

// CLIENT BeginDeferred thunk: record a keyless-family load spawn by actor pointer. Called from
// OnBeginDeferredSpawnObserve's client branch, before the host gate. No-op when disabled or not
// armed.
void NoteKeylessSpawn(void* newActor, Family family);

// CLIENT load quiescence, at the sweep-fire point in the client reconcile tick: walk the
// surviving keyless natives, check each was recorded, emit the per-family verdict and disarm.
// No-op when disabled or not armed.
void EmitVerdictAtQuiescence();

}  // namespace coop::dev::spawn_order_probe
