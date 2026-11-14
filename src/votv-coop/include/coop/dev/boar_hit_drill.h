// coop/dev/boar_hit_drill.h -- [dev] a client's swings at a boar's copy reach the host's boar.
//
// `boar_hit_drill=1`, BOTH peers. A boar is a prop whose graph a client's copy does not run, so the copy
// takes no damage of its own (coop/world/copy_graph_gate) and a client's swing is a hit intent the host
// judges (coop/creatures/hit_target_creature). The host spawns one gray boar in front of the client's
// puppet once the client stands in the world, holds it there within reach, and reads its health every two
// seconds. The client takes
// an axe and, once it holds a boar copy, calls the copy's addDamage with its own player as the damager
// every 700 ms, as a player's swings. `[BOAR-HIT-DRILL] [H] PASS` when the host has applied judged
// hits and its boar's health fell or the boar died; `FAIL` after ninety seconds otherwise, with the
// count of hits applied.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::boar_hit_drill {

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::boar_hit_drill
