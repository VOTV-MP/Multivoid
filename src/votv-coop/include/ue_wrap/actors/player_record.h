// ue_wrap/actors/player_record.h -- the player's own save record: what the game keeps of a player between
// sessions, read off the live pawn and read and written in a save object.
//
// The game saves the player as one struct_betterSave in saveSlot.betterObjectsData, like any other saved actor:
// mainPlayer_C::betterGetData writes its inventory component's SaveGame properties into `save_inventorydata`
// (ThingsAndStuffLib::ObjectToBytes) and then its own into the record's actorData, so the record holds the vitals
// (save_health, save_food...), the hotbar, the bag and the worn equipment. loadObjects hands the record back to the
// level's mainPlayer_C by its key (gatherDataFromKey: gather, not spawn), whose betterLoadData restores both. The
// record's bytes are the game's own stream and are carried whole; a named property inside them is read and written
// through the same native library the game's own saveSlot_C::reset_player_stats uses.
//
// Engine-wrapper layer (principle 7): no network or coop logic. Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap::player_record {

// The live pawn's record bytes as the game saves them: its own betterGetData, the actorData of the struct it
// returns. betterGetData first writes each held hand item's current state back into its hotbar slot
// (insertHandItemIntoHotbar_main/_offhand), as every save of the game does. False when the call does not resolve
// or fails, or the record comes back empty.
bool CaptureLocal(void* pawn, std::vector<uint8_t>& out);

// The pawn's inventory component as the game serialises it (ObjectToBytes of inventoryComponent_player): the
// hotbar, the bag and the worn equipment, without the vitals. Cheap enough to poll. False when it does not resolve.
bool InventoryBytes(void* pawn, std::vector<uint8_t>& out);

// The number of the player's records in `save`'s betterObjectsData: the entries whose class is mainPlayer_C. -1
// when the array or its element struct does not resolve.
int CountInSave(void* save);

// Every player record in `save` takes `bytes` as its actorData (the previous buffer freed); returns how many, -1
// when the array does not resolve. An empty `bytes` is refused (-1): a record with no stream is not one the game
// writes.
int WriteToSave(void* save, const std::vector<uint8_t>& bytes);

// The player records leave `save`'s betterObjectsData, the elements after them moved down; returns how many, -1
// when the array does not resolve. With none, loadObjects leaves the level's pawn as its class made it: the
// class's vitals and an empty inventory.
int RemoveFromSave(void* save);

// The mainPlayer_C class: found loaded, else loaded from the soft class path of a player record in `save` (which
// may be null). Null when neither works.
void* PlayerClass(void* save);

// Every player record in `save` takes the named property values, each as the property's text form, through
// ThingsAndStuffLib::SetPropertyInObjectBytes. Returns how many records were written; -1 when the class, the array
// or the native does not resolve, or a write fails (records written before it stay written).
int SetPropertiesInSave(void* save, const std::vector<std::pair<std::wstring, std::wstring>>& props);

// The property `prop` inside `bytes`, an object stream of class `cls` (a record's actorData), takes `value` (its
// text form) through ThingsAndStuffLib::SetPropertyInObjectBytes, `bytes` replaced by the result. False when the
// native does not resolve, `bytes` is empty or the write fails, `bytes` then unchanged.
bool SetPropertyInBytes(void* cls, std::vector<uint8_t>& bytes, const std::wstring& prop, const std::wstring& value);

// The property `prop` inside `bytes` (an object stream of class `cls`) as its text form, through ThingsAndStuffLib::
// GetPropertyFromObjectBytes. False when the native does not resolve, `bytes` is empty or the read fails.
bool GetPropertyInBytes(void* cls, const std::vector<uint8_t>& bytes, const std::wstring& prop, std::wstring& value);

// `save`'s startupEquipment takes its defaultStartupEquipment, the items the inventory's first construction gives
// a player who has no record (addStartupEquipment), through the game's own saveSlot_C::reset_player_equipment. It
// runs before the world exists, where that function has no gamemode to spawn its scratch player through: it then
// skips to the copy, and its hint has no gamemode to show on. So it must find a player record to reach the copy,
// and is called while one is there. False when the function does not resolve or the call fails.
bool CopyStartupEquipment(void* save);

}  // namespace ue_wrap::player_record
