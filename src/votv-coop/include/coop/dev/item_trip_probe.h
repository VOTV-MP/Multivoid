// coop/dev/item_trip_probe.h -- [dev] an item's state across a trip into a player's inventory and back
// (VOTVCOOP_ITEM_TRIP_PROBE=client|host, VOTVCOOP_ITEM_TRIP_ITEM=can|ext|disc, VOTVCOOP_ITEM_TRIP_ROUTE=hand|inv), for
// Ghezra/Multivoid issues 33 and 34: the host spawns the item, one player uses it, takes it into its hand or its
// inventory and drops it, and both log the item's state and every hand mirror of it.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::item_trip_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::item_trip_probe
