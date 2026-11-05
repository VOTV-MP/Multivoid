// ue_wrap/actors/ambient_creature.h -- the deer and the walking tree, the dirt holes and the beehive branches: a tree's size
// and landing, a deer's dissolve and a hole's place. Each class is read by reflection on the game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace ue_wrap::ambient_creature {

// The live deer and walking trees.
void ListDeer(std::vector<void*>& out);
void ListTrees(std::vector<void*>& out);

// A tree's mesh scale and whether it has landed: its mesh shows from the landing on.
bool ReadTree(void* tree, float& scale, bool& landed);

// A copy never lands by itself, as its movement does not run: this writes what the landing and the begin play did.
void WriteTree(void* tree, float scale, bool landed);

bool Destroy(void* creature);

// The deer's own dissolve; it ends in the deer's destroy.
bool Dissolve(void* deer);

// A hexahive's and a sus hole's dirt holes, and the beehive spawner's branches.
void ListHoles(std::vector<void*>& out);
void ListBranches(std::vector<void*>& out);

// A plain copy of either: a branch spawned this way makes no nest of its own.
void* SpawnFixture(bool branch, const FVector& at, const FRotator& rot);

// The sus hole spawner's own gate: it rolls only while the game instance's mode is 0 or 1. Game thread.
bool HolesRoll();

// The sus hole spawner's own test of a spot: a trace 10 m above `point` to 10 m below it against static geometry, kept when it
// hits grass. `at` is the hit and `rot` its normal as a rotator, as the spawner places the hole. Game thread.
bool HoleSpot(const FVector& point, FVector& at, FRotator& rot);

}  // namespace ue_wrap::ambient_creature
