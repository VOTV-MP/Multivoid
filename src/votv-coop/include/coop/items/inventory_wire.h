// coop/inventory_wire.h -- serialize the per-player inventory POD <-> a byte blob.
//
// Increment 2b. Turns ue_wrap::inventory::PlayerInventory (read off the live saveSlot by
// ue_wrap/inventory) into a self-contained, version-prefixed little-endian byte blob, and
// back. FNames + UClasses are wired as STRINGS (non-portable as pointers; re-interned /
// FindClass'd on the Inc-4 apply, exact case preserved). The FTransform packs as 10 floats.
// The 0x70 signal sub-element reuses the proven coop/signal_wire serializer (no reinvention).
//
// The blob is what Increment 3 chunks over coop/blob_chunks (client->host) + persists to
// <save>/coop_players/<guid>.json, and what Increment 4 applies on join.

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
