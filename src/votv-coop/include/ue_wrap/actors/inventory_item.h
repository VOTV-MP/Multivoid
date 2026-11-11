// ue_wrap/actors/inventory_item.h -- the Fstruct_inventoryItem record: one entry of an inventory component's
// canvasInventory (a world container's contents) or slotArray_A (a player's hotbar), its POD form and its codec.
//
// An item is DATA, not an actor: a struct_betterSave (the soft class, the transform, the name, tag, key and
// attachKey FNames, the actor's own serialised SaveGame bytes in actorData, and an index) plus where it lies on the
// inventory canvas (position, rotation, compartment) and how many it stands for (amount). The actor's state is the
// game's ActorToBytes stream, carried as opaque bytes: it is read and written whole, never parsed. FNames and the
// soft class are read as strings, since neither is portable across peers as a pointer or an index.
//
// Every member offset, the element stride and each member's type are read off the live UserDefinedStructs by
// their stable name prefixes (the GUID tails are not), and checked against the size and FField class each member
// must have; a layout that does not check out leaves the codec unresolved, never guessed. Engine-wrapper layer
// (principle 7). Game thread.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory_item {

struct Item {
    // struct_betterSave
    std::wstring classPath;            // the soft class as FSoftObjectPath::ToString writes it; empty = an empty entry
    std::array<float, 10> xform{};     // FTransform: quat(x,y,z,w) + loc(x,y,z) + scale(x,y,z)
    std::wstring name;                 // the prop's list name (FName)
    std::wstring tag;                  // FName
    std::wstring key;                  // the save key (FName)
    std::wstring attachKey;            // the save key of the actor it is attached to (FName)
    std::vector<uint8_t> actorData;    // the actor's ActorToBytes stream, opaque
    int32_t index = -1;
    // struct_inventoryItem
    float posX = 0.f, posY = 0.f;      // position on the inventory canvas
    float rotation = 0.f;
    uint8_t compartment = 0;
    uint8_t amount = 0;
};

// An entry with no class: removeInventoryItem blanks an entry in place rather than removing it, and trimInventory
// drops the blanks later, so an array can hold these between the two.
inline bool IsEmpty(const Item& it) { return it.classPath.empty(); }

// The entry's prop name; false when it carries none.
inline bool ItemName(const Item& it, std::wstring& out) {
    if (it.name.empty()) return false;
    out = it.name;
    return true;
}

// The class's leaf name, the part after the last '.' of the path ("/Game/objects/prop_apple.prop_apple_C" ->
// "prop_apple_C"); the whole string when it has no '.'.
std::wstring ClassLeaf(const Item& it);

// The upper bound on actorData a peer's word is held to: a real actor's stream is a few kilobytes.
inline constexpr size_t kMaxActorData = 64 * 1024;

// Resolve the layout. False until both structs are loaded and every member checks out; asked again at most once
// a second, so a call before the world is up costs one bounded lookup.
bool Resolve();

// The element stride of a TArray<Fstruct_inventoryItem>: the struct's size rounded up to its 16-byte alignment
// (the FTransform inside struct_betterSave is 16-aligned). 0 while unresolved.
int32_t Stride();

// Read the entry at `base`. False while unresolved.
bool Read(const void* base, Item& out);

// Write `in` into the ZEROED element at `base` (save_record::AllocZeroed), with engine-owned FNames, FString and
// actorData buffer. False while unresolved or when a buffer does not allocate; the element is then left partly
// written and zero elsewhere, which the engine reads as an empty entry.
bool Write(uint8_t* base, const Item& in);

// `actor`'s entry as addItemAsObject builds it: the actor's own betterGetData, an amount of 1, at the canvas origin.
// False when the actor has no betterGetData or the call fails.
bool Capture(void* actor, Item& out);

}  // namespace ue_wrap::inventory_item
