// coop/dev/set_clock.h -- DEV: set the world day + time-of-day from the F1 menu.
//
// HOST-AUTHORITATIVE: writes the host's AdaynightCycle_C clock (via ue_wrap::daynightcycle); the
// existing coop::time_sync host poll broadcasts the new clock to clients each tick, so the sky/day
// snaps on every peer. Host-only (dev_gate refuses on a connected client, like every dev verb).
//
// The clock is `totalTime` (the within-day clock, [0, MaxTime)) + `Day` (the counter). The sun is a
// pure function of totalTime/MaxTime, so setting the fraction moves the sun; setting Day advances the
// day counter without touching the sun. We never drive the sun/light fields directly (the purple-light
// lesson) -- only the clock, and the cycle's own ReceiveTick re-derives the sun.

#pragma once

namespace coop::dev::set_clock {

// Read the live cycle's day (integer) + time-of-day FRACTION (totalTime/MaxTime in [0,1)). Returns
// false if the world clock is not resolved yet (world not up). Game thread.
bool ReadCurrent(int& dayOut, float& fracOut);

// HOST-only (dev_gate): set the day counter, preserving the current time-of-day + TimeScale. Clamped
// to >= 0. Posted to the game thread (the clock write touches BP state).
void SetDay(int day);

// HOST-only (dev_gate): set the time-of-day as a FRACTION of one day (0 = day start -> ~1 = day end;
// totalTime := frac * MaxTime), preserving the current Day + TimeScale. Clamped to [0, 0.999]. Posted
// to the game thread.
void SetTimeFraction(float frac);

}  // namespace coop::dev::set_clock
