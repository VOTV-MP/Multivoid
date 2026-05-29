// ue_wrap/engine_mainplayer.cpp -- AmainPlayer_C accessors (grab state + flashlight).
//
// Extracted from ue_wrap/engine.cpp (2026-05-29 modular refactor, M-1).
// Public API lives in ue_wrap/engine.h; this TU implements the
// AmainPlayer_C-scoped Read/Write/Set wrappers in `namespace ue_wrap::engine`.
//
// Scope: every function here reads or writes a field of AmainPlayer_C (or
// dispatches on its directly-owned components -- the PhysicsHandle for grab
// release, the Light_R + spot-light component for flashlight/cone setters).
// Per Principle 7 these are the canonical engine-substrate accessors; gameplay
// /coop code uses them in place of inline `*reinterpret_cast<T*>(self + off)`
// derefs.
//
// Used by:
//   - coop/item_activate.cpp (flashlight Read/Write + light/cone setters)
//   - coop/grab_observer.cpp (grab state reads + PHC release)
//   - coop/net_pump.cpp (grab state read for held-prop replication)
//   - coop/remote_prop.cpp (ReleaseMainPlayerGrabIfHolding on destroy)
//   - harness/autotest.cpp (grab-pair writes + component-ptr reads)
//
// Anon-namespace caches in this TU are file-private per the engine_pawn
// precedent (independent FindClass per TU, no header-level shared cache).
// All cached globals are plain void* -- no std::atomic / std::mutex moves,
// so the MSVC incremental-link DLL corruption rule (RULE 2026-05-29) does
// not structurally trigger.

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// Cached PHC class + ReleaseComponent UFunction for the release-before-destroy
// path. Eager-resolved (audit fix #1, 2026-05-25) so the first cross-peer
// PropDestroy doesn't hit a not-yet-resolved class on a peer that just
// connected. The PhysicsHandleComponent UClass is engine-stable and loads
// with the world; by the time any session connects it is resolvable.
void* g_phcClsCache       = nullptr;
void* g_phcReleaseFnCache = nullptr;

bool ResolvePhcReleaseCached() {
    if (g_phcReleaseFnCache && R::IsLive(g_phcClsCache)) return true;
    g_phcClsCache = R::FindClass(P::name::PhysicsHandleComponentClass);
    if (!g_phcClsCache) return false;
    g_phcReleaseFnCache = R::FindFunction(g_phcClsCache, P::name::ReleaseComponentFn);
    return g_phcReleaseFnCache != nullptr;
}

// Cached UFunctions for the light/cone setters below. Resolved on first
// successful call. Pre-A-1 (2026-05-29) these were duplicated across
// coop/item_activate.cpp's ApplyToPuppet + DebugForceToggle as static
// locals; Principle-7 wrapper extraction folds the cache here (one
// resolve per process) and lets gameplay code stay reflection-free.
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

bool WarmupPhcReleaseCache() {
    const bool ok = ResolvePhcReleaseCached();
    if (ok) {
        UE_LOGI("engine::WarmupPhcReleaseCache: PHC.ReleaseComponent cached @ %p (cls @ %p)",
                g_phcReleaseFnCache, g_phcClsCache);
    } else {
        UE_LOGW("engine::WarmupPhcReleaseCache: PHC class or UFunction not loaded yet -- will retry on next caller");
    }
    return ok;
}

bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor) {
    if (!localPlayer || !actor) return false;
    // 2026-05-25 audit fix #2: validate localPlayer liveness before any field
    // dereference. mainPlayer_C is normally persistent across the session
    // but a level unload mid-disconnect could leave the cached pointer
    // dangling -- IsLive catches it via the FUObjectItem.Flags read.
    if (!R::IsLive(localPlayer)) return false;
    void** grabbingSlot = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    if (*grabbingSlot != actor) return false;
    if (!ResolvePhcReleaseCached()) {
        // PHC class still not loaded somehow (defensive). Clear the slot
        // anyway so subsequent reads don't see a doomed pointer; the BP
        // destGrabbed delegate will run the PHC teardown via the actor's
        // OnDestroyed broadcast (less ideal timing but functional).
        UE_LOGW("engine::ReleaseMainPlayerGrabIfHolding: PHC.ReleaseComponent unresolved -- clearing grabbing_actor only; destGrabbed delegate path will run PHC teardown");
        *grabbingSlot = nullptr;
        return false;
    }
    void* phc = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + ue_wrap::reflected_offset::MainPlayer_grabHandle());
    if (phc && R::IsLive(phc)) {
        R::CallFunction(phc, g_phcReleaseFnCache, nullptr);
        UE_LOGI("engine::ReleaseMainPlayerGrabIfHolding: PHC.ReleaseComponent dispatched on doomed actor=%p",
                actor);
    } else {
        UE_LOGW("engine::ReleaseMainPlayerGrabIfHolding: PHC pointer null/dead on localPlayer=%p -- only clearing grabbing_actor",
                localPlayer);
    }
    // Mirror destGrabbed delegate cleanup so subsequent state reads
    // (other observers in same frame) don't see the dangling pointer.
    *grabbingSlot = nullptr;
    return true;
}

void* ReadPhysicsHandleGrabbedComponent(void* phc) {
    if (!phc || !R::IsLive(phc)) return nullptr;
    void* comp = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(phc) + P::off::UPhysicsHandleComponent_GrabbedComponent);
    return comp;  // may be nullptr if the PHC has no current grabbed component
}

bool ReadMainPlayerGrabState(void* mainPlayer, MainPlayerGrabState& out) {
    out = {};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    out.grabbingActor = *reinterpret_cast<void**>(base + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    // holding_actor: chipPile / clump morph carry slot. Added in a later VOTV
    // recook; if reflected_offset returns -1 we leave the field null rather
    // than dereffing a negative offset.
    const int32_t holdingOff = ue_wrap::reflected_offset::MainPlayer_holding_actor();
    if (holdingOff >= 0) {
        out.holdingActor = *reinterpret_cast<void**>(base + holdingOff);
    }
    out.grabsHeavy    = *reinterpret_cast<bool*>(base + ue_wrap::reflected_offset::MainPlayer_grabsHeavy());
    out.heavy         = *reinterpret_cast<bool*>(base + ue_wrap::reflected_offset::MainPlayer_Heavy());
    out.grabLen       = *reinterpret_cast<float*>(base + ue_wrap::reflected_offset::MainPlayer_grabLen());
    return true;
}

bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    *reinterpret_cast<void**>(base + ue_wrap::reflected_offset::MainPlayer_grabbing_actor())     = actor;
    *reinterpret_cast<void**>(base + ue_wrap::reflected_offset::MainPlayer_grabbing_component()) = component;
    return true;
}

void* ReadMainPlayerGrabHandle(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayer) + ue_wrap::reflected_offset::MainPlayer_grabHandle());
}

void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayer) + ue_wrap::reflected_offset::MainPlayer_heavyGrab());
}

void* ReadMainPlayerGrabTimeline(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayer) + ue_wrap::reflected_offset::MainPlayer_grabTimeline());
}

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
    out.flashlight      = *reinterpret_cast<bool*>(base + P::off::AmainPlayer_flashlight);
    out.hasFlashlight   = *reinterpret_cast<bool*>(base + P::off::AmainPlayer_hasFlashlight);
    out.crankFlashlight = *reinterpret_cast<bool*>(base + P::off::AmainPlayer_crankFlashlight);
    out.mode            = *reinterpret_cast<uint8_t*>(base + P::off::AmainPlayer_flashlightMode);
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
