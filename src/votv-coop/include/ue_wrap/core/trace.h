// ue_wrap/trace.h -- standalone engine access for line-of-sight traces
// (UKismetSystemLibrary::LineTraceSingleForObjects). Principle-7 engine-wrapper
// layer: wraps the KSL CDO/UFunction resolve + the ParamFrame plumbing of a world
// line trace. NO network logic, NO gameplay state.
//
// One owner of the trace primitive (extracted 2026-07-04 from wisp.cpp's canReach
// parity trace when nameplate occlusion became the second consumer).

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::trace {

// Line trace start->end against the {WorldStatic, WorldDynamic} object set (the
// native canReach's own set: EObjectTypeQuery1/2 -- level geometry, doors, props;
// NOT pawns, so player bodies never self-block). bTraceComplex=false,
// bIgnoreSelf=true. `worldCtx` is any live actor (the WorldContextObject).
//
// Returns  1 = blocked (something static/dynamic between start and end),
//          0 = clear line,
//         -1 = unresolvable (KSL CDO/UFunction not ready, null ctx, call failed)
//              -- the caller picks its own safe default.
// Game thread only (dispatches a UFunction).
int LineBlockedStatDyn(void* worldCtx, const FVector& start, const FVector& end);

}  // namespace ue_wrap::trace
