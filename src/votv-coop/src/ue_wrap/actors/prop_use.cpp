// ue_wrap/actors/prop_use.cpp -- see ue_wrap/actors/prop_use.h.

#include "ue_wrap/actors/prop_use.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/hit_result.h"

namespace ue_wrap::prop_use {

namespace R = ue_wrap::reflection;

bool CallActionOptionIndex(void* prop, void* player, void* hitComponent, const FVector& hitLocation, uint8_t action,
                           void* lookAtComponent) {
    if (!prop || !R::IsLive(prop) || !player) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", prop, hitComponent, hitLocation) &&
           f.Set<uint8_t>(L"action", action) && f.Set<void*>(L"lookAtComponent", lookAtComponent) && Call(prop, f);
}

bool CallPlayerUsedOn(void* prop, void* player, void* hitComponent, const FVector& hitLocation, void* lookAtComponent,
                      void* holdObject, R::FName holdPropName) {
    if (!prop || !R::IsLive(prop) || !player) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"playerUsedOn");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", prop, hitComponent, hitLocation) &&
           f.Set<void*>(L"lookAtComponent", lookAtComponent) && f.Set<void*>(L"holdObject", holdObject) &&
           f.SetRaw(L"holdPropName", &holdPropName, static_cast<int32_t>(sizeof(holdPropName))) && Call(prop, f);
}

}  // namespace ue_wrap::prop_use
