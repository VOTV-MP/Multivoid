// ue_wrap/engine_component.cpp -- Scene/Actor component operations.
//
// Extracted from ue_wrap/engine.cpp (2026-05-25 modular refactor).
// Public API lives in ue_wrap/engine.h; this TU implements the
// component-related functions in `namespace ue_wrap::engine`.
//
// Covers:
//   - USceneComponent: GetComponentLocation/Forward, GetComponentRelativeLocation
//     (raw field read), SetComponentVisible
//   - UActorComponent: DestroyComponent, SetComponentTickEnabled
//   - USkinnedMeshComponent: SetAnimTickAlways (raw byte write),
//     SetSkeletalMesh, SetAnimClass
//   - Character helpers: GetCharacterMovementComponent (via ChildObjectsOf),
//     SetMovementVelocity (reflection-resolved property offset)

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

void* g_sceneCompClass = nullptr;
void* g_setVisFn = nullptr;
void* g_setHiddenFn = nullptr;
void* g_getCompLocFn = nullptr;
void* g_getCompFwdFn = nullptr;

void* SceneCompClass() {
    if (!g_sceneCompClass) g_sceneCompClass = R::FindClass(P::name::SceneComponentClass);
    return g_sceneCompClass;
}

bool ResolveCompVis() {
    if (SceneCompClass()) {
        if (!g_setVisFn) g_setVisFn = R::FindFunction(g_sceneCompClass, P::name::SetVisibilityFn);
        if (!g_setHiddenFn) g_setHiddenFn = R::FindFunction(g_sceneCompClass, P::name::SetHiddenInGameFn);
    }
    return g_sceneCompClass && g_setVisFn && g_setHiddenFn;
}

void* g_actorCompClass = nullptr;
void* g_destroyCompFn = nullptr;

bool ResolveDestroy() {
    if (!g_actorCompClass) g_actorCompClass = R::FindClass(P::name::ActorComponentClass);
    if (g_actorCompClass && !g_destroyCompFn)
        g_destroyCompFn = R::FindFunction(g_actorCompClass, P::name::DestroyComponentFn);
    return g_destroyCompFn != nullptr;
}

void* g_skinnedMeshClass = nullptr;   // owns SetSkeletalMesh
void* g_skeletalMeshClass = nullptr;  // owns SetAnimClass
void* g_setSkeletalMeshFn = nullptr;
void* g_setAnimClassFn = nullptr;

bool ResolveMeshFns() {
    if (!g_skinnedMeshClass) g_skinnedMeshClass = R::FindClass(P::name::SkinnedMeshComponentClass);
    if (g_skinnedMeshClass && !g_setSkeletalMeshFn)
        g_setSkeletalMeshFn = R::FindFunction(g_skinnedMeshClass, P::name::SetSkeletalMeshFn);
    if (!g_skeletalMeshClass) g_skeletalMeshClass = R::FindClass(P::name::SkeletalMeshComponentClass);
    if (g_skeletalMeshClass && !g_setAnimClassFn)
        g_setAnimClassFn = R::FindFunction(g_skeletalMeshClass, P::name::SetAnimClassFn);
    return g_setSkeletalMeshFn && g_setAnimClassFn;
}

}  // namespace

FVector GetComponentLocation(void* component) {
    FVector loc;
    if (!component || !SceneCompClass()) return loc;
    if (!g_getCompLocFn) g_getCompLocFn = R::FindFunction(g_sceneCompClass, P::name::GetComponentLocationFn);
    if (!g_getCompLocFn) return loc;
    ParamFrame f(g_getCompLocFn);
    if (!Call(component, f)) return loc;
    f.GetRaw(L"ReturnValue", &loc, sizeof(loc));
    return loc;
}

FVector GetComponentForwardVector(void* component) {
    FVector fwd;
    if (!component || !SceneCompClass()) return fwd;
    if (!g_getCompFwdFn) g_getCompFwdFn = R::FindFunction(g_sceneCompClass, P::name::GetComponentForwardFn);
    if (!g_getCompFwdFn) return fwd;
    ParamFrame f(g_getCompFwdFn);
    if (!Call(component, f)) return fwd;
    f.GetRaw(L"ReturnValue", &fwd, sizeof(fwd));
    return fwd;
}

FVector GetComponentRelativeLocation(void* component) {
    if (!component) return {};
    // Raw field read at USceneComponent::RelativeLocation @ +0x011C (FVector,
    // 12 bytes). NOT a UFunction call -- this is intentional: K2_GetComponent
    // Location returns the computed WORLD transform which composes RelLoc with
    // the parent's world transform AND any transient mid-init state, defeating
    // the whole point of capturing the BP-authored static offset. The raw
    // field carries the value the BP construction script wrote.
    return *reinterpret_cast<FVector*>(
        reinterpret_cast<uint8_t*>(component) + P::off::USceneComponent_RelativeLocation);
}

bool SetComponentVisible(void* component, bool visible, bool propagate) {
    if (!component || !ResolveCompVis()) {
        UE_LOGE("engine: SetComponentVisible unresolved (cls=%p setVis=%p setHidden=%p)",
                g_sceneCompClass, g_setVisFn, g_setHiddenFn);
        return false;
    }
    {
        ParamFrame f(g_setVisFn);
        f.Set<bool>(L"bNewVisibility", visible);
        f.Set<bool>(L"bPropagateToChildren", propagate);
        Call(component, f);
    }
    {
        ParamFrame f(g_setHiddenFn);
        f.Set<bool>(L"NewHidden", !visible);  // UE4.27 param is "NewHidden" (no b prefix)
        f.Set<bool>(L"bPropagateToChildren", propagate);
        Call(component, f);
    }
    return true;
}

bool DestroyComponent(void* component, void* contextObject) {
    if (!component || !ResolveDestroy()) {
        UE_LOGE("engine: DestroyComponent unresolved (cls=%p fn=%p)",
                g_actorCompClass, g_destroyCompFn);
        return false;
    }
    ParamFrame f(g_destroyCompFn);
    f.Set<void*>(L"Object", contextObject);  // the calling object (auth check)
    return Call(component, f);
}

bool SetAnimTickAlways(void* component) {
    if (!component) return false;
    // EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones == 0.
    reinterpret_cast<uint8_t*>(component)[P::off::USkinnedMesh_VisibilityBasedAnimTickOption] = 0;
    return true;
}

bool SetSkeletalMesh(void* component, void* skeletalMeshAsset) {
    if (!component || !skeletalMeshAsset || !ResolveMeshFns()) {
        UE_LOGE("engine: SetSkeletalMesh unresolved (comp=%p mesh=%p fn=%p)",
                component, skeletalMeshAsset, g_setSkeletalMeshFn);
        return false;
    }
    ParamFrame f(g_setSkeletalMeshFn);
    f.Set<void*>(L"NewMesh", skeletalMeshAsset);
    f.Set<bool>(L"bReinitPose", true);
    return Call(component, f);
}

bool SetAnimClass(void* component, void* animBlueprintClass) {
    if (!component || !animBlueprintClass || !ResolveMeshFns()) {
        UE_LOGE("engine: SetAnimClass unresolved (comp=%p cls=%p fn=%p)",
                component, animBlueprintClass, g_setAnimClassFn);
        return false;
    }
    ParamFrame f(g_setAnimClassFn);
    f.Set<void*>(L"NewClass", animBlueprintClass);
    return Call(component, f);
}

void* GetCharacterMovementComponent(void* characterPawn) {
    if (!characterPawn) return nullptr;
    for (const auto& c : R::ChildObjectsOf(characterPawn)) {
        if (c.className == L"CharacterMovementComponent") {
            return c.object;
        }
    }
    UE_LOGW("engine: GetCharacterMovementComponent -- no CMC subobject on %p", characterPawn);
    return nullptr;
}

bool SetComponentTickEnabled(void* component, bool enabled) {
    if (!component) return false;
    static void* sFn = nullptr;
    if (!sFn) {
        void* acc = R::FindClass(P::name::ActorComponentClass);
        if (acc) sFn = R::FindFunction(acc, P::name::SetComponentTickEnabledFn);
    }
    if (!sFn) return false;
    ParamFrame f(sFn);
    f.Set<bool>(L"bEnabled", enabled);
    return Call(component, f);
}

bool SetMovementVelocity(void* movementComp, const FVector& velocity) {
    if (!movementComp) return false;
    // Resolve UMovementComponent::Velocity offset ONCE via reflection
    // (FindPropertyOffset walks the class's FProperty chain). Cached.
    static int32_t sVelocityOffset = -1;
    if (sVelocityOffset < 0) {
        void* mcClass = R::FindClass(L"MovementComponent");
        if (!mcClass) {
            UE_LOGE("engine: SetMovementVelocity -- UMovementComponent class not found");
            return false;
        }
        sVelocityOffset = R::FindPropertyOffset(mcClass, L"Velocity");
        if (sVelocityOffset < 0) {
            UE_LOGE("engine: SetMovementVelocity -- 'Velocity' property not found on UMovementComponent");
            return false;
        }
        UE_LOGI("engine: UMovementComponent::Velocity offset = 0x%X", sVelocityOffset);
    }
    *reinterpret_cast<FVector*>(reinterpret_cast<uint8_t*>(movementComp) + sVelocityOffset) = velocity;
    return true;
}

}  // namespace ue_wrap::engine
