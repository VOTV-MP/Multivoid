// coop/world/event_lanes.h -- lanes the session runs outside subsystems.cpp, in one place.
//
// subsystems.cpp sits at the size cap, so a lane added after the 0.10.0 port installs, ticks and tears down here.

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
