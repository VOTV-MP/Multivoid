// coop/props/fireext_thrust_sync.h -- a fire extinguisher's runaway thrust is the host's.
//
// prop_fireExt_C::shoot is the runaway: a damage over 5, a fire's damage or a physics damage calls it, and it turns
// the thruster, its particles and its sound on, sets `active` and drains the charge until it is spent. Each peer
// judged its own copy's impacts, so a copy bumped on one machine alone flew off there and stood still on the others.
//
// A client's copy never starts the runaway: its `shoot` is refused. The host's does, and from that call the host
// claims the extinguisher into the driven-prop stream (coop/props/prop_drive_host) while `active` holds and charge
// remains, then lets it coast to rest. A client sees the flight, without the thrust's particles and sound.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::fireext_thrust_sync {

// Both roles, pre-world: the session, and the watch on prop_fireExt_C::shoot (once per process).
void Install(coop::net::Session* s);

// Host, game thread, every gameplay tick, before prop_drive_host::Pump. Returns at once when none thrusts.
void Tick(coop::net::Session* s);

// Forgets the extinguishers it held.
void OnDisconnect();

}  // namespace coop::fireext_thrust_sync
