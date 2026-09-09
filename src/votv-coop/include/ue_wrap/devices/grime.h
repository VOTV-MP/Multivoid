// ue_wrap/devices/grime.h -- standalone engine access for VOTV surface grime (Agrime_C and its
// grime_* subclasses: grime_oil_C, grime_blood2_C, grime_dusty_C, ...). Principle-7
// engine-wrapper layer, wrapping the reflection, struct-offset and UFunction-thunk details of
// a grime actor: no network logic, no coop state, both of which coop::grime_sync owns.
//
// Agrime_C derives from AActor directly, not from a save or trigger actor, so it has no Key
// field: it is UNKEYED, persisted as a primitive JSON blob [type, process] indexed by save
// order. Its dirt is the whole-decal scalar `process`, higher being dirtier, and the decal is
// painted from the ratio `process / maxProcess` on its dynamic material. A sponge wipe calls
// clean(sponge, Sub, noSound), which does `process -= Sub * cleanStrength * 1.2`, sets that
// scalar itself, and K2_DestroyActor()s the decal once `process < 0`.
//
// A decal is STATIC -- level-placed, transform saved, never moving -- so its WORLD POSITION is
// a deterministic cross-peer identity, and coop::grime_sync keys grime by a quantized position
// string, not a host-allocated eid. Runtime projectile splatter is outside that model.

#pragma once

#include <cstdint>

namespace ue_wrap::grime {

// Resolve the grime_C UClass, the `process` / `Type` / `maxProcess` / `cleanParameter` /
// `dynmat` field offsets, applyMaterial and the material's SetScalarParameterValue.
// Idempotent; true once the class resolved (false while the grime_C blueprint class is not
// loaded yet -- the caller retries on a later tick). Game thread.
bool EnsureResolved();

// True iff `obj`'s class is grime_C or one of its grime_* subclasses. Cheap (a bounded
// SuperStruct walk, no allocation). False if not yet resolved.
bool IsGrime(void* obj);

// Read the grime's `process` (dirt amount) into `out`. Returns false on null / unresolved;
// leaves `out` untouched on failure.
bool ReadProcess(void* grime, float& out);

// Read the grime's `Type` (variant selector) into `out`. Used only to disambiguate two decals
// at the same quantized position in the cross-peer key. Returns false on null / unresolved.
bool ReadType(void* grime, int32_t& out);

// Write the grime's `process` scalar and repaint the decal at the new `process / maxProcess`
// ratio, by setting that scalar on the decal's own dynamic material -- the same step the
// game's clean() performs. maxProcess is a per-instance constant identical across peers (same
// save), so the wire carries only `process`. A decal with no material yet gets one through
// applyMaterial instead. Returns false on null or unresolved. The receiver of a remote wipe
// uses this; coop::grime_sync echo-suppresses so the resulting field change is not
// re-broadcast.
bool WriteProcessAndApply(void* grime, float process);

}  // namespace ue_wrap::grime
