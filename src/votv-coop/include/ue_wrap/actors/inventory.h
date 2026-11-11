// ue_wrap/actors/inventory.h -- the local player's items as the game holds them in play, and the save object.
//
// The player's items live on the pawn: mainPlayer_C.inventoryComponent_player, whose slotArray_A holds the hotbar
// (slotArray_A[i] for the UI's hotbarSlots[i]), canvasInventory the bag (what mainPlayer_C.addObjectToInventory fills,
// as a container's canvasInventory holds its contents) and slotArray_B the worn
// equipment, each a TArray of struct_inventoryItem (ue_wrap/actors/inventory_item.h). The hand actors are spawned
// from a slot and stay backed by it (better_holdItem_mainhand / _offhand). What persists of them is the player's
// own save record (ue_wrap/actors/player_record.h). Engine-wrapper layer: no network logic. Game thread.

#pragma once

#include "ue_wrap/actors/inventory_item.h"  // Item, the whole entry ReadLiveItems hands out
#include "ue_wrap/actors/save_record.h"  // SaveRecord, the form the readers below hand an entry out in

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory {

// Resolve the live UsaveSlot_C (via mainGamemode_C.save_slot). Cached + IsLive-revalidated;
// re-walks on a level transition. null if not yet resolvable. Game thread.
void* ResolveSaveSlot();

// The player's entries, each as a save_record::SaveRecord holding what an inventory entry has: the class leaf,
// the transform, the key, and the item's name and tag as the first names group (where Aprop_C::getData puts
// them, so save_record::RecordName reads the name). Empty entries (a slot the game blanked) are skipped.
struct PlayerInventory {
    std::vector<ue_wrap::save_record::SaveRecord> inventory;  // slotArray_A (the hotbar), then canvasInventory (the bag)
    std::vector<ue_wrap::save_record::SaveRecord> equipment;  // slotArray_B: worn
};

// Read what the local player carries and wears into `out` (cleared first). False before the pawn, its inventory
// component or the entry layout resolves. Field reads only. Game thread.
bool ReadAll(PlayerInventory& out);

// What the player carries right now (the hotbar and the bag), for the dev drills that look for an item by class or key.
struct LivePersonalStore {
    std::vector<ue_wrap::save_record::SaveRecord> records;
};

// Read the carried entries into `out` (cleared first). False, with `out` empty, when ReadAll's would be. Game
// thread.
bool ReadLivePersonalStore(LivePersonalStore& out);

// The carried entries whole, actorData included, which the SaveRecord form leaves out: the hotbar into `hotbar`
// (slotArray_A) and the bag into `bag` (canvasInventory), each cleared first and either may be null. For a test that
// asks whether an item's own saved state went into the pocket with it. False when ReadLivePersonalStore's would be.
// Game thread.
bool ReadLiveItems(std::vector<ue_wrap::inventory_item::Item>* hotbar, std::vector<ue_wrap::inventory_item::Item>* bag);

// How many non-empty entries the player carries RIGHT NOW. -1 when unresolvable. Game thread.
int32_t LivePersonalStoreCount();

// Which of the player's two hands.
enum class Hand : uint8_t { Main = 0, Off = 1 };

// The item name the hand holds: mainPlayer_C.holdingObjectName_main / _offhand, the name of the hotbar entry the
// hand actor was spawned from (better_holdItem_mainhand copies it from the entry), and the key the player looks the
// game's item tables up by (list_weapons, list_props). Empty when the hand holds nothing or it cannot be read.
// Game thread.
std::wstring ReadHeldItemName(Hand hand = Hand::Main);

// A property of the worn item of class `classLeaf` takes `value` (its text form) inside the item's own saved
// stream, where the game itself keeps a worn item's state: the flashlight's charge is `battery` in its entry's
// actorData, which ticker_playerFlashlight drains. For a test that sets that state. False when no such item is
// worn, its class is not loaded, or the write fails. Game thread.
bool WriteWornItemProperty(const wchar_t* classLeaf, const wchar_t* prop, const std::wstring& value);

// The same property read back, as its text form: the worn item of class `classLeaf`'s actorData. False when it is not found or does not read.
bool ReadWornItemProperty(const wchar_t* classLeaf, const wchar_t* prop, std::wstring& value);

}  // namespace ue_wrap::inventory
