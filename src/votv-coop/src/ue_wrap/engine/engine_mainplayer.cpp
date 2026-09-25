// ue_wrap/engine/engine_mainplayer.cpp -- the player pawn accessors (grab state, flashlight,
// ragdoll, damage). The public API lives in ue_wrap/engine/engine.h; this TU implements the
// wrappers scoped to the player class: every function reads or writes a field of it or
// dispatches on its directly owned components (the physics handle for grab release, the
// light and spot-light components for the flashlight setters). These are the canonical
// engine-substrate accessors; gameplay code uses them in place of inline offset
// dereferences. The caches here are file-private plain pointers, resolved once per process.

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"   // GetComponentLocation (the hold frame's camera)

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/reflected_offset.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// The cached physics-handle class and its release UFunction for the release-before-destroy
// path, eager-resolved so the first cross-peer destroy does not hit an unresolved class on a
// peer that just connected. The class is engine-stable and loads with the world.
ue_wrap::CachedObjRef g_phcClsCache;  // a slot-validated cache
void* g_phcReleaseFnCache = nullptr;

bool ResolvePhcReleaseCached() {
    if (g_phcReleaseFnCache && g_phcClsCache.Alive()) return true;
    g_phcClsCache.Set(R::FindClass(P::name::PhysicsHandleComponentClass));
    if (!g_phcClsCache.Raw()) return false;
    g_phcReleaseFnCache = R::FindFunction(g_phcClsCache.Raw(), P::name::ReleaseComponentFn);
    return g_phcReleaseFnCache != nullptr;
}

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

// The cached ragdoll UFunctions, both owned by the player class. Resolved on the first
// successful call; the class loads with gameplay, so by the time any puppet exists they
// resolve.
void* g_ragdollModeFn  = nullptr;
void* g_forceGetUpFn   = nullptr;
void* g_forceWakeupFn  = nullptr;

void ResolveRagdollFns() {
    if (g_ragdollModeFn && g_forceGetUpFn && g_forceWakeupFn) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;
    if (!g_ragdollModeFn)  g_ragdollModeFn  = R::FindFunction(cls, P::name::MainPlayerRagdollModeFn);
    if (!g_forceGetUpFn)   g_forceGetUpFn   = R::FindFunction(cls, P::name::MainPlayerForceGetUpFn);
    if (!g_forceWakeupFn)  g_forceWakeupFn  = R::FindFunction(cls, P::name::MainPlayerForceWakeupFn);
}

// The cached add-player-damage UFunction (resolves once the player class is loaded, like the
// ragdoll ones).
void* g_addPlayerDamageFn = nullptr;

void ResolveAddPlayerDamageFn() {
    if (g_addPlayerDamageFn) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;
    g_addPlayerDamageFn = R::FindFunction(cls, P::name::MainPlayerAddPlayerDamageFn);
}


// The damage body pulse: the cached gore hurt material and the primitive component's
// material UFunctions (count, get, set).
ue_wrap::CachedObjRef g_hurtMat;  // a slot-validated cache
void* g_getNumMatFn = nullptr, *g_getMatFn = nullptr, *g_setMatFn = nullptr;

void* ResolveHurtMat() {
    if (g_hurtMat.Alive()) return g_hurtMat.Raw();
    g_hurtMat.Set(ResolveMaterialByName(P::name::PlayerHurtFlashMaterialName));
    return g_hurtMat.Raw();
}
void ResolveMatFns() {
    if (g_getNumMatFn && g_getMatFn && g_setMatFn) return;
    void* c = R::FindClass(L"PrimitiveComponent");
    if (!c) return;
    if (!g_getNumMatFn) g_getNumMatFn = R::FindFunction(c, L"GetNumMaterials");
    if (!g_getMatFn)    g_getMatFn    = R::FindFunction(c, L"GetMaterial");
    if (!g_setMatFn)    g_setMatFn    = R::FindFunction(c, L"SetMaterial");
}

// Swap every material slot on one component to `mat`, appending each (component, index,
// original) into `saved`, caller-owned, for the restore.
void SwapComponentMaterials(void* comp, void* mat, std::vector<SavedMaterial>& saved) {
    if (!comp || !R::IsLive(comp) || !mat) return;
    ResolveMatFns();
    if (!g_getNumMatFn || !g_getMatFn || !g_setMatFn) return;
    int32_t num = 0;
    { ParamFrame f(g_getNumMatFn); if (Call(comp, f)) num = f.Get<int32_t>(L"ReturnValue"); }
    for (int32_t i = 0; i < num && i < 16; ++i) {
        void* orig = nullptr;
        { ParamFrame f(g_getMatFn); f.Set<int32_t>(L"ElementIndex", i); if (Call(comp, f)) orig = f.Get<void*>(L"ReturnValue"); }
        SavedMaterial sm;
        sm.component.Set(comp);  // comp validated at entry; orig fresh from GetMaterial
        sm.index = i;
        sm.original.Set(orig);
        saved.push_back(sm);
        { ParamFrame f(g_setMatFn); f.Set<int32_t>(L"ElementIndex", i); f.Set<void*>(L"Material", mat); Call(comp, f); }
    }
}

// The puppet's two visible body meshes (the native character mesh and the player-visible
// mesh): both render, and both carry the skin.
void* PuppetVisibleMesh(void* puppet, size_t off) {
    if (!puppet || !R::IsLive(puppet)) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(puppet) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

}  // namespace

void* ResolveMaterialByName(const wchar_t* name) {
    if (!name) return nullptr;
    // A material instance constant first (most pak materials), then a base material, then an
    // any-class fallback.
    void* m = R::FindObject(name, P::name::MaterialInstanceConstantClassName);
    if (m && R::IsLive(m)) return m;
    m = R::FindObject(name, L"Material");
    if (m && R::IsLive(m)) return m;
    m = R::FindObject(name, nullptr);
    return (m && R::IsLive(m)) ? m : nullptr;
}

bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved) {
    // Self-protecting: a non-empty `saved` means a flash is already applied, and a second apply
    // without a restore in between would re-read the hurt material as the original and stick it
    // permanently. The caller is edge-gated, but the guard makes the API safe for any caller.
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
            void* comp = s.component.Get();  // slot-validated
            if (!comp) continue;  // component GC'd
            // If the cached original material was collected during the flash window, restore null:
            // a null material reverts the slot to the mesh asset's default, the skin, the correct
            // outcome.
            void* orig = s.original.Get();  // null -> revert to the asset default (correct)
            ParamFrame f(g_setMatFn);
            f.Set<int32_t>(L"ElementIndex", s.index);
            f.Set<void*>(L"Material", orig);
            Call(comp, f);
        }
    }
    saved.clear();
    return true;
}

// Eager-resolve the hurt material and the material UFunctions, so the first damage flash
// does no object-array name walks. Called once per puppet spawn, cached forever on success;
// if the material is not loaded yet the warm-up is a no-op and the first flash resolves
// lazily, but it is resident base content, so steady-state flashes never re-walk the array.
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
    // The read-only twin of ReleaseMainPlayerGrabIfHolding (the same grabbing-actor slot, no
    // mutation): the snapshot bind paths skip the physics reconcile and teleport converge on a
    // prop the local player is holding, since forcing physics off mid-hold breaks the
    // physics-handle grab at a re-bracket.
    if (!localPlayer || !actor) return false;
    if (!R::IsLive(localPlayer)) return false;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_grabbing_actor();
    if (off < 0) return false;
    void* const* grabbingSlot = reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(localPlayer) + off);
    return *grabbingSlot == actor;
}

bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor) {
    if (!localPlayer || !actor) return false;
    // Validate the player's liveness before any field dereference: the pawn is normally
    // persistent across the session, but a level unload mid-disconnect could leave the cached
    // pointer dangling.
    if (!R::IsLive(localPlayer)) return false;
    const int32_t offGrabbing = ue_wrap::reflected_offset::MainPlayer_grabbing_actor();
    if (offGrabbing < 0) return false;
    void** grabbingSlot = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + offGrabbing);
    if (*grabbingSlot != actor) return false;
    // Every path that cannot reach the physics handle still CLEARS the slot: callers destroy the
    // actor whether or not this returns true, so leaving the slot set would dangle it. Only the
    // handle teardown is lost, and the blueprint's grab-destroyed delegate runs that from the
    // actor's own destroyed broadcast -- later, but functional.
    const int32_t offHandle = ue_wrap::reflected_offset::MainPlayer_grabHandle();
    if (offHandle < 0 || !ResolvePhcReleaseCached()) {
        UE_LOGW("engine::ReleaseMainPlayerGrabIfHolding: grabHandle offset (%d) or PHC.ReleaseComponent unresolved -- clearing grabbing_actor only; destGrabbed delegate path will run PHC teardown",
                offHandle);
        *grabbingSlot = nullptr;
        return false;
    }
    void* phc = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + offHandle);
    if (phc && R::IsLive(phc)) {
        R::CallFunction(phc, g_phcReleaseFnCache, nullptr);
        UE_LOGI("engine::ReleaseMainPlayerGrabIfHolding: PHC.ReleaseComponent dispatched, actor=%p let go",
                actor);
    } else {
        UE_LOGW("engine::ReleaseMainPlayerGrabIfHolding: PHC pointer null/dead on localPlayer=%p -- only clearing grabbing_actor",
                localPlayer);
    }
    // Mirror the grab-destroyed delegate's cleanup, so state reads later in the same frame do
    // not see the dangling pointer. The component goes with the actor: a caller whose actor lives
    // on (a clump let go) must not leave the pawn naming a body it no longer holds.
    *grabbingSlot = nullptr;
    const int32_t offComp = ue_wrap::reflected_offset::MainPlayer_grabbing_component();
    if (offComp >= 0)
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(localPlayer) + offComp) = nullptr;
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
    const int32_t grabbingOff = ue_wrap::reflected_offset::MainPlayer_grabbing_actor();
    if (grabbingOff >= 0) {
        out.grabbingActor = *reinterpret_cast<void**>(base + grabbingOff);
    }
    // The holding actor: the chip-pile and clump carry slot, added in a later game recook; a
    // missing offset leaves the field null rather than dereferencing a negative offset.
    const int32_t holdingOff = ue_wrap::reflected_offset::MainPlayer_holding_actor();
    if (holdingOff >= 0) {
        out.holdingActor = *reinterpret_cast<void**>(base + holdingOff);
    }
    const int32_t grabsHeavyOff = ue_wrap::reflected_offset::MainPlayer_grabsHeavy();
    const int32_t heavyOff       = ue_wrap::reflected_offset::MainPlayer_Heavy();
    const int32_t grabLenOff     = ue_wrap::reflected_offset::MainPlayer_grabLen();
    if (grabsHeavyOff >= 0) out.grabsHeavy = *reinterpret_cast<bool*>(base + grabsHeavyOff);
    if (heavyOff >= 0)      out.heavy      = *reinterpret_cast<bool*>(base + heavyOff);
    if (grabLenOff >= 0)    out.grabLen    = *reinterpret_cast<float*>(base + grabLenOff);
    return true;
}

void* ReadMainPlayerHitActor(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    // Two offsets, resolved once: hitResult on the player, then the actor inside FHitResult. The
    // engine holds that actor as a TWeakObjectPtr -- a slot and a serial -- so it is resolved
    // through the object array rather than dereferenced, and a recycled slot answers null.
    static void*   sCls = nullptr;
    static int32_t sHitOff = -1;
    static int32_t sActorOff = -1;
    void* cls = R::ClassOf(mainPlayer);
    if (!cls) return nullptr;
    if (cls != sCls) {
        sCls = cls;
        sHitOff = R::FindPropertyOffset(cls, L"hitResult");
        sActorOff = -1;
        if (void* inner = R::PropertyInnerStruct(cls, L"hitResult"))
            sActorOff = R::FindPropertyOffset(inner, L"Actor");
    }
    if (sHitOff < 0 || sActorOff < 0) return nullptr;
    const uint8_t* weak = reinterpret_cast<const uint8_t*>(mainPlayer) + sHitOff + sActorOff;
    int32_t idx = 0, serial = 0;
    std::memcpy(&idx, weak, sizeof(idx));
    std::memcpy(&serial, weak + sizeof(idx), sizeof(serial));
    return R::ResolveWeakObject(idx, serial);
}

void* ReadMainPlayerLookAtActor(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_lookAtActor();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off);
}

bool WriteMainPlayerLookAtActor(void* mainPlayer, void* actor) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_lookAtActor();
    if (off < 0) return false;
    // The look-at actor is the cached interaction-trace result the game re-derives every tick,
    // not a setter-managed field with side-effect setup, so a direct write is safe and is the
    // established in-tree pattern (the device screen nulls and restores it around a use
    // dispatch). Setting it to the aimed actor for the single dispatch that follows lets the
    // blueprint's cast resolve to it; the next tick's trace overwrites it. Game thread only.
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off) = actor;
    return true;
}

bool ReadMainPlayerLookAt(void* mainPlayer, MainPlayerLookAt& out) {
    out = MainPlayerLookAt{};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t offActor  = ue_wrap::reflected_offset::MainPlayer_lookAtActor();
    const int32_t offComp   = ue_wrap::reflected_offset::MainPlayer_lookAtComponent();
    const int32_t offBounds = ue_wrap::reflected_offset::MainPlayer_lookAtBoundsReplace();
    const int32_t offVerify = ue_wrap::reflected_offset::MainPlayer_lookAtVerify();
    const int32_t offState  = ue_wrap::reflected_offset::MainPlayer_lookAtState();
    // All five or none: a caller comparing a partial set would read a field that never moves as
    // agreement, which is the opposite of what it asked.
    if (offActor < 0 || offComp < 0 || offBounds < 0 || offVerify < 0 || offState < 0) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    out.actor         = *reinterpret_cast<void**>(base + offActor);
    out.component     = *reinterpret_cast<void**>(base + offComp);
    out.boundsReplace = *reinterpret_cast<void**>(base + offBounds);
    out.verify        = *reinterpret_cast<uint8_t*>(base + offVerify);
    out.state         = *reinterpret_cast<uint8_t*>(base + offState);
    return true;
}

bool ReadMainPlayerLookAtLocals(void* lookAtFunction, const uint8_t* locals,
                                MainPlayerLookAtLocals& out) {
    out = MainPlayerLookAtLocals{};
    if (!lookAtFunction || !locals) return false;
    // A UFunction is a UStruct, so the walk that resolves an instance field resolves a local the
    // same way: the function's own ChildProperties chain carries its parameters and its locals
    // together. Cached against the UFunction, because the walk is linear over a body with 115 of
    // them and this runs once per tick per pawn.
    static void*   sFn    = nullptr;
    static int32_t sActor = -1, sComp = -1, sBound = -1, sNum = -1, sState = -1;
    if (lookAtFunction != sFn) {
        sFn    = lookAtFunction;
        sActor = R::FindPropertyOffset(lookAtFunction, L"lookatActorCurrent");
        sComp  = R::FindPropertyOffset(lookAtFunction, L"lookatComponentCurrent");
        sBound = R::FindPropertyOffset(lookAtFunction, L"lookatBoundObject");
        sNum   = R::FindPropertyOffset(lookAtFunction, L"lookatNUmber");
        // The compiler's own temporary for the BitsToByte(!activeInterface) result. It is the
        // literal left operand of the state comparison, so it is read; deriving it from
        // activeInterface would bake in an assumption about which bit that library call fills.
        // The ONE local here that is not write-once: the body recomputes it into the same slot on
        // the disagree path before storing lookAtState. Both computations read !activeInterface
        // with no write to it in between, so a POST read is the compare's operand today -- a cook
        // that wrote activeInterface between them would silently invalidate this column alone.
        sState = R::FindPropertyOffset(lookAtFunction, L"CallFunc_BitsToByte_Byte");
        if (sActor < 0 || sComp < 0 || sBound < 0 || sNum < 0 || sState < 0)
            UE_LOGW("engine::ReadMainPlayerLookAtLocals: a LookAtFunction local did not resolve "
                    "(actor %d component %d bound %d number %d state %d) -- the compare cannot be "
                    "read on this cook", sActor, sComp, sBound, sNum, sState);
    }
    if (sActor < 0 || sComp < 0 || sBound < 0 || sNum < 0 || sState < 0) return false;
    out.actor       = *reinterpret_cast<void* const*>(locals + sActor);
    out.component   = *reinterpret_cast<void* const*>(locals + sComp);
    out.boundObject = *reinterpret_cast<void* const*>(locals + sBound);
    out.number      = *(locals + sNum);
    out.stateByte   = *(locals + sState);
    return true;
}

bool ReadMainPlayerRadialSelect(void* mainPlayer, bool& releaseEToUse, int32_t& actionIndex) {
    releaseEToUse = false;
    actionIndex   = -1;
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t offRel = ue_wrap::reflected_offset::MainPlayer_releaseEToUse();
    const int32_t offIdx = ue_wrap::reflected_offset::MainPlayer_actionIndex();
    if (offRel < 0 || offIdx < 0) return false;  // BP class not loaded / fields renamed (recook)
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    releaseEToUse = *reinterpret_cast<bool*>(base + offRel);
    actionIndex   = *reinterpret_cast<int32_t*>(base + offIdx);
    return true;
}

bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t offActor = ue_wrap::reflected_offset::MainPlayer_grabbing_actor();
    const int32_t offComp  = ue_wrap::reflected_offset::MainPlayer_grabbing_component();
    if (offActor < 0 || offComp < 0) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    *reinterpret_cast<void**>(base + offActor) = actor;
    *reinterpret_cast<void**>(base + offComp)  = component;
    return true;
}

bool SetPhysicsHandleTarget(void* phc, const FVector& location, const FRotator& rotation) {
    if (!phc || !R::IsLive(phc)) return false;
    // Resolved once, found or not: this runs every tick of a carry, and a miss must not turn into a
    // class walk and a function-list read per tick.
    static void* fn = nullptr;
    static bool  tried = false;
    if (!tried) {
        tried = true;
        if (void* cls = R::FindClass(P::name::PhysicsHandleComponentClass))
            fn = R::FindFunction(cls, P::name::SetTargetLocationAndRotationFn);
        if (!fn) UE_LOGW("engine::SetPhysicsHandleTarget: PhysicsHandleComponent.SetTargetLocationAndRotation unresolved -- a puppet's carried clump will not follow its hand");
    }
    if (!fn) return false;
    ParamFrame f(fn);
    f.SetRaw(L"NewLocation", &location, sizeof(location));
    f.SetRaw(L"NewRotation", &rotation, sizeof(rotation));
    return Call(phc, f);
}

bool ReadMainPlayerCameraLocation(void* mainPlayer, FVector& out) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t camOff = ue_wrap::reflected_offset::MainPlayer_Camera();
    if (camOff < 0) return false;
    void* cam = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + camOff);
    if (!cam || !R::IsLive(cam)) return false;
    out = GetComponentLocation(cam);
    return true;
}

bool ReadMainPlayerArmLength(void* mainPlayer, float& out) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_armLength();
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(mainPlayer) + off);
    return true;
}

void* ReadMainPlayerGrabHandle(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_grabHandle();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off);
}

void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_heavyGrab();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off);
}

void* ReadMainPlayerGrabTimeline(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_grabTimeline();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + off);
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

bool ReadMainPlayerSittingOn(void* mainPlayer, void*& sittingOn) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    static int32_t s_off = -2;  // -2 not looked at, -1 looked and failed
    if (s_off == -2) s_off = R::FindPropertyOffset(R::ClassOf(mainPlayer), L"sittingOn");
    if (s_off < 0) return false;
    sittingOn = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + s_off);
    return true;
}

bool WriteMainPlayerDead(void* mainPlayer, bool dead) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    // FindBoolProperty, not the plain-byte offset the READ above uses: a UE bool property can be a
    // bitfield sharing its byte with its neighbours, and a byte-wide store would clear them. The
    // read can afford the shortcut; a write cannot. Resolved once per class, then a masked store.
    static void* sCls = nullptr;
    static int32_t sByte = -1;
    static uint8_t sMask = 0;
    void* cls = R::ClassOf(mainPlayer);
    if (cls != sCls || sMask == 0) {
        int32_t b = -1; uint8_t m = 0;
        if (!R::FindBoolProperty(cls, L"dead", b, m)) return false;
        sCls = cls; sByte = b; sMask = m;
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(mainPlayer) + sByte;
    if (dead) *p |= sMask; else *p &= static_cast<uint8_t>(~sMask);
    return ((*p & sMask) != 0) == dead;   // read BACK: wrote is not holds
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

bool ForceMainPlayerWakeup(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    ResolveRagdollFns();
    if (!g_forceWakeupFn) return false;
    ParamFrame f(g_forceWakeupFn);  // no params
    return Call(mainPlayer, f);
}

namespace {
// The can-ragdoll flag: the ragdoll verb's own pre-condition early-out, and the single choke
// point for every ragdoll cause on a pawn, death included, since the dead flag is set only
// through the fallen path, reachable only from the ragdoll verb, whose first instruction
// checks this flag. Two lanes hold it shut: the killer-wisp false-grab belt (the drop notify
// fires an unconditional ragdoll death from bytecode we cannot intercept, and a health pin
// cannot stop a ragdoll death while this flag can) and the knockout-respawn death gate. A
// plain blueprint bool with no setter and no write sites in the player's own bytecode, so
// nothing in the game fights our value. The byte and mask are cached once: property layout
// is stable for a game build (only class pointers go stale across level travel, and none is
// cached here).
bool ResolveCanRagdoll(void* mainPlayer, uint8_t*& byteOut, uint8_t& maskOut) {
    static int32_t sCanRagByte = -1;
    static uint8_t sCanRagMask = 0;
    if (sCanRagByte < 0) {
        int32_t b = -1; uint8_t m = 0;
        if (!R::FindBoolProperty(R::ClassOf(mainPlayer), L"canRagdoll", b, m)) {
            UE_LOGW("engine: mainPlayer canRagdoll bool not resolvable -- ragdoll gate unavailable");
            return false;
        }
        sCanRagByte = b; sCanRagMask = m;
    }
    byteOut = reinterpret_cast<uint8_t*>(mainPlayer) + sCanRagByte;
    maskOut = sCanRagMask;
    return true;
}
}  // namespace

bool SetMainPlayerCanRagdoll(void* mainPlayer, bool allowed) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    uint8_t* p = nullptr; uint8_t mask = 0;
    if (!ResolveCanRagdoll(mainPlayer, p, mask)) return false;
    if (allowed) *p |= mask; else *p &= static_cast<uint8_t>(~mask);
    return true;
}

bool ReadMainPlayerCanRagdoll(void* mainPlayer, bool& allowed) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    uint8_t* p = nullptr; uint8_t mask = 0;
    if (!ResolveCanRagdoll(mainPlayer, p, mask)) return false;
    allowed = (*p & mask) != 0;
    return true;
}


bool ReadMainPlayerArm(void* player, FVector& start, FVector& end) {
    if (!player || !R::IsLive(player)) return false;
    // Revalidated on every hit, so a world unload cannot hand back a freed function.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), P::name::MainPlayerArmFn);
    if (!fn) return false;
    ParamFrame f(fn);   // customLength stays zero: the player's own reach
    if (!Call(player, f)) return false;
    start = f.Get<FVector>(P::name::MainPlayerArmStartParam);
    end = f.Get<FVector>(P::name::MainPlayerArmEndParam);
    return true;
}

bool MainPlayerArmOutOffsets(void* armFunction, int32_t& startOff, int32_t& endOff) {
    if (!armFunction) return false;
    startOff = R::FindParamOffset(armFunction, P::name::MainPlayerArmStartParam);
    endOff = R::FindParamOffset(armFunction, P::name::MainPlayerArmEndParam);
    return startOff >= 0 && endOff >= 0;
}

bool InvokeAddPlayerDamage(void* mainPlayer, float damage, bool blood) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    ResolveAddPlayerDamageFn();
    if (!g_addPlayerDamageFn) return false;
    ParamFrame f(g_addPlayerDamageFn);
    f.Set<float>(L"Damage", damage);  // damageLocation/fullBody/Source zero-init
    // The blood parameter guards the block that adds the blood-loss effect; leaving it false
    // makes a synthetic hit unlike any hit the game produces.
    if (blood) f.Set<bool>(L"blood", true);
    return Call(mainPlayer, f);
}


}  // namespace ue_wrap::engine
