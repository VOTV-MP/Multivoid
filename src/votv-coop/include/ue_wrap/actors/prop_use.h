// ue_wrap/actors/prop_use.h -- a world prop's use verbs as a player's press runs them, and the fields that say
// whether a use will consume the prop. Engine-wrapper layer (principle 7). Implementation: src/ue_wrap/actors/prop_use.cpp.

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

// An unpackable prop's own `spawn()` (no parameters; it rolls the contents of an MRE or a first aid kit and reads nothing
// of a player), resolved on the prop's own class (FindDispatchFunctionCached), since each class has its own. False when
// it does not resolve or the prop is not live. Game thread.
bool CallUnpackSpawn(void* prop);

// prop_medkit_pack_C's `drop` (a TArray of the props its `spawn` rolls): its Num on this live instance, read through the
// property's reflected offset. False when the prop is not live or its class has no `drop`. Game thread.
bool ReadUnpackDropCount(void* prop, int32_t& num);

// prop_food_C's `uses` (int32) and `notEdible` (a bitfield bool); false when either is unreadable. Game thread.
bool ReadFoodUse(void* prop, int32_t& uses, bool& notEdible);
// prop_food_soap_C's `soapAmount` (int32). Game thread.
bool ReadSoapAmount(void* soap, int32_t& amount);
// prop_sponge_C's `soapAmount` (a float on the sponge, prop_sponge.cpp:22). Game thread.
bool ReadSpongeSoap(void* sponge, float& amount);

}  // namespace ue_wrap::prop_use
