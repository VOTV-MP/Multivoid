// ue_wrap/actors/dig_tool.h -- engine access for the tools that dig a chip pile into scrap, and the dig itself.
// Principle-7 engine-wrapper layer: no network or coop state.
//
// Two hand tools dig a pile. The shovel's right mouse button calls mainPlayer_C::useShovel and the rake's
// stroke runs its own graph; each traces from the holder's arm, and a chip pile the trace hits gets the
// pile's own turnIntoScrap, which spawns one trash item named by the pile's chipType (a random variant:
// g2_metal_3, trWood_0, a food for organic chips), destroys the pile and plays the dig sound in 2D.
// Everything a dig changes in the world is in that one pile function, so it is the dig's seam and the
// call a host makes to dig for someone else.

//
// A shovel's dig into the ground instead makes a hole: useShovel traces sand, dirt, gravel or grass and spawns
// a dirthole_C at the trace's point, its matType naming the ground. The same class is the child actor every
// sus hole (susDirtHole_C) holds, so a dug hole is a dirthole_C that is no child actor. A dug hole is in no save
// (the class has no save interface): it lives until a buried item, a planted seed or a dirt ball fills it.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::dig_tool {

// True iff `obj` is a shovel or a rake (prop_shovel_C, prop_rake_C or a subclass of either). Walks the
// class chain by name, so it needs no loaded class to compare against. Game thread.
bool IsDigTool(void* obj);

// Run `pile`'s actorChipPile_C::turnIntoScrap through ProcessEvent, the way a dig calls it. False when
// `pile` is not a live chip pile or the call did not dispatch. Game thread.
bool TurnIntoScrap(void* pile);

// True iff `obj` is a shovel (prop_shovel_C or a subclass): the one tool whose dig makes a hole. Game thread.
bool IsShovel(void* obj);

// True iff `obj` is a live dirthole_C that no child actor component spawned: a shovel's hole, not a sus
// hole's. Game thread.
bool IsDugHole(void* obj);

// A dug hole's ground (its matType: 0 dirt or grass, 1 gravel, 2 sand); false when it does not read.
bool ReadHoleGround(void* hole, uint8_t& matType);

// Spawn a dug hole as useShovel does: a deferred spawn of dirthole_C at `at`, `rot`, with matType written
// before its begin play. Null when the class is not loaded or the spawn failed. Game thread.
void* SpawnDugHole(const FVector& at, const FRotator& rot, uint8_t matType);

// How many dug holes stand within `radiusUU` of `at`, and the nearest in `nearest` (may be null). Game thread.
int CountDugHolesNear(const FVector& at, float radiusUU, void** nearest);

// End a dug hole (K2_DestroyActor). False when it is not live or the call did not dispatch. Game thread.
bool DestroyHole(void* hole);

}  // namespace ue_wrap::dig_tool
