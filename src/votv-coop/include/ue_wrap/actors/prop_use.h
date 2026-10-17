// ue_wrap/actors/prop_use.h -- a world prop's use verbs as a player's press runs them. Engine-wrapper layer
// (principle 7). Implementation: src/ue_wrap/actors/prop_use.cpp.

#pragma once

#include <cstdint>

#include "ue_wrap/core/reflection.h"   // FName
#include "ue_wrap/core/types.h"        // FVector

namespace ue_wrap::prop_use {

// The prop's actionOptionIndex(player, hit, action, lookAtComponent), resolved on its own class
// (FindDispatchFunctionCached). `hit` is written from `hitComponent` and `hitLocation` on `prop`
// (hit_result::Write). False when the call could not run. Game thread.
bool CallActionOptionIndex(void* prop, void* player, void* hitComponent, const FVector& hitLocation, uint8_t action,
                           void* lookAtComponent);

// The prop's playerUsedOn(player, hit, lookAtComponent, holdObject, holdPropName), as above. Game thread.
bool CallPlayerUsedOn(void* prop, void* player, void* hitComponent, const FVector& hitLocation, void* lookAtComponent,
                      void* holdObject, ue_wrap::reflection::FName holdPropName);

}  // namespace ue_wrap::prop_use
