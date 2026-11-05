// ue_wrap/actors/growing_plant.h -- a growingPlant_C and every class under it, read and written by reflection on the game
// thread: a player's planted seed, the winter bush and the mushrooms share its fields, inherited from the one base.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::growing_plant {

struct Values {
    float grow = 0.f;
    float fertilizer = 0.f;
    float water = 0.f;
    float fruitGrow = 0.f;
    float dryness = 0.f;
    int32_t amount = 0;
};

// Every live plant: an instance of growingPlant_C or of any class whose super chain reaches it. The classes are found
// from the object index, so a class a game build adds is a plant here with no change.
void List(std::vector<void*>& out);

// Whether `cls` is growingPlant_C or a class under it: a walk of its super chain by pointer against growingPlant_C's class, so
// a class with no live instance answers true as well. False while growingPlant_C is not loaded. Game thread.
bool IsPlantClass(void* cls);

// Whether `cls` is prop_farmPot_C or a class under it (the flat and the small pot): the same walk of the super chain by pointer.
// False while prop_farmPot_C is not loaded. Game thread.
bool IsPotClass(void* cls);

// Its key, minted by its own getKey when it has none yet. "" on failure.
std::wstring Key(void* plant);

std::wstring Row(void* plant);          // its list_farmPlants row

// The class the game's own list_farmPlants row plants (the row's plantObject, as a pot reads it), or null for an empty row, a row
// the table does not have, a table not readable yet or a class no longer live. The row names are read once per world, and again
// at most every few seconds while the table is not readable. The class is not checked against the family: IsPlantClass is the
// caller's. Game thread.
void* FarmRowPlantClass(const std::wstring& row);
// Whether `row` is a row name of the game's list_farmPlants data table (without case, as an FName compares). Read from the same table
// as FarmRowPlantClass (then false while it is not readable). Game thread.
bool IsFarmRow(const std::wstring& row);
std::wstring AttachParent(void* plant); // the key of the pot it stands in
bool HideDirt(void* plant);

bool Read(void* plant, Values& out);

// Writes the values and refreshes what the plant shows from them: its row's data when the row changed, its grown mesh, its
// fruit mesh, its size.
void Apply(void* plant, const std::wstring& row, const Values& v);

// A copy of class `cls` with the given identity. A pot it names has to exist here. Null for a null class. The class is the
// caller's to find and to check against the family (IsPlantClass, for a name that came off the wire): a class just loaded is
// not listed by the object index until its load drains, so a caller that loaded it passes the pointer it got.
void* Spawn(void* cls, const std::wstring& key, const std::wstring& row, const std::wstring& attachParent,
            bool hideDirt, const FVector& at, const FRotator& rot);

bool Destroy(void* plant);

}  // namespace ue_wrap::growing_plant
