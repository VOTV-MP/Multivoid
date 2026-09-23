// coop/dev/set_clock.h -- DEV: set the world day and clock time from the F1 menu.
//
// HOST-AUTHORITATIVE: it writes the host's AdaynightCycle_C clock through ue_wrap::daynightcycle,
// and coop::time_sync sends the moved clock as its next sample, so the sky snaps on every peer.
// Host-only: dev_gate refuses on a connected client.
//
// TWO clocks live on the cycle. The NAMED clock `timeZ` is an FIntVector of hour, minute and day
// number, rebuilt each tick from the accumulator and saveSlot.savedtime.Z and handed to settime,
// which persists it and walks the scheduled events. The float accumulators are `totalTime` and
// `day`: `day` is the within-day counter the midnight cascade fires on and what the sun derives
// from, `totalTime` drives nothing in the blueprint. Neither is the day number, which lives in
// savedtime.Z. Setting the clock writes the day number and the accumulators; the next tick rebuilds
// `timeZ` from them. A FORWARD day jump fires every skipped scheduled row
// through settime, which the event lane mirrors to clients.

#pragma once

namespace coop::dev::set_clock {

// DAY CONVENTION: the game's own save rows display `savedtime.Z + 1` (uicomp_saveSlot::upd
// bytecode). This API speaks the DISPLAYED day, 1-based, what the game UI shows; the raw scheduler
// day-Z is displayed minus 1.

// Read the live NAMED clock (displayed day = timeZ.Z + 1, hour/minute from timeZ) + the sun
// FRACTION (totalTime/MaxTime in [0,1)). Returns false if the world clock is not resolved yet.
// Raw field reads (render-thread tolerable, the existing menu pattern).
bool ReadCurrent(int& hourOut, int& minuteOut, int& dayOut, float& sunFracOut);

// HOST-only (dev_gate): set the clock to (displayed day, hour, minute). Clamps day >= 1, hour
// 0..23, minute 0..59, and writes the day number (day-1) with both accumulators. Posted to the
// game thread; the cycle's next tick rebuilds the named clock and feeds it through
// saveSlot.settime, and a forward jump fires the skipped scheduled events natively.
void SetClock(int day, int hour, int minute);

// HOST-only (dev_gate): set the SUN position as a fraction of one day, keeping the same day: both
// accumulators, from which the next tick rebuilds the HUD's clock. Clamped [0, 0.999]. Posted to
// the game thread, so any thread may call it.
void SetTimeFraction(float frac);

// The same set, done now, for a caller already on the game thread that has to know it landed (a
// drill). False, having written nothing, when the dev gate refuses or the clock is not resolved.
bool ApplyTimeFraction(float frac);

}  // namespace coop::dev::set_clock
