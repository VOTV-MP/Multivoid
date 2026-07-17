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
// RE: research/findings/architecture-audits/votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.

#pragma once

#include <cstdint>

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

// Read MaxTime -- the length of ONE day in `totalTime` units (the within-day clock runs [0, MaxTime),
// wrapping to advance Day; the sun angle is totalTime/MaxTime of a full rotation). Lets a caller map a
// time-of-day FRACTION (0..1) to a totalTime (frac * MaxTime). False if unresolved. Game thread.
bool ReadMaxTime(float& maxTime);

// Overwrite the cycle's clock (direct field writes -- the loadtime equivalent, no
// UFunction, unconditional). The client applies the host's authoritative clock here; the
// cycle's own ReceiveTick then re-derives the sun. No-op if not resolved. Game thread.
void ApplyClock(float totalTime, float day, float timeScale);

// ---- the NAMED clock: `timeZ` (FIntVector @0x02D0: X=hour, Y=minute, Z=day) ----
// This is the game's own running (hour, minute, day) triple -- the cycle's minute pulse
// calls saveSlot.settime(timeZ) and rebuilds timeZ from settime's incremented outs
// (bytecode: ExecuteUbergraph_daynightCycle settime call region; getNamedTime is a pure
// unpacker of this triple). The DAY NUMBER lives here (timeZ.Z) -- NOT in the float `Day`
// field, which is a within-day accumulator (the midnight cascade threshold `day > MaxTime`).
// Reads/writes are plain FIntVector field access (BP writes it via Let, no setter); a
// WRITE is picked up by the next minute pulse -> settime(newTriple) -> savedtime persists
// + the scheduled-event walk runs natively. Game thread. False/no-op if unresolved.
bool ReadTimeZ(int32_t& hour, int32_t& minute, int32_t& day);
void WriteTimeZ(int32_t hour, int32_t minute, int32_t day);

// ---- U6 day-roll suppression (v65; coop/time_sync drives these) ----
// RE (2026-06-12 daynight agent pass): the midnight task/email/points cascade
// is a TICK-THRESHOLD trigger (`day > MaxTime` inside ReceiveTick), armed on
// every peer -- a connected client would roll its OWN task batch (different
// RNG) and the 6am `func_newHour` would place a DUPLICATE automatic drone
// order (incl. an instant one at join when the first clock snap crosses
// 06:00 with dailyDelivery=false). Suppression is state-level, not a timer
// kill: the client's day only advances via our corrections (always < MaxTime
// -- the host wraps in-tick), so writing TimeScale=0 makes the whole cascade
// structurally unreachable client-side while sun/sky visuals keep deriving
// from the corrected `day`.

// Write TimeScale alone (1.0f = the game's own restore value, uber @10703;
// used by the disconnect restore). No-op if unresolved.
void WriteTimeScale(float scale);

// saveSlot.dailyDelivery := true -- the game's OWN 6am-order latch (its only
// writers are the suppressed midnight reset and the order placement itself,
// so one write holds for the session). Called with every clock correction;
// cheap (cached offsets + one bool write). False until saveSlot resolves.
bool LatchDailyDelivery();

}  // namespace ue_wrap::daynightcycle
