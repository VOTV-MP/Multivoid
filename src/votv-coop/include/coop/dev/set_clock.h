// coop/dev/set_clock.h -- DEV: set the world day + clock time from the F1 menu.
//
// HOST-AUTHORITATIVE: writes the host's AdaynightCycle_C clock (via ue_wrap::daynightcycle); the
// existing coop::time_sync host poll broadcasts the accumulator clock to clients each tick, so the
// sky snaps on every peer. Host-only (dev_gate refuses on a connected client, like every dev verb).
//
// TWO clocks live on the cycle (see ue_wrap/world/daynightcycle.h):
//   - the NAMED clock `timeZ` (FIntVector: X=hour, Y=minute, Z=day number) -- the game's own
//     running triple, which the cycle rebuilds each tick from the accumulator and
//     saveSlot.savedtime.Z and then hands to settime, which persists it and walks the
//     scheduled events. The DAY NUMBER lives in savedtime.Z, mirrored here.
//   - the float accumulators `totalTime` and `day`. `day` is the within-day counter the
//     midnight cascade fires on AND what the sun derives from; `totalTime` is the absolute
//     elapsed clock and drives nothing in the Blueprint. Neither is the day number, which is
//     why printing the accumulator as "day" was a display bug.
// Setting the clock therefore writes BOTH: the accumulators, so the sky moves at once, and
// `timeZ`, so the HUD agrees before the next rebuild. A FORWARD day jump fires every skipped
// scheduled row through the game's own settime, and the event lane mirrors those to clients.

#pragma once

namespace coop::dev::set_clock {

// DAY CONVENTION: the game's own save rows display `savedtime.Z + 1` (uicomp_saveSlot::upd
// bytecode -- the save_browser "Day 3566" fix 2026-06-10 established this). This API speaks
// the DISPLAYED day (1-based, what the game UI shows); the raw scheduler day-Z = displayed-1.

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
