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

#include "ue_wrap/desk/signal_dynamic.h"  // Row -- the 0x70 signal sub-element (reused, not reinvented)

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory {

// One Fstruct_save record as engine-agnostic POD (the wire layer serializes this).
struct SaveRecord {
    std::wstring className;          // TSubclassOf<AActor> leaf name; empty = null class
    std::array<float, 10> xform{};   // FTransform: quat(x,y,z,w) + loc(x,y,z) + scale(x,y,z)
    std::wstring key;                // FName
    // Value groups: TArray<Fstruct_mX> (each Fstruct_mX = TArray<X>) -> vector<vector<X>>.
    std::vector<std::vector<uint8_t>>               bools;
    std::vector<std::vector<float>>                 floats;
    std::vector<std::vector<int32_t>>               ints;
    std::vector<std::vector<std::wstring>>          strings;
    std::vector<ue_wrap::signal_dynamic::Row>       signals;  // TArray<signal> directly (flat)
    std::vector<std::vector<std::wstring>>          classes;  // TSubclassOf leaf names
    std::vector<std::vector<std::array<float, 3>>>  vectors;  // FVector
    std::vector<std::vector<std::array<float, 3>>>  rotators; // FRotator (pitch,yaw,roll)
    std::vector<std::vector<std::array<float, 10>>> transforms; // FTransform packed
    std::vector<std::vector<uint8_t>>               bytes;
    std::vector<std::vector<std::wstring>>          names;    // FName
};

// One Fstruct_equipment record (a propDynamic id + an embedded Fstruct_save + a tag).
struct EquipRecord {
    std::wstring propName;  // Fstruct_propDynamic.name FName
    std::wstring propKey;   // Fstruct_propDynamic.key FName
    SaveRecord   data;      // embedded Fstruct_save (occupies a 0x100 slot)
    std::wstring tag;       // FName
};

// The full player-scoped inventory snapshot.
struct PlayerInventory {
    std::vector<SaveRecord>  inventory;  // saveSlot.inventoryData
    std::vector<EquipRecord> equipment;  // saveSlot.equipment (worn)
    std::vector<EquipRecord> hold;       // saveSlot.hold (hands)
};

// Resolve the live UsaveSlot_C (via mainGamemode_C.saveSlot). Cached + IsLive-revalidated;
// re-walks on a level transition. null if not yet resolvable. Game thread.
void* ResolveSaveSlot();

// Read the player-scoped inventory off the live saveSlot into `out` (cleared first). False if
// the saveSlot is unresolvable. Pure field reads + FName::ToString (no UFunction dispatch).
// Game thread.
bool ReadAll(PlayerInventory& out);

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
