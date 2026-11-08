// coop/dev/dig_drill.h -- [dev] dig_drill: a client's dig of a chip pile, judged on both peers.
//
// The host spawns a wood chip pile beside the client's body once the client stands in its world; the client finds
// it, takes a shovel into its hand (dig_drill=shovel) or keeps the hand empty (dig_drill=empty), and runs the pile's
// own turnIntoScrap, the call a shovel's or a rake's dig ends in, so the client's dig gate and the host's lane
// (coop/props/scrap_pile_intent) take it as a player's dig. Each peer then reads what stands where the pile stood:
// with a shovel the pile is gone and one scrap item lies there, with an empty hand the pile stands and no item does.
// The host says the verdict (coop/session/rig_verdict, tag dig-drill); the client says FAIL when its own copy
// disagrees. Off by default; a single cached read when off. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dig_drill {

// Per gameplay tick from dev_lanes. Game thread.
void Tick(coop::net::Session& session);

}  // namespace coop::dev::dig_drill
