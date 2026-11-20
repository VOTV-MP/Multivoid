// coop/props/miner_bounds.h -- what a mining rig can hold, as the game lets it: the one place the wire's checks, the record
// gate and the client's apply read their limits from.
//
// A rig's `upgrade` stops at 16 slots, puts a GPU only in a free slot (16 at most) and takes 6 coolers at most (the bytecode of
// prop_miner_C, 0.9.0n). The rig's tick clamps the points it mines to 32 per GPU (`maxPoints`), so 512 is the most any rig
// holds. The points are bound by that 512 alone, not by 32 per GPU of the rig's own count: a fire cuts GPUs without
// clamping the points, and a forged record sets 16 GPUs anyway. The heat has no ceiling: the tick
// raises it every tick while the rig is over its points cap, so an overheated rig's honest heat grows without limit and a
// ceiling would make every peer drop the host's state of it. The heat is bound only to a finite number that is not negative,
// which keeps a forged NaN or infinity from freezing a copy.

#pragma once

#include <cmath>
#include <cstdint>

namespace coop::miner_sync {

inline constexpr int32_t kMaxSlots = 16;
inline constexpr int32_t kMaxGpus = 16;
inline constexpr int32_t kMaxFans = 6;
inline constexpr int32_t kPointsPerGpu = 32;
inline constexpr int32_t kMaxPoints = kPointsPerGpu * kMaxGpus;

// Whether the five numbers of a rig are inside the bounds; the heat must be finite and not negative.
inline bool InBounds(int32_t gpus, int32_t slots, int32_t fans, int32_t points, float heat) {
    return gpus >= 0 && gpus <= kMaxGpus && slots >= 0 && slots <= kMaxSlots && fans >= 0 && fans <= kMaxFans &&
           points >= 0 && points <= kMaxPoints && std::isfinite(heat) && heat >= 0.f;
}

}  // namespace coop::miner_sync
