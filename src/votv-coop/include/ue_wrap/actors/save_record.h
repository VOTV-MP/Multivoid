// ue_wrap/actors/save_record.h -- the Fstruct_save1 record: its POD form, its engine codec,
// and the TArray primitives it and the other struct codecs are built on.
//
// Principle-7 engine-wrapper layer. Fstruct_save1 is the game's OLD typed record, the one getData and
// loadData still pass: a class, a transform, a key and typed key/value groups. The game's own persistence is
// betterGetData / betterLoadData now (a prop's state rides ue_wrap/actors/prop_record, an inventory entry's
// ue_wrap/actors/inventory_item), so what this file serves is the lanes whose payload is the typed groups
// themselves: the hook anchor, which edits two of them, and the kerfur disc carry, which reads a disc's.
//
// A saved item is DATA, not an actor. Reading one means walking the nested TArrays: each value group is
// a TArray<Fstruct_mX> whose element wraps a single TArray<X>, giving a vector of vectors. FNames and UClasses
// are read as STRINGS, since neither pointer is portable across peers; they are re-interned or resolved by
// name on apply.
//
// Game thread (the write side mints FStrings + interns FNames through the engine).

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_record {

// How a class leaf name on a record resolves to a UClass as the record is written. A null resolver
// means reflection::FindClass, the authoritative walk-the-array-on-a-miss that is correct everywhere
// but costs a whole-array walk per unresolved name. A peer applying data IT RECEIVED FROM AN
// ATTACKER (the host taking a client's record) passes ue_wrap::object_index::ClassByName instead: a
// one-step lookup that answers a loaded class and a miss alike, so an attacker's unknown name costs a
// hash probe, not a 250k-object walk. A client applying the host's own
// record keeps the default: its object index can trail during a blocking world load, where the walk
// still finds a class the index does not list yet, and a cheating host is out of scope here.
using ClassResolver = void* (*)(const wchar_t*);

// One Fstruct_save1 record as engine-agnostic POD (the wire layer serializes this).
struct SaveRecord {
    std::wstring className;          // TSubclassOf<AActor> leaf name; empty = null class
    std::array<float, 10> xform{};   // FTransform: quat(x,y,z,w) + loc(x,y,z) + scale(x,y,z)
    std::wstring key;                // FName
    // Value groups: TArray<Fstruct_mX> (each Fstruct_mX = TArray<X>) -> vector<vector<X>>.
    std::vector<std::vector<uint8_t>>               bools;
    std::vector<std::vector<float>>                 floats;
    std::vector<std::vector<int32_t>>               ints;
    std::vector<std::vector<std::wstring>>          strings;
    std::vector<std::vector<std::wstring>>          classes;  // TSubclassOf leaf names
    std::vector<std::vector<std::array<float, 3>>>  vectors;  // FVector
    std::vector<std::vector<std::array<float, 3>>>  rotators; // FRotator (pitch,yaw,roll)
    std::vector<std::vector<std::array<float, 10>>> transforms; // FTransform packed
    std::vector<std::vector<uint8_t>>               bytes;
    std::vector<std::vector<std::wstring>>          names;    // FName
};

// ARRAY ELEMENT STRIDE is the 16-ALIGNED struct size, not the dump's raw `Size:` line.
// Fstruct_save1 embeds an FTransform, whose FQuat is 16-aligned, so the struct's own alignment
// is 16 and its element stride is the size rounded up to that. The engine walks every
// TArray<Fstruct_save1> at THIS stride, so using the unrounded size reads element i+1's class at the
// wrong byte: a wild class pointer and a heap over-read, which is a crash from the second element onward.
// In a dump the bottom-line `Size:` is PropertiesSize; the per-field embedded `size:` is the stride.
inline constexpr int32_t kSaveStride   = 0xF0;
// The record's own FIELD span: the last value group is a TArray, and every field ends inside this
// many bytes. The stride above is that rounded up to the struct's 16 alignment, and the difference
// is real padding. This is the bound a parameter frame is checked against before the codec reads or
// writes a record inside it -- the bytes past the span belong to whatever parameter the engine laid
// down next, so a frame with less than this much room after the offset is refused.
inline constexpr int32_t kSaveRecordBytes = 0xE8;
inline constexpr int32_t kMxStride     = 0x10;  // Fstruct_mX wraps a single TArray<X> @ +0; already 16-aligned

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

// Read one Fstruct_save1 at `base` into `rec`. Pure field reads + FName::ToString / FString copy
// (no UFunction dispatch). `rec` is overwritten field-by-field; pass a fresh record.
void ReadSaveRecord(const void* base, SaveRecord& rec);

// Write one Fstruct_save into the ZEROED slot at `base`, constructing engine-OWNED
// TArrays via reflection::EngineAlloc (FNames interned, FStrings engine-minted, UClasses
// FindClass'd). The slot MUST be zeroed by the caller (AllocZeroed); any previous buffers it
// referenced are intentionally orphaned -- recursively freeing nested Fstruct_save1 sub-arrays +
// FString buffers is far more crash-prone than a bounded leak, and the engine never double-frees
// a buffer it has lost the pointer to. Buffers WE allocate are GMalloc-owned (EngineAlloc), so
// the engine's later Array realloc / GC free of them is allocator-matched.
// `resolve` governs how every class leaf (the record's own class and its `classes` group) resolves;
// null = reflection::FindClass (see ClassResolver).
void WriteSaveRecord(uint8_t* base, const SaveRecord& r, ClassResolver resolve = nullptr);

// ---- The typed pair, on a LIVE prop ------------------------------------------------------------
//
// The two functions above read and write a record at a raw address. The ones below get one FROM a
// live prop and give one back TO it, through the prop's own `getData` and `loadData`. A prop's
// SAVE STATE no longer travels this way (ue_wrap/actors/prop_record carries it through betterGetData
// and betterLoadData, as the game does); this is for the kerfur disc carry, whose payload is a disc's
// typed leaf groups.
//
// Both climb to the most-derived declaration, because reflection::FindFunction is exact-owner and
// the override is the whole point -- Aprop_floppyDisc_C::getData calls its parent's and then
// appends the disc's `data` and `readWrites`, so a call resolved at Aprop_C would return the base
// record and silently drop exactly the state this exists to carry.

// `actor`'s record as the game's own addObject files it: the most-derived `getData`, with no splice convention, since
// a put stores the record raw. False when `actor` is not a prop, `getData` does not resolve or the call fails.
bool CaptureOwnRecord(void* actor, SaveRecord& out);

// Whether `cls` is Aprop_C or a descendant of it: the classes a prop record can name. One walk of the class chain.
bool IsPropClass(void* cls);

// The Aprop_C class, or null while it is not loaded (asked again at most once a second).
void* PropBaseClass();

// The prop's name, names[0][0] (Aprop_C::getData packs [name, nametag] as the first names group). False when the
// record has no such element: a record that came off the wire is never indexed without this.
inline bool RecordName(const SaveRecord& r, std::wstring& out) {
    if (r.names.empty() || r.names[0].empty()) return false;
    out = r.names[0][0];
    return true;
}

// Hand a record back to `actor` through its own `loadData` -- the LEAF half of it only.
//
// Aprop_C::loadData restores the save Key, the two names, the four saved bools, the scale and the
// lifespan from whatever record it is handed, then re-runs `init()`, `physicsImpact->init()` and
// `setNametag()`. Taken at face value that is a primitive for rewriting any prop's identity and for
// destroying it through SetLifeSpan, and every one of those fields already rides the prop's spawn
// row, which is their authority. So the receiver's OWN base record is read first and spliced over
// the incoming one, ELEMENT by element over the base's own extent: what survives from `r` is
// exactly what Aprop_C::getData does not write, which is the leaf class's own save state and the
// only thing this pair exists to move -- including the TAIL of a group the base also writes, which
// is where a food keeps its temperature. Which elements those are is read off the base class at
// runtime, never listed here. Two dispatches: the base capture and the apply.
// `resolve` is forwarded to the write of the merged record (null = FindClass; see ClassResolver).
bool ApplyRecord(void* actor, const SaveRecord& r, ClassResolver resolve = nullptr);

// ---- the same dispatch, without the prop lineage ----------------------------------------------
//
// The pair above is the PROP codec: it resolves its verbs only for classes on the `Aprop_C` chain,
// and its apply splices the receiver's own base half over the sender's because for a prop the base
// fields already ride the spawn row. A class off that chain -- `Ahook_C` is an `Aactor_save_C`, and
// the two lineages part at `AActor` -- needs the same dispatch and neither the gate nor the splice,
// and it needs a CALLER-supplied UFunction because the lineage is what `VerbsFor` keys on.
//
// These are the dispatch half alone, so there is one implementation of it rather than a copy in
// every wrapper that serializes a non-prop. The lineage question, the splice and whatever
// authority the record's fields need belong to the caller, which is the only party that knows.

// Run `recordFn` (a `getData`-shaped UFunction with a `data` out-parameter) on `actor` and read the
// record it leaves in the frame. False if the frame has no `data` parameter of the right size or
// the call fails.
bool CaptureRecordVia(void* actor, void* recordFn, SaveRecord& out);

// Write `r` into `loadFn`'s `data` parameter and call it. THE RECORD IS APPLIED AS GIVEN: there is
// no splice here, so a caller handing this a record that came off the wire has already decided,
// field by field, which parts it is willing to take from the sender.
// `resolve` is forwarded to the record write (null = FindClass; see ClassResolver).
bool ApplyRecordVia(void* actor, void* loadFn, const SaveRecord& r, ClassResolver resolve = nullptr);

// Drop the cached class lookups (level change / disconnect).
void ResetCodecCache();

}  // namespace ue_wrap::save_record
