// coop/dev/drone_cargo_take_drill.h -- [dev] a client's take from the delivery drone's cargo, on the 0.10 inventory
// screen's seat (coop/props/container_act_request: PRE inventoryComponent_C::removeInventoryItem, the drone's cargo named
// by container_target::kDroneCargoEid). Both peers run it with the same value (ini drone_cargo_take_drill, env
// VOTVCOOP_DRONE_CARGO_TAKE_DRILL=1); lines are tagged [DRONE-CARGO-DRILL].
//
//   HOST   -- once a client is world-ready, fills the drone's cargo with two copies of a record read off a world
//             container (the order's compileOrder is a day's wait; the cargo is the same entry array), says so in the
//             lobby chat, and then logs every change of the cargo's record count. When the client says its take
//             stood, it judges the cargo itself: one record fewer, the taken record's class gone from the count.
//             The dormant drone is parked tens of thousands of centimetres off, so the host first sets it down beside its
//             own player, who stands where the rig's client (join_at_host=1) stands: reach is judged on the host's copy.
//   CLIENT -- waits for the host's word and for its mirror of the cargo to hold the records, then moves the first record
//             into its bag the way the screen's drag does (the bag's
//             addItemAdvanced, then removeInventoryItem on the cargo's component). It reads cargo and bag after the
//             host has had time to answer, and again later: the cargo must hold one record fewer, the bag one more,
//             and both must still hold at the second read (a refusal would have given the entry back).
// The client's DONE line carries its verdict; the host's PASS or FAIL line is the authority's.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::drone_cargo_take_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The seeded cargo, the walk and the steps belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::drone_cargo_take_drill
