// ue_wrap/core/trace.h -- standalone engine access for line-of-sight traces
// (UKismetSystemLibrary::LineTraceSingleForObjects). Principle-7 engine-wrapper
// layer: wraps the KSL CDO/UFunction resolve + the ParamFrame plumbing of a world
// line trace. NO network logic, NO gameplay state.
//
// One owner of the trace primitive: wisp.cpp's canReach parity trace, nameplate
// occlusion, and the director's stuck line and slope detour, which read what it hit.

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

// What the same trace hits first (static and dynamic geometry; a physics body is not in the
// set): the hit's actor and component (live, or null), and its
// impact point and normal, read from the call's FHitResult by the struct's reflected member
// offsets. False when unresolvable (as -1 above); `out->blocked` false on a clear line.
// Game thread only.
struct Hit {
    bool    blocked = false;
    void*   actor = nullptr;
    void*   component = nullptr;
    FVector point{};
    FVector normal{};
};
bool LineHitStatDyn(void* worldCtx, const FVector& start, const FVector& end, Hit* out);

}  // namespace ue_wrap::trace
