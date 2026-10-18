// ue_wrap/actors/zombie.h -- engine access for the zombie family: npc_zombie_C and the Blueprint classes below it.
// Two things a client does for its mirror of one, whose own Blueprint graph is refused: the part of BeginPlay that
// is presentation (the animation instance the class keeps, and its walk blend), and the hit feedback the class's
// addDamage plays. Engine-wrapper layer (principle 7): reflected reads, writes and calls, with no gameplay,
// network or coop state. Game thread. Implementation: src/ue_wrap/actors/zombie.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::zombie {

// BeginPlay's guarded cast, reproduced: the mesh's animation instance is written into the zombie's `anim` member
// only when its class is, or derives from, the kerfur Omega AnimBP the family shares (a class with no animation
// blueprint, the deer, keeps `anim` empty as in the game), and then `setRandomAnimation` picks the walk blend. True
// when every step that applies ran, or none applied; false, and said once per class, when a step that applies
// did not resolve or its call faulted. Game thread.
bool ApplyBeginPlayPresentation(void* zombie);

// The feedback of a hit at `hitLoc`, the function the class's addDamage picks and calls: `hitHeadArmor` while
// `headArmor` is above zero, else `hitBoney` for a class with the `boney` flag, else `hitFlesh`. Each spawns an
// emitter and plays a sound at the point and touches no world state. Returns the function's name (a literal), or
// null when none played. Game thread.
const wchar_t* PlayHitFeedback(void* zombie, const FVector& hitLoc);

}  // namespace ue_wrap::zombie
