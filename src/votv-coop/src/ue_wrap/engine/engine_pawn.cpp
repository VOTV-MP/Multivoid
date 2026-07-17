// ue_wrap/engine_pawn.cpp -- Pawn / Controller / Camera operations.
//
// Extracted from ue_wrap/engine.cpp (2026-05-25 modular refactor).
// Public API lives in ue_wrap/engine.h; this TU implements the
// pawn/controller/camera-related functions in `namespace ue_wrap::engine`.
//
// Covers:
//   - APawn: GetController, DetachFromController, SpawnDefaultController
//   - AActor: DestroyActor (sharing the same resolver cache as pawn)
//   - AController: GetControlRotation, SetControlRotation (direct field write)
//   - APlayerController: SetViewTargetWithBlend
//   - APlayerCameraManager: GetCameraLocation, GetCameraRotation
//
// The actor/component/widget functions live in their respective TUs.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// Pawn UFunction cache. ResolvePawnFns lazily resolves on first call.
void* g_pawnClass = nullptr;
void* g_actorClass = nullptr;  // shared by destroy + checks; engine_actor caches its own
void* g_getControllerFn = nullptr;
void* g_detachFn = nullptr;
void* g_destroyActorFn = nullptr;
void* g_spawnDefControllerFn = nullptr;

bool ResolvePawnFns() {
    if (!g_pawnClass) g_pawnClass = R::FindClass(P::name::PawnClassName);
    if (g_pawnClass) {
        if (!g_getControllerFn) g_getControllerFn = R::FindFunction(g_pawnClass, P::name::GetControllerFn);
        if (!g_detachFn) g_detachFn = R::FindFunction(g_pawnClass, P::name::DetachFromControllerFn);
        if (!g_spawnDefControllerFn) g_spawnDefControllerFn = R::FindFunction(g_pawnClass, P::name::SpawnDefaultControllerFn);
    }
    if (!g_actorClass) g_actorClass = R::FindClass(P::name::ActorClassName);
    if (g_actorClass && !g_destroyActorFn) g_destroyActorFn = R::FindFunction(g_actorClass, P::name::DestroyActorFn);
    return g_getControllerFn && g_detachFn && g_destroyActorFn;
}

// Controller + camera caches.
void* g_controllerClass = nullptr;
void* g_getControlRotFn = nullptr;
void* g_setControlRotFn = nullptr;
void* g_pcClass = nullptr;
void* g_setViewTargetFn = nullptr;
void* g_camMgrClass = nullptr;
void* g_getCamLocFn = nullptr;
void* g_getCamRotFn = nullptr;
void* g_camMgr = nullptr;  // cached instance; FindObjectByClass walks the array

bool ResolveCamMgrFns() {
    if (!g_camMgrClass) g_camMgrClass = R::FindClass(P::name::PlayerCameraManagerClass);
    if (g_camMgrClass) {
        if (!g_getCamLocFn) g_getCamLocFn = R::FindFunction(g_camMgrClass, P::name::GetCameraLocationFn);
        if (!g_getCamRotFn) g_getCamRotFn = R::FindFunction(g_camMgrClass, P::name::GetCameraRotationFn);
    }
    return g_getCamLocFn && g_getCamRotFn;
}

// Cached camera manager; only walk the GUObjectArray when the cache is empty or
// the previous instance was destroyed (level change). Safe for per-frame callers.
void* CamMgr() {
    if (g_camMgr && !R::IsLive(g_camMgr)) g_camMgr = nullptr;
    if (!g_camMgr) g_camMgr = R::FindObjectByClass(P::name::PlayerCameraManagerClass);
    return g_camMgr;
}

}  // namespace

void* GetController(void* pawn) {
    if (!pawn || !ResolvePawnFns()) return nullptr;
    ParamFrame f(g_getControllerFn);
    if (!Call(pawn, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

void SetControlRotation(void* controller, const FRotator& rot) {
    if (!controller) return;
    // Audit H4 (2026-05-27): call K2_SetControlRotation via reflection
    // instead of writing the field directly. The UFunction's body runs
    // ProcessViewRotation + UpdateRotation in addition to the field
    // assignment; the direct write skipped those. Lazy resolve on first
    // call (PlayerController is loaded by engine boot, well before any
    // puppet controller is wired). Falls back to the direct write only
    // if resolution fails (PlayerController class somehow not loaded --
    // wouldn't happen in a working build but defensive).
    if (!g_controllerClass) g_controllerClass = R::FindClass(P::name::ControllerClassName);
    if (g_controllerClass && !g_setControlRotFn)
        g_setControlRotFn = R::FindFunction(g_controllerClass, P::name::SetControlRotationFn);
    if (g_setControlRotFn) {
        ParamFrame f(g_setControlRotFn);
        f.Set<FRotator>(L"NewRotation", rot);
        Call(controller, f);
        return;
    }
    UE_LOGW("engine_pawn: SetControlRotation falling back to direct write "
            "(K2_SetControlRotation unresolved)");
    *reinterpret_cast<FRotator*>(reinterpret_cast<uint8_t*>(controller)
                                 + P::off::AController_ControlRotation) = rot;
}

FRotator GetControlRotation(void* controller) {
    FRotator rot;
    if (!controller) return rot;
    if (!g_controllerClass) g_controllerClass = R::FindClass(P::name::ControllerClassName);
    if (g_controllerClass && !g_getControlRotFn)
        g_getControlRotFn = R::FindFunction(g_controllerClass, P::name::GetControlRotationFn);
    if (!g_getControlRotFn) return rot;
    ParamFrame f(g_getControlRotFn);
    if (!Call(controller, f)) return rot;
    f.GetRaw(L"ReturnValue", &rot, sizeof(rot));
    return rot;
}

bool SetViewTargetWithBlend(void* playerController, void* newViewTarget, float blendTime) {
    if (!playerController || !newViewTarget) return false;
    if (!g_pcClass) g_pcClass = R::FindClass(P::name::PlayerControllerClassName);
    if (g_pcClass && !g_setViewTargetFn)
        g_setViewTargetFn = R::FindFunction(g_pcClass, P::name::SetViewTargetWithBlendFn);
    if (!g_setViewTargetFn) { UE_LOGE("engine: SetViewTargetWithBlend unresolved"); return false; }
    ParamFrame f(g_setViewTargetFn);
    f.Set<void*>(L"NewViewTarget", newViewTarget);
    f.Set<float>(L"BlendTime", blendTime);
    f.Set<float>(L"BlendExp", 0.f);
    f.Set<bool>(L"bLockOutgoing", false);
    return Call(playerController, f);
}

FVector GetCameraLocation() {
    FVector loc;
    if (!ResolveCamMgrFns()) return loc;
    void* mgr = CamMgr();
    if (!mgr) return loc;
    ParamFrame f(g_getCamLocFn);
    if (!Call(mgr, f)) return loc;
    f.GetRaw(L"ReturnValue", &loc, sizeof(loc));
    return loc;
}

FRotator GetCameraRotation() {
    FRotator rot;
    if (!ResolveCamMgrFns()) return rot;
    void* mgr = CamMgr();
    if (!mgr) return rot;
    ParamFrame f(g_getCamRotFn);
    if (!Call(mgr, f)) return rot;
    f.GetRaw(L"ReturnValue", &rot, sizeof(rot));
    return rot;
}

bool DetachFromController(void* pawn) {
    if (!pawn || !ResolvePawnFns()) return false;
    ParamFrame f(g_detachFn);  // DetachFromControllerPendingDestroy: no params
    return Call(pawn, f);
}

bool DestroyActor(void* actor) {
    if (!actor || !ResolvePawnFns()) return false;
    ParamFrame f(g_destroyActorFn);  // K2_DestroyActor: no params
    return Call(actor, f);
}

bool SpawnDefaultController(void* pawn) {
    if (!pawn || !ResolvePawnFns() || !g_spawnDefControllerFn) {
        UE_LOGE("engine: SpawnDefaultController unresolved (fn=%p)", g_spawnDefControllerFn);
        return false;
    }
    ParamFrame f(g_spawnDefControllerFn);  // no params; spawns AIControllerClass + possesses
    return Call(pawn, f);
}

}  // namespace ue_wrap::engine
