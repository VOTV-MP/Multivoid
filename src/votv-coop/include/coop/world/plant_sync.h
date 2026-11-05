// coop/world/plant_sync.h -- one state for every plant: the host's.
//
// A growingPlant_C is a saved actor that a tick grows, so each peer's copy ran on its own water, sun and random rain. The host
// now runs every plant and sends its row, place and values as they change. A client's copy does not tick, grow or rain: its
// bodies are refused at the script gate (coop/world/plant_gate), so any change on it was this client's player: its water,
// its harvest, its removal or a seed it planted, each of which goes to the host as an edit. The host applies it and its
// next state answers. A plant is found by its save key, so what a join's load brought is already the host's. The winter bush
// and the mushrooms are classes under growingPlant_C, so they are plants here too, and their spawners are refused on a
// client (coop/world/spawn_authority), so the bushes are the host's.
//
// TRANSITIONAL (principle 4): the client's PlantEdit poll stands in for player-act intents, as the door's verbs are. RETIREMENT
// PLAN: they replace it once (1) a host-run verb's player-facing feedback (PlaySound2D, addHint) reaches the acting client
// through one shared seam, and (2) the hose and the bury lane are synced. Until then the poll carries only what the client's
// player changed (its simulation is refused at the gate, plant_gate), and the host takes an edit as a client's act could make
// it: a plant class, within the sender's reach (coop/element/intent_authority), every value bounded.

#pragma once

#include "ue_wrap/actors/growing_plant.h"

#include <cstdint>
#include <string>

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

// The host's receiver: what the client in `senderSlot` says its player did to a plant. The next poll applies it, and only after
// the class, the reach and the bounds are judged; an edit that waits for the sender's body stays queued, in a bounded queue per
// sender.
void OnEdit(uint8_t senderSlot, const coop::net::PlantStatePayload& p);

// HOST, what the edit gate did this session: plantings refused for a class that is no loaded plant class, edits refused for a
// plant or place outside the sender's reach, values cut to their bounds, edits that name no key, plantings of a row that is none
// of list_farmPlants' (or an edit that changes a plant to one) and plantings in a parent that is no pot the host holds. Read-only, for drills. Game thread.
struct Counts {
    std::uint64_t refusedClass = 0;
    std::uint64_t refusedReach = 0;
    std::uint64_t clamped = 0;
    std::uint64_t refusedKey = 0;
    std::uint64_t refusedRow = 0;
    std::uint64_t refusedParent = 0;
};
Counts GetCounts();

// Every plant again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// CLIENT, read-only, for drills: the host's last values for a plant that the inbox applied here, whatever this player did to the
// copy since. False when none was applied. Game thread.
bool LastHostValues(const std::wstring& key, ue_wrap::growing_plant::Values& out);

// Session end: what this peer held of the plants is forgotten.
void OnDisconnect();

}  // namespace coop::plant_sync
