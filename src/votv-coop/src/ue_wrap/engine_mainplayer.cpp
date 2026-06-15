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
#include <vector>

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

// Cached ragdoll UFunctions (Inc2b). Both owned by mainPlayer_C. Resolved on
// first successful call; mainPlayer_C loads with gameplay so by the time any
// puppet exists these resolve.
void* g_ragdollModeFn = nullptr;
void* g_forceGetUpFn  = nullptr;

void ResolveRagdollFns() {
    if (g_ragdollModeFn && g_forceGetUpFn) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;
    if (!g_ragdollModeFn) g_ragdollModeFn = R::FindFunction(cls, P::name::MainPlayerRagdollModeFn);
    if (!g_forceGetUpFn)  g_forceGetUpFn  = R::FindFunction(cls, P::name::MainPlayerForceGetUpFn);
}

// vitals Inc3-WIRE: cached "Add Player Damage" UFunction (resolves once mainPlayer_C
// is loaded, like the ragdoll fns above).
void* g_addPlayerDamageFn = nullptr;

void ResolveAddPlayerDamageFn() {
    if (g_addPlayerDamageFn) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;
    g_addPlayerDamageFn = R::FindFunction(cls, P::name::MainPlayerAddPlayerDamageFn);
}


// Inc3 damage body-pulse: cached solid-red material + the UPrimitiveComponent
// material UFunctions (GetNumMaterials / GetMaterial / SetMaterial).
void* g_hurtMat = nullptr;
void* g_getNumMatFn = nullptr, *g_getMatFn = nullptr, *g_setMatFn = nullptr;

void* ResolveHurtMat() {
    if (g_hurtMat && R::IsLive(g_hurtMat)) return g_hurtMat;
    g_hurtMat = ResolveMaterialByName(P::name::PlayerHurtFlashMaterialName);
    return g_hurtMat;
}
void ResolveMatFns() {
    if (g_getNumMatFn && g_getMatFn && g_setMatFn) return;
    void* c = R::FindClass(L"PrimitiveComponent");
    if (!c) return;
    if (!g_getNumMatFn) g_getNumMatFn = R::FindFunction(c, L"GetNumMaterials");
    if (!g_getMatFn)    g_getMatFn    = R::FindFunction(c, L"GetMaterial");
    if (!g_setMatFn)    g_setMatFn    = R::FindFunction(c, L"SetMaterial");
}

// Swap EVERY material slot on ONE component `comp` to `mat`, APPENDING each
// (comp, index, original) into `saved` (caller-owned) for RestoreHurtFlashMaterial.
void SwapComponentMaterials(void* comp, void* mat, std::vector<SavedMaterial>& saved) {
    if (!comp || !R::IsLive(comp) || !mat) return;
    ResolveMatFns();
    if (!g_getNumMatFn || !g_getMatFn || !g_setMatFn) return;
    int32_t num = 0;
    { ParamFrame f(g_getNumMatFn); if (Call(comp, f)) num = f.Get<int32_t>(L"ReturnValue"); }
    for (int32_t i = 0; i < num && i < 16; ++i) {
        void* orig = nullptr;
        { ParamFrame f(g_getMatFn); f.Set<int32_t>(L"ElementIndex", i); if (Call(comp, f)) orig = f.Get<void*>(L"ReturnValue"); }
        saved.push_back({comp, i, orig});
        { ParamFrame f(g_setMatFn); f.Set<int32_t>(L"ElementIndex", i); f.Set<void*>(L"Material", mat); Call(comp, f); }
    }
}

// The puppet's two visible body meshes (the native ACharacter slot @0x280 AND
// mesh_playerVisible @0x4F8 -- both render, both carry the kel4 skin).
void* PuppetVisibleMesh(void* puppet, size_t off) {
    if (!puppet || !R::IsLive(puppet)) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(puppet) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

}  // namespace

void* ResolveMaterialByName(const wchar_t* name) {
    if (!name) return nullptr;
    // MaterialInstanceConstant first (most pak materials), then a base Material
    // (e.g. EmissiveMeshMaterial), then any-class fallback.
    void* m = R::FindObject(name, P::name::MaterialInstanceConstantClassName);
    if (m && R::IsLive(m)) return m;
    m = R::FindObject(name, L"Material");
    if (m && R::IsLive(m)) return m;
    m = R::FindObject(name, nullptr);
    return (m && R::IsLive(m)) ? m : nullptr;
}

bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved) {
    // Self-protecting: if `saved` is non-empty a flash is ALREADY applied. A second
    // apply without an intervening restore would re-read the gore material as the
    // "original" and stick it permanently. The caller (RemotePlayer flash edge) is
    // edge-gated, but guarding here makes the ue_wrap API safe for any caller.
    if (!saved.empty()) return false;
    void* hurtMat = ResolveHurtMat();
    if (!hurtMat || !puppet || !R::IsLive(puppet)) return false;
    SwapComponentMaterials(PuppetVisibleMesh(puppet, P::off::ACharacter_Mesh), hurtMat, saved);
    SwapComponentMaterials(PuppetVisibleMesh(puppet, P::off::AmainPlayer_mesh_playerVisible), hurtMat, saved);
    return !saved.empty();
}

bool RestoreHurtFlashMaterial(void* /*puppet*/, std::vector<SavedMaterial>& saved) {
    ResolveMatFns();
    if (g_setMatFn) {
        for (const auto& s : saved) {
            if (!s.component || !R::IsLive(s.component)) continue;  // component GC'd
            // If the cached original material was GC'd during the ~0.5 s window,
            // restore nullptr -- SetMaterial(null) reverts the slot to the
            // SkeletalMesh asset's default (the kel skin), the correct outcome.
            void* orig = (s.original && R::IsLive(s.original)) ? s.original : nullptr;
            ParamFrame f(g_setMatFn);
            f.Set<int32_t>(L"ElementIndex", s.index);
            f.Set<void*>(L"Material", orig);
            Call(s.component, f);
        }
    }
    saved.clear();
    return true;
}

// Eager-resolve the hurt material + the UPrimitiveComponent material UFunctions so
// the first damage flash does zero GUObjectArray name walks. Called once per puppet
// spawn (cached forever on success). If the gore material isn't loaded yet it's a
// no-op and the first flash resolves lazily -- but it is resident base content, so
// the warmup succeeds and steady-state flashes never re-walk the object array.
void WarmupHurtFlashCache() {
    ResolveHurtMat();
    ResolveMatFns();
}

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

bool IsMainPlayerGrabbing(void* localPlayer, void* actor) {
    // Read-only twin of ReleaseMainPlayerGrabIfHolding (same grabbing_actor
    // slot, no mutation). Fork B 2d: the snapshot bind paths skip the
    // physics-reconcile + teleport-converge on a prop the LOCAL player is
    // holding -- forcing SimulatePhysics off mid-hold breaks the
    // PhysicsHandle grab at a re-bracket.
    if (!localPlayer || !actor) return false;
    if (!R::IsLive(localPlayer)) return false;
    void* const* grabbingSlot = reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(localPlayer) +
        ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    return *grabbingSlot == actor;
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

void* ReadMainPlayerLookAtActor(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_lookAtActor();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off);
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

bool ReadMainPlayerRagdollState(void* mainPlayer, bool& isRagdoll, bool& dead) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t offRag  = ue_wrap::reflected_offset::MainPlayer_isRagdoll();
    const int32_t offDead = ue_wrap::reflected_offset::MainPlayer_dead();
    if (offRag < 0 || offDead < 0) return false;  // BP class not loaded / field renamed
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    isRagdoll = *reinterpret_cast<bool*>(base + offRag);
    dead      = *reinterpret_cast<bool*>(base + offDead);
    return true;
}

bool SetMainPlayerRagdollMode(void* mainPlayer, bool ragdoll, bool passOut, bool death) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    ResolveRagdollFns();
    if (!g_ragdollModeFn) return false;
    ParamFrame f(g_ragdollModeFn);
    f.Set<bool>(L"ragdoll", ragdoll);
    f.Set<bool>(L"passOut", passOut);
    f.Set<bool>(L"death", death);
    return Call(mainPlayer, f);
}

bool ForceMainPlayerGetUp(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    ResolveRagdollFns();
    if (!g_forceGetUpFn) return false;
    ParamFrame f(g_forceGetUpFn);  // no params
    return Call(mainPlayer, f);
}


bool InvokeAddPlayerDamage(void* mainPlayer, float damage) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    ResolveAddPlayerDamageFn();
    if (!g_addPlayerDamageFn) return false;
    ParamFrame f(g_addPlayerDamageFn);
    f.Set<float>(L"Damage", damage);  // damageLocation/fullBody/blood/Source zero-init
    return Call(mainPlayer, f);
}

void* AddPlayerDamageFunctionPtr() {
    // Resolve (idempotent) + return the mainPlayer_C "Add Player Damage" UFunction so a coop
    // module can install a ProcessEvent PRE-interceptor on it (the Killer Wisp host-neutralize:
    // zero the wisp's limb-tear damage to the HOST while it false-grabs a client). null until
    // mainPlayer_C is loaded. Game thread.
    ResolveAddPlayerDamageFn();
    return g_addPlayerDamageFn;
}

}  // namespace ue_wrap::engine
