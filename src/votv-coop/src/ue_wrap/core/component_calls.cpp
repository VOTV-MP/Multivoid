// ue_wrap/core/component_calls.cpp -- see component_calls.h. A function the component's own class declares keeps a
// lazy cache; one a parent declares resolves through the dispatch cache, which climbs to it.

#include "ue_wrap/core/component_calls.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

namespace ue_wrap::component_calls {
namespace {

namespace R = ue_wrap::reflection;

void* g_setTextFn = nullptr;             // UTextBlock::SetText(FText)
const wchar_t* g_setTextParam = nullptr;
void* g_setSoundFn = nullptr;            // UAudioComponent::SetSound(USoundBase*)

}  // namespace

bool SetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !text) return false;
    if (!g_setTextFn) {
        if (void* cls = R::ClassOf(textBlock))
            g_setTextFn = R::FindFunction(cls, L"SetText");
        // One spelling answers for both: the param lookup compares insensitively, and what is
        // being tested here is whether the parameter EXISTS at all.
        if (g_setTextFn && R::FindParamOffset(g_setTextFn, L"InText") >= 0)
            g_setTextParam = L"InText";
    }
    if (!g_setTextFn || !g_setTextParam) return false;
    uint8_t ftext[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::MintFText(text, ftext)) return false;
    ue_wrap::ParamFrame f(g_setTextFn);
    if (!f.valid() || !f.SetRaw(g_setTextParam, ftext, sizeof(ftext))) return false;
    return ue_wrap::Call(textBlock, f);
}

bool SetSound(void* comp, void* sound) {
    if (!comp || !sound) return false;
    if (!g_setSoundFn) {
        if (void* cls = R::ClassOf(comp))
            g_setSoundFn = R::FindFunction(cls, L"SetSound");
    }
    if (!g_setSoundFn) return false;
    ue_wrap::ParamFrame f(g_setSoundFn);
    if (!f.valid() || !f.SetRaw(L"NewSound", &sound, sizeof(sound))) return false;
    return ue_wrap::Call(comp, f);
}

bool Activate(void* comp) {
    // Declared on UActorComponent, which no component class a caller holds is.
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"Activate") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bReset", true);
    return ue_wrap::Call(comp, f);
}

bool SetActive(void* comp, bool value, bool reset) {
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetActive") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bNewActive", value);
    f.Set<bool>(L"bReset", reset);
    return ue_wrap::Call(comp, f);
}

bool SetVolumeMultiplier(void* comp, float mult) {
    if (!comp) return false;
    static void* sFn = nullptr;
    if (!sFn) {
        if (void* cls = R::ClassOf(comp))
            sFn = R::FindFunction(cls, L"SetVolumeMultiplier");
    }
    if (!sFn) return false;
    ue_wrap::ParamFrame f(sFn);
    if (!f.valid()) return false;
    f.Set<float>(L"NewVolumeMultiplier", mult);
    return ue_wrap::Call(comp, f);
}

bool SetWorldScale3D(void* comp, float uniformScale) {
    // Declared on USceneComponent, which no component class a caller holds is.
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetWorldScale3D") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<ue_wrap::FVector>(L"NewScale", ue_wrap::FVector{uniformScale, uniformScale, uniformScale}) &&
           ue_wrap::Call(comp, f);
}

bool CallParamless(void* obj, void* fn) {
    if (!obj || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

bool CallParamlessNamed(void* obj, const wchar_t* verb) {
    if (!obj || !verb) return false;
    return CallParamless(obj, R::FindDispatchFunctionCached(R::ClassOf(obj), verb));
}

}  // namespace ue_wrap::component_calls
