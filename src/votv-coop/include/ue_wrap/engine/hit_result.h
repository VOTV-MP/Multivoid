// ue_wrap/engine/hit_result.h -- an FHitResult the engine builds (GameplayStatics::MakeHitResult),
// written into a parameter of another call's frame or into an object's property, and the component
// a hit names, read back, and a hit read out of an object's property or a raw FHitResult. Principle-7 engine-wrapper layer: no network logic, no coop state.
//
// A hit's actor and component are weak object pointers, a slot and a serial; MakeHitResult assigns them
// as the game's own Blueprints do (prop_tvremote_C calls it), and building the hit through it keeps
// FHitResult's layout out of this file. The struct moves whole between the two frames, its size read
// from each live UFunction, so no field of it is mirrored here. The read side finds a member the same way, by
// its reflected name in FHitResult's own UScriptStruct.

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

// The same hit, written into `object`'s FHitResult property `field`. False as above, or when the
// class has no such property or its struct differs in size. Game thread.
bool WriteField(void* object, const wchar_t* field, void* actor, void* component, const FVector& location);

// What an FHitResult holds, as the game reads it through BreakHitResult. `actor` and `component` are the
// hit's weak pointers resolved to the live object: null when the pointer is empty, stale (its slot recycled)
// or its object is pending kill. `location` and `normal` are the impact point and impact normal.
struct Hit {
    bool    blocking = false;
    void*   actor = nullptr;
    void*   component = nullptr;
    FVector location{};
    FVector normal{};
};

// Can Parse read an FHitResult at all (the struct and every member it reads resolve)? Lets a caller refuse
// before it runs a call whose hit it could not read afterwards. Game thread.
bool Readable();

// Read the FHitResult of `bytes` bytes at `hit` (a trace's OutHit, a frame's local) into `out`. False when
// the struct does not resolve or `bytes` cannot hold it. Game thread.
bool Parse(const void* hit, int32_t bytes, Hit* out);

// The counterpart of WriteField: `object`'s FHitResult property `field` read into `out`. False when `object` is
// not live, the class has no such property, or the property is not an FHitResult. Game thread.
bool ReadField(void* object, const wchar_t* field, Hit* out);

// The component an FHitResult at `hit` names (its weak `Component`), as BreakHitResult reads it; null when
// it is stale or the struct does not resolve. Game thread.
void* Component(const void* hit);

}  // namespace ue_wrap::hit_result
