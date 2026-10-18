// ue_wrap/desk/dish_writers.cpp -- see ue_wrap/desk/dish_writers.h.

#include "ue_wrap/desk/dish_writers.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

namespace ue_wrap::dish_writers {

namespace R = reflection;

namespace {

void* SpawnOf(const wchar_t* className, const FVector& at) {
    void* cls = object_index::ClassByName(className);
    return cls ? engine::SpawnActor(cls, at) : nullptr;
}

}  // namespace

void* SpawnUncalibrator(const FVector& at) { return SpawnOf(kUncalibratorClass, at); }
void* SpawnTool(void* toolClass, const FVector& at) { return toolClass ? engine::SpawnActor(toolClass, at) : nullptr; }
void* SpawnToolgun(const FVector& at) { return SpawnOf(kToolgunClass, at); }

bool CallUncalibratorUse(void* uncalibrator, void* player) {
    void* fn = uncalibrator ? R::FindDispatchFunctionCached(R::ClassOf(uncalibrator), kUncalibratorVerb) : nullptr;
    if (!fn || !player) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && Call(uncalibrator, f);
}

bool CallToolInitByIndex(void* tool, void* toolgun, int32_t index, float value) {
    if (!tool || !toolgun || !R::IsLive(tool) || !R::IsLive(toolgun)) return false;
    void* toolCls = R::ClassOf(tool);
    const int32_t offCal = R::FindPropertyOffset(toolCls, L"calibration");
    const int32_t offIdx = R::FindPropertyOffset(toolCls, L"index");
    const int32_t offGm = R::FindPropertyOffset(toolCls, L"gamemode");
    int32_t rmbByte = -1;
    uint8_t rmbMask = 0;
    void* fn = R::FindDispatchFunctionCached(toolCls, kToolVerb);
    if (offCal < 0 || offIdx < 0 || !fn || !R::FindBoolProperty(R::ClassOf(toolgun), L"isRMB", rmbByte, rmbMask))
        return false;
    auto* t = reinterpret_cast<uint8_t*>(tool);
    *reinterpret_cast<float*>(t + offCal) = value;
    *reinterpret_cast<int32_t*>(t + offIdx) = index;
    if (offGm >= 0 && !*reinterpret_cast<void**>(t + offGm))
        *reinterpret_cast<void**>(t + offGm) = world_singleton::Gamemode();
    auto* rmb = reinterpret_cast<uint8_t*>(toolgun) + rmbByte;
    *rmb = static_cast<uint8_t>(*rmb | rmbMask);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"toolgun", toolgun) && Call(tool, f);
}


bool CallToolInit(void* tool, void* toolgun) {
    if (!tool || !toolgun || !R::IsLive(tool) || !R::IsLive(toolgun)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tool), kToolVerb);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"toolgun", toolgun) && Call(tool, f);
}

}  // namespace ue_wrap::dish_writers
