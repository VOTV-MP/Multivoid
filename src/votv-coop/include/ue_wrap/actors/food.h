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

// A white mushroom's (prop_food_mushroom7_C) own roll to become a puffball, stopped: its BeginPlay sets a looping 180 s
// timer on Transform, which each time turns it into a prop_puffballMature_C one time in twenty and destroys it. Cleared
// with K2_ClearTimer, the inverse of the BP's K2_SetTimerDelegate on {self, Transform}, so the roll runs on one peer.
// Call it after the actor's BeginPlay. False when the clear does not resolve or the call fails. Game thread.
bool StopPuffballTransform(void* actor);

// Whether that timer is set on `actor` (K2_IsTimerActive): the host's white mushrooms are, a client's are not. Game thread.
bool PuffballTransformArmed(void* actor);

}  // namespace ue_wrap::food
