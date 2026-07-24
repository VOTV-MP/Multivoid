// ue_wrap/inventory.h -- read/write the player-scoped inventory off the live UsaveSlot_C.
//
// Principle-7 engine-wrapper layer: it owns the saveSlot reflection + the Fstruct_save /
// Fstruct_equipment struct walk (offsets dump-authoritative; saveSlot.hpp inventoryData@0x02E0 /
// equipment@0x0440 / hold@0x0450). NOTE the TArray ELEMENT STRIDE is the 16-ALIGNED struct size
// (Fstruct_save 0x100, Fstruct_equipment 0x120 -- both embed a 16-aligned FTransform), NOT the
// dump's raw `Size:` line (0xF8 / 0x118); see the kSaveStride/kEquipStride comments in the .cpp.
// NO network logic -- coop/inventory_wire serializes the POD below; the host
// persistence + transport live in coop/player_inventory_sync.
//
// THE KEY RE FACT (votv-inventory-impl-plan-2026-06-14.md): an inventory item is DATA, not an
// actor -- a Fstruct_save record (class + transform + key + typed key/value groups). So
// reading it = walking the nested TArrays; the value-group arrays are TArray<Fstruct_mX> where
// each Fstruct_mX wraps a single TArray<X> (-> a vector<vector<X>>), except `signals` which is
// a TArray<Fstruct_signalDataDynamic> directly. FNames/UClasses are read as STRINGS (non-
// portable across peers as pointers; re-interned on the Inc-4 apply).

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
// the saveSlot is unresolvable. Pure field reads + FName::ToString (no UFunction dispatch).
// Game thread.
//
// WHAT THIS READS, precisely (measured 2026-07-24 -- read before building on it): the three
// arrays above are the SAVE-SIDE view. `inventoryData` in particular is a PROJECTION written by
// mainGamemode::saveObjects, and the cooked game contains NO gameplay reader for it at all (its
// only reader is the save-slot menu's repair routine). What a player actually carries lives in
// the LIVE store -- see ReadLivePersonalStore below. `equipment`/`hold` DO have live gameplay
// readers, so those two arrays mean what they look like; `inventory` does not.
// research/findings/inventory-items/votv-player-inventory-two-layer-RE-2026-07-24.md SS4.1.
bool ReadAll(PlayerInventory& out);

// ---- THE LIVE PERSONAL STORE (read-only) ------------------------------------------------------
//
// What the player is ACTUALLY carrying right now, as the game maintains it during play:
//   mainGamemode.playerContainer -> .propInventory -> saveSlot.GObjStack[.Index]
//
// Measured 2026-07-24 (votv-player-inventory-two-layer-RE-2026-07-24.md SS4.2/SS5.4):
//  - the player's inventory IS a container -- Aprop_inventoryContainer_player_C : Aprop_container_C
//    -- so its contents live in the ONE global saveSlot.GObjStack, addressed by propInventory.Index,
//    exactly like a world container's ([[lesson-container-contents-live-in-one-global-gobjstack]]);
//  - that slot is BAKED AT CONSTRUCTION, not restored: the class's component template
//    `propInventory_GEN_VARIABLE` serializes index=0, player=True, customVolume=50000 (the base
//    prop_container_C's template has no overrides at all). GObjStack[0] IS the player's inventory.
//    This is why the class's loadData override is an empty stub -- correct, not suspicious;
//  - both live write paths land here: the world pickup (mainPlayer::putObjectInventory2 ->
//    playerContainer.propInventory.addObject) and the container slot press (getObject -> addObject).
//
// `Player == true` is used here as the ADDRESS ASSERTION, and it is the same flag that
// coop/props/container_contents_sync's BOUNDARY 1 uses as a REFUSAL -- deliberately, from opposite
// sides of one boundary: that lane must never author a personal store, this reader must never read
// anything else. If the resolved component is NOT flagged personal we refuse rather than read the
// wrong container (fail-closed in this direction too).
//
// READ-ONLY BY CONSTRUCTION -- there is no live-store writer in this header, on purpose. What is
// done with these records (wire / persist / apply / arbitration) is gated on the open per-player
// scope questions in votv-per-player-inventory-scope-BRIEF-2026-07-24.md and is NOT built yet.
struct LivePersonalStore {
    int32_t slotIndex = -1;  // propInventory.Index -- measured to be 0 by construction
    std::vector<ue_wrap::save_record::SaveRecord> records;  // the live contents
};

// Read the live personal store into `out` (cleared first). False -- with `out` left empty -- if the
// gamemode / playerContainer / component / GObjStack slot is unresolvable, the index is out of
// range, or the component is not flagged personal. Pure field reads (no UFunction dispatch).
// Game thread.
//
// STATUS: AS-BUILT 2026-07-24, NOT VERIFIED. Exercised by ONE 60 s autonomous LAN smoke (both
// peers resolved GObjStack[0] and the client tracked a live take through it). No hands-on. See
// coop/dev/live_store_readout.h for the tested-vs-untested breakdown before trusting it for
// anything load-bearing.
bool ReadLivePersonalStore(LivePersonalStore& out);

// INCREMENT 4 -- the WRITE side (the live apply on join).
//
// Overwrite the player-scoped arrays (inventoryData / equipment / hold) on `saveSlot` with `inv`,
// constructing engine-OWNED TArrays via reflection::EngineAlloc (FNames interned, FStrings
// engine-minted, UClasses FindClass'd). This is the RULE-1 apply path: the caller writes the
// REGISTERED save object BEFORE the game's native loadObjects() materializes it on the next
// load, so the game's OWN code builds the live inventory from our data -- no live obj_11/TArray
// poke, no second reload, sidestepping all four mid-session live-apply unknowns.
//
// The PREVIOUS array buffers are intentionally orphaned (a bounded one-time-per-join leak of a
// few KB): recursively freeing the old nested Fstruct_save sub-arrays + FString buffers is far
// more crash-prone than leaking them, and the engine never double-frees a buffer it has lost the
// pointer to. Buffers WE allocate are GMalloc-owned (EngineAlloc), so the engine's later Array
// realloc / GC free of them is allocator-matched.
//
// Returns false if `saveSlot` is null/dead or GMalloc is unresolved (EngineAlloc returns null ->
// the arrays degrade to empty rather than corrupt). Game thread.
bool ApplyToSaveObject(void* saveSlot, const PlayerInventory& inv);

}  // namespace ue_wrap::inventory
