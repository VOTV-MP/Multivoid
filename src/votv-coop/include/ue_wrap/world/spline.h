// ue_wrap/world/spline.h -- a USplineComponent's points, read and written through the engine's own functions. Engine
// access only: no gameplay, no network. Points are in the component's local space. Game thread.
#pragma once

#include "ue_wrap/core/types.h"  // FVector

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ue_wrap::spline {

// Read at most `maxPoints` points of `spline` into `out` (cleared first). False when a call did not resolve or ran
// short, or the spline holds none.
bool ReadLocalPoints(void* spline, int32_t maxPoints, std::vector<FVector>& out);

// Replace the spline's points with `pts` (cleared, each added, then updated once). False when a call did not resolve.
bool WriteLocalPoints(void* spline, const std::vector<FVector>& pts);

// The spline's length. False when the call did not resolve.
bool Length(void* spline, float& out);

}  // namespace ue_wrap::spline
