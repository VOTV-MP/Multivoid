// coop/items/inventory_item_wire.h -- serialize ONE inventory entry (ue_wrap::inventory_item::Item) <-> bytes, in the
// coop/items/save_record_wire byte grammar. ONE implementation for every lane that carries an inventory entry: a world
// container's contents (container_slice_wire), a container act's put and its commit (container_act_wire).
//
// Layout: [wstr classPath][10 x f32 transform][wstr name][wstr tag][wstr key][wstr attachKey][u32 n][n bytes actorData]
// [i32 index][f32 posX][f32 posY][f32 rotation][u8 compartment][u8 amount]. FNames and the soft class ride as strings;
// actorData rides as the opaque bytes the game's ActorToBytes wrote.
//
// HOSTILE-INPUT CONTRACT: every read is bounds-checked and fails cleanly on overrun; actorData is held to
// inventory_item::kMaxActorData and to the bytes remaining before anything is allocated. Pure byte work: no engine
// access, callable off the game thread.

#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::inventory_item_wire {

// One entry's bytes, at most, when it travels on its own (a put's request, its commit): actorData's cap plus the
// strings of a real entry with room to spare.
inline constexpr size_t kMaxItemBytes = ue_wrap::inventory_item::kMaxActorData + 4096;
// The longest prop name a request may carry (the game's own are short): a longer one is refused before any engine call.
inline constexpr size_t kMaxNameChars = 128;

void SerItem(std::vector<uint8_t>& b, const ue_wrap::inventory_item::Item& it);
bool DeItem(const std::vector<uint8_t>& b, size_t& o, ue_wrap::inventory_item::Item& it);

// The entry's wire form, and its size.
std::vector<uint8_t> ItemWire(const ue_wrap::inventory_item::Item& it);
size_t ItemBytes(const ue_wrap::inventory_item::Item& it);

// `[u32 n][n bytes]` of one entry appended to `b`; false, with nothing appended, when the entry is past kMaxItemBytes.
bool AppItem(std::vector<uint8_t>& b, const ue_wrap::inventory_item::Item& it);

// `[u32 n][n bytes]` read at `o`; the entry must end exactly at its length. False, with `why` naming the first check
// that failed. `o` is advanced on success.
bool RdItem(const std::vector<uint8_t>& b, size_t& o, ue_wrap::inventory_item::Item& out, const char** why);

// The entry names a prop and the name is at most kMaxNameChars.
bool NameOk(const ue_wrap::inventory_item::Item& it);

}  // namespace coop::inventory_item_wire
