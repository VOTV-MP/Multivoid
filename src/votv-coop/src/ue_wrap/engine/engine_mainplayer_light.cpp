// ue_wrap/engine/engine_mainplayer_light.cpp -- the player pawn's flashlight and the light components' setters (see
// ue_wrap/engine/engine.h): the pawn's light component, its snapshot and state, the battery ticker's repoint, and the
// intensity, visibility and cone-angle calls the flashlight puppet drives. The player pawn's other accessors are in
// engine_mainplayer.cpp.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// The cached UFunctions for the light and cone setters below, resolved on the first
// successful call: one resolve per process, and gameplay code stays reflection-free.
void* g_setLightIntensityFn      = nullptr;
void* g_setSceneVisibilityFn     = nullptr;
void* g_setSpotOuterConeAngleFn  = nullptr;
void* g_setSpotInnerConeAngleFn  = nullptr;

void* ResolveLightIntensityFn() {
    if (g_setLightIntensityFn) return g_setLightIntensityFn;
    void* cls = R::FindClass(L"LightComponent");
    if (!cls) return nullptr;
    g_setLightIntensityFn = R::FindFunction(cls, P::name::SetIntensityFn);
    return g_setLightIntensityFn;
}
void* ResolveSceneVisibilityFn() {
    if (g_setSceneVisibilityFn) return g_setSceneVisibilityFn;
    void* cls = R::FindClass(P::name::SceneComponentClass);
    if (!cls) return nullptr;
    g_setSceneVisibilityFn = R::FindFunction(cls, P::name::SetVisibilityFn);
    return g_setSceneVisibilityFn;
}
void ResolveSpotConeFns() {
    if (g_setSpotOuterConeAngleFn && g_setSpotInnerConeAngleFn) return;
    void* cls = R::FindClass(P::name::SpotLightComponentClass);
    if (!cls) return;
    if (!g_setSpotOuterConeAngleFn) g_setSpotOuterConeAngleFn = R::FindFunction(cls, P::name::SetOuterConeAngleFn);
    if (!g_setSpotInnerConeAngleFn) g_setSpotInnerConeAngleFn = R::FindFunction(cls, P::name::SetInnerConeAngleFn);
}

}  // namespace

void* GetMainPlayerLightR(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    void* light = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayer) + P::off::AmainPlayer_light_R);
    if (!light || !R::IsLive(light)) return nullptr;
    return light;
}

bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out) {
    if (!light || !R::IsLive(light)) return false;
    auto* base = reinterpret_cast<uint8_t*>(light);
    out.intensity       = *reinterpret_cast<float*>(base + P::off::ULightComponentBase_Intensity);
    out.outerConeAngle  = *reinterpret_cast<float*>(base + P::off::USpotLightComponent_OuterConeAngle);
    out.innerConeAngle  = *reinterpret_cast<float*>(base + P::off::USpotLightComponent_InnerConeAngle);
    const uint8_t flags = *reinterpret_cast<uint8_t*>(base + P::off::USceneComponent_VisFlagsByte);
    out.visible         = (flags & 0x10) != 0;
    return true;
}

bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out) {
    out = {};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    const int32_t equippedOff = ue_wrap::reflected_offset::MainPlayer_equipped_flashlight();
    if (equippedOff < 0) return false;
    out.flashlight         = *reinterpret_cast<bool*>(base + P::off::AmainPlayer_flashlight);
    out.equippedFlashlight = *reinterpret_cast<bool*>(base + equippedOff);
    out.mode               = *reinterpret_cast<uint8_t*>(base + P::off::AmainPlayer_flashlightMode);
    return true;
}

bool RepointFlashlightTicker(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    // The ticker is the one the local pawn's own BeginPlay found or made and kept in `ticker_flashlightController`: the object the
    // game's puppets then took over by GetActorOfClass. The object index lists another ticker object of that class (a dead world's,
    // measured: its `player` was the local pawn while the live one drained a puppet), so the pawn's own field is what names it.
    static int32_t s_ctlOff = -2;
    static void* s_ctlCls = nullptr;
    if (void* const pc = R::ClassOf(mainPlayer); pc != s_ctlCls) {
        s_ctlCls = pc;
        s_ctlOff = R::FindPropertyOffset(pc, L"ticker_flashlightController");
    }
    if (s_ctlOff < 0) return false;
    void* const ticker = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mainPlayer) + s_ctlOff);
    if (!ticker || !R::IsLive(ticker)) return false;
    static int32_t s_off = -2;
    static void* s_cls = nullptr;
    if (void* const cls = R::ClassOf(ticker); cls != s_cls) { s_cls = cls; s_off = R::FindPropertyOffset(cls, L"player"); }
    if (s_off < 0) return false;
    void** const slot = reinterpret_cast<void**>(static_cast<uint8_t*>(ticker) + s_off);
    {   // the state it saw, every ten seconds: a repoint that never logs is either right or looking at another ticker
        static uint64_t s_diagMs = 0;
        const uint64_t nowMs = ::GetTickCount64();
        if (nowMs - s_diagMs >= 10000) {
            s_diagMs = nowMs;
            UE_LOGI("flashlight: battery ticker %p drains %p; the local player is %p", ticker, *slot, mainPlayer);
        }
    }
    if (*slot == mainPlayer) return false;
    UE_LOGI("flashlight: the battery ticker drained %p, not the local player %p (a puppet's BeginPlay took it) -- pointed back",
            *slot, mainPlayer);
    *slot = mainPlayer;
    return true;
}

bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    *reinterpret_cast<bool*>(base + P::off::AmainPlayer_flashlight) = newState;
    return true;
}

bool SetLightIntensity(void* light, float newIntensity) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveLightIntensityFn();
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<float>(L"NewIntensity", newIntensity);
    return Call(light, f);
}

bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren) {
    if (!sceneComponent || !R::IsLive(sceneComponent)) return false;
    void* fn = ResolveSceneVisibilityFn();
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<bool>(L"bNewVisibility", newVisibility);
    f.Set<bool>(L"bPropagateToChildren", propagateToChildren);
    return Call(sceneComponent, f);
}

bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle) {
    if (!spotLight || !R::IsLive(spotLight)) return false;
    ResolveSpotConeFns();
    if (!g_setSpotOuterConeAngleFn) return false;
    ParamFrame f(g_setSpotOuterConeAngleFn);
    f.Set<float>(L"NewOuterConeAngle", newAngle);
    return Call(spotLight, f);
}

bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle) {
    if (!spotLight || !R::IsLive(spotLight)) return false;
    ResolveSpotConeFns();
    if (!g_setSpotInnerConeAngleFn) return false;
    ParamFrame f(g_setSpotInnerConeAngleFn);
    f.Set<float>(L"NewInnerConeAngle", newAngle);
    return Call(spotLight, f);
}

}  // namespace ue_wrap::engine
