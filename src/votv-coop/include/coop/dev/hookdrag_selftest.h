// coop/dev/hookdrag_selftest.h -- a prop dragged by a hook, driven (`VOTVCOOP_HOOKDRAG_SELFTEST=1`
// for one run, on BOTH peers).
//
// An idle two-peer run never ties a hook to a prop, so the channel that carries a prop nobody is
// holding is never exercised. The planting peer -- the host, the client, or both, per the
// `hookdrag_role` row -- plants a hook into the nearest keyed physics prop with the game's own
// attach verb, hands it to the player the way the item does, walks the player out and back so the
// constraint drags the prop, then destroys the hook so the prop coasts and rests. A client's plant
// exercises the whole client direction: the host ties the client's hook on its mirror and the
// channel carries the prop back; both planting into one prop is two host constraints on one body.
// Both peers name the same target -- the nearest keyed physics prop to the host player at the arm
// -- and log its position on one clock, so the verdict is the distance between two series read
// out of two logs, and a run in which the two peers named different props says so instead of
// comparing strangers. It MUTATES the world (the prop is moved), so it is armed per run from the
// environment and never left standing in an ini.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hookdrag_selftest {

// Cache the session pointer. Call once at boot. No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Fires the step due for this role and samples the target on both. No-op
// with the flag off or before both peers are in the world.
void Tick();

// Print the target and how far it moved here. Called at the schedule's end and at teardown, since
// the rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Clear the schedule and the target so a reconnect re-runs the drag.
void OnDisconnect();

}  // namespace coop::dev::hookdrag_selftest
