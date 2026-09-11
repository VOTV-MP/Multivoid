// coop/dev/hand_drop_selftest.h -- a prop through a player's HAND and back out, driven
// (`VOTVCOOP_HAND_DROP_SELFTEST=1` for one run).
//
// An idle two-peer run never puts a prop in a hotbar hand, so the seam where a held item is
// released back into the world -- the one path that broadcasts a spawn while the other peer still
// shows a display mirror of that same item at that same place -- is invisible to it. This drives
// the game's own two verbs on a per-role timer: `Hold Object` with the target passed directly,
// then `throwHoldingProp`, first with the host holding and then with the client holding, so both
// directions are measured rather than one.
//
// The verdict is the WATCHER's, and it needs no key: each episode censuses the live actors of the
// target's class near the holder, excluding the hand axis, on both peers and at three moments --
// before the pickup, while it is held, and after the drop. A prop that comes back is one row
// restored; a prop the receiving peer lost is a census that stays short, which is the symptom a
// field log recorded as a pose stream addressing an actor that no longer exists.
//
// It MUTATES the world: it picks up and drops a prop that was standing there. Armed per run from
// the environment, never left standing in an ini.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hand_drop_selftest {

// Cache the session pointer. Call once at boot. No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Fires the step due for this role and censuses on both. No-op with the
// flag off or before both peers are in the world.
void Tick();

// Print what each episode did, including the ones that never fired. Called on the periodic census
// and at teardown, since the rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Clear the schedule and the target so a reconnect re-runs the episodes.
void OnDisconnect();

}  // namespace coop::dev::hand_drop_selftest
