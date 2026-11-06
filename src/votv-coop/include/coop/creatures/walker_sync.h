// coop/creatures/walker_sync.h -- the walking mannequins: one set, the host's.
//
// A walking mannequin is a prop and crosses as one, with its brain parked on a client and its spawner's tick off there; the
// host's drive stream carries its walking, and this poll holds it there. A walker freezes for any viewer: a client runs the
// walker's own view test on its copy and tells the host, which holds the walker's saw flag while any client has it on
// screen. An anger crosses both ways: a client's fire on its copy angers the host's walker, and the host's angry walker
// angers every copy. The host steers a walker toward the nearest player, a client's body as much as its own.

#pragma once

namespace coop::net {
class Session;
struct WalkerWatchPayload;
}  // namespace coop::net

namespace coop::walker_sync {

void Install(coop::net::Session* session);

// The poll, on either role.
void Tick();

// The host's receiver: a client's view of a walker.
void OnWalkerWatch(const coop::net::WalkerWatchPayload& p, int senderSlot);

// A client's receiver: a walker of the host's is angry.
void OnWalkerAngry(const coop::net::WalkerWatchPayload& p);

// Every angry walker again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// A client's spawners tick again and its walkers' brains run, for the world it stays in.
void OnDisconnect();

}  // namespace coop::walker_sync
