// coop/interactables/atv_hit_guard.h -- the ONE thing that makes a mirrored ATV differ from a
// native one: it may not author collision damage.
//
// Extracted verbatim from atv_sync.cpp (2026-08-30, cut 2 of the >800 LOC reduction). ONE
// concept: seven ComponentHit delegates, intercepted PRE-dispatch, cancelled on a peer that does
// not own the rig's tick. Everything about WHO owns it stays in atv_sync.cpp -- this module is
// handed the owned set and answers yes/no.
//
// It is deliberately the whole difference. The freeze/park model that used to distinguish a
// mirror is gone (RULE 2, ATV.md 14): a mirror runs the rig natively, so the only asymmetry left
// is authorship of damage, and a ComponentHit delegate is dispatched by the physics scene where
// no tick switch can reach it.

#pragma once

#include <cstdint>

namespace coop::atv_hit_guard {

// Register the seven interceptors. Idempotent; logs and leaves the lane INERT if fewer than 7
// resolve or register, because a partially guarded rig is worse than an unguarded one -- it
// destroys vehicles on some peers and not others. Game thread.
void InstallHitGuard();

// Publish the set of ATVs whose tick THIS peer owns. A hit on anything not in this set is
// cancelled. Called from atv_sync's Tick each pass. Game thread to call; read on the physics
// dispatch, hence the atomics inside.
void PublishOwned(void** owned, int n);

// How many the published set holds. atv_sync sizes its scratch buffer with this, so the cap lives
// with the set rather than being agreed by two files independently -- and it is constexpr so that
// agreement is checked by the COMPILER. A runtime accessor would have made a future cap increase
// a silent early-return in the caller instead of a build error.
inline constexpr int kMaxOwned = 64;

// Is `actor` in the published set AND is the lane running? Both halves matter: every early return
// in atv_sync's Tick leaves the previous set in place, so the set alone cannot say "not running".
// coop::atv_sync::OwnsTick is this function under the lane's own name -- callers read the SAME set
// the collision dispatch consults, on purpose, because an instrument that recomputes the predicate
// agrees with itself while disagreeing with the code under test.
bool Owns(void* actor);

// Arm/disarm the whole lane. False in single-player: the game must keep its own damage.
void SetActive(bool active);

// Did all seven delegates register? A lane that never armed runs INERT by design.
bool Armed();

// Session totals, for the teardown log. `armed` says whether all seven registered -- a guard that
// never armed and a guard that armed but never fired look identical in a counter alone, and the
// 2026-08-30 run needed to tell them apart.
struct Counters {
    unsigned long long cancelled = 0;
    unsigned long long allowed   = 0;
    bool               armed     = false;
};
Counters ReadCounters();

}  // namespace coop::atv_hit_guard
