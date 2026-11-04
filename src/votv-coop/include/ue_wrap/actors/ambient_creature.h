// ue_wrap/actors/ambient_creature.h -- the deer and the walking tree, the dirt holes and the beehive branches: their spawners,
// a tree's size and landing, a deer's dissolve, a hole's place and the beehive's timer. Each class is read by reflection on
// the game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace ue_wrap::ambient_creature {

// The live spawners a client's tick poll parks: the deer's, the tree's, the hexahive's and the sus hole's.
void ListSpawners(std::vector<void*>& out);

// Their live creatures.
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

// The beehive spawner's repeating timer, which has no tick to take off. True once a spawner stands and every one reads as asked:
// the timer arms a moment into the world, so a pause before that takes nothing.
bool PauseBeehiveTimers(bool paused);

// Whether a spawner's timer stands paused; false with no spawner.
bool BeehiveTimerPaused();

}  // namespace ue_wrap::ambient_creature
