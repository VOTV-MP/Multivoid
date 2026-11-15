// coop/dev/atv_trunk_drill.h -- [dev] a client's take from and put into an ATV's trunk, and the host's own change of it,
// on the 0.10 inventory screen's seat (coop/props/container_act_request: PRE inventoryComponent_C::removeInventoryItem and
// addItemAdvanced; the trunk named by container_target::IsAtvTrunk, mirrored by coop/interactables/atv_trunk_sync). Both
// peers run it with the same value (ini atv_trunk_drill, env VOTVCOOP_ATV_TRUNK_DRILL=1) and join_at_host=1; lines are
// tagged [ATV-TRUNK-DRILL]. It MUTATES the first ATV's trunk.
//   HOST   -- once a client is world-ready, copies a record off a world container into the first ATV's trunk twice, sets
//             that ATV down beside its own player and says the trunk's target id and record count in the lobby chat. It
//             reads the trunk at each word of the client's (after the take one record fewer, after the put the count
//             back), then adds a record itself, which the client must see. Its PASS or FAIL line is the authority's.
//   CLIENT -- waits for its mirror of the trunk to hold the host's records. TAKE: it files the first record's copy in its
//             bag and removes the entry from the trunk's component, as the screen's drag does; the trunk must read one
//             fewer, the bag one more, at a second read too (a refusal would have given the entry back). PUT: it opens
//             the screen on the trunk and drops the bag item on the trunk's canvas as a mouse release does (the
//             screen's enddrag); the trunk must read the count again and the bag one fewer. HOST CHANGE: the record the
//             host added must be in its mirror. Its DONE line carries its verdict.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::atv_trunk_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The seeded trunk and the steps belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::atv_trunk_drill
