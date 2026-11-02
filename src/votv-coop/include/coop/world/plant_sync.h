// coop/world/plant_sync.h -- one state for every plant: the host's.
//
// A growingPlant_C is a saved actor that a tick grows, so each peer's copy ran on its own water, sun and random rain. The host
// now runs every plant and sends its row, place and values as they change. A client's copy does not tick, grow or rain: its
// bodies are refused at the script gate (coop/world/plant_gate), so any change on it was this client's player: its water,
// its harvest, its removal or a seed it planted, each of which goes to the host as an edit. The host judges it
// (plant_edit_gate), applies what passes, and its next state answers. A plant is found by its save key, so what a join's
// load brought is already the host's. The winter bush and the mushrooms are classes under growingPlant_C, so they are
// plants here too, and their spawners are refused on a client (coop/world/spawn_authority), so the bushes are the host's.

#pragma once

#include "ue_wrap/actors/growing_plant.h"

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct PlantStatePayload;
struct WireClassName;
}  // namespace coop::net

// TRANSITIONAL (principle 4): the client's PlantEdit poll stands in for player-act intents, as the door's verbs are. RETIREMENT
// PLAN: they replace it once (1) a host-run verb's player-facing feedback (PlaySound2D, addHint) reaches the acting client
// through one shared seam, and (2) the hose and the bury lane are synced. Until then the poll carries only what the client's
// player changed (its simulation is refused at the gate, plant_gate), and the host takes an edit as a client's act could make
// it: a plant class, within the sender's reach (coop/element/intent_authority), every value bounded (coop/world/plant_edit_gate).
// A refused edit of a plant the host holds is answered with the host's state, so the client's copy returns to the host's value.
// A refused removal is not (the client skips the states of a plant it removed) and neither is a refused planting: the named
// limit of this path, reached only by an out-of-reach or cheating client, and ended by the intents that replace it.

namespace coop::plant_sync {

void Install(coop::net::Session* session);

// The poll, on either role.
void Tick();

// A client's receiver: a plant's state, or its end.
void OnState(const coop::net::PlantStatePayload& p);

// What a plant's wire state is here, for the host's edit gate (coop/world/plant_edit_gate): the class name it carries, the values
// it holds, and the plant it describes, spawned under `cls` (Materialize: game thread).
std::wstring StringFromClassName(const coop::net::WireClassName& c);
ue_wrap::growing_plant::Values ValuesOf(const coop::net::PlantStatePayload& p);
void* Materialize(const coop::net::PlantStatePayload& p, void* cls);

// How many harvest sounds this peer played for another player's harvest: the harvester's game plays its own.
uint32_t HarvestSoundsPlayed();

// Another player's harvest of `plant` (`key`): its own harvest sound, here. The host's edit gate calls it for a client's.
void PlayHarvestSound(void* plant, const std::wstring& key);

// Every plant again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// CLIENT, read-only, for drills: the host's last values for a plant that the inbox applied here, whatever this player did to the
// copy since. False when none was applied. Game thread.
bool LastHostValues(const std::wstring& key, ue_wrap::growing_plant::Values& out);

// Session end: what this peer held of the plants is forgotten.
void OnDisconnect();

}  // namespace coop::plant_sync
