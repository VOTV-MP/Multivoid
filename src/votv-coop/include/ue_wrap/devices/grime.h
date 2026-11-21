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

#include "ue_wrap/core/types.h"

#include <cstddef>
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
// uses this; it runs no lowering verb, so coop::grime_sync never hears it back as a fall.
bool WriteProcessAndApply(void* grime, float process);

// The decal's own settings, read where the host's judge weighs a client's fall (coop/interactables/grime_judge). Each
// false on null, an unresolved field or a dead object; `out` is untouched then. Game thread.
// `cleanStrength`: the multiplier clean() puts on a call's amount (1 on the base decal, 2 on beer, coffee and wine, 4 on
// the blood drip).
bool ReadCleanStrength(void* grime, float& out);
// `isCleanable`: false on a crack and on fallen leaves, whose clean() returns without a fall.
bool ReadIsCleanable(void* grime, bool& out);
// `resistRain`: a decal the rain's own pass leaves alone.
bool ReadResistRain(void* grime, bool& out);
// Every live grime decal of every class (grime_C and its subclasses), once, in no particular order: not a class default
// object, not dying. `fn` must not create or destroy objects. Walks each class's instances; for a dev drill's pick, never
// a tick. Game thread.
using DecalFn = void (*)(void* ctx, void* decal);
size_t ForEachDecal(DecalFn fn, void* ctx);
// The gamemode's `isWinter`: the season is winter (or it is Christmas, which sets the season to winter), when the rain's
// own pass runs no cleaning. False when the gamemode or the field does not read. Game thread.
bool ReadWinter(bool& out);
// True iff `obj` is a grime_crack_C (or a descendant): the decal the wall fixer repairs.
bool IsCrack(void* obj);
// The decal's `Box` component, the volume every cleaning verb's overlap tests: its world centre and half the length of
// its scaled extent (the farthest an overlap can touch from the centre). False when the box or its extent does not read.
bool ReadBox(void* grime, FVector& centre, float& halfDiagonalCm);

}  // namespace ue_wrap::grime
