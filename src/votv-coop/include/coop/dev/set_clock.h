// coop/dev/set_clock.h -- DEV: set the world day and clock time from the F1 menu.
//
// HOST-AUTHORITATIVE: it writes the host's AdaynightCycle_C clock through ue_wrap::daynightcycle,
// and coop::time_sync's host poll broadcasts the accumulator clock each tick, so the sky snaps on
// every peer. Host-only: dev_gate refuses on a connected client.
//
// TWO clocks live on the cycle. The NAMED clock `timeZ` is an FIntVector of hour, minute and day
// number, rebuilt each tick from the accumulator and saveSlot.savedtime.Z and handed to settime,
// which persists it and walks the scheduled events. The float accumulators are `totalTime` and
// `day`: `day` is the within-day counter the midnight cascade fires on and what the sun derives
// from, `totalTime` drives nothing in the blueprint. Neither is the day number, which is why
// printing the accumulator as "day" was a display bug. Setting the clock writes BOTH: the
// accumulators, so the sky moves at once, and `timeZ`, so the HUD agrees before the next rebuild. A
// FORWARD day jump fires every skipped scheduled row through settime, which the event lane mirrors
// to clients.

#pragma once

namespace coop::dev::set_clock {

// DAY CONVENTION: the game's own save rows display `savedtime.Z + 1` (uicomp_saveSlot::upd
// bytecode). This API speaks the DISPLAYED day, 1-based, what the game UI shows; the raw scheduler
// day-Z is displayed minus 1.

// Read the live NAMED clock (displayed day = timeZ.Z + 1, hour/minute from timeZ) + the sun
// FRACTION (totalTime/MaxTime in [0,1)). Returns false if the world clock is not resolved yet.
// Raw field reads (render-thread tolerable, the existing menu pattern).
bool ReadCurrent(int& hourOut, int& minuteOut, int& dayOut, float& sunFracOut);

// HOST-only (dev_gate): set the named clock to (displayed day, hour, minute). Clamps day >= 1,
// hour 0..23, minute 0..59; writes timeZ = (hour, minute, day-1). Posted to the game thread;
// the cycle's next minute pulse feeds it through saveSlot.settime (savedtime persists; skipped
// scheduled events fire natively on a forward jump). The sun is NOT moved (sun slider = visuals).
void SetClock(int day, int hour, int minute);

// HOST-only (dev_gate): set the SUN position as a fraction of one day (visual only --
// totalTime := frac * MaxTime; the named clock/timeZ is untouched). Clamped [0, 0.999].
void SetTimeFraction(float frac);

}  // namespace coop::dev::set_clock
