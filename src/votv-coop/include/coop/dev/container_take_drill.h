// coop/dev/container_take_drill.h -- [dev] a client's take from a shared world container, on the 0.10 inventory screen's
// seat (coop/props/container_act_request: PRE inventoryComponent_C::removeInventoryItem). Both peers run it with the same
// value (ini container_take_drill, env VOTVCOOP_CONTAINER_TAKE_DRILL=1); lines are tagged [CTAKE-DRILL].
//
//   CLIENT -- once its join is over, picks the world container a nav route reaches that holds at least two records, walks
//             there with the director, opens it on the inventory screen through the game's own opencontainer, and moves
//             its first record into the bag the way the screen's drag does: the bag's addItemAdvanced with the record, then
//             removeInventoryItem on the container's component. It then reads its own container and bag after the host
//             has had time to answer, and again later: the container must hold one record fewer, the bag one more, and
//             both must still hold at the second read (a refusal would have given the entry back).
//   HOST   -- logs every change of a world container's record count, which is the host's truth of the take.
// The client's DONE line carries the verdict; the host's lines are read beside it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_take_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The container, the walk and the steps belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_take_drill
