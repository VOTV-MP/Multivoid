// coop/items/inventory_wire.h -- serialize the per-player profile <-> a byte blob.
//
// Turns coop::player_profile::Profile (items, vitals and pose, read off the live save object
// and the pawn) into a self-contained, version-prefixed little-endian byte blob, and back. FNames and UClasses
// are wired as STRINGS -- pointers are not portable -- and re-interned or FindClass'd on apply,
// with the exact case preserved. The FTransform packs as 10 floats, and the 0x70 signal
// sub-element reuses the coop/signal_wire serializer rather than a second one.
//
// The blob is what coop/blob_chunks carries in chunks (client->host), what persists to
// <mod data dir>/coop_players/<hostSlot>/<guid>.json, and what the join apply reads back.

#pragma once

#include "coop/items/player_profile.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::inventory_wire {

// Current blob format version (the first byte). Bump on any change of layout OR meaning.
//   1 -- the inventory third was saveSlot.inventoryData, a save-side projection the game never
//        reads back; equipment and hold were live data. Still parsed (same layout), so the host
//        can lift a stored profile out of it; see Deserialize's `outVersion`.
//   2 -- the inventory third became what the player carries (saveSlot.GObjStack[0]); three
//        arrays and nothing else. Never released, and not parsed.
//   3 -- the vitals (behind a present byte) and the pose follow the three arrays.
inline constexpr uint8_t kVersion = 3;
inline constexpr uint8_t kVersionProjection = 1;

// Serialize `p` into a fresh blob (always succeeds; bounded by the inventory size).
std::vector<uint8_t> Serialize(const coop::player_profile::Profile& p);

// The same blob in its two halves, for a caller that polls: the items change when the player
// acts and the vitals every second, so the items half is built and hashed on its own, and the
// state half is appended only when the blob is going to be sent.
std::vector<uint8_t> SerializeItems(const ue_wrap::inventory::PlayerInventory& items);
void AppendState(std::vector<uint8_t>& blob, const ue_wrap::vitals::Snapshot* vitals,
                 const coop::player_profile::Pose& pose);  // null vitals = none were read

// Parse `blob` back into `out` (cleared first). False on a truncated / malformed / unknown-
// version blob (a corrupt or hostile blob must never over-read or over-allocate). With
// `outVersion` null only kVersion parses; with it set, a kVersionProjection blob parses too and
// the caller is told which it got -- its inventory third is then NOT what the player carried,
// and it has no vitals and no pose (`out` keeps default ones).
bool Deserialize(const std::vector<uint8_t>& blob, coop::player_profile::Profile& out,
                 uint8_t* outVersion = nullptr);

}  // namespace coop::inventory_wire
