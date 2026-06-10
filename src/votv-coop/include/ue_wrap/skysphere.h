// ue_wrap/skysphere.h -- standalone engine access for VOTV's visible NIGHT SKY actor
// (Anewsky_C: the star dome + celestial body meshes, owned by AdaynightCycle_C::skysphere).
// Principle-7 engine-wrapper layer (no network/coop state); coop::sky_sync drives the sync.
//
// Two values diverge per-peer and are NOT covered by TimeSync (which already converges the
// sun/moon ORBIT + brightness from the clock):
//   (1) the star-dome orientation -- Anewsky_C's `sky` mesh is given an UNSEEDED random yaw
//       RandomFloatInRange(-45,-135) at BeginPlay + a slow per-tick spin, so each peer's
//       fresh world rolls a different star orientation ("stars look completely different");
//   (2) `moonPhase_mirror` -- read from the save's moonPhase (host progressed vs client blank).
// This wrapper reads/writes the sky component's WORLD rotation + moonPhase_mirror so the host
// can snapshot them (host-authoritative, the same shape as the clock in daynightcycle).
//
// RE: research/findings/votv-sky-stars-celestial-sync-RE-2026-06-08.md.

#pragma once

#include "ue_wrap/types.h"  // FRotator

namespace ue_wrap::skysphere {

// Resolve the newsky_C UClass + the sky/moonPhase_mirror offsets + the setMoonPhase UFunction.
// Idempotent; true once resolved (false while the BP class is not yet loaded). Game thread.
bool EnsureResolved();

// The live Anewsky_C* (cached; re-resolved if it dies). nullptr until it has streamed in.
// Game thread.
void* Sky();

// Read the star-dome's WORLD rotation + moonPhase into the outs. False if not resolved / not
// streamed in (outs untouched). Game thread.
bool ReadSky(FRotator& skyWorldRot, float& moonPhase);

// Apply the host's values: SetComponentWorldRotation on the `sky` mesh + write
// moonPhase_mirror + re-apply the moon material param (setMoonPhase) so the phase shows
// immediately. No-op if not resolved / not streamed in. Game thread.
void ApplySky(const FRotator& skyWorldRot, float moonPhase);

}  // namespace ue_wrap::skysphere
