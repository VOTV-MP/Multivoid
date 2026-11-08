// ue_wrap/actors/dig_tool.h -- engine access for the tools that dig a chip pile into scrap, and the dig itself.
// Principle-7 engine-wrapper layer: no network or coop state.
//
// Two hand tools dig a pile. The shovel's right mouse button calls mainPlayer_C::useShovel and the rake's
// stroke runs its own graph; each traces from the holder's arm, and a chip pile the trace hits gets the
// pile's own turnIntoScrap, which spawns one trash item named by the pile's chipType (a random variant:
// g2_metal_3, trWood_0, a food for organic chips), destroys the pile and plays the dig sound in 2D.
// Everything a dig changes in the world is in that one pile function, so it is the dig's seam and the
// call a host makes to dig for someone else.

#pragma once

namespace ue_wrap::dig_tool {

// True iff `obj` is a shovel or a rake (prop_shovel_C, prop_rake_C or a subclass of either). Walks the
// class chain by name, so it needs no loaded class to compare against. Game thread.
bool IsDigTool(void* obj);

// Run `pile`'s actorChipPile_C::turnIntoScrap through ProcessEvent, the way a dig calls it. False when
// `pile` is not a live chip pile or the call did not dispatch. Game thread.
bool TurnIntoScrap(void* pile);

}  // namespace ue_wrap::dig_tool
