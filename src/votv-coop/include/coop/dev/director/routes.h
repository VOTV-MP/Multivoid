// coop/dev/director/routes.h -- the director's routes over the level's NavMesh: one route from wherever the walker
// stands, reachability asked leg by leg, and the points of a ring about something no route ends at (a desk's button)
// that a route reaches. Every walk and every drill's reachability question goes through here. Dev only; game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace coop::director {

// A NavMesh route from `from` to `to`, its first point `from`. Where the path query cannot place `from` on the
// NavMesh -- a walker slid onto ground the NavMesh leaves out, where every query from it failed on a fresh New Game --
// `from` is projected onto it (the engine's ProjectPointToNavigation, in a 4 m box each way, then a 20 m one) and the
// route walks there first. False when there is no route even so.
bool RouteFrom(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to,
               std::vector<ue_wrap::FVector>* route);

// Whether a NavMesh route from `from` reaches `to` (its end within `reachCm`, flat), asked in legs: the engine's path
// query spends a bounded search and returns a PARTIAL route on a long walk -- measured on a fresh New Game, whose
// players start about 790 m from the base and whose route to it stopped on a 2048-cm tile boundary -- so each leg asks
// again from the last one's end, as the walker does when it reaches it (GotoProcess). False when a leg finds no route
// or ends less than a metre closer to `to` than it began (Detour ends a partial route at the polygon nearest the goal,
// so a winding approach still ends nearer), or after 16 legs. `outLength`, when given, is the route's flat length.
// Every drill's reachability question: a single path query refuses whatever lies a long walk away.
bool RouteInLegs(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float reachCm,
                 float* outLength = nullptr);

// The `count` points of a ring of `ringCm` about `about`, at the height `player` stands at, that a route from `player`
// reaches, shortest route first: the legs toward the ring's centre walked once (RouteInLegs' rule; toward its points
// when the centre ends no route), each point asked from the start of the leg that reaches the ring. Empty when
// `player` does not read or no route reaches one.
std::vector<ue_wrap::FVector> ReachableStandpoints(void* player, const ue_wrap::FVector& about, float ringCm, int count,
                                                   float reachCm);

}  // namespace coop::director
