// ue_wrap/actors/save_record.h -- the Fstruct_save record: its POD form, its engine codec,
// and the TArray primitives both are built on.
//
// Principle-7 engine-wrapper layer. Extracted from ue_wrap/actors/inventory.cpp 2026-07-22
// when the container-contents lane (coop/props/container_contents_sync -- take-4 R11) needed
// the SAME codec: a container's contents are a TArray<Fstruct_save> living in the global
// saveSlot.GObjStack, i.e. the identical serialization currency as the player inventory
// (research/findings/inventory-items/votv-container-contents-gobjstack-RE-2026-07-22.md SS3).
// RULE 2: ONE implementation -- inventory.cpp consumes this, it does not keep a copy.
//
// THE KEY RE FACT (votv-inventory-impl-plan-2026-06-14.md): a saved item is DATA, not an
// actor -- a Fstruct_save record (class + transform + key + typed key/value groups). Reading
// it = walking the nested TArrays; the value-group arrays are TArray<Fstruct_mX> where each
// Fstruct_mX wraps a single TArray<X> (-> a vector<vector<X>>), except `signals` which is a
// TArray<Fstruct_signalDataDynamic> directly. FNames/UClasses are read as STRINGS (non-
// portable across peers as pointers; re-interned / FindClass'd on apply).
//
// Game thread (the write side mints FStrings + interns FNames through the engine).

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"  // Row -- the 0x70 signal sub-element (reused, not reinvented)

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_record {

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

// ARRAY ELEMENT STRIDE = the 16-ALIGNED struct size, NOT the dump's raw `Size: 0xF8` line.
// Fstruct_save embeds an FTransform (16-aligned FQuat) at 0x10, so the struct's alignment is 16
// and its TArray element stride / embedded-field size is Align(0xF8,16) = 0x100. This is the
// SAME value the SDK reports for every embedded Fstruct_save field (struct_equipment.hpp data
// @0x10..0x110 = 0x100; saveSlot.hpp:38 drone size 0x100; dirthole_item/drone/eriePlush/kerfur).
// The engine's native loadObjects() iterates inventoryData at THIS stride; a mismatch makes it
// read element i+1's class at the wrong byte -> wild UClass* deref + heap over-read. (Using the
// raw 0xF8 was a real N>=2 crash, caught by the 2026-06-14 adversarial verify panel.) The dump's
// bottom-line `Size:` is PropertiesSize; the per-field embedded `size:` annotation is the stride.
// See [[feedback-tarray-stride-aligned-not-raw-size]].
inline constexpr int32_t kSaveStride   = 0x100;
inline constexpr int32_t kMxStride     = 0x10;  // Fstruct_mX wraps a single TArray<X> @ +0; already 16-aligned
inline constexpr int32_t kSignalStride = 0x70;  // Fstruct_signalDataDynamic; no 16-aligned member

// A corrupt/uninitialized TArray Num must never drive a runaway loop or a giant reserve.
inline constexpr int32_t kMaxArr = 200000;

// ---- TArray primitives (shared by every struct-array walk) ----------------------------------

// A pointer that could plausibly be a live UObject/heap buffer: non-null, above the null page,
// 8-aligned, inside the Win64 user-address range. Rejects garbage misread as a pointer BEFORE
// the engine IsLive/NameOf derefs it (a wild pointer faults the whole game-thread tick).
inline bool PlausibleObjPtr(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && (v & 0x7) == 0 && v < 0x7FFFFFFFFFFFull;
}

// A read TArray header, with a garbage header rejected to {nullptr, 0}.
struct Arr { const uint8_t* data = nullptr; int32_t num = 0; };
Arr ReadArr(const void* base, int32_t off);

// Write a UE TArray header {Data, Num, Max=Num} at base+off. (Max==Num is correct for a
// freshly-built array the engine will read then, on its next mutation, realloc wholesale.)
void WriteArrHeader(void* base, int32_t off, void* data, int32_t num);

// Allocate `count*elemSize` engine bytes, zeroed. Null (-> caller writes an empty array) on a
// zero count or an EngineAlloc failure (GMalloc unresolved). Zeroing makes every unset FString/
// FName/TArray field a valid empty (null,0,0) -- no garbage the engine could deref.
void* AllocZeroed(size_t count, size_t elemSize);

// FName read/write at a raw field address (empty string <-> NAME_None).
std::wstring ReadFNameAt(const void* base, int32_t off);
void WriteFNameField(void* dst, const std::wstring& leaf);

// ---- The record codec ------------------------------------------------------------------------

// Read one Fstruct_save at `base` into `rec`. Pure field reads + FName::ToString / FString copy
// (no UFunction dispatch). `rec` is overwritten field-by-field; pass a fresh record.
void ReadSaveRecord(const void* base, SaveRecord& rec);

// Write one Fstruct_save into the ZEROED 0x100 slot at `base`, constructing engine-OWNED
// TArrays via reflection::EngineAlloc (FNames interned, FStrings engine-minted, UClasses
// FindClass'd). The slot MUST be zeroed by the caller (AllocZeroed); any previous buffers it
// referenced are intentionally orphaned -- recursively freeing nested Fstruct_save sub-arrays +
// FString buffers is far more crash-prone than a bounded leak, and the engine never double-frees
// a buffer it has lost the pointer to. Buffers WE allocate are GMalloc-owned (EngineAlloc), so
// the engine's later Array realloc / GC free of them is allocator-matched.
void WriteSaveRecord(uint8_t* base, const SaveRecord& r);

}  // namespace ue_wrap::save_record
