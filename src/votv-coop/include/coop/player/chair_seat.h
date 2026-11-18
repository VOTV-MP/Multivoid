// coop/player/chair_seat.h -- a player sitting on a chair is seated on that chair on every peer, and the host owns who
// sits where.
//
// The game's sit (mainPlayer_C::sit, from a sitBox_C's use) attaches the sitter to the chair at the box's `sitting`
// location; its unsit stands it up again. Each peer reads its own player's `sittingOn` for those edges. HOST: it files
// its own sit and judges a client's (ChairSeatIntent): within reach, the chair free and held by no other player. It sends
// every seat to every peer (ChairSeatState), and tells a sitter to get up (op 2) when it refused the sit or another player
// takes the chair in hand. EVERY PEER: another player's body sits on its own copy of the chair through the seat owner
// (coop/player/puppet_seat), under the key `chair:<eid>`, in the AnimBP's seat pose, and moves with the chair. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ChairSeatPayload;
}  // namespace coop::net

namespace coop::chair_seat {

void Install(coop::net::Session* session);

// The local player's sit and stand edges; the host's chair-held test. Once per pump tick.
void Tick();

// HOST: a client's sit or stand. `slot` is the sender's.
void OnIntent(int slot, const coop::net::ChairSeatPayload& p);

// CLIENT: the host's seat change or its word to get up.
void OnState(const coop::net::ChairSeatPayload& p);

// HOST: every seat, to a joiner at its ready edge.
void SendJoin(int slot);

// HOST: the slot's player left; its seat goes with it.
void OnPeerLeft(int slot);

void OnDisconnect();

}  // namespace coop::chair_seat
