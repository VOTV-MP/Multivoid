// ue_wrap/world/skysphere.h -- standalone engine access for VOTV's visible NIGHT SKY actor
// (Anewsky_C: the star dome + celestial body meshes, owned by AdaynightCycle_C::skysphere).
// Principle-7 engine-wrapper layer (no network/coop state); coop::sky_sync drives the sync.
//
// Two values diverge per-peer and are NOT covered by TimeSync (which already converges the
// sun/moon ORBIT + brightness from the clock):
//   (1) the star-dome orientation -- Anewsky_C's `sky` mesh is given an UNSEEDED random yaw,
//       RandomFloatInRange(-45,-135), once on the BeginPlay path, then spun by DeltaSeconds/32
//       per Tick, so each peer's fresh world rolls a different star orientation;
//   (2) the moon phase -- the BP recomputes it from the LOCAL wall clock, so peers in
//       different timezones disagree.
// This wrapper reads/writes the sky component's WORLD rotation and the moon phase so the host
// can snapshot them (host-authoritative, the same shape as the clock in daynightcycle).

#pragma once

#include "ue_wrap/core/types.h"  // FRotator

namespace ue_wrap::skysphere {

// Resolve the newsky_C UClass + the sky/moonPhase_mirror offsets + the setMoonPhase UFunction.
// Idempotent; true once resolved (false while the BP class is not yet loaded). Game thread.
bool EnsureResolved();

// The live Anewsky_C* (cached; re-resolved if it dies). nullptr until it has streamed in.
// Game thread.
void* Sky();

// Read the star-dome's WORLD rotation + moonPhase into the outs. The phase comes from
// Anewsky_C::moonPhase_mirror, which the BP's Tick re-assigns from saveSlot.moonPhase every
// frame, so it is that value one frame old. False if not resolved / not streamed in (outs
// untouched). Game thread.
bool ReadSky(FRotator& skyWorldRot, float& moonPhase);

// Apply the host's values: SetComponentWorldRotation on the `sky` mesh, then the phase into
// BOTH moonPhase_mirror (right for this frame) and saveSlot.moonPhase (what survives, since
// the Tick overwrites the mirror from it). The BP re-derives saveSlot.moonPhase from the local
// clock on its own repeating timer, which sky_sync's ~1 Hz push then corrects. No-op if not
// resolved / not streamed in. Game thread.
void ApplySky(const FRotator& skyWorldRot, float moonPhase);

}  // namespace ue_wrap::skysphere
