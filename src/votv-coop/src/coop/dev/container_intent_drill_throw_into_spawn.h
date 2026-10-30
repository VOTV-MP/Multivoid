// coop/dev/container_intent_drill_throw_into_spawn.h -- where the throwinto host places its baskets and drives: a point beside a pose, a
// spot with a ring of navmesh round it, and the open basket itself. A private header of coop/dev/container_intent_drill_throw_into*.cpp
// (src tree, not include/). See container_intent_drill_throw_into_spawn.cpp.

#pragma once

#include "container_intent_drill_throw_into.h"

#include <cstddef>

namespace coop::dev::container_intent_drill::detail::into {

// The point `ahead` along the unit `fwd` from `at` and `side` to its right, a drop's height above `at`.
FVector Place(const FVector& at, const FVector& fwd, float ahead, float side);

// The open basket at `where`, spawned at pitch 0 and roll 0 and facing `faceTo` in yaw. Null (and a warning with the class and the actor)
// when it does not load or is not a container.
void* SpawnBasket(const FVector& where, const FVector& faceTo);

// A spot for a basket with a ring of navmesh round it: at least `minValid` of the kFan points kRingCm out project and have a route from `from`
// (the client's place; RouteTo, container_intent_drill_throw_into_stand.h). From `first`, moved along
// the unit (sx, sy) in kRingStepCm steps, at most kRingSteps; the chosen spot (a few cm above the floor the ring's points stand on) and its
// routed count are printed. False, with `why` filled, when no step has a ring.
bool RingSpot(void* ctx, const FVector& from, const FVector& first, float sx, float sy, int minValid, FVector& out, char* why, size_t whyLen);

// RingSpot fanned over kFan directions round `at`: the unit `fwd` first, then turned by 45 degrees to either side in turn. The first
// point is `ahead` along the direction and `side` to its right; the steps go along the direction, or along its right when `stepSide`. False,
// with `why` filled with the last direction's text, `at` and `fwd`, when no direction has a ring. The chosen direction's index is printed.
bool RingSpotFan(void* ctx, const FVector& from, const FVector& at, const FVector& fwd, float ahead, float side, bool stepSide, int minValid, FVector& out,
                 char* why, size_t whyLen);

}  // namespace coop::dev::container_intent_drill::detail::into
