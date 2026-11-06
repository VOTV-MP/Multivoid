// coop/world/plant_edit_gate.h -- the host's receive boundary for a client's plant edits.
//
// A client's PlantEdit is its player's act, so the host takes it as that act could make it: it names a key, a planting plants the
// class of its own list_farmPlants row under a key no other holds and names an empty pot the host holds in the sender's reach (or
// none), a changed row is one of those rows, every edit lies within the sender's reach and every value within bounds. The edits
// wait in a bounded queue per sender and are applied in the host's plant poll (coop/world/plant_sync), on the game thread.
//
// TRANSITIONAL (principle 4): this gate belongs to the client's PlantEdit poll and goes with it (plant_sync.h's retirement plan).

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::net {
class Session;
struct PlantStatePayload;
}  // namespace coop::net

namespace coop::plant_edit_gate {

// The host's receiver: what the client in `senderSlot` says its player did to a plant. It waits for the next poll's ApplyEdits.
void OnEdit(uint8_t senderSlot, const coop::net::PlantStatePayload& p);

// HOST, in its plant poll: judges and applies the queued edits against `index` (the host's plants by key; a planting is added,
// a removal erased). The keys of the plants an edit changed are appended to `changed`. An edit that waits for the sender's body
// stays at its queue head. Game thread.
void ApplyEdits(coop::net::Session& s, std::unordered_map<std::wstring, void*>& index, std::vector<std::wstring>& changed);

// The world changed: the edits queued for the old one are dropped, as are the senders' waits, for they named its plants and its
// bodies. Game thread.
void OnWorldChange();

// What the gate did this session: plantings refused because their row plants no plant class named as the wire names it, edits
// refused for a plant or place outside the sender's reach, values cut to their bounds, edits refused for their key, plantings of
// a row that is none of list_farmPlants' (or an edit that changes a plant to such a row, or to one of another class) and
// plantings in a parent that is no empty pot in the sender's reach at the planting. Read-only, for drills. Game thread.
struct Counts {
    std::uint64_t refusedClass = 0;
    std::uint64_t refusedReach = 0;
    std::uint64_t clamped = 0;
    std::uint64_t refusedKey = 0;
    std::uint64_t refusedRow = 0;
    std::uint64_t refusedParent = 0;
};
Counts GetCounts();

// Session end: say what the gate did and start the queues and counts again from zero.
void OnDisconnect();

}  // namespace coop::plant_edit_gate
