// coop/interactables/kitchen_sync.h -- the microwave's and the mini oven's controls on every peer.
//
// A press on either machine runs on the presser's copy, so without this the host's machine stayed off, and since the
// host is a food's one author (coop/props/prop_food_sync) a client's cooking came to nothing. Each peer polls the machines
// for a change its own player made and sends it; every other peer applies it through the machine's own start and
// repaint and primes its baseline. A microwave's natural end runs on each peer's own timer, so only its start crosses.

#pragma once

namespace coop::net {
class Session;
struct KitchenStatePayload;
}  // namespace coop::net

namespace coop::kitchen_sync {

void Install(coop::net::Session* session);

// Per gameplay tick: the poll, four times a second. Game thread.
void Tick();

// A peer changed a machine. Validated at the router; a client's copy is relayed at the host's receive edge.
void OnState(int senderSlot, const coop::net::KitchenStatePayload& p);

// HOST: a joiner reached its ready edge: every machine not at its defaults. An oven's controls are in no save.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

}  // namespace coop::kitchen_sync
