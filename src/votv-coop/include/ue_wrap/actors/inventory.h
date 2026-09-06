// ue_wrap/inventory.h -- read/write the player-scoped inventory off the live UsaveSlot_C.
//
// Engine-wrapper layer: it owns the saveSlot reflection and the Fstruct_save /
// Fstruct_equipment struct walk. The field offsets and the TArray element strides are named
// constants in the .cpp, which is also where the stride's one trap is explained -- the stride
// is the 16-ALIGNED struct size, not the dump's raw `Size:` line. No network logic here:
// coop/inventory_wire serializes the POD below, and the host persistence and transport live
// in coop/player_inventory_sync.
//
// An inventory item is DATA, not an actor -- an Fstruct_save record (class, transform, key,
// typed key/value groups) -- so reading one means walking the nested TArrays. The value-group
// arrays are TArray<Fstruct_mX> where each Fstruct_mX wraps a single TArray<X>, giving a
// vector of vectors, except `signals`, which is a TArray<Fstruct_signalDataDynamic> directly.
// FNames and UClasses are read as STRINGS: as pointers they do not carry across peers, so the
// apply re-interns them.

#pragma once

#include "ue_wrap/actors/save_record.h"  // SaveRecord + its engine codec (the shared Fstruct_save home)

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory {

// One Fstruct_equipment record (a propDynamic id + an embedded Fstruct_save + a tag).
struct EquipRecord {
    std::wstring propName;  // Fstruct_propDynamic.name FName
    std::wstring propKey;   // Fstruct_propDynamic.key FName
    ue_wrap::save_record::SaveRecord data;  // embedded Fstruct_save (occupies a 0x100 slot)
    std::wstring tag;       // FName
};

// The full player-scoped inventory snapshot.
struct PlayerInventory {
    std::vector<ue_wrap::save_record::SaveRecord> inventory;  // saveSlot.inventoryData
    std::vector<EquipRecord> equipment;  // saveSlot.equipment (worn)
    std::vector<EquipRecord> hold;       // saveSlot.hold (hands)
};

// Resolve the live UsaveSlot_C (via mainGamemode_C.saveSlot). Cached + IsLive-revalidated;
// re-walks on a level transition. null if not yet resolvable. Game thread.
void* ResolveSaveSlot();

// Read the player-scoped inventory off the live saveSlot into `out` (cleared first). False if
// the saveSlot is unresolvable. Pure field reads plus FName::ToString, no UFunction dispatch.
// Game thread.
//
// WHAT THIS READS, precisely: the three arrays above are the SAVE-SIDE view. `inventoryData`
// is a PROJECTION written by mainGamemode::saveObjects, and the cooked game has no gameplay
// reader for it at all -- its only reader is the save-slot menu's repair routine. What a
// player actually carries lives in the LIVE store, ReadLivePersonalStore below.
// `equipment` and `hold` DO have live gameplay readers, so those two mean what they look
// like; `inventory` does not.
bool ReadAll(PlayerInventory& out);

// ---- THE LIVE PERSONAL STORE (read-only) ------------------------------------------------------
//
// What the player is ACTUALLY carrying right now, as the game maintains it during play:
//   mainGamemode.playerContainer -> .propInventory -> saveSlot.GObjStack[.Index]
//
// The player's inventory IS a container (Aprop_inventoryContainer_player_C derives from
// Aprop_container_C), so its contents live in the ONE global saveSlot.GObjStack addressed by
// propInventory.Index, exactly like a world container's. That slot is BAKED AT CONSTRUCTION
// rather than restored: the class's component template serializes index=0, player=true,
// customVolume=50000, so GObjStack[0] IS the player's inventory -- which is why the class's
// loadData override is an empty stub. Both live write paths land here: the world pickup
// (mainPlayer::putObjectInventory2 -> playerContainer.propInventory.addObject) and the
// container slot press (getObject -> addObject).
struct LivePersonalStore {
    int32_t slotIndex = -1;  // propInventory.Index -- measured to be 0 by construction
    std::vector<ue_wrap::save_record::SaveRecord> records;  // the live contents
};

// Read the live personal store into `out` (cleared first). False, with `out` left empty, if
// the gamemode, playerContainer, component or GObjStack slot is unresolvable, the index is
// out of range, or the component is not flagged personal. Pure field reads, no UFunction
// dispatch. Game thread.
//
// `Player == true` is the ADDRESS ASSERTION here, and the same flag is a REFUSAL in
// coop/props/container_contents_sync -- deliberately, from opposite sides of one boundary:
// that lane must never author a personal store, and this reader must never read anything
// else. READ-ONLY BY CONSTRUCTION: there is no live-store writer in this header, and nothing
// wires these records to the network yet. coop/dev/live_store_readout.h says what has and
// has not been exercised.
bool ReadLivePersonalStore(LivePersonalStore& out);

// The WRITE side: the apply on join.
//
// Overwrite the player-scoped arrays (inventoryData, equipment, hold) on `saveSlot` with
// `inv`, constructing engine-OWNED TArrays through reflection::EngineAlloc (FNames interned,
// FStrings engine-minted, UClasses FindClass'd). The caller writes the REGISTERED save object
// BEFORE the game's native loadObjects() materializes it on the next load, so the game's own
// code builds the live inventory from our data: no live TArray poke, no second reload.
//
// The PREVIOUS array buffers are intentionally orphaned, a bounded few kilobytes once per join:
// recursively freeing the old nested Fstruct_save sub-arrays and FString buffers is far more
// crash-prone than leaking them, and the engine never double-frees a buffer it has lost the
// pointer to. Buffers WE allocate are GMalloc-owned, so the engine's later realloc or GC free of
// them is allocator-matched. Returns false if `saveSlot` is null or dead, or GMalloc is
// unresolved -- EngineAlloc then returns null and the arrays degrade to empty. Game thread.
bool ApplyToSaveObject(void* saveSlot, const PlayerInventory& inv);

}  // namespace ue_wrap::inventory
