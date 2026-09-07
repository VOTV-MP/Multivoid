// ue_wrap/world/daynightcycle.h -- standalone engine access for VOTV's world clock
// (daynightCycle_C, the singleton owning time of day and the weather scheduler). A principle-7
// engine wrapper over the cycle's CLOCK fields: no network logic, no coop state, both of which
// coop/world/time_sync owns and drives through here.
//
// The clock is three floats. `day` is the WITHIN-day accumulator, advanced each tick by
// deltaSeconds * timeScale and then by the difficulty, day-speed and sleep multipliers, and
// wrapped modulo `maxTime` at the roll; `totalTime` takes the same increment and is never
// wrapped, so it is the absolute elapsed clock; `timeScale` is the rate. Sun and moon follow
// `day`, not totalTime: the cycle recomputes phase = ((day + offset) % maxTime) / maxTime and
// rotates the lights by phase * 360 on its own sun-rotation timer, not every tick, so writing
// `day` makes the sun follow within that period and we never drive the light fields ourselves.
// The native loadtime(totalTime, day) writes both members and only while `skipDaySet` is
// false -- with that flag set it writes nothing at all -- and fans out to nothing else.

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

// Read maxTime -- the length of ONE day in `day` units; the within-day clock runs [0, maxTime)
// and wraps at the midnight roll. Lets a caller map a time-of-day FRACTION (0..1) onto a `day`
// value, which is how the named clock reads it. The SUN is that fraction plus a mode-dependent
// offset, so it is not the same angle. False if unresolved. Game thread.
bool ReadMaxTime(float& maxTime);

// Overwrite the cycle's clock by direct field write: totalTime and day, as loadtime writes
// them but without its skipDaySet test, AND timeScale, which loadtime never touches and the
// day-roll suppression below depends on. The client applies the host's authoritative clock
// here and the cycle re-derives the sun from `day`. No-op if unresolved. Game thread.
void ApplyClock(float totalTime, float day, float timeScale);

// ---- the NAMED clock: `timeZ` (FIntVector: X=hour, Y=minute, Z=day number) ----
// The game's own running triple. The cycle rebuilds it every tick -- hour and minute derived
// from `day` and `maxTime`, Z copied from saveSlot.savedtime.Z, which is where the DAY NUMBER
// actually lives -- and hands it to saveSlot.settime, whose new-minute and new-hour out flags
// drive those two cascades. Its new-day flag is read nowhere; the day roll is the tick
// threshold `day > maxTime` instead, which is also what increments savedtime.Z. The float
// `day` is NOT the day number: it is the within-day accumulator that threshold fires on.
// Reads and writes here are plain FIntVector field access; the Blueprint writes it with a Let
// and offers no setter. Game thread; false or a no-op if unresolved.
bool ReadTimeZ(int32_t& hour, int32_t& minute, int32_t& day);
void WriteTimeZ(int32_t hour, int32_t minute, int32_t day);

// ---- day-roll suppression (coop/world/time_sync drives these) ----
// The midnight cascade -- the task roll every night, the results email and points on the week
// boundary -- is a tick threshold, `day > maxTime` inside the cycle's own tick chain, and it
// is armed on every peer: a connected client would roll its OWN task batch on different RNG,
// and func_newHour would place a duplicate automatic drone order (hour >= 6 while
// dailyDelivery is false), including an instant one at a join whose first clock snap crosses
// 06:00. The suppression is state-level rather than a timer kill: a client's `day` advances
// only through our corrections, always below maxTime because the host wraps in-tick, so
// writing timeScale = 0 puts the whole cascade structurally out of reach while the sky keeps
// deriving from the corrected `day`.

// Write timeScale alone. 1.0f is the game's own running value, the one its rewind restores
// after the hour it spends at -1; used by the disconnect restore. No-op if unresolved.
void WriteTimeScale(float scale);

// saveSlot.dailyDelivery := true -- the game's OWN 6am-order latch (its only
// writers are the suppressed midnight reset and the order placement itself,
// so one write holds for the session). Called with every clock correction;
// cheap (cached offsets + one bool write). False until saveSlot resolves.
bool LatchDailyDelivery();

}  // namespace ue_wrap::daynightcycle
