// ue_wrap/actors/food.h -- a food's live condition: temperature, ripeness and use count.
//
// These live on the actor and its tick moves them. Reads and writes go straight to the fields, never through loadData,
// which reads the capturing peer's clock stamp as elapsed time. Engine-wrapper layer: no network. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::food {

struct Condition {
    float   temperature = 0.f;  // kelvin
    float   ripeness = 0.f;
    int32_t uses = 0;
    float   cookProgress = -1.f;  // a cooking food's, -1 for any other food and when a writer leaves it alone
};

// True for prop_food_C and every class below it; the answer is kept per class.
bool IsFoodClass(void* cls);
bool IsFood(void* actor);

// False when the actor is not a live food or a field did not resolve.
bool Read(void* actor, Condition& out);

// Writes the condition, and `celsius`, which the tick derives from the temperature and a copy refused its tick never derives.
bool Write(void* actor, const Condition& in);

// What the tick paints from the temperature, for a copy whose tick is off: the steam above 50 C and, across 0 C, the
// frozen flag, the mesh's frost, the impact material, the impact's sound and the class's freezed. A frozen edge is
// painted once. It does not ignite a hot food. False when the actor is not a live food.
bool PaintLooks(void* actor);

// The steam's state and the frozen flag, as PaintLooks or the tick left them.
bool ReadLooks(void* actor, bool& steam, bool& frozen);

}  // namespace ue_wrap::food
