// coop/world/event_lanes.h -- the event lanes the session runs, in one place.
//
// The lanes that carry what an event moves and nobody holds: the props the anti-gravity event lifts
// (coop/world/agrav_sync), the props and clumps the trash prank flings (coop/world/trash_fling_sync) and the gray
// boars (coop/creatures/boar_sync), and with them an extinguisher in runaway thrust
// (coop/props/fireext_thrust_sync), each onto the driven-prop stream, so their ticks run before
// prop_drive_host::Pump. The teardown also ends the client's copy gates (coop/world/copy_graph_gate,
// coop/world/obelisk_copy), whose installs are pre-world (harness/session_runtime).

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::event_lanes {

// Both roles, every install tick.
void Install(coop::net::Session& session);

// Every gameplay tick, before prop_drive_host::Pump.
void Tick(coop::net::Session& session, bool isHost);

// Net disconnect.
void OnDisconnect();

}  // namespace coop::event_lanes
