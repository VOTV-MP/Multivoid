// ue_wrap/actors/character_floor.h -- a character's floor, recomputed by the engine's own query. A character's
// CharacterMovement keeps the floor it stands on in `CurrentFloor`, written by the movement tick while it walks. A mirror
// whose movement tick is parked has no one to write it, so what the game reads from it (a deer's body lean) stays the
// floor the mirror spawned on. This runs the engine's query at the place the mirror was moved to and stores the answer
// where the movement tick would have.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::character_floor {

// Ask the actor's CharacterMovement for the floor under `capsuleAt` (K2_FindFloor, a capsule sweep: the capsule centre of an
// ACharacter is its actor location) and write the whole result into the component's CurrentFloor. False, with CurrentFloor
// untouched, on a null or dead actor or movement component, or when the engine's layout is not the one this reads (said
// once). One sweep per call, no allocation after the first. Game thread.
bool Refresh(void* characterActor, const FVector& capsuleAt);

}  // namespace ue_wrap::character_floor
