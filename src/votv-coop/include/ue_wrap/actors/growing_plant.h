// ue_wrap/actors/growing_plant.h -- a growingPlant_C and its family, read and written by reflection on the game thread: a
// player's planted seed and the winter bush are the one class.

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

// Every live plant of the family.
void List(std::vector<void*>& out);

// Every live winter bush spawner (ticker_bushSpawning_C): its tick plants bushes of this family.
void ListBushSpawners(std::vector<void*>& out);

// Its key, minted by its own getKey when it has none yet. "" on failure.
std::wstring Key(void* plant);

std::wstring Row(void* plant);          // its list_farmPlants row
std::wstring AttachParent(void* plant); // the key of the pot it stands in
bool HideDirt(void* plant);

bool Read(void* plant, Values& out);

// Writes the values and refreshes what the plant shows from them: its row's data when the row changed, its grown mesh, its
// fruit mesh, its size. A plant's refresh turns its tick on while it lives, so a client parks it again after.
void Apply(void* plant, const std::wstring& row, const Values& v);

// A copy with the given identity. A pot it names has to exist here. Null when its class is not loaded.
void* Spawn(const std::wstring& className, const std::wstring& key, const std::wstring& row, const std::wstring& attachParent,
            bool hideDirt, const FVector& at, const FRotator& rot);

bool Destroy(void* plant);

}  // namespace ue_wrap::growing_plant
