// ue_wrap/world/level_instance.cpp -- see ue_wrap/world/level_instance.h.

#include "ue_wrap/world/level_instance.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <cstdio>

namespace ue_wrap::level_instance {
namespace {

namespace R = reflection;

bool CallBool(void* obj, const wchar_t* fn) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ParamFrame pf(f);
    return f && pf.valid() && Call(obj, pf) && pf.Get<bool>(L"ReturnValue");
}

bool CallWithBool(void* obj, const wchar_t* fn, const wchar_t* param, bool v) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ParamFrame pf(f);
    return f && pf.valid() && pf.Set<bool>(param, v) && Call(obj, pf);
}

}  // namespace

void* Load(void* worldContext, const std::wstring& map, const FVector& at) {
    void* cdo = R::FindClassDefaultObject(L"LevelStreamingDynamic");
    void* fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"LoadLevelInstance") : nullptr;
    ParamFrame pf(fn);
    if (!fn || !pf.valid() || !worldContext) return nullptr;
    // The engine copies the name out of the frame; the buffer only has to outlive the call.
    std::wstring name = map;
    const R::FString s{name.data(), static_cast<int32_t>(name.size() + 1), static_cast<int32_t>(name.size() + 1)};
    if (!pf.Set<void*>(L"WorldContextObject", worldContext) || !pf.Set<R::FString>(L"LevelName", s) ||
        !pf.Set<FVector>(L"Location", at) || !pf.Set<FRotator>(L"Rotation", FRotator{}) || !Call(cdo, pf))
        return nullptr;
    return pf.Get<bool>(L"bOutSuccess") ? pf.Get<void*>(L"ReturnValue") : nullptr;
}

void* LoadedLevel(void* streaming) {
    if (!streaming || !R::IsLive(streaming) || !CallBool(streaming, L"IsLevelLoaded") ||
        !CallBool(streaming, L"IsLevelVisible"))
        return nullptr;
    void* f = R::FindDispatchFunctionCached(R::ClassOf(streaming), L"GetLoadedLevel");
    ParamFrame pf(f);
    return (f && pf.valid() && Call(streaming, pf)) ? pf.Get<void*>(L"ReturnValue") : nullptr;
}

std::string DescribeState(void* streaming) {
    if (!streaming || !R::IsLive(streaming)) return "no streaming level";
    void* f = R::FindDispatchFunctionCached(R::ClassOf(streaming), L"GetLoadedLevel");
    ParamFrame pf(f);
    void* level = (f && pf.valid() && Call(streaming, pf)) ? pf.Get<void*>(L"ReturnValue") : nullptr;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "loaded=%d visible=%d level=%p", CallBool(streaming, L"IsLevelLoaded") ? 1 : 0,
                  CallBool(streaming, L"IsLevelVisible") ? 1 : 0, level);
    return buf;
}

bool Unload(void* streaming) {
    if (!streaming || !R::IsLive(streaming)) return false;
    const bool hidden = CallWithBool(streaming, L"SetShouldBeVisible", L"bInShouldBeVisible", false);
    const bool unloaded = CallWithBool(streaming, L"SetShouldBeLoaded", L"bInShouldBeLoaded", false);
    CallWithBool(streaming, L"SetIsRequestingUnloadAndRemoval", L"bInIsRequestingUnloadAndRemoval", true);
    return hidden && unloaded;
}

bool DeactivateUntaggedReverb(void* worldContext) {
    void* cdo = R::FindClassDefaultObject(L"GameplayStatics");
    void* fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"DeactivateReverbEffect") : nullptr;
    ParamFrame pf(fn);
    return fn && pf.valid() && worldContext && pf.Set<void*>(L"WorldContextObject", worldContext) && Call(cdo, pf);
}

}  // namespace ue_wrap::level_instance
