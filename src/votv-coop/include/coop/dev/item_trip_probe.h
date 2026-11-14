// coop/dev/item_trip_probe.h -- [dev] an item's state across a trip into a player's inventory and back
// (VOTVCOOP_ITEM_TRIP_PROBE=client|host, VOTVCOOP_ITEM_TRIP_ITEM=can|ext), for Ghezra/Multivoid issues 33 and 34: the
// host spawns the item, one player uses it, takes it into its inventory and drops it, and both log the item's state.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::item_trip_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::item_trip_probe
