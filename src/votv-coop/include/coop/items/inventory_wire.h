// coop/items/inventory_wire.h -- serialize the per-player inventory POD <-> a byte blob.
//
// Turns ue_wrap::inventory::PlayerInventory (read off the live saveSlot by ue_wrap/inventory)
// into a self-contained, version-prefixed little-endian byte blob, and back. FNames and UClasses
// are wired as STRINGS -- pointers are not portable -- and re-interned or FindClass'd on apply,
// with the exact case preserved. The FTransform packs as 10 floats, and the 0x70 signal
// sub-element reuses the coop/signal_wire serializer rather than a second one.
//
// The blob is what coop/blob_chunks carries in chunks (client->host), what persists to
// <save>/coop_players/<guid>.json, and what the join apply reads back.

#pragma once

#include "ue_wrap/actors/inventory.h"  // PlayerInventory

#include <cstdint>
#include <vector>

namespace coop::inventory_wire {

// Current blob format version (the first byte). Bump on any layout change.
inline constexpr uint8_t kVersion = 1;

// Serialize `inv` into a fresh blob (always succeeds; bounded by the inventory size).
std::vector<uint8_t> Serialize(const ue_wrap::inventory::PlayerInventory& inv);

// Parse `blob` back into `out` (cleared first). False on a truncated / malformed / wrong-
// version blob (a corrupt or hostile blob must never over-read or over-allocate).
bool Deserialize(const std::vector<uint8_t>& blob, ue_wrap::inventory::PlayerInventory& out);

}  // namespace coop::inventory_wire
