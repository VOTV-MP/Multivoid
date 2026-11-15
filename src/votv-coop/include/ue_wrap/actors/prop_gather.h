// ue_wrap/actors/prop_gather.h -- the facts the game reads before it lets a player pocket a prop
// (mainPlayer's addObjectToInventory and the inventory's addItemAsObject) or hold one (Add Object to Free Hotbar Slot).
// Engine-wrapper layer (principle 7): reads and calls only, no network or coop state, and no judgement; the
// caller decides what the facts allow. Game thread only. Every reader answers false when a fact could not be
// read, with the struct then undefined. Implementation: src/ue_wrap/actors/prop_gather.cpp.

#pragma once

#include <string>
#include <vector>

namespace ue_wrap::prop_gather {

// The three flags of a prop's data row. Read from the live struct in the prop, or, for a frozen prop, from the
// class's row in list_props (the row the game's own init() resets a frozen prop from).
struct PropDataFlags {
    bool canCollect, canHold, heavy;
};

// What addObjectToInventory and addItemAsObject test of the prop. takeRefused is the BP function
// canBeCollectedOrPickedUp answering ignore (a refusal), ignoreSave the BP function of that name, attached the engine's
// GetAttachParentActor naming an actor. saveClass is false for a class whose getData saves nothing (or a hose being
// placed), which the game cannot store.
struct PocketFacts {
    bool takeRefused, ignoreSave, child, attached, saveClass;
};

// What Add Object to Free Hotbar Slot tests of the prop: the pocket's answers, and the root simulating physics in place
// of the attach parent.
struct HoldFacts {
    bool takeRefused, ignoreSave, child, simulating, saveClass;
};

// The prop's propData flags as the live struct holds them (no frozen rule). False when the struct's members do
// not resolve.
bool ReadPropData(void* prop, PropDataFlags& out);

bool ReadPocketFacts(void* prop, PocketFacts& out);
bool ReadHoldFacts(void* prop, HoldFacts& out);

// The prop's volume (the game's Get Volume on its mesh; zero when it has none) and the largest volume a
// container a player can reach holds.
bool ReadVolumeAndBound(void* prop, float& volume, float& bound);

// The list_props row of a prop NAME (any case), the row the game's own init() resets a frozen prop from. False when
// the table cannot be read; `found` says whether it has the row, and `out` is the row's flags only then.
bool ReadRowFlags(const std::wstring& name, PropDataFlags& out, bool& found);

// The volume a container sums for a record it holds, by the prop's name: the game's own Get Volume (what the
// inventory's addItemAdvanced asks of a put's name), asked with the name as the game's prop list spells it. The name is
// looked up in a map of that list (the gamemode's propRenderer Names; rebuilt when it changes); no name is made for a
// string a peer sent. False when the list cannot be read, the game has not filled it yet or the call fails; `valid` is
// false for a name the list lacks, and `out` is then no volume. (Get Volume's own `valid` is true for every name in
// 0.10, so the list is what tells an unknown one.)
bool VolumeOfName(const std::wstring& name, float& out, bool& valid);

// Every name in the game's prop list (the gamemode's propRenderer Names), as spelled. False when the list cannot be read
// or is not filled yet. Dev drills use it to find the smallest item by VolumeOfName. Game thread.
bool ListNames(std::vector<std::wstring>& out);

}  // namespace ue_wrap::prop_gather
