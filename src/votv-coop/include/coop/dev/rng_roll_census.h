// coop/dev/rng_roll_census.h -- the live client-roll census probe.
//
// A READ-ONLY diagnostic, ini-gated on `[dev] rng_roll_census=1` and free when off (the
// interceptors early-out on one memoized bool). It measures which of the game's autonomous RNG
// rollers are actually LIVE on each role, so suppressing them structurally rather than by
// allowlist is decided from data and not from the static upper bound. Five channels: (a) the
// spawns npc_sync's BeginDeferred interceptor passes through untouched, which keeps discovery
// name-agnostic; (b) DRIVERS -- POST observers on the timer, delay and tick-interval natives,
// where the ARM is the liveness signal whatever the spawn gate then decides; (c) live
// ticker_base_C descendants; (d) a log-only QuitGame guard; (e) a world blueprint-actor
// histogram whose residue is a new actor with no registry binding, off the hand axis and
// outside the join episode. Records carry the role, the join episode, and whether our own
// dispatch produced them. The Note* callbacks fire on the ProcessEvent-dispatching thread,
// possibly a parallel-animation worker, so their state is mutex-guarded counters and GNames
// reads only, with no engine dispatch off-thread; Tick() is game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::rng_roll_census {

// Memoized `[dev] rng_roll_census` ini flag.
bool IsEnabled();

// Resolve natives + register interceptors. Called from the subsystems Install fanout (pre-world,
// per-pump-tick until latched; self-throttled). No-op when the ini flag is off.
void Install(coop::net::Session* session);

// Channel (a) entry: npc_sync's BeginDeferred interceptor reports a spawn it passes through
// untouched (actorClass = the UClass* about to spawn). Any-thread safe.
void NotePassThrough(void* actorClass, bool isHostRole);

// Periodic censuses (c)+(e) + the (b)/(d) counter dump. Game thread; call every pump tick
// (self-throttled to the census cadence; single bool read when disabled/idle).
void Tick();

}  // namespace coop::dev::rng_roll_census
