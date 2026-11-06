// coop/dev/director/leg_blocker.h -- what stands on the leg of the route the walker follows. The NavMesh routes
// through physics props it does not know; a walker slid sideways by one ends against a wall, and the trace along its
// own line to the waypoint names the wall. The prop that stopped it is on the route's leg. Dev director only; game
// thread; run on a persistent stuck, never per tick.

#pragma once

#include "coop/dev/director/director.h"   // PlayerContext

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::director {

// Traces the leg of `route` the walker is on, at knee height, from `route[heading - 1]` to `route[heading]` (the
// route's first point is its start, so `heading` is the index of the point the walker heads for), and says what it
// hits. It names any prop (a prop_C descendant, frozen or not). True with `prop` the prop struck and `spot` the point
// struck when the first thing the leg meets is one; false when the leg is clear, meets static geometry first, or
// cannot be read. Logs the leg and what it meets. The hit may lie anywhere along the leg: whether it is the prop
// holding the walker is PropHoldsWalker's judgement.
bool LegBlocker(const PlayerContext& ctx, const std::vector<ue_wrap::FVector>& route, size_t heading, void** prop,
                ue_wrap::FVector* spot);

enum class Hold : uint8_t {
    Holds,     // the prop's bounds lie ahead along the leg and within reach of the walker's body
    TooFar,    // ahead, but the gap from the body to the bounds is past the reach
    Behind,    // the whole of the prop's bounds lie behind the walker along the leg
    Unread,    // the bounds, the body or the leg cannot be read: nothing is claimed
};

// Whether `prop`, named by LegBlocker on the same `route` and `heading`, holds the walker: the gap (flat, cm) from the
// walker's capsule to the closest point of the prop's world box (the engine's GetActorBounds, colliding components),
// at most `reachCm`, and the box reaching ahead of the walker along the leg's direction. `gapCm` receives the gap when
// the bounds were read.
Hold PropHoldsWalker(const PlayerContext& ctx, const std::vector<ue_wrap::FVector>& route, size_t heading, void* prop,
                     float reachCm, float* gapCm);

}  // namespace coop::director
