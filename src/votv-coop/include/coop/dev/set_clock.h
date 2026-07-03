// coop/dev/set_clock.h -- DEV: set the world day + clock time from the F1 menu.
//
// HOST-AUTHORITATIVE: writes the host's AdaynightCycle_C clock (via ue_wrap::daynightcycle); the
// existing coop::time_sync host poll broadcasts the accumulator clock to clients each tick, so the
// sky snaps on every peer. Host-only (dev_gate refuses on a connected client, like every dev verb).
//
// TWO clocks live on the cycle (bytecode-verified 2026-07-03; ue_wrap/daynightcycle.h):
//   - the NAMED clock `timeZ` (FIntVector: X=hour, Y=minute, Z=DAY) -- the game's own running
//     triple. The minute pulse calls saveSlot.settime(timeZ) (persists savedtime + runs the
//     scheduled-event walk) and rebuilds timeZ from settime's outs. THE day number lives here.
//   - the float accumulators `totalTime`/`Day` ([0, MaxTime) within-day counters; the sun angle
//     derives from totalTime, the midnight cascade from Day). The old dev menu printed the raw
//     `Day` ACCUMULATOR as "day" (the 4029-ticking display bug) and wrote it in SetDay -- both
//     wrong; the accumulator is not the day number.
// Setting the clock = write `timeZ`; the game's own pulse picks it up within a game-minute and
// runs settime natively (a FORWARD day jump fires every skipped scheduled row -- native settime
// semantics, and v95 EventFire mirrors them to clients per policy). The sun slider stays a
// separate, visual-only lever (totalTime), exactly as before.

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
