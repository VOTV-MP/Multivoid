// coop/dev/director/standpoints.h -- where a drill's walker stands to act on something no route ends at, as a
// desk's button: the points of a ring about it that a NavMesh route reaches. Dev only (the drills' walks); game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace coop::director {

// Whether a NavMesh route from `from` reaches `to` (its end within `reachCm`, flat), asked in legs: the engine's path
// query spends a bounded search and returns a PARTIAL route on a long walk -- measured on a fresh New Game, whose
// players start about 790 m from the base and whose route to it stopped on a 2048-cm tile boundary -- so each leg asks
// again from the last one's end, as the walker does when it reaches it (GotoProcess). False when a leg finds no route
// or ends no closer to `to` than it began, or after `maxLegs`. `outLength`, when given, is the route's flat length.
bool RouteInLegs(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float reachCm, int maxLegs,
                 float* outLength);

// The `count` points of a ring of `ringCm` about `about`, at the height `player` stands at, that a route from `player`
// reaches (RouteInLegs), shortest route first: the director's own test for a reachable pile (PickReachablePile).
// Empty when `player` does not read or no route reaches one.
std::vector<ue_wrap::FVector> ReachableStandpoints(void* player, const ue_wrap::FVector& about, float ringCm, int count,
                                                   float reachCm);

}  // namespace coop::director
