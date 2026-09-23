// coop/dev/rollover_watch.h -- dev-only, read-only instrument for the world clock's day rollover
// (ini rollover_watch=1 / env VOTVCOOP_ROLLOVER_WATCH, off by default; run it on BOTH peers).
//
// The rollover is a tick threshold inside the day-night cycle and everything it drives is a
// Blueprint body no ProcessEvent hook sees, so this watches twelve functions at the script gate by
// name: the roll's own verbs (the dish hash codes, the task shuffle, the Bad Sun), the minute and
// hour pulses, every Blueprint bound to a pulse or to the new-day broadcast, and the save's event
// walk. Each watch counts its pre and its post: a call another consumer refused, or a body whose
// fault the firewall absorbed, reads as reached above ran. It never refuses a call.
//
// Lines: ARMED once per world (the clock's rate inputs, the game mode, the calendar day and the
// achievement the midnight roll branches on, the gate switch, the watches live); DAY when a day
// number changes (the host's own; on a client the host's, as the clock stream carries it, and its
// own), with every watch's total; DIGEST when the 24 hash codes change; one line per pump tick for
// each roll verb or rare consumer that ran in it.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::rollover_watch {

// The memoized flag.
bool IsEnabled();

// Cache the session, for the role. Called from the subsystems Install fanout every pump tick;
// idempotent. No-op when the flag is off.
void Install(coop::net::Session* session);

// Register the watches (once per process), arm per world, flush this tick's bursts, and print the
// DAY and DIGEST lines. Every pump tick in a world, after time_sync's. Game thread.
void Tick();

// How many times a watched function's body has run this session (its post count); 0 for a name that
// is not one of the twelve. For a drill that judges a window of the run. Game thread.
uint64_t RanCount(const wchar_t* name);

// The session ended: reset the per-world state and the counters. The watches stay registered.
void OnDisconnect();

}  // namespace coop::dev::rollover_watch
