// ue_wrap/core/component_calls.cpp -- see component_calls.h. Each helper keeps its own
// lazy per-function cache.

#include "ue_wrap/core/component_calls.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::component_calls {
namespace {

namespace R = ue_wrap::reflection;

void* g_setTextFn = nullptr;             // UTextBlock::SetText(FText)
const wchar_t* g_setTextParam = nullptr;
void* g_setSoundFn = nullptr;            // UAudioComponent::SetSound(USoundBase*)
void* g_activateFn = nullptr;            // UActorComponent::Activate(bool bReset)
void* g_setActiveFn = nullptr;           // UActorComponent::SetActive(bNewActive, bReset)
void* g_setVisibilityFn = nullptr;       // USceneComponent::SetVisibility(bNewVisibility, bPropagate)

}  // namespace

bool SetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !text) return false;
    if (!g_setTextFn) {
        if (void* cls = R::ClassOf(textBlock))
            g_setTextFn = R::FindFunction(cls, L"SetText");
        if (g_setTextFn) {
            if (R::FindParamOffset(g_setTextFn, L"InText") >= 0) g_setTextParam = L"InText";
            else if (R::FindParamOffset(g_setTextFn, L"inText") >= 0) g_setTextParam = L"inText";
        }
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
    if (!comp) return false;
    if (!g_activateFn) {
        if (void* cls = R::ClassOf(comp))
            g_activateFn = R::FindFunction(cls, L"Activate");
    }
    if (!g_activateFn) return false;
    ue_wrap::ParamFrame f(g_activateFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bReset", true);
    return ue_wrap::Call(comp, f);
}

// UActorComponent::SetActive(bNewActive, bReset) -- the hum loops' native
// switch (uber [1116/1150/1155] uses SetActive(value, true)).
bool SetActive(void* comp, bool value) {
    if (!comp) return false;
    if (!g_setActiveFn) {
        if (void* cls = R::ClassOf(comp))
            g_setActiveFn = R::FindFunction(cls, L"SetActive");
    }
    if (!g_setActiveFn) return false;
    ue_wrap::ParamFrame f(g_setActiveFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bNewActive", value);
    f.Set<bool>(L"bReset", true);
    return ue_wrap::Call(comp, f);
}

// USceneComponent::SetVisibility(bNewVisibility, bPropagateToChildren) -- the
// unit lamps (uber [1115/1126/1149/1154] use SetVisibility(value, false)).
bool SetVisibility(void* comp, bool value) {
    if (!comp) return false;
    if (!g_setVisibilityFn) {
        if (void* cls = R::ClassOf(comp))
            g_setVisibilityFn = R::FindFunction(cls, L"SetVisibility");
    }
    if (!g_setVisibilityFn) return false;
    ue_wrap::ParamFrame f(g_setVisibilityFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bNewVisibility", value);
    f.Set<bool>(L"bPropagateToChildren", false);
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

bool CallParamless(void* obj, void* fn) {
    if (!obj || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

}  // namespace ue_wrap::component_calls
