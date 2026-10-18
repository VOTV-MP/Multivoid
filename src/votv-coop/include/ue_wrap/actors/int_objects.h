// ue_wrap/actors/int_objects.h -- the game's `int_objects` interface as an actor answers it. A held tool's swing
// hits an actor through the interface's `addDamage(actor, damage, hit, impact, skipSetting)`, and every class that
// implements it (a door, a creature) runs its own body. Engine-wrapper layer (principle 7): one UFunction call,
// with no gameplay, network or coop state. Game thread. Implementation: src/ue_wrap/actors/int_objects.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::int_objects {

// `target`'s own `addDamage`, as the player's swing calls it. `actor` is the damager (null for none). The hit
// result is built on `target`'s Mesh component (null when the class has none) at `hitLoc`, a blocking hit; the
// body's own branches read it for where the hit effects go. `skipSetting` is false. False when `target` is not
// live, the class has no `addDamage`, the hit cannot be built, or the call faulted.
bool InvokeAddDamage(void* target, void* actor, float damage, const FVector& hitLoc, const FVector& impact);

}  // namespace ue_wrap::int_objects
