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

namespace {
// Cached across the harness's boot retry loop (LoadStorySave is polled until we're
// in gameplay). The CDO + UFunctions never move; the slot is loaded from disk ONCE.
void* g_storyGsCdo = nullptr;
void* g_loadGameFn = nullptr;
void* g_setSaveSlotFn = nullptr;
void* g_storySave = nullptr;  // cached USaveGame* (LoadGameFromSlot once)
}  // namespace

// Called repeatedly by the harness boot loop. Returns true ONLY once a mainPlayer_C
// is in the real level (non-origin) -- i.e. we've reached story gameplay. While
// still at preLoad / the OMEGA WARNING / the menu it (re)issues `open untitled_1`
// each call; the user confirmed `open` travels straight to gameplay from the OMEGA
// screen (Proceed only loads preLoad, which we DON'T want). A single early open
// fired during preLoad is silently dropped, hence the retry. It will NOT re-open
// once the gameplay world is already loading (that would restart the load).
bool LoadStorySave(const wchar_t* slot) {
    if (!slot || !*slot) return false;

    // (a) Already in gameplay? mainPlayer_C placed in the real level (non-origin).
    if (void* lp = R::FindObjectByClass(P::name::MainPlayerClass)) {
        const FVector p = GetActorLocation(lp);
        if (std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) > 100.f) {
            UE_LOGI("engine: LoadStorySave -- in gameplay (mainPlayer @ %.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
            return true;
        }
    }
    // (b) Gameplay map already loading? The gameplay world is "Untitled" (map
    // untitled_1); preLoad/menu are other worlds. If we're in/loading it, DON'T
    // re-open -- just wait for the player to spawn.
    if (void* w = R::FindObjectByClass(P::name::WorldClass)) {
        if (R::ToString(R::NameOf(w)).find(L"ntitled") != std::wstring::npos) return false;
    }

    // (c) Still at preLoad / OMEGA / menu: register the save (once) + (re)issue open.
    auto makeFStr = [](std::wstring& b) {
        R::FString fs{};
        fs.Data = b.data();
        fs.Num = static_cast<int32_t>(b.size()) + 1;  // FString::Num counts the null
        fs.Max = fs.Num;
        return fs;
    };
    if (!g_storyGsCdo) g_storyGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_storyGsCdo && !g_loadGameFn) {
        if (void* cls = R::ClassOf(g_storyGsCdo)) g_loadGameFn = R::FindFunction(cls, P::name::LoadGameFromSlotFn);
    }
    void* gi = R::FindObjectByClass(P::name::GameInstanceClass);
    if (!g_storyGsCdo || !g_loadGameFn || !gi) {
        UE_LOGW("engine: LoadStorySave -- not up yet (cdo=%p fn=%p gi=%p); retry", g_storyGsCdo, g_loadGameFn, gi);
        return false;
    }
    if (!g_setSaveSlotFn) {
        if (void* gicls = R::ClassOf(gi)) g_setSaveSlotFn = R::FindFunction(gicls, P::name::SetSaveSlotObjectFn);
    }

    // Load the slot from disk ONCE (cached).
    if (!g_storySave) {
        std::wstring b(slot);
        R::FString fs = makeFStr(b);
        ParamFrame f(g_loadGameFn);
        f.SetRaw(L"SlotName", &fs, sizeof(fs));
        f.Set<int32_t>(L"UserIndex", 0);
        if (!Call(g_storyGsCdo, f)) { UE_LOGE("engine: LoadStorySave -- LoadGameFromSlot call failed"); return false; }
        g_storySave = f.Get<void*>(L"ReturnValue");
        if (!g_storySave) { UE_LOGW("engine: LoadStorySave -- slot '%ls' missing/empty", slot); return false; }
        UE_LOGI("engine: LoadStorySave -- loaded save '%ls' = %p", slot, g_storySave);
    }

    // Register on the (persistent) GameInstance + flag the GameMode to APPLY it on
    // BeginPlay. Re-asserted each retry (cheap, no disk) so it's fresh at the travel.
    if (g_setSaveSlotFn) {
        std::wstring b(slot);
        R::FString fs = makeFStr(b);
        ParamFrame f(g_setSaveSlotFn);
        f.Set<void*>(L"save_gameInst", g_storySave);
        f.SetRaw(L"SlotName", &fs, sizeof(fs));
        Call(gi, f);
    } else {
        UE_LOGW("engine: LoadStorySave -- setSaveSlotObject unresolved");
    }
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(gi) + P::off::mainGameInstance_loadObjects) = 1;

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: LoadStorySave -- at preLoad/menu; (re)issuing '%ls' (save '%ls' registered)",
            openCmd.c_str(), slot);
    ExecuteConsoleCommand(openCmd.c_str());
    return false;  // not in gameplay yet -> caller keeps retrying
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
void* g_translucentTextMat = nullptr;
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
    if (!g_translucentTextMat) {
        g_translucentTextMat = R::FindObject(P::name::UnlitTextMaterialName, P::name::MaterialClassName);
        if (g_translucentTextMat) {
            const uint8_t blend = *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint8_t*>(g_translucentTextMat) + P::off::UMaterial_BlendMode);
            UE_LOGI("engine: text material '%ls' = %p, BlendMode=%u (0=Opaque,1=Masked,2=Translucent)",
                    P::name::UnlitTextMaterialName, g_translucentTextMat, blend);
        }
    }
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

    // Bind the stock translucent text material so the FColor alpha actually shows
    // (the default is opaque and discards alpha). Without this the nameplate is
    // fully solid regardless of alpha.
    if (g_setTextMaterialFn && g_translucentTextMat) {
        ParamFrame f(g_setTextMaterialFn); f.Set<void*>(L"Material", g_translucentTextMat); Call(trc, f);
        UE_LOGI("engine: SpawnTextActor bound translucent text material %p", g_translucentTextMat);
    } else {
        UE_LOGW("engine: SpawnTextActor -- %ls not resident / SetTextMaterial unresolved (mat=%p fn=%p)",
                P::name::UnlitTextMaterialName, g_translucentTextMat, g_setTextMaterialFn);
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
// Identity FTransform (0x30): FQuat{0,0,0,1} @0x00, FVector Translation{0} @0x10,
// FVector Scale3D{1,1,1} @0x20.
void MakeIdentityTransform(uint8_t (&xform)[0x30]) {
    std::memset(xform, 0, sizeof(xform));
    float* f = reinterpret_cast<float*>(xform);
    f[3] = 1.f;                       // Quat.W
    f[8] = 1.f; f[9] = 1.f; f[10] = 1.f;  // Scale3D
}

void* g_npActorClass = nullptr, *g_npCompClass = nullptr, *g_npWidgetClass = nullptr;
void* g_npAddFn = nullptr, *g_npFinishFn = nullptr, *g_npDrawFn = nullptr;
void* g_npTintFn = nullptr, *g_npGetWidgetFn = nullptr, *g_npSetTextFn = nullptr;
void* g_npKtlCdo = nullptr, *g_npConvFn = nullptr;
void* g_npWidgetBaseClass = nullptr, *g_npSetVisFn = nullptr, *g_npRedrawFn = nullptr;
void* g_npRenderUpdateFn = nullptr, *g_npTickFn = nullptr;
void* g_npTransMat = nullptr, *g_npTransMatOneSided = nullptr;
void* g_npTbClass = nullptr, *g_npTbSetTextFn = nullptr, *g_npTbSetColorFn = nullptr;
bool ResolveNameplateFns() {
    if (!g_npActorClass) g_npActorClass = R::FindClass(P::name::ActorClassName);
    if (!g_npCompClass) g_npCompClass = R::FindClass(P::name::WidgetComponentClass);
    if (!g_npWidgetClass) g_npWidgetClass = R::FindClass(P::name::NameplateWidgetClass);
    if (g_npActorClass && !g_npAddFn) g_npAddFn = R::FindFunction(g_npActorClass, P::name::AddComponentByClassFn);
    if (g_npActorClass && !g_npFinishFn) g_npFinishFn = R::FindFunction(g_npActorClass, P::name::FinishAddComponentFn);
    if (g_npCompClass) {
        if (!g_npDrawFn) g_npDrawFn = R::FindFunction(g_npCompClass, P::name::SetWidgetDrawSizeFn);
        if (!g_npTintFn) g_npTintFn = R::FindFunction(g_npCompClass, P::name::SetTintColorAndOpacityFn);
        if (!g_npGetWidgetFn) g_npGetWidgetFn = R::FindFunction(g_npCompClass, P::name::GetUserWidgetObjectFn);
        if (!g_npRedrawFn) g_npRedrawFn = R::FindFunction(g_npCompClass, P::name::RequestRedrawFn);
        if (!g_npRenderUpdateFn) g_npRenderUpdateFn = R::FindFunction(g_npCompClass, P::name::RequestRenderUpdateFn);
    }
    if (void* acc = R::FindClass(P::name::ActorComponentClass)) {
        if (!g_npTickFn) g_npTickFn = R::FindFunction(acc, P::name::SetComponentTickEnabledFn);
    }
    if (!g_npTransMat)
        g_npTransMat = R::FindObject(P::name::Widget3DTranslucentMatName, P::name::MaterialInstanceConstantClass);
    if (!g_npTransMatOneSided)
        g_npTransMatOneSided = R::FindObject(P::name::Widget3DTranslucentOneSidedMatName, P::name::MaterialInstanceConstantClass);
    if (g_npWidgetClass && !g_npSetTextFn) g_npSetTextFn = R::FindFunction(g_npWidgetClass, P::name::NameplateSetTextFn);
    if (!g_npTbClass) g_npTbClass = R::FindClass(P::name::TextBlockClass);
    if (g_npTbClass) {
        if (!g_npTbSetTextFn) g_npTbSetTextFn = R::FindFunction(g_npTbClass, P::name::NameplateSetTextFn);  // UTextBlock::SetText(FText)
        if (!g_npTbSetColorFn) g_npTbSetColorFn = R::FindFunction(g_npTbClass, P::name::TextBlockSetColorFn);
    }
    if (!g_npWidgetBaseClass) g_npWidgetBaseClass = R::FindClass(P::name::WidgetBaseClass);
    if (g_npWidgetBaseClass && !g_npSetVisFn) g_npSetVisFn = R::FindFunction(g_npWidgetBaseClass, P::name::SetVisibilityFn);
    if (!g_npKtlCdo) g_npKtlCdo = R::FindClassDefaultObject(P::name::KismetTextLibraryClass);
    if (g_npKtlCdo && !g_npConvFn) {
        if (void* c = R::ClassOf(g_npKtlCdo)) g_npConvFn = R::FindFunction(c, P::name::ConvStringToTextFn);
    }
    return g_npActorClass && g_npCompClass && g_npWidgetClass && g_npAddFn && g_npFinishFn;
}
}  // namespace

void* SpawnNameplateWidget(const FVector& location, const wchar_t* text, float opacity) {
    if (!ResolveNameplateFns()) {
        UE_LOGE("engine: SpawnNameplateWidget unresolved (actor=%p comp=%p wcls=%p add=%p fin=%p)",
                g_npActorClass, g_npCompClass, g_npWidgetClass, g_npAddFn, g_npFinishFn);
        return nullptr;
    }
    void* actor = SpawnActor(g_npActorClass, location);
    if (!actor) { UE_LOGE("engine: SpawnNameplateWidget -- SpawnActor failed"); return nullptr; }

    uint8_t xform[0x30];
    MakeIdentityTransform(xform);

    // 1) AddComponentByClass(WidgetComponent, bManualAttachment=false, identity, deferred=true)
    void* comp = nullptr;
    {
        ParamFrame f(g_npAddFn);
        f.Set<void*>(L"Class", g_npCompClass);
        f.Set<bool>(L"bManualAttachment", false);
        f.SetRaw(L"relativeTransform", xform, sizeof(xform));
        f.Set<bool>(L"bDeferredFinish", true);
        if (!Call(actor, f)) { UE_LOGE("engine: SpawnNameplateWidget -- AddComponentByClass call failed"); return actor; }
        comp = f.Get<void*>(L"ReturnValue");
    }
    if (!comp) { UE_LOGE("engine: SpawnNameplateWidget -- no WidgetComponent returned"); return actor; }

    // 2) Set WidgetClass + BlendMode + two-sided BEFORE finishing, so that on
    // register InitWidget/UpdateMaterialInstance build the MID over the correct
    // material. ROOT CAUSE (IDA-confirmed): GetMaterial(0) routes by BlendMode, and
    // the ctor DEFAULTS BlendMode=Masked(1) -> the MID was built over
    // Widget3DPassThrough_Masked, which alpha-clips our low-alpha/transparent content
    // -> the whole quad is discarded (invisible). Setting BlendMode=Transparent(2)
    // routes to TranslucentMaterial (0x4F0) -> real partial alpha.
    auto preU8 = reinterpret_cast<uint8_t*>(comp);
    *reinterpret_cast<void**>(preU8 + P::off::UWidgetComponent_WidgetClass) = g_npWidgetClass;
    *reinterpret_cast<uint8_t*>(preU8 + P::off::UWidgetComponent_BlendMode) = 2;   // Transparent
    *reinterpret_cast<uint8_t*>(preU8 + P::off::UWidgetComponent_bIsTwoSided) = 1;  // two-sided -> 0x4F0 slot

    // 3) FinishAddComponent -> register -> InitWidget creates the uicomp_helpText_C instance.
    {
        ParamFrame f(g_npFinishFn);
        f.Set<void*>(L"Component", comp);
        f.Set<bool>(L"bManualAttachment", false);
        f.SetRaw(L"relativeTransform", xform, sizeof(xform));
        Call(actor, f);
    }

    // 4) Draw size + translucency tint (Space=World and two-sided are set pre-register).
    if (g_npDrawFn) { ParamFrame f(g_npDrawFn); FVector2D sz{200.f, 52.f}; f.SetRaw(L"Size", &sz, sizeof(sz)); Call(comp, f); }
    if (g_npTintFn) { ParamFrame f(g_npTintFn); FLinearColor c{1.f, 1.f, 1.f, 1.f}; f.SetRaw(L"NewTintColorAndOpacity", &c, sizeof(c)); Call(comp, f); }  // full tint; translucency is on the text alpha
    // No background pill -- the TEXT itself is translucent (set on text_help below).
    { FLinearColor bg{0.f, 0.f, 0.f, 0.f};
      *reinterpret_cast<FLinearColor*>(reinterpret_cast<uint8_t*>(comp) + P::off::UWidgetComponent_BackgroundColor) = bg; }

    // ROOT-CAUSE FIX (two agents converged): a runtime AddComponentByClass'd
    // WidgetComponent never ticks -> DrawWidgetToRenderTarget never runs -> its
    // RenderTarget + MaterialInstance stay null -> GetMaterial()==null -> the quad
    // is invisible (the Slate widget itself is fine). Enable tick + force a render
    // update; ensure auto-redraw (bManuallyRedraw=0, RedrawTime=0); and populate the
    // translucent material slot in case NewObject didn't copy the CDO's default.
    auto compU8 = reinterpret_cast<uint8_t*>(comp);
    *reinterpret_cast<uint8_t*>(compU8 + P::off::UWidgetComponent_bManuallyRedraw) = 0;
    *reinterpret_cast<float*>(compU8 + P::off::UWidgetComponent_RedrawTime) = 0.f;
    if (g_npTransMat)
        *reinterpret_cast<void**>(compU8 + P::off::UWidgetComponent_TranslucentMaterial) = g_npTransMat;
    if (g_npTransMatOneSided)
        *reinterpret_cast<void**>(compU8 + P::off::UWidgetComponent_TranslucentMaterialOneSided) = g_npTransMatOneSided;
    if (g_npTickFn) { ParamFrame f(g_npTickFn); f.Set<bool>(L"bEnabled", true); Call(comp, f); }
    if (g_npRenderUpdateFn) { ParamFrame f(g_npRenderUpdateFn); Call(comp, f); }

    // 5) Text. widget = comp.GetUserWidgetObject(); then drive the INNER text_help
    // UTextBlock directly (uicomp_helpText_C::SetText doesn't paint it for our use):
    // SetText + opaque-white SetColorAndOpacity + SetVisibility(Visible). Without
    // real text painted, the translucent RT is fully transparent -> invisible quad.
    void* widget = nullptr;
    if (g_npGetWidgetFn) { ParamFrame f(g_npGetWidgetFn); if (Call(comp, f)) widget = f.Get<void*>(L"ReturnValue"); }
    void* textBlock = widget ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(widget) + 0x268) : nullptr;

    if (g_npConvFn) {
        uint8_t ftext[0x18] = {};
        std::wstring b(text);
        R::FString fs{b.data(), static_cast<int32_t>(b.size()) + 1, static_cast<int32_t>(b.size()) + 1};
        { ParamFrame cf(g_npConvFn); cf.SetRaw(L"inString", &fs, sizeof(fs)); Call(g_npKtlCdo, cf);  // param is 'inString' (lowercase i)
          cf.GetRaw(L"ReturnValue", ftext, sizeof(ftext)); }
        if (widget && g_npSetTextFn) { ParamFrame f(g_npSetTextFn); f.SetRaw(L"InText", ftext, sizeof(ftext)); Call(widget, f); }
        if (textBlock && g_npTbSetTextFn) { ParamFrame f(g_npTbSetTextFn); f.SetRaw(L"InText", ftext, sizeof(ftext)); Call(textBlock, f); }
    }
    // FSlateColor (0x28): SpecifiedColor{1,1,1,1} @0x00, ColorUseRule=UseColor_Specified(0) @0x10.
    if (textBlock && g_npTbSetColorFn) {
        uint8_t slate[0x28] = {};
        FLinearColor c{1.f, 1.f, 1.f, opacity};  // TRANSLUCENT white text (alpha = opacity, ~0.6)
        *reinterpret_cast<FLinearColor*>(slate) = c;  // slate[0x10]=ColorUseRule=0 already
        ParamFrame f(g_npTbSetColorFn); f.SetRaw(L"InColorAndOpacity", slate, sizeof(slate)); Call(textBlock, f);
    }
    // Force visible (reused help-text widgets default Collapsed). ESlateVisibility::Visible=0.
    if (widget && g_npSetVisFn) { ParamFrame f(g_npSetVisFn); f.Set<uint8_t>(L"InVisibility", 0); Call(widget, f); }
    if (textBlock && g_npSetVisFn) { ParamFrame f(g_npSetVisFn); f.Set<uint8_t>(L"InVisibility", 0); Call(textBlock, f); }
    // A runtime-added WidgetComponent may not auto-draw its render target -> force it.
    if (g_npRedrawFn) { ParamFrame f(g_npRedrawFn); Call(comp, f); }

    UE_LOGI("engine: SpawnNameplateWidget '%ls' actor=%p comp=%p widget=%p opacity=%.2f at (%.0f,%.0f,%.0f)",
            text, actor, comp, widget, opacity, location.X, location.Y, location.Z);
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
