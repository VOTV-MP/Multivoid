// coop/props/fireext_thrust_sync.h -- a fire extinguisher's runaway thrust is the host's, and the owner of the `shoot` seam.
//
// prop_fireExt_C::shoot is the runaway: a damage over 5, a fire's damage or a physics damage calls it, and it turns
// the thruster, its particles and its sound on, sets `active` and drains the charge until it is spent. Each peer
// judged its own copy's impacts, so a copy bumped on one machine alone flew off there and stood still on the others.
//
// The PRE of `shoot` decides what runs where. A remote hand mirror never runs it, on any peer. The host's own held
// extinguisher runs it as the game does, and a client's own held one never does (a hand actor has no element id and no
// host copy, so its damage reaches nobody). A client's other copy never starts the runaway itself: it asks the host
// (fireext_sync::AskRunaway), whose game verb runs the runaway on the host's copy, unless the call is the copy's own
// `loadData` (a record apply, which the host's record already covers). The host's `shoot` is the start of the lane:
// from that call it says the thrust on, claims the extinguisher into the driven-prop stream (coop/props/prop_drive_host)
// while `active` holds and charge remains, says the charge twice a second, and says it off when the charge is spent or the
// extinguisher is gone, then lets it coast to rest. A client's copy shows the thrust's particles and sound and takes the
// charge (fireext_sync::OnState), and sees the flight.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::fireext_thrust_sync {

// Both roles, pre-world: the session, and the watch on prop_fireExt_C::shoot (once per process).
void Install(coop::net::Session* s);

// Both roles, game thread, every gameplay tick, before prop_drive_host::Pump: resolves the extinguisher's class names
// (ue_wrap/actors/fire_extinguisher) once, a top-level pass as their resolve needs; then, on the host, streams the
// extinguishers in runaway thrust and says their charge. Returns at once when none thrusts.
void Tick(coop::net::Session* s);

// HOST, game thread: an extinguisher already thrusting joins the held list without being said (the join snapshot says it
// to the joiner). True when `ext` is held afterwards; false when the list is full, and off the host.
bool Adopt(void* ext);

// True once Tick has run in this process: the drill's question of whether the lane had its chance to resolve the names.
bool HasTicked();

// CLIENT, for the drill: how many `shoot` calls on a world copy the fireext_cancel_off red let run this session, the one
// count that says a client copy's own shoot ran (a thruster read after the call cannot), and how many the PRE cancelled.
uint32_t ClientShootsRun();
uint32_t ClientShootsCancelled();

// Forgets the extinguishers it held.
void OnDisconnect();

}  // namespace coop::fireext_thrust_sync
