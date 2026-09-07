// ue_wrap/world/daynightcycle.h -- standalone engine access for VOTV's world clock
// (daynightCycle_C, the singleton owning time of day and the weather scheduler). A principle-7
// engine wrapper over the cycle's CLOCK fields: no network logic, no coop state, both of which
// coop/world/time_sync owns and drives through here.
//
// The clock is three floats. `day` is the WITHIN-day accumulator, advanced each tick by
// deltaSeconds * timeScale (times the difficulty and sleep multipliers) and wrapped modulo
// `maxTime` at the roll; `totalTime` takes the same increment and is never wrapped, so it is
// the absolute elapsed clock; `timeScale` is the rate. Sun and moon are a pure function of
// `day`: the cycle recomputes phase = ((day + offset) % maxTime) / maxTime every tick and
// rotates both lights phase * 360 degrees, so writing `day` makes the sun follow and we never
// drive the light fields ourselves. The native loadtime(totalTime, day) setter writes BOTH
// members, and only while `skipDaySet` is false -- with that flag set it writes nothing -- and
// fans out to nothing else, so a direct field write is the same drive without the flag.

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

// Read maxTime -- the length of ONE day in `day` units (the within-day clock runs [0, maxTime)
// and wraps at the midnight roll; the sun angle is that fraction of a full rotation). Lets a
// caller map a time-of-day FRACTION (0..1) onto a `day` value. False if unresolved. Game thread.
bool ReadMaxTime(float& maxTime);

// Overwrite the cycle's clock by direct field write -- what loadtime does on its unguarded
// path, minus the skipDaySet test. The client applies the host's authoritative clock here and
// the cycle's own tick re-derives the sun from `day`. No-op if unresolved. Game thread.
void ApplyClock(float totalTime, float day, float timeScale);

// ---- the NAMED clock: `timeZ` (FIntVector: X=hour, Y=minute, Z=day number) ----
// The game's own running triple. The cycle rebuilds it every tick -- hour and minute derived
// from `day` and `maxTime`, Z copied from saveSlot.savedtime.Z, which is where the DAY NUMBER
// actually lives -- and hands it to saveSlot.settime, whose out flags drive the new-minute,
// new-hour and new-day cascades. The float `day` is NOT the day number: it is the within-day
// accumulator that the midnight threshold `day > maxTime` fires on. Reads and writes here are
// plain FIntVector field access; the Blueprint writes it with a Let and offers no setter. Game
// thread; false or a no-op if unresolved.
bool ReadTimeZ(int32_t& hour, int32_t& minute, int32_t& day);
void WriteTimeZ(int32_t hour, int32_t minute, int32_t day);

// ---- day-roll suppression (coop/world/time_sync drives these) ----
// The midnight task, email and points cascade is a tick threshold, `day > maxTime` inside the
// cycle's own tick chain, and it is armed on every peer: a connected client would roll its OWN
// task batch on different RNG, and func_newHour would place a duplicate automatic drone order
// (hour >= 6 while dailyDelivery is false), including an instant one at a join whose first
// clock snap crosses 06:00. The suppression is state-level rather than a timer kill: a
// client's `day` advances only through our corrections, always below maxTime because the host
// wraps in-tick, so writing timeScale = 0 puts the whole cascade structurally out of reach
// while the sky keeps deriving from the corrected `day`.

// Write timeScale alone (1.0f is the value the cycle's own tick restores); used by the
// disconnect restore. No-op if unresolved.
void WriteTimeScale(float scale);

// saveSlot.dailyDelivery := true -- the game's OWN 6am-order latch (its only
// writers are the suppressed midnight reset and the order placement itself,
// so one write holds for the session). Called with every clock correction;
// cheap (cached offsets + one bool write). False until saveSlot resolves.
bool LatchDailyDelivery();

}  // namespace ue_wrap::daynightcycle
