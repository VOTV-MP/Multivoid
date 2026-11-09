// coop/dev/dig_hole_drill.h -- [dev] dig_drill=hole|hole_empty: a shovel's hole in the ground, judged on both peers.
//
// hole: once the client stands in its world the host digs a hole beside the client's body; the client takes a shovel
// into its hand and must see exactly one copy of it. The client then digs its own hole on its other side, the way
// useShovel spawns one, so its begin-play gate and the host's lane (coop/world/dug_hole_sync) take it as a player's
// dig: one hole must stand there, the host's. Last the client ends its copy of the host's hole, as a buried item does,
// and the host's must end. hole_empty: the hand stays empty and the client's dig must be refused: no hole on either
// peer. The host says the verdict (coop/session/rig_verdict, tag dig-drill); the client says FAIL when its own world
// disagrees. Called by coop/dev/dig_drill. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dig_hole_drill {

// Per gameplay tick while dig_drill is hole (`empty` false) or hole_empty (true). Game thread.
void Tick(coop::net::Session& session, bool empty);

}  // namespace coop::dev::dig_hole_drill
