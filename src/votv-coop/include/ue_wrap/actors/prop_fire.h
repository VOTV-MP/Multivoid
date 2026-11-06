// ue_wrap/actors/prop_fire.h -- the prop's fire members: the flame a prop carries once it burns, and the verbs that
// light it. Engine-wrapper layer (principle 7). Implementation: src/ue_wrap/actors/prop_fire.cpp.

#pragma once

#include <vector>

namespace ue_wrap::prop {

// Whether the prop's `flameBase` (an object field of prop_C) holds a live object, so whether the prop is burning.
// False when the prop's class declares no such field. Game thread.
bool PropFlameBaseValid(void* prop);

// The prop's attemptIgnite(), resolved on its own class (FindDispatchFunctionCached). False when the function does
// not resolve or the call could not run. Game thread.
bool CallAttemptIgnite(void* prop);

// The prop_C's ignite(fuel) on `prop`, resolved on its own class as above. False when the function does not resolve
// or the call could not run. Game thread.
bool CallIgnite(void* prop, float fuel);

// A prop's flame: its flameBase, a comp_flammable_C, burning, with its fuel and the speed it spends it.
struct FireState {
    bool  burning = false;
    float fuel = 0.f;
    float fuelSpeed = 0.f;
};

// The prop's flame as it stands; a prop with no flame reads not burning. False when the prop's flame does not read.
// Game thread.
bool ReadPropFire(void* prop, FireState& out);

// Every prop of the current world whose flame burns. Game thread.
void ListBurningProps(std::vector<void*>& out);

// The prop's flame put out, by the flame's own extinguish. True when it had none. Game thread.
bool CallExtinguish(void* prop);

// Shows `state` on the prop: lit by prop_C's own ignite when it was not burning, so no subclass's ignite runs (a salt
// rocket's would fly), then the fuel and speed written on the flame; or put out. False when a step fails. Game thread.
bool ApplyPropFire(void* prop, const FireState& state);

}  // namespace ue_wrap::prop
