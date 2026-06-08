// ue_wrap/grime.h -- standalone engine access for VOTV surface grime (Agrime_C +
// ~20 subclasses: grime_oil_C / grime_blood2_C / grime_dusty_C / ...). Principle-7
// engine-wrapper layer: wraps the reflection / struct-offset / UFunction-thunk details
// of a grime actor. NO network logic, NO gameplay/coop state -- coop::grime_sync owns
// those and talks to the engine through here.
//
// Agrime_C : AActor (directly -- NOT a save/trigger actor, so it has NO Key field; it is
// UNKEYED, persisted as a primitive JSON blob [type, process] indexed by save order). Its
// dirt is the whole-decal scalar `process`@0x0250 (float; higher = dirtier). The visible
// dirtiness is `process / maxProcess`@0x0268. A sponge wipe calls clean(sponge, Sub) which
// does `process -= Sub*cleanStrength*1.2` and, at `process < 0`, K2_DestroyActor()s the decal.
//
// Because a grime decal is STATIC (a level-placed decal with a saved transform that never
// moves), its WORLD POSITION is a deterministic cross-peer identity (both peers load the same
// save -> the same decal at the same transform). coop::grime_sync keys grime by a quantized
// world-position string, NOT by a host-allocated eid -- the decal's own position is its
// identity. (Runtime projectile-splatter grime, whose spawn position is non-deterministic, is
// out of scope for that model and deferred.)
//
// RE: research/findings/votv-dirt-window-cleaning-RE-and-coop-sync-design-2026-06-07a.md.

#pragma once

#include <cstdint>

namespace ue_wrap::grime {

// Resolve the grime_C UClass + the `process` / `Type` / `maxProcess` field offsets +
// the applyMaterial UFunction. Idempotent; returns true once everything resolved (false
// while the grime_C BP class is not yet loaded -- the caller retries on a later tick). Game thread.
bool EnsureResolved();

// The grime_C UClass pointer (nullptr until EnsureResolved succeeds). Exposed for IsGrime.
void* GrimeClass();

// True iff `obj`'s class is grime_C or a subclass (the ~20 grime_* variants). Cheap (a bounded
// SuperStruct walk; no allocation). False if not yet resolved.
bool IsGrime(void* obj);

// Read the grime's `process` (dirt amount) into `out`. Returns false on null / unresolved;
// leaves `out` untouched on failure.
bool ReadProcess(void* grime, float& out);

// Read the grime's `Type` (variant selector) into `out`. Used only to disambiguate two decals
// at the same quantized position in the cross-peer key. Returns false on null / unresolved.
bool ReadType(void* grime, int32_t& out);

// Write the grime's `process` scalar + call Agrime_C::applyMaterial() to repaint the decal at
// the new `process / maxProcess` ratio (maxProcess is a per-instance constant identical across
// peers -- same save -- so the wire only carries `process`). applyMaterial re-creates the
// dynamic material; it fires only on a process-change delta (a wipe), never per-frame, so the
// cost is bounded. Returns false on null / unresolved. The receiver of a remote wipe uses this;
// coop::grime_sync echo-suppresses so the resulting field change isn't re-broadcast.
bool WriteProcessAndApply(void* grime, float process);

}  // namespace ue_wrap::grime
