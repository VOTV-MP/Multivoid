// ue_wrap/daynightcycle.h -- standalone engine access for VOTV's world clock
// (AdaynightCycle_C, the singleton that owns time-of-day + the weather scheduler).
// Principle-7 engine-wrapper layer: it wraps the reflection / struct-offset details
// of the cycle's CLOCK fields. NO network logic, NO coop state -- coop::time_sync owns
// those and reads/writes the clock through here.
//
// The clock is three floats on the cycle: `totalTime` (absolute elapsed game time, the
// authoritative continuous clock), `Day` (the day number), `TimeScale` (the advance
// rate). The SUN/MOON position -- hence world brightness -- is a pure function of
// `totalTime` recomputed every `ReceiveTick` (setSunAndMoonRotation), so syncing the
// clock makes the sun follow; we never drive the sun/light fields directly (the
// purple-light lesson). The native `loadtime(totalTime, Day)` setter disassembles to
// just `totalTime=...; if(!skipDaySet) Day=...` (no fan-out), so a direct field write is
// the equivalent, unconditional, UFunction-free drive.
//
// RE: research/findings/votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.

#pragma once

namespace ue_wrap::daynightcycle {

// Resolve the daynightCycle_C UClass + the totalTime / Day / TimeScale field offsets.
// Idempotent; true once resolved (false while the BP class is not yet loaded -- the
// caller retries on a later tick). Game thread.
bool EnsureResolved();

// The live AdaynightCycle_C singleton (cached; re-resolved if it dies). nullptr until it
// has streamed in. Game thread.
void* Cycle();

// Read the cycle's clock into the outs. False if the cycle / offsets are not resolved
// (outs untouched on failure). Game thread.
bool ReadClock(float& totalTime, float& day, float& timeScale);

// Overwrite the cycle's clock (direct field writes -- the loadtime equivalent, no
// UFunction, unconditional). The client applies the host's authoritative clock here; the
// cycle's own ReceiveTick then re-derives the sun. No-op if not resolved. Game thread.
void ApplyClock(float totalTime, float day, float timeScale);

}  // namespace ue_wrap::daynightcycle
