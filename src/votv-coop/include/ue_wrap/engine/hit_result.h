// ue_wrap/engine/hit_result.h -- an FHitResult the engine builds (GameplayStatics::MakeHitResult),
// written into a parameter of another call's frame. Principle-7 engine-wrapper layer: no network
// logic, no coop state.
//
// A hit's actor and component are weak object pointers, a slot and a serial that only the engine
// mints; MakeHitResult assigns them as the game's own Blueprints do (prop_tvremote_C calls it). The
// struct moves whole between the two frames, its size read from each live UFunction, so no field of
// it is mirrored here.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap { class ParamFrame; }

namespace ue_wrap::hit_result {

// Fill `frame`'s FHitResult parameter `param` with a blocking hit on `component` of `actor` at
// `location`. False when MakeHitResult is unresolved, its call fails, the two parameters differ in
// size, or the hit it built does not resolve back to that actor and component. Game thread
// (dispatches a UFunction).
bool Write(ue_wrap::ParamFrame& frame, const wchar_t* param, void* actor, void* component,
           const FVector& location);

}  // namespace ue_wrap::hit_result
