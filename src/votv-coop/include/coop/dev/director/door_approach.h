// coop/dev/director/door_approach.h -- where a drill's walker stands to reach a door: in front of its
// leaf or behind it, off the door's origin along its forward axis (a closed door's own origin sits in
// its nav modifier, where no route ends), on the side this peer's navmesh reaches by the shorter route.
// Dev only (the drills' walks); game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>

namespace coop::director {

constexpr float kDoorApproachCm = 90.f;   // in front of or behind the leaf
constexpr float kRouteEndCm     = 100.f;  // a route that ends farther from its point never gets there

// The best approach so far, and the approach points refused on the way, for the caller's line.
struct DoorApproach {
    ue_wrap::FVector              target{};
    std::vector<ue_wrap::FVector> route;
    float                         len = 1e30f;
    int                           noRoute = 0, endsFar = 0;
};

// Takes into `best` the approach of `door` whose route from `at`, on the navmesh `player` queries, is
// shorter than best's own; true when one was. `endsShortOk` keeps a route that stops short of its point:
// from far off the route search stops at its node limit, and the director's re-path finishes it on the
// way.
bool PickDoorApproach(void* player, const ue_wrap::FVector& at, void* door, bool endsShortOk,
                      DoorApproach& best);

}  // namespace coop::director
