// coop/props/toolgun_limits.h -- the bounds every toolgun request lane of the host shares: how many requests one sender
// may have waiting, how fast they are served, and how far a shot's hit point may lie from the prop it names. Each lane
// keeps its own queue and budget; the numbers are these.

#pragma once

#include "coop/net/intent_bucket.h"

#include <cstddef>

namespace coop::toolgun_limits {

// A sender's requests run in arrival order from a bounded queue at a bounded rate (kerfur_convert_host's shape): a
// pace of 4 then 2 a second, 8 waiting. A request past the cap is refused into the lane's tally.
inline constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
inline constexpr size_t kMaxPending = 8;

// How far the shot's hit point may lie beyond the prop's bounding sphere, and how far short of the hit point a
// blocker may stand before the segment counts as blocked: a prop's surface is not its bounds, and the hit point is
// on a surface.
inline constexpr float kOnPropSlackCm = 50.f;
inline constexpr float kBlockedSlackCm = 25.f;

}  // namespace coop::toolgun_limits
