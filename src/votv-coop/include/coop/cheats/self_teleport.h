// coop/cheats/self_teleport.h -- moving this machine's OWN body: beside another player, or out of a place it is stuck
// in. A player's body is its own machine's to place (its pose is what the other peers see), so this writes nothing of
// the shared world and a client may do it: the "Player tools" panel's Teleport to host and Unstuck, and the host's
// Teleport me to a player. The move is the game's own teleport (coop/session/teleport_client::ApplyLocally). Any
// thread; the move is posted to the game thread.

#pragma once

namespace coop::cheats::self_teleport {

// Put this machine's body 1 m behind the player in `slot` (0 = the host), facing where they face. Logs and does
// nothing when that player has no pose here yet.
void ToPlayer(int slot);

// Lift this machine's body 1.5 m straight up, to fall back onto whatever floor is there.
void Unstuck();

}  // namespace coop::cheats::self_teleport
