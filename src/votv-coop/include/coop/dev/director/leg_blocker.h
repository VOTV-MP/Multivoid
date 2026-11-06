// coop/dev/director/leg_blocker.h -- what stands on the leg of the route the walker follows. The NavMesh routes
// through physics props it does not know; a walker slid sideways by one ends against a wall, and the trace along its
// own line to the waypoint names the wall. The prop that stopped it is on the route's leg. Dev director only; game
// thread; run on a persistent stuck, never per tick.

#pragma once

#include "coop/dev/director/director.h"   // PlayerContext

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <vector>

namespace coop::director {

// A prop the NavMesh does not know and the body cannot pass: a prop_C descendant that is neither frozen nor static.
bool IsPhysicsProp(void* actor);

// Traces the leg of `route` the walker is on, at knee height, from `route[heading - 1]` to `route[heading]` (the
// route's first point is its start, so `heading` is the index of the point the walker heads for), and says what it
// hits. True with `prop` the physics prop struck and `spot` the point struck when the first thing the leg meets is
// one; false when the leg is clear, meets static geometry first, or cannot be read. Logs the leg and what it meets.
bool LegBlocker(const PlayerContext& ctx, const std::vector<ue_wrap::FVector>& route, size_t heading, void** prop,
                ue_wrap::FVector* spot);

}  // namespace coop::director
