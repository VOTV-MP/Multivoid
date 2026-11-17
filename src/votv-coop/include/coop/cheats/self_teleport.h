// coop/cheats/self_teleport.h -- moving this machine's OWN body: beside another player, or out of a place it is stuck
// in. A player's body is its own machine's to place (its pose is what the other peers see), so this writes nothing of
// the shared world: the "Player tools" panel's Teleport to host (offered only to a player who holds the cheats right)
// and Unstuck, and the host's Teleport me to a player. The move is the game's own teleport
// (coop/session/teleport_client::ApplyLocally), onto the NavMesh point nearest the wanted place, so a body is never
// put inside a wall; the view's pitch and roll are kept. Any thread; the move is posted to the game thread.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::cheats::self_teleport {

// Put this machine's body 1 m behind the player in `slot` (0 = the host), facing where they face, on the NavMesh
// nearest that point. Logs and does nothing when that player has no pose here yet; tells the player and does not move
// when no NavMesh lies near the point.
void ToPlayer(int slot);

// Move this machine's body to the NavMesh point nearest where it stands, to put it back on a floor. Tells the player
// and does not move when no NavMesh lies near.
void Unstuck();

// How many moves this process landed on the NavMesh, how many came to nothing (no NavMesh near, or the game's teleport
// call reported failure), and where the last landing put the body's centre, for the cheat drill to wait on and to
// check the body against. Any thread.
struct Landings { unsigned landed; unsigned refused; ue_wrap::FVector target; };
Landings Outcomes();

}  // namespace coop::cheats::self_teleport
