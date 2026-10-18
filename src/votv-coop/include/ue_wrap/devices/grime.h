// ue_wrap/devices/grime.h -- engine access for VOTV surface grime (Agrime_C and grime_* subclasses).
// This wrapper owns reflection, field offsets and UFunction calls; coop::grime_sync owns network
// state and behavior.
//
// Agrime_C has no save-actor Key, so persistence uses [type, process] in save order. The `process`
// scalar controls decal dirt through `process / maxProcess`. A sponge wipe subtracts
// `Sub * cleanStrength * 1.2`; the game destroys the decal when process becomes negative.
//
// Level-placed decals never move, so grime_sync identifies them by quantized world position.
// Runtime coffee-projectile splatter is outside this persistence lane.

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
// uses this; it runs no lowering verb, so coop::grime_sync never hears it back as a fall.
bool WriteProcessAndApply(void* grime, float process);

}  // namespace ue_wrap::grime
