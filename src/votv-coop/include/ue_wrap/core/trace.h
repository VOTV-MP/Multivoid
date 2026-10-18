// ue_wrap/core/trace.h -- standalone engine access for line-of-sight traces
// (UKismetSystemLibrary::LineTraceSingleForObjects). Principle-7 engine-wrapper
// layer: wraps the KSL CDO/UFunction resolve + the ParamFrame plumbing of a world
// line trace. NO network logic, NO gameplay state.
//
// One owner of the trace primitive: LineHitForObjects, over any object-type set and ignore list.
// The {WorldStatic, WorldDynamic} helpers below are that call with the native canReach's own set;
// wisp.cpp's canReach parity trace, nameplate occlusion, and the director's stuck line and slope
// detour use them.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

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

// What a trace hits first: the hit's actor and component (live, or null), and its
// impact point and normal, read from the call's FHitResult by the struct's reflected member
// offsets (ue_wrap/engine/hit_result). `blocked` false on a clear line.
struct Hit {
    bool    blocked = false;
    void*   actor = nullptr;
    void*   component = nullptr;
    FVector point{};
    FVector normal{};
};

// The same trace as LineBlockedStatDyn, over the object types `objectTypes[0..nTypes)` (EObjectTypeQuery
// values, one byte each: 0 WorldStatic, 1 WorldDynamic, 2 Pawn, 3 PhysicsBody) and ignoring the actors
// `ignore[0..nIgnore)` (null when nIgnore is 0). The arrays are only read, for the length of the call.
// bTraceComplex=false, bIgnoreSelf=true: `worldCtx`'s own actor is ignored too. False when
// unresolvable (as -1 above), on a null `out`, or when FHitResult's layout does not resolve: a
// hit that cannot be read is "could not trace", never a clear line.
// Game thread only.
bool LineHitForObjects(void* worldCtx, const FVector& start, const FVector& end, const uint8_t* objectTypes,
                       int32_t nTypes, void* const* ignore, int32_t nIgnore, Hit* out);

// What the {WorldStatic, WorldDynamic} trace hits first (a pawn and a physics body are not in the set).
// False when unresolvable (as -1 above); `out->blocked` false on a clear line. Game thread only.
bool LineHitStatDyn(void* worldCtx, const FVector& start, const FVector& end, Hit* out);

}  // namespace ue_wrap::trace
