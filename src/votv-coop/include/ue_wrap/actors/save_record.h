// ue_wrap/actors/save_record.h -- the Fstruct_save record: its POD form, its engine codec,
// and the TArray primitives both are built on.
//
// Principle-7 engine-wrapper layer, and the one implementation of this codec: a container's
// contents are a TArray<Fstruct_save> in the global saveSlot.GObjStack, exactly what the player
// inventory is, so both lanes read it from here rather than each keeping a copy.
//
// The fact the whole file rests on: a saved item is DATA, not an actor -- a Fstruct_save
// record of class, transform, key and typed key/value groups. Reading one means walking the
// nested TArrays: each value group is a TArray<Fstruct_mX> whose element wraps a single
// TArray<X>, giving a vector of vectors, except `signals`, which is a TArray of
// Fstruct_signalDataDynamic directly. FNames and UClasses are read as STRINGS, since neither
// pointer is portable across peers; they are re-interned or resolved by name on apply.
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

// ARRAY ELEMENT STRIDE is the 16-ALIGNED struct size, not the dump's raw `Size:` line.
// Fstruct_save embeds an FTransform, whose FQuat is 16-aligned, so the struct's own alignment
// is 16 and its element stride is the size rounded up to that -- which is what the SDK reports
// for every embedded Fstruct_save field. The engine's native loadObjects() walks inventoryData
// at THIS stride, so using the unrounded size reads element i+1's class at the wrong byte: a
// wild class pointer and a heap over-read, which is a crash from the second element onward.
// In a dump the bottom-line `Size:` is PropertiesSize; the per-field embedded `size:` is the
// stride.
inline constexpr int32_t kSaveStride   = 0x100;
// The record's own FIELD span: the last value group is a TArray, and every field ends inside this
// many bytes. The stride above is that rounded up to the struct's 16 alignment, and the difference
// is real padding. This is the bound a parameter frame is checked against before the codec reads or
// writes a record inside it -- the bytes past the span belong to whatever parameter the engine laid
// down next, so a frame with less than this much room after the offset is refused.
inline constexpr int32_t kSaveRecordBytes = 0xF8;
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

// ---- The GAME's own codec, on a LIVE actor --------------------------------------------------
//
// The two functions above read and write a record at a raw address. The two below get one FROM a
// live actor and give one back TO it, through the actor's own `getData` and `loadData`. That is
// the difference between a set of carried fields WE enumerate per class and the one the game
// serializes: a class that adds save state is carried without a line of our code changing.
//
// Both climb to the most-derived declaration, because reflection::FindFunction is exact-owner and
// the override is the whole point -- Aprop_floppyDisc_C::getData calls its parent's and then
// appends the disc's `data` and `readWrites`, so a call resolved at Aprop_C would return the base
// record and silently drop exactly the state this exists to carry.

// True when `actor`'s class declares a `getData` of its own somewhere below Aprop_C -- i.e. it
// serializes state beyond the base prop record. Resolved from the live class chain and cached per
// UClass, so it is a membership test the GAME answers; a hand-written class list would go stale
// the first time VOTV adds a save-backed prop. False for a non-prop or an unresolvable class.
bool OverridesGetData(void* cls);

// Capture `actor`'s save record as the actor itself serializes it. False when `getData` does not
// resolve or the call fails.
bool CaptureRecord(void* actor, SaveRecord& out);

// Hand a record back to `actor` through its own `loadData` -- the LEAF half of it only.
//
// Aprop_C::loadData restores the save Key, the two names, the four saved bools, the scale and the
// lifespan from whatever record it is handed, then re-runs `init()`, `physicsImpact->init()` and
// `setNametag()`. Taken at face value that is a primitive for rewriting any prop's identity and for
// destroying it through SetLifeSpan, and every one of those fields already rides the prop's spawn
// row, which is their authority. So the receiver's OWN base record is read first and spliced over
// the incoming one: what survives from `r` is exactly the groups Aprop_C::getData does not write,
// which is the leaf class's own save state and the only thing this pair exists to move. Which
// groups those are is read off the base class at runtime, never listed here. Two dispatches: the
// base capture and the apply.
bool ApplyRecord(void* actor, const SaveRecord& r);

// Drop the cached class lookups (level change / disconnect).
void ResetCodecCache();

}  // namespace ue_wrap::save_record
