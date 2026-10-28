// ue_wrap/actors/prop_gather.h -- the facts the game reads before it lets a player pocket a prop
// (mainPlayer's putObjectInventory2 and the inventory's addObject) or hold one (Hold Object and updateHold).
// Engine-wrapper layer (principle 7): reads and calls only, no network or coop state, and no judgement; the
// caller decides what the facts allow. Game thread only. Every reader answers false when a fact could not be
// read, with the struct then undefined. Implementation: src/ue_wrap/actors/prop_gather.cpp.

#pragma once

#include <string>

namespace ue_wrap::prop_gather {

// The three flags of a prop's data row. Read from the live struct in the prop, or, for a frozen prop, from the
// class's row in list_props (the row the game's own init() resets a frozen prop from).
struct PropDataFlags {
    bool canCollect, canHold, heavy;
};

// What putObjectInventory2 and addObject test. collectRefused is the BP function canBeCollected answering
// true (a refusal); volumeFits is the prop's volume within the largest container a player can reach.
struct PocketFacts {
    bool collectRefused, child, frozen, simulating, canCollect, heavy, meshRoot, volumeFits, saveClass;
};

// What Hold Object and updateHold test. pickupRefused is canBePickedUp answering true, ignoreSave is the BP
// function of that name; equipRow is a row by the prop's name in list_weapons or list_props.
struct HoldFacts {
    bool pickupRefused, ignoreSave, child, frozen, simulating, canHold, heavy, saveClass, equipRow;
};

// The prop's propData flags as the live struct holds them (no frozen rule). False when the struct's members do
// not resolve.
bool ReadPropData(void* prop, PropDataFlags& out);

bool ReadPocketFacts(void* prop, PocketFacts& out);
bool ReadHoldFacts(void* prop, HoldFacts& out);

// The game's own gather test (putObjectInventory2, the prop path): not a child actor, and frozen or the root
// simulating physics; otherwise "Cannot gather object".
bool IsGatherable(void* prop);

// The prop's volume (the game's Get Volume on its mesh; zero when it has none) and the largest volume a
// container a player can reach holds: the two numbers ReadPocketFacts compares.
bool ReadVolumeAndBound(void* prop, float& volume, float& bound);

// The list_props row of a prop NAME (any case), the row the game's own init() resets a frozen prop from. False when
// the table cannot be read; `found` says whether it has the row, and `out` is the row's flags only then.
bool ReadRowFlags(const std::wstring& name, PropDataFlags& out, bool& found);

// The volume a container sums for a record it holds, by the prop's name: read from the game's own name table (the
// gamemode's propRenderer names beside its prop_volumes), which is what Get Volume with no mesh indexes. The name
// is looked up in a map of that table, kept current by the arrays' counts; no engine call is made and no name is made
// for a string a peer sent. False when the table cannot be read or the game has not filled it yet; `valid` is false for
// a name the table lacks, and `out` is then no volume.
bool VolumeOfName(const std::wstring& name, float& out, bool& valid);

}  // namespace ue_wrap::prop_gather
