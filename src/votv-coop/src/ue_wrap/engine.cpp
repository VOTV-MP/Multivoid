#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command,
// SpecificPlayer) parameter frame (UE4.27 x64 ABI). Layout is fixed by the
// function's UProperties: object ptr, then FString (16B), then object ptr.
#pragma pack(push, 1)
struct ExecuteConsoleCommandParams {
    void* WorldContextObject;   // 0x00
    R::FString Command;         // 0x08  (Data ptr / Num / Max)
    void* SpecificPlayer;       // 0x18  (nullptr = the first local player)
};                              // size 0x20
#pragma pack(pop)
static_assert(sizeof(ExecuteConsoleCommandParams) == 0x20, "param frame layout");

// Resolved once (the CDO + UFunction never move; the GameInstance persists for
// the process lifetime, so caching its pointer is safe across level loads).
void* g_kslCdo = nullptr;
void* g_execFn = nullptr;
void* g_worldContext = nullptr;

void* ResolveWorldContext() {
    // The GameInstance persists across level loads and is a valid world context.
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) return gi;
    // Fall back to any live World (e.g. before the GameInstance is up).
    return R::FindObjectByClass(P::name::WorldClass);
}

bool Resolve() {
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(P::name::KismetSystemLibraryClass);
    if (g_kslCdo && !g_execFn) {
        if (void* cls = R::ClassOf(g_kslCdo)) {
            g_execFn = R::FindFunction(cls, P::name::ExecuteConsoleCommandFn);
        }
    }
    // World context can become available later than the CDO; re-resolve until
    // found. Also drop it if it was destroyed (a stale World from the pre-
    // GameInstance fallback would otherwise be used after a level reload).
    if (g_worldContext && !R::IsLive(g_worldContext)) g_worldContext = nullptr;
    if (!g_worldContext) g_worldContext = ResolveWorldContext();
    return g_kslCdo && g_execFn && g_worldContext;
}

}  // namespace

bool ExecuteConsoleCommand(const wchar_t* command) {
    if (!command) return false;
    if (!Resolve()) {
        UE_LOGE("engine: ExecuteConsoleCommand unresolved (cdo=%p fn=%p world=%p)",
                g_kslCdo, g_execFn, g_worldContext);
        return false;
    }

    // The command FString. ExecuteConsoleCommand takes a const FString& and only
    // reads it (it forwards to GEngine->Exec); it does not take ownership, so a
    // local buffer is correct -- nothing frees it. UE's FString::Num counts the
    // null terminator.
    std::wstring buf(command);
    R::FString cmd{};
    cmd.Data = buf.data();
    cmd.Num = static_cast<int32_t>(buf.size()) + 1;
    cmd.Max = cmd.Num;

    ExecuteConsoleCommandParams params{};
    params.WorldContextObject = g_worldContext;
    params.Command = cmd;
    params.SpecificPlayer = nullptr;

    const bool ok = R::CallFunction(g_kslCdo, g_execFn, &params);
    if (ok) {
        UE_LOGI("engine: console command issued: %ls", command);
    } else {
        UE_LOGE("engine: CallFunction failed for console command: %ls", command);
    }
    return ok;
}

// ---- actor spawning + transform -----------------------------------------
namespace {

void* g_gsCdo = nullptr;       // Default__GameplayStatics
void* g_beginSpawnFn = nullptr;
void* g_finishSpawnFn = nullptr;
void* g_actorClass = nullptr;  // the Actor UClass (owns K2_Get/SetActorLocation)
void* g_getLocFn = nullptr;
void* g_setLocFn = nullptr;
void* g_getFwdFn = nullptr;
void* g_setRotFn = nullptr;
void* g_setTickFn = nullptr;

// ESpawnActorCollisionHandlingMethod::AlwaysSpawn -- spawn no matter what
// (the orphan must exist even if it overlaps geometry).
constexpr uint8_t kAlwaysSpawn = 1;

bool ResolveSpawn() {
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo) {
        void* cls = R::ClassOf(g_gsCdo);
        if (cls && !g_beginSpawnFn) g_beginSpawnFn = R::FindFunction(cls, P::name::BeginDeferredSpawnFn);
        if (cls && !g_finishSpawnFn) g_finishSpawnFn = R::FindFunction(cls, P::name::FinishSpawningActorFn);
    }
    return g_gsCdo && g_beginSpawnFn && g_finishSpawnFn;
}

bool ResolveActorFns() {
    if (!g_actorClass) g_actorClass = R::FindClass(P::name::ActorClassName);
    if (g_actorClass) {
        if (!g_getLocFn) g_getLocFn = R::FindFunction(g_actorClass, P::name::GetActorLocationFn);
        if (!g_setLocFn) g_setLocFn = R::FindFunction(g_actorClass, P::name::SetActorLocationFn);
        if (!g_getFwdFn) g_getFwdFn = R::FindFunction(g_actorClass, P::name::GetActorForwardVectorFn);
        if (!g_setRotFn) g_setRotFn = R::FindFunction(g_actorClass, P::name::SetActorRotationFn);
        if (!g_setTickFn) g_setTickFn = R::FindFunction(g_actorClass, P::name::SetActorTickEnabledFn);
    }
    return g_actorClass && g_getLocFn && g_setLocFn;
}

}  // namespace

void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn) {
    if (!actorClass) return nullptr;
    if (!ResolveSpawn()) {
        UE_LOGE("engine: SpawnActor unresolved (cdo=%p begin=%p finish=%p)",
                g_gsCdo, g_beginSpawnFn, g_finishSpawnFn);
        return nullptr;
    }
    if (!g_worldContext) g_worldContext = ResolveWorldContext();

    const FTransform xform = MakeTransform(location);

    // 1) BeginDeferredActorSpawnFromClass -> AActor* (uninitialized).
    ParamFrame begin(g_beginSpawnFn);
    begin.Set<void*>(L"WorldContextObject", g_worldContext);
    begin.Set<void*>(L"ActorClass", actorClass);
    begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
    begin.Set<void*>(L"Owner", nullptr);
    if (!Call(g_gsCdo, begin)) {
        UE_LOGE("engine: BeginDeferredActorSpawnFromClass call failed");
        return nullptr;
    }
    void* actor = begin.Get<void*>(L"ReturnValue");
    if (!actor) {
        UE_LOGE("engine: BeginDeferredActorSpawnFromClass returned null");
        return nullptr;
    }

    // 1b) ROOT-CAUSE remote-pawn fix: BEFORE FinishSpawningActor runs BeginPlay,
    //     zero the fields that make a pawn behave as a local player. BeginPlay's
    //     native auto-possess reads AutoPossessPlayer; clearing it here prevents
    //     the orphan from grabbing a 2nd PlayerController (which stole the local
    //     player's input/view). These are plain data fields -> direct writes.
    if (inertPawn) {
        auto* a = reinterpret_cast<uint8_t*>(actor);
        a[P::off::APawn_AutoPossessPlayer] = 0;   // no PLAYER controller (no input/view hijack)
        a[P::off::APawn_AutoPossessAI] = 0;        // we possess explicitly post-spawn
        a[P::off::AActor_AutoReceiveInput] = 0;    // EAutoReceiveInput::Disabled
        a[P::off::AActor_bBlockInput] = 1;         // swallow any stray input
        UE_LOGI("engine: SpawnActor inertPawn -> no player possess, bBlockInput=1");
    }

    // 2) FinishSpawningActor(actor, transform) -> runs the actor's construction
    //    + BeginPlay. Returns the (same) actor.
    ParamFrame finish(g_finishSpawnFn);
    finish.Set<void*>(L"Actor", actor);
    finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    if (!Call(g_gsCdo, finish)) {
        UE_LOGE("engine: FinishSpawningActor call failed");
        return nullptr;
    }
    void* finished = finish.Get<void*>(L"ReturnValue");
    UE_LOGI("engine: SpawnActor -> %p (finished %p) at (%.0f,%.0f,%.0f)",
            actor, finished, location.X, location.Y, location.Z);
    return finished ? finished : actor;
}

FVector GetActorLocation(void* actor) {
    FVector loc;
    if (!actor || !ResolveActorFns()) return loc;
    ParamFrame f(g_getLocFn);
    if (!Call(actor, f)) return loc;
    f.GetRaw(L"ReturnValue", &loc, sizeof(loc));
    return loc;
}

namespace {
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

bool SetComponentVisible(void* component, bool visible) {
    if (!component || !ResolveCompVis()) {
        UE_LOGE("engine: SetComponentVisible unresolved (cls=%p setVis=%p setHidden=%p)",
                g_sceneCompClass, g_setVisFn, g_setHiddenFn);
        return false;
    }
    {
        ParamFrame f(g_setVisFn);
        f.Set<bool>(L"bNewVisibility", visible);
        f.Set<bool>(L"bPropagateToChildren", true);
        Call(component, f);
    }
    {
        ParamFrame f(g_setHiddenFn);
        f.Set<bool>(L"NewHidden", !visible);  // UE4.27 param is "NewHidden" (no b prefix)
        f.Set<bool>(L"bPropagateToChildren", true);
        Call(component, f);
    }
    return true;
}

namespace {
void* g_actorCompClass = nullptr;
void* g_destroyCompFn = nullptr;
bool ResolveDestroy() {
    if (!g_actorCompClass) g_actorCompClass = R::FindClass(P::name::ActorComponentClass);
    if (g_actorCompClass && !g_destroyCompFn)
        g_destroyCompFn = R::FindFunction(g_actorCompClass, P::name::DestroyComponentFn);
    return g_destroyCompFn != nullptr;
}
}  // namespace

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

namespace {
void* g_traClass = nullptr;
void* g_trcClass = nullptr;
void* g_setTextFn = nullptr;
void* g_setWorldSizeFn = nullptr;
void* g_setTextColorFn = nullptr;
void* g_setHAlignFn = nullptr;
void* g_setTextMaterialFn = nullptr;
void* g_unlitTextMat = nullptr;
bool ResolveTextActorFns() {
    if (!g_traClass) g_traClass = R::FindClass(P::name::TextRenderActorClass);
    if (!g_trcClass) g_trcClass = R::FindClass(P::name::TextRenderComponentClass);
    if (g_trcClass) {
        if (!g_setTextFn) g_setTextFn = R::FindFunction(g_trcClass, P::name::SetTextFn);
        if (!g_setWorldSizeFn) g_setWorldSizeFn = R::FindFunction(g_trcClass, P::name::SetWorldSizeFn);
        if (!g_setTextColorFn) g_setTextColorFn = R::FindFunction(g_trcClass, P::name::SetTextRenderColorFn);
        if (!g_setHAlignFn) g_setHAlignFn = R::FindFunction(g_trcClass, P::name::SetHorizontalAlignmentFn);
        if (!g_setTextMaterialFn) g_setTextMaterialFn = R::FindFunction(g_trcClass, P::name::SetTextMaterialFn);
    }
    if (!g_unlitTextMat) g_unlitTextMat = R::FindObject(P::name::UnlitTextMaterialName, P::name::MaterialClassName);
    return g_traClass && g_setTextFn;
}
}  // namespace

void* SpawnTextActor(const FVector& location, const wchar_t* text, float worldSize,
                     const FColor& color) {
    if (!ResolveTextActorFns()) {
        UE_LOGE("engine: SpawnTextActor unresolved (tra=%p trc=%p setText=%p)",
                g_traClass, g_trcClass, g_setTextFn);
        return nullptr;
    }
    void* actor = SpawnActor(g_traClass, location);
    if (!actor) return nullptr;
    void* trc = nullptr;
    for (const auto& c : R::ChildObjectsOf(actor)) {
        if (c.className == P::name::TextRenderComponentClass) { trc = c.object; break; }
    }
    if (!trc) { UE_LOGW("engine: SpawnTextActor -- no TextRenderComponent"); return actor; }

    // SetText takes a plain FString (the FString overload, NOT K2_SetText/FText).
    std::wstring sbuf(text);
    R::FString fs{sbuf.data(), static_cast<int32_t>(sbuf.size()) + 1,
                  static_cast<int32_t>(sbuf.size()) + 1};
    { ParamFrame f(g_setTextFn); f.SetRaw(L"Value", &fs, sizeof(fs)); Call(trc, f); }

    // Translucent text material so the color alpha actually shows (the default is
    // opaque). Without this the nameplate is fully solid regardless of alpha.
    if (g_setTextMaterialFn && g_unlitTextMat) {
        ParamFrame f(g_setTextMaterialFn); f.Set<void*>(L"Material", g_unlitTextMat); Call(trc, f);
    } else {
        UE_LOGW("engine: SpawnTextActor -- UnlitText material/SetTextMaterial unresolved (mat=%p fn=%p); text will be opaque",
                g_unlitTextMat, g_setTextMaterialFn);
    }
    if (g_setWorldSizeFn) { ParamFrame f(g_setWorldSizeFn); f.Set<float>(L"Value", worldSize); Call(trc, f); }
    if (g_setTextColorFn) { ParamFrame f(g_setTextColorFn); f.SetRaw(L"Value", &color, sizeof(color)); Call(trc, f); }
    if (g_setHAlignFn) { ParamFrame f(g_setHAlignFn); f.Set<uint8_t>(L"Value", 1); Call(trc, f); }  // EHTA_Center
    SetComponentVisible(trc, true);
    UE_LOGI("engine: SpawnTextActor '%ls' actor=%p at (%.0f,%.0f,%.0f)",
            text, actor, location.X, location.Y, location.Z);
    return actor;
}

namespace {
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

FVector GetActorForwardVector(void* actor) {
    FVector fwd;
    if (!actor || !ResolveActorFns() || !g_getFwdFn) return fwd;
    ParamFrame f(g_getFwdFn);
    if (!Call(actor, f)) return fwd;
    f.GetRaw(L"ReturnValue", &fwd, sizeof(fwd));
    return fwd;
}

bool SetActorLocation(void* actor, const FVector& location) {
    if (!actor || !ResolveActorFns()) return false;
    ParamFrame f(g_setLocFn);
    f.SetRaw(L"NewLocation", &location, sizeof(location));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);  // snap to the absolute pose (no sweep)
    if (!Call(actor, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool SetActorRotation(void* actor, const FRotator& rotation) {
    if (!actor || !ResolveActorFns() || !g_setRotFn) {
        UE_LOGE("engine: SetActorRotation unresolved (fn=%p)", g_setRotFn);
        return false;
    }
    ParamFrame f(g_setRotFn);
    f.SetRaw(L"NewRotation", &rotation, sizeof(rotation));
    f.Set<bool>(L"bTeleportPhysics", true);
    if (!Call(actor, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool SetActorTickEnabled(void* actor, bool enabled) {
    if (!actor || !ResolveActorFns() || !g_setTickFn) {
        UE_LOGE("engine: SetActorTickEnabled unresolved (fn=%p)", g_setTickFn);
        return false;
    }
    ParamFrame f(g_setTickFn);
    f.Set<bool>(L"bEnabled", enabled);
    return Call(actor, f);
}

namespace {
void* g_pawnClass = nullptr;
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
}  // namespace

void* GetController(void* pawn) {
    if (!pawn || !ResolvePawnFns()) return nullptr;
    ParamFrame f(g_getControllerFn);
    if (!Call(pawn, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

namespace {
void* g_controllerClass = nullptr;
void* g_getControlRotFn = nullptr;
void* g_pcClass = nullptr;
void* g_setViewTargetFn = nullptr;
void* g_camMgrClass = nullptr;
void* g_getCamLocFn = nullptr;
void* g_getCamRotFn = nullptr;
}  // namespace

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

namespace {
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
