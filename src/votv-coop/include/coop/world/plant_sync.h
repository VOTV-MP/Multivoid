// coop/world/plant_sync.h -- one state for every plant: the host's.
//
// A growingPlant_C is a saved actor that a tick grows, so each peer's copy ran on its own water, sun and random rain. The host
// now runs every plant and sends its row, place and values as they change. A client's copy has its tick off, so any change on
// it was this client's player: its water, its harvest, its removal or a seed it planted, each of which goes to the host as an
// edit. The host applies it and its next state answers. A plant is found by its save key, so what a join's load brought is
// already the host's. The winter bush is the same class, and its spawner's tick is off on a client, so the bushes are
// the host's too.

#pragma once

namespace coop::net {
class Session;
struct PlantStatePayload;
}  // namespace coop::net

namespace coop::plant_sync {

void Install(coop::net::Session* session);

// The poll, on either role.
void Tick();

// A client's receiver: a plant's state, or its end.
void OnState(const coop::net::PlantStatePayload& p);

// The host's receiver: what a client's player did to a plant.
void OnEdit(const coop::net::PlantStatePayload& p);

// Every plant again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// A client's copies tick again, for the world it stays in.
void OnDisconnect();

}  // namespace coop::plant_sync
