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

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

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

void* g_midPrimCompClass = nullptr;   // UPrimitiveComponent (owns CreateDynamicMaterialInstance)
void* g_createMidFn = nullptr;
void* g_midClass = nullptr;           // UMaterialInstanceDynamic (owns SetTextureParameterValue)
void* g_setTexParamFn = nullptr;

bool ResolveMidFns() {
    if (!g_midPrimCompClass) g_midPrimCompClass = R::FindClass(L"PrimitiveComponent");
    if (g_midPrimCompClass && !g_createMidFn)
        g_createMidFn = R::FindFunction(g_midPrimCompClass, L"CreateDynamicMaterialInstance");
    if (!g_midClass) g_midClass = R::FindClass(L"MaterialInstanceDynamic");
    if (g_midClass && !g_setTexParamFn)
        g_setTexParamFn = R::FindFunction(g_midClass, L"SetTextureParameterValue");
    return g_createMidFn && g_setTexParamFn;
}

void* g_staticMeshCompClass = nullptr;  // owns SetStaticMesh
void* g_setStaticMeshFn = nullptr;

bool ResolveStaticMeshFn() {
    if (!g_staticMeshCompClass)
        g_staticMeshCompClass = R::FindClass(P::name::StaticMeshComponentClass);
    if (g_staticMeshCompClass && !g_setStaticMeshFn)
        g_setStaticMeshFn = R::FindFunction(g_staticMeshCompClass, P::name::SetStaticMeshFn);
    return g_setStaticMeshFn != nullptr;
}

void* g_sceneCompMobCls = nullptr;   // owns SetMobility (USceneComponent)
void* g_setMobilityFn = nullptr;

bool ResolveSetMobilityFn() {
    if (!g_sceneCompMobCls) g_sceneCompMobCls = R::FindClass(L"SceneComponent");
    if (g_sceneCompMobCls && !g_setMobilityFn)
        g_setMobilityFn = R::FindFunction(g_sceneCompMobCls, L"SetMobility");
    return g_setMobilityFn != nullptr;
}

void* g_primCompClass = nullptr;     // owns SetMaterial (UPrimitiveComponent)
void* g_setMaterialFn = nullptr;
void* g_staticMeshAssetClass = nullptr;  // UStaticMesh (owns GetMaterial(MaterialIndex))
void* g_smGetMaterialFn = nullptr;

bool ResolveSetMaterialFn() {
    if (!g_primCompClass) g_primCompClass = R::FindClass(L"PrimitiveComponent");
    if (g_primCompClass && !g_setMaterialFn)
        g_setMaterialFn = R::FindFunction(g_primCompClass, L"SetMaterial");
    return g_setMaterialFn != nullptr;
}

bool ResolveStaticMeshGetMaterialFn() {
    if (!g_staticMeshAssetClass) g_staticMeshAssetClass = R::FindClass(L"StaticMesh");
    if (g_staticMeshAssetClass && !g_smGetMaterialFn)
        g_smGetMaterialFn = R::FindFunction(g_staticMeshAssetClass, L"GetMaterial");
    return g_smGetMaterialFn != nullptr;
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


void* GetParticleSystemTemplate(void* particleSystemComponent) {
    if (!particleSystemComponent) return nullptr;
    // UParticleSystemComponent::Template is a UObjectProperty (UParticleSystem*). Resolve its
    // offset ONCE via reflection (FindPropertyOffset walks the FProperty chain) -- same idiom as
    // SetMovementVelocity below. The raw pointer read is safe (the component is game-thread-owned;
    // the caller polls on the game thread).
    static int32_t sTemplateOffset = -1;
    if (sTemplateOffset < 0) {
        void* pscClass = R::FindClass(L"ParticleSystemComponent");
        if (!pscClass) return nullptr;  // class not loaded yet -- caller retries next poll
        sTemplateOffset = R::FindPropertyOffset(pscClass, L"Template");
        if (sTemplateOffset < 0) {
            UE_LOGE("engine: GetParticleSystemTemplate -- 'Template' property not found on "
                    "UParticleSystemComponent (CXX dump stale?)");
            return nullptr;
        }
        UE_LOGI("engine: UParticleSystemComponent::Template offset = 0x%X", sTemplateOffset);
    }
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(particleSystemComponent) + sTemplateOffset);
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

void* CreateDynamicMaterialInstance(void* component, int32_t elementIndex) {
    if (!component || !ResolveMidFns()) {
        UE_LOGE("engine: CreateDynamicMaterialInstance unresolved (comp=%p fn=%p)",
                component, g_createMidFn);
        return nullptr;
    }
    ParamFrame f(g_createMidFn);
    f.Set<int32_t>(L"ElementIndex", elementIndex);
    // SourceMaterial stays null (use the slot's current material as the MID parent);
    // OptionalName stays zeroed (FName{0,0} == NAME_None).
    if (!Call(component, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

bool SetTextureParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, void* texture) {
    if (!materialInstanceDynamic || !texture || !ResolveMidFns()) return false;
    const R::FName pn = ue_wrap::fname_utils::StringToFName(paramName);
    ParamFrame f(g_setTexParamFn);
    f.Set<R::FName>(L"ParameterName", pn);
    f.Set<void*>(L"Value", texture);
    return Call(materialInstanceDynamic, f);
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

bool ClearSkeletalMesh(void* component) {
    // SetSkeletalMesh(NewMesh=null): the component renders NOTHING, but its
    // visibility flags + AttachParent graph are untouched. Used by
    // StartPuppetMeshRagdoll to suppress the native ACharacter::Mesh @0x0280
    // double-image during the puppet flop WITHOUT hiding it -- mesh_playerVisible
    // @0x4F8 is a CHILD of @0x0280 and UE4 USceneComponent::IsVisible() cascades
    // up the parent chain, so a bHiddenInGame hide on @0x0280 would kill the
    // simulating child too (the reverted 2026-05-25 "no visible model" regression).
    // Clearing the mesh asset avoids that cascade. bReinitPose=false (no mesh to
    // repose from). Separate fn because SetSkeletalMesh deliberately rejects null.
    if (!component || !ResolveMeshFns() || !g_setSkeletalMeshFn) return false;
    ParamFrame f(g_setSkeletalMeshFn);
    f.Set<void*>(L"NewMesh", nullptr);
    f.Set<bool>(L"bReinitPose", false);
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

bool SetStaticMesh(void* component, void* staticMeshAsset) {
    // UStaticMeshComponent::SetStaticMesh(NewMesh) -- swap the static-mesh asset
    // onto a component (the trash-proxy mirror re-skin). Recomputes the
    // component's bounds + collision body from the new mesh in the SAME call
    // (atomic on the game thread -- no window where the visual is the new mesh
    // but collision is still the old bound). Rejects null: a proxy must never go
    // invisible, so the caller resolves a fallback mesh instead. Game thread only.
    if (!component || !staticMeshAsset || !ResolveStaticMeshFn()) {
        UE_LOGE("engine: SetStaticMesh unresolved (comp=%p mesh=%p fn=%p)",
                component, staticMeshAsset, g_setStaticMeshFn);
        return false;
    }
    ParamFrame f(g_setStaticMeshFn);
    f.Set<void*>(L"NewMesh", staticMeshAsset);
    return Call(component, f);
}

bool SetComponentMobility(void* component, uint8_t mobility) {
    // USceneComponent::SetMobility(NewMobility). Movable (2) re-registers the render state so a
    // runtime SetStaticMesh + SetActorLocation actually apply (a Static component no-ops both).
    if (!component || !ResolveSetMobilityFn()) {
        UE_LOGE("engine: SetComponentMobility unresolved (comp=%p fn=%p)", component, g_setMobilityFn);
        return false;
    }
    ParamFrame f(g_setMobilityFn);
    f.Set<uint8_t>(L"NewMobility", mobility);
    return Call(component, f);
}

bool SetComponentMaterial(void* component, int32_t elementIndex, void* material) {
    // UPrimitiveComponent::SetMaterial(ElementIndex, Material). `material` may be null:
    // SetMaterial(0, null) reverts the slot to the mesh asset's default material (the
    // proxy uses this to clear a stale clump override when re-skinning back to a pile).
    // Resolved on PrimitiveComponent (the BlueprintCallable lives there); dispatched on
    // the StaticMeshComponent instance works -- ProcessEvent keys on the function object.
    if (!component || !ResolveSetMaterialFn()) {
        UE_LOGE("engine: SetComponentMaterial unresolved (comp=%p fn=%p)", component, g_setMaterialFn);
        return false;
    }
    ParamFrame f(g_setMaterialFn);
    f.Set<int32_t>(L"ElementIndex", elementIndex);
    f.Set<void*>(L"Material", material);
    return Call(component, f);
}

void* GetStaticMeshMaterial(void* staticMeshAsset, int32_t materialIndex) {
    // UStaticMesh::GetMaterial(MaterialIndex) -> UMaterialInterface*. The clump form's
    // per-chipType look (setTex: SetMaterial(0, getChipPileType(chipType).GetMaterial(0))).
    if (!staticMeshAsset || !ResolveStaticMeshGetMaterialFn()) return nullptr;
    ParamFrame f(g_smGetMaterialFn);
    f.Set<int32_t>(L"MaterialIndex", materialIndex);
    if (!Call(staticMeshAsset, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
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

void* GetStaticMeshComponent(void* actor) {
    // The UStaticMeshComponent subobject of `actor` (an AStaticMeshActor's root,
    // or any actor that owns one). Reflection child-walk -- no UFunction. Used to
    // reach the host-authoritative trash proxy's mesh component (SetStaticMesh /
    // collision). Game thread.
    if (!actor) return nullptr;
    for (const auto& c : R::ChildObjectsOf(actor)) {
        if (c.className == L"StaticMeshComponent") {
            return c.object;
        }
    }
    return nullptr;
}

FRotator GetVisibleMeshWorldRotation(void* actor) {
    // See engine.h: a chipPile's visible orientation lives on its StaticMesh
    // component's relative rotation (random roll from UserConstructionScript),
    // not the actor root. Read the mesh component's WORLD rotation so a mirror
    // proxy reproduces the per-instance orientation. Fall back to the actor
    // rotation if the actor owns no StaticMeshComponent.
    void* comp = GetStaticMeshComponent(actor);
    if (!comp) return GetActorRotation(actor);
    return GetComponentWorldRotation(comp);
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
