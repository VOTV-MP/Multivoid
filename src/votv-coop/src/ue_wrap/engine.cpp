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
void* g_getRotFn = nullptr;
void* g_getVelFn = nullptr;
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
        if (!g_getRotFn) g_getRotFn = R::FindFunction(g_actorClass, P::name::GetActorRotationFn);
        if (!g_getVelFn) g_getVelFn = R::FindFunction(g_actorClass, P::name::GetActorVelocityFn);
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
// Identity FTransform (0x30): FQuat{0,0,0,1} @0x00, FVector Translation{0} @0x10,
// FVector Scale3D{1,1,1} @0x20.
void MakeIdentityTransform(uint8_t (&xform)[0x30]) {
    std::memset(xform, 0, sizeof(xform));
    float* f = reinterpret_cast<float*>(xform);
    f[3] = 1.f;                       // Quat.W
    f[8] = 1.f; f[9] = 1.f; f[10] = 1.f;  // Scale3D
}

void* g_npActorClass = nullptr, *g_npCompClass = nullptr;
void* g_npAddFn = nullptr, *g_npFinishFn = nullptr, *g_npTintFn = nullptr;
void* g_npRedrawFn = nullptr, *g_npRenderUpdateFn = nullptr, *g_npTickFn = nullptr, *g_npSetWidgetFn = nullptr;
void* g_npTransMat = nullptr, *g_npTransMatOneSided = nullptr;
void* g_npKtlCdo = nullptr, *g_npConvFn = nullptr;
// Own-widget construction (NewObject = UGameplayStatics::SpawnObject).
void* g_npGsCdo = nullptr, *g_npSpawnObjFn = nullptr;
void* g_npUserWidgetClass = nullptr, *g_npWidgetTreeClass = nullptr, *g_npTbClass = nullptr;
void* g_npTbSetTextFn = nullptr, *g_npFont = nullptr;
bool ResolveNameplateFns() {
    if (!g_npActorClass) g_npActorClass = R::FindClass(P::name::ActorClassName);
    if (!g_npCompClass) g_npCompClass = R::FindClass(P::name::WidgetComponentClass);
    if (g_npActorClass && !g_npAddFn) g_npAddFn = R::FindFunction(g_npActorClass, P::name::AddComponentByClassFn);
    if (g_npActorClass && !g_npFinishFn) g_npFinishFn = R::FindFunction(g_npActorClass, P::name::FinishAddComponentFn);
    if (g_npCompClass) {
        if (!g_npTintFn) g_npTintFn = R::FindFunction(g_npCompClass, P::name::SetTintColorAndOpacityFn);
        if (!g_npSetWidgetFn) g_npSetWidgetFn = R::FindFunction(g_npCompClass, P::name::SetWidgetFn);
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
    // NewObject path + UMG classes + font.
    if (!g_npGsCdo) g_npGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_npGsCdo && !g_npSpawnObjFn) {
        if (void* c = R::ClassOf(g_npGsCdo)) g_npSpawnObjFn = R::FindFunction(c, P::name::SpawnObjectFn);
    }
    if (!g_npUserWidgetClass) g_npUserWidgetClass = R::FindClass(P::name::UserWidgetClass);
    if (!g_npWidgetTreeClass) g_npWidgetTreeClass = R::FindClass(P::name::WidgetTreeClass);
    if (!g_npTbClass) g_npTbClass = R::FindClass(P::name::TextBlockClass);
    if (g_npTbClass && !g_npTbSetTextFn) g_npTbSetTextFn = R::FindFunction(g_npTbClass, P::name::NameplateSetTextFn);  // UTextBlock::SetText(FText)
    if (!g_npFont) g_npFont = R::FindObject(P::name::FontName, P::name::FontClassName);
    if (!g_npKtlCdo) g_npKtlCdo = R::FindClassDefaultObject(P::name::KismetTextLibraryClass);
    if (g_npKtlCdo && !g_npConvFn) {
        if (void* c = R::ClassOf(g_npKtlCdo)) g_npConvFn = R::FindFunction(c, P::name::ConvStringToTextFn);
    }
    return g_npActorClass && g_npCompClass && g_npAddFn && g_npFinishFn &&
           g_npGsCdo && g_npSpawnObjFn && g_npUserWidgetClass && g_npWidgetTreeClass && g_npTbClass;
}

// NewObject by class via the reflected UGameplayStatics::SpawnObject(objectClass, Outer).
void* SpawnObject(void* objectClass, void* outer) {
    if (!g_npGsCdo || !g_npSpawnObjFn || !objectClass || !outer) return nullptr;
    ParamFrame f(g_npSpawnObjFn);
    f.Set<void*>(L"objectClass", objectClass);
    f.Set<void*>(L"Outer", outer);
    if (!Call(g_npGsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// Set a UTextBlock's text via Conv_StringToText -> UTextBlock::SetText. Requires
// ResolveNameplateFns() to have run (resolves g_npConvFn/g_npTbSetTextFn/g_npKtlCdo).
void SetTextOnBlock(void* txt, const wchar_t* text) {
    if (!txt || !g_npConvFn || !g_npTbSetTextFn || !g_npKtlCdo) return;
    uint8_t ftext[0x18] = {};
    std::wstring b(text);
    R::FString fs{b.data(), static_cast<int32_t>(b.size()) + 1, static_cast<int32_t>(b.size()) + 1};
    { ParamFrame cf(g_npConvFn); cf.SetRaw(L"inString", &fs, sizeof(fs)); Call(g_npKtlCdo, cf);  // 'inString' (lowercase i)
      cf.GetRaw(L"ReturnValue", ftext, sizeof(ftext)); }
    ParamFrame tf(g_npTbSetTextFn); tf.SetRaw(L"InText", ftext, sizeof(ftext)); Call(txt, tf);
}

// Build UUserWidget(outer) -> UWidgetTree -> UTextBlock(root) with the given font
// size / colour / justification and initial text. Shared by the world-space
// nameplate and the screen-space HUD feed (RULE 2: one UMG-text-build, not two).
// Returns {root, txt}; {nullptr,nullptr} on failure.
struct BuiltText { void* root; void* txt; };
BuiltText BuildTextWidget(void* outer, const wchar_t* text, const FLinearColor& color,
                          int32_t fontSize, uint8_t justification) {
    void* root = SpawnObject(g_npUserWidgetClass, outer);
    void* tree = root ? SpawnObject(g_npWidgetTreeClass, root) : nullptr;
    void* txt  = tree ? SpawnObject(g_npTbClass, tree) : nullptr;
    if (!root || !tree || !txt) {
        UE_LOGE("engine: BuildTextWidget SpawnObject failed (root=%p tree=%p txt=%p)", root, tree, txt);
        return {nullptr, nullptr};
    }
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + P::off::UUserWidget_WidgetTree) = tree;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tree) + P::off::UWidgetTree_RootWidget) = txt;
    auto tU8 = reinterpret_cast<uint8_t*>(txt);
    if (g_npFont) *reinterpret_cast<void**>(tU8 + P::off::UTextBlock_Font) = g_npFont;
    *reinterpret_cast<int32_t*>(tU8 + P::off::UTextBlock_Font + P::off::FSlateFontInfo_Size) = fontSize;
    *reinterpret_cast<FLinearColor*>(tU8 + P::off::UTextBlock_ColorAndOpacity) = color;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextBlock_ColorAndOpacity + P::off::FSlateColor_ColorUseRule) = 0;
    *reinterpret_cast<uint8_t*>(tU8 + P::off::UTextLayoutWidget_Justification) = justification;
    SetTextOnBlock(txt, text);
    return {root, txt};
}

// Screen-space (viewport) widget functions, resolved on the UserWidget class.
void* g_addToVpFn = nullptr, *g_removeFromVpFn = nullptr, *g_widgetSetVisFn = nullptr;
void* g_setPosVpFn = nullptr, *g_setAlignVpFn = nullptr;
bool ResolveScreenWidgetFns() {
    if (!ResolveNameplateFns()) return false;  // UMG classes + SpawnObject + text fns
    if (g_npUserWidgetClass) {
        if (!g_addToVpFn) g_addToVpFn = R::FindFunction(g_npUserWidgetClass, P::name::AddToViewportFn);
        if (!g_removeFromVpFn) g_removeFromVpFn = R::FindFunction(g_npUserWidgetClass, P::name::RemoveFromViewportFn);
        if (!g_setPosVpFn) g_setPosVpFn = R::FindFunction(g_npUserWidgetClass, P::name::SetPositionInViewportFn);
        if (!g_setAlignVpFn) g_setAlignVpFn = R::FindFunction(g_npUserWidgetClass, P::name::SetAlignmentInViewportFn);
    }
    // SetVisibility is owned by UWidget (a parent), and FindFunction matches the
    // OWNING class only -- resolve it on "Widget", not "UserWidget".
    if (!g_widgetSetVisFn) {
        if (void* wc = R::FindClass(P::name::WidgetClass))
            g_widgetSetVisFn = R::FindFunction(wc, P::name::WidgetSetVisibilityFn);
    }
    return g_addToVpFn && g_widgetSetVisFn;
}
}  // namespace

void* SpawnNameplateWidget(const FVector& location, const wchar_t* text, float opacity) {
    if (!ResolveNameplateFns()) {
        UE_LOGE("engine: SpawnNameplateWidget unresolved (actor=%p comp=%p add=%p fin=%p spawnObj=%p uw=%p tb=%p)",
                g_npActorClass, g_npCompClass, g_npAddFn, g_npFinishFn, g_npSpawnObjFn, g_npUserWidgetClass, g_npTbClass);
        return nullptr;
    }
    void* actor = SpawnActor(g_npActorClass, location);
    if (!actor) { UE_LOGE("engine: SpawnNameplateWidget -- SpawnActor failed"); return nullptr; }

    uint8_t xform[0x30];
    MakeIdentityTransform(xform);

    // 1) AddComponentByClass(WidgetComponent, deferred) -- own widget, NOT a cooked one.
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
    auto cU8 = reinterpret_cast<uint8_t*>(comp);

    // 2) BEFORE register: BlendMode=Transparent(2) + two-sided + draw-at-desired-size.
    // (IDA: GetMaterial(0) routes by BlendMode; the ctor defaults Masked(1), which
    // alpha-clips the content -> invisible. Transparent routes to TranslucentMaterial.)
    // Leave WidgetClass NULL -- we build + SetWidget our OWN UMG (no cooked widget).
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_BlendMode) = 2;
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bIsTwoSided) = 1;
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bDrawAtDesiredSize) = 1;

    // 3) FinishAddComponent -> register.
    {
        ParamFrame f(g_npFinishFn);
        f.Set<void*>(L"Component", comp);
        f.Set<bool>(L"bManualAttachment", false);
        f.SetRaw(L"relativeTransform", xform, sizeof(xform));
        Call(actor, f);
    }
    // Component draw config: auto-redraw, the translucent material slot, full tint.
    *reinterpret_cast<uint8_t*>(cU8 + P::off::UWidgetComponent_bManuallyRedraw) = 0;
    *reinterpret_cast<float*>(cU8 + P::off::UWidgetComponent_RedrawTime) = 0.f;
    if (g_npTransMat) *reinterpret_cast<void**>(cU8 + P::off::UWidgetComponent_TranslucentMaterial) = g_npTransMat;
    if (g_npTransMatOneSided) *reinterpret_cast<void**>(cU8 + P::off::UWidgetComponent_TranslucentMaterialOneSided) = g_npTransMatOneSided;
    if (g_npTintFn) { ParamFrame f(g_npTintFn); FLinearColor c{1.f, 1.f, 1.f, 1.f}; f.SetRaw(L"NewTintColorAndOpacity", &c, sizeof(c)); Call(comp, f); }

    // 4) Build OUR widget tree (shared builder): UUserWidget -> UWidgetTree ->
    // UTextBlock(root), translucent-white centred text at font size 14.
    BuiltText bt = BuildTextWidget(actor, text, FLinearColor{1.f, 1.f, 1.f, opacity}, 14, /*Center*/ 1);
    void* root = bt.root;
    void* txt = bt.txt;
    if (!root || !txt) { UE_LOGE("engine: SpawnNameplateWidget -- BuildTextWidget failed"); return actor; }

    // 5) Attach our widget (re-parents + rebuilds the slate/render-target), then make
    // the runtime-added component tick so it actually draws its RT.
    if (g_npSetWidgetFn) { ParamFrame f(g_npSetWidgetFn); f.Set<void*>(L"Widget", root); Call(comp, f); }
    if (g_npTickFn) { ParamFrame f(g_npTickFn); f.Set<bool>(L"bEnabled", true); Call(comp, f); }
    if (g_npRenderUpdateFn) { ParamFrame f(g_npRenderUpdateFn); Call(comp, f); }
    if (g_npRedrawFn) { ParamFrame f(g_npRedrawFn); Call(comp, f); }

    UE_LOGI("engine: SpawnNameplateWidget(own) '%ls' actor=%p comp=%p root=%p txt=%p font=%p at (%.0f,%.0f,%.0f)",
            text, actor, comp, root, txt, g_npFont, location.X, location.Y, location.Z);
    return actor;
}

bool SetWidgetText(void* textBlock, const wchar_t* text) {
    if (!textBlock || !ResolveNameplateFns()) return false;
    SetTextOnBlock(textBlock, text);
    return true;
}

bool AddWidgetToViewport(void* userWidget, int zOrder) {
    if (!userWidget || !ResolveScreenWidgetFns() || !g_addToVpFn) return false;
    ParamFrame f(g_addToVpFn);
    f.Set<int32_t>(L"ZOrder", zOrder);
    return Call(userWidget, f);
}

bool RemoveWidgetFromViewport(void* userWidget) {
    if (!userWidget || !ResolveScreenWidgetFns() || !g_removeFromVpFn) return false;
    ParamFrame f(g_removeFromVpFn);
    return Call(userWidget, f);
}

bool SpawnHudFeedWidget(void* outer, int zOrder, void** outRoot, void** outText) {
    if (outRoot) *outRoot = nullptr;
    if (outText) *outText = nullptr;
    if (!outer || !ResolveScreenWidgetFns()) {
        UE_LOGE("engine: SpawnHudFeedWidget unresolved (uw=%p addVp=%p setVis=%p)",
                g_npUserWidgetClass, g_addToVpFn, g_widgetSetVisFn);
        return false;
    }
    // Opaque-white, RIGHT-justified, multi-line text (the feed rebuilds the whole
    // string on each push); font 16. Outer = a persistent object (GameInstance) so
    // it survives level loads.
    BuiltText bt = BuildTextWidget(outer, L"", FLinearColor{1.f, 1.f, 1.f, 1.f}, 16, /*Right*/ 2);
    if (!bt.root || !bt.txt) return false;
    // Visible but input-transparent (ESlateVisibility::HitTestInvisible = 3) so the
    // overlay never steals mouse/keyboard focus from the game.
    if (g_widgetSetVisFn) { ParamFrame f(g_widgetSetVisFn); f.Set<uint8_t>(L"InVisibility", 3); Call(bt.root, f); }
    if (!AddWidgetToViewport(bt.root, zOrder)) {
        UE_LOGE("engine: SpawnHudFeedWidget -- AddToViewport failed");
        return false;
    }
    // Anchor to the TOP-RIGHT: alignment (1,0) makes the widget's top-right corner
    // the pivot, positioned near the top-right of the viewport (by the right lamp).
    // Pixel coords assume the 1920x1080 test window; tune if the resolution differs.
    if (g_setAlignVpFn) { ParamFrame f(g_setAlignVpFn); FVector2D a{1.f, 0.f}; f.SetRaw(L"Alignment", &a, sizeof(a)); Call(bt.root, f); }
    if (g_setPosVpFn)   { ParamFrame f(g_setPosVpFn); FVector2D p{1900.f, 40.f}; f.SetRaw(L"Position", &p, sizeof(p)); f.Set<bool>(L"bRemoveDPIScale", true); Call(bt.root, f); }
    if (outRoot) *outRoot = bt.root;
    if (outText) *outText = bt.txt;
    UE_LOGI("engine: SpawnHudFeedWidget root=%p txt=%p z=%d", bt.root, bt.txt, zOrder);
    return true;
}

namespace {
void* g_numBonesFn = nullptr, *g_boneNameFn = nullptr, *g_socketLocFn = nullptr;
uint8_t g_headBone[8] = {};       // cached FName of the head bone (ComparisonIndex,Number)
bool g_headBoneResolved = false;
bool ResolveBoneFns() {
    if (void* sk = R::FindClass(P::name::SkinnedMeshComponentClass)) {
        if (!g_numBonesFn) g_numBonesFn = R::FindFunction(sk, P::name::GetNumBonesFn);
        if (!g_boneNameFn) g_boneNameFn = R::FindFunction(sk, P::name::GetBoneNameFn);
    }
    if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
        if (!g_socketLocFn) g_socketLocFn = R::FindFunction(sc, P::name::GetSocketLocationFn);
    }
    return g_numBonesFn && g_boneNameFn && g_socketLocFn;
}
}  // namespace

bool GetHeadWorldLocation(void* skelMeshComp, FVector& out) {
    if (!skelMeshComp || !ResolveBoneFns()) return false;
    if (!g_headBoneResolved) {
        // Enumerate bones ONCE; cache the head bone's FName (the player skin rides the
        // kerfurOmega skeleton -- bone names are stable across instances).
        int32_t n = 0;
        { ParamFrame f(g_numBonesFn); if (Call(skelMeshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
        for (int32_t i = 0; i < n; ++i) {
            ParamFrame f(g_boneNameFn); f.Set<int32_t>(L"BoneIndex", i);
            if (!Call(skelMeshComp, f)) break;
            uint8_t name[8] = {};
            f.GetRaw(L"ReturnValue", name, sizeof(name));
            const std::wstring s = R::ToString(*reinterpret_cast<const R::FName*>(name));
            if (s == L"head" || s == L"Head") {
                std::memcpy(g_headBone, name, sizeof(g_headBone));
                UE_LOGI("engine: nameplate head bone = '%ls' (index %d/%d)", s.c_str(), i, n);
                break;
            }
        }
        g_headBoneResolved = true;  // give up after one pass; zero FName ("None") -> actor loc fallback
    }
    ParamFrame f(g_socketLocFn);
    f.SetRaw(L"InSocketName", g_headBone, sizeof(g_headBone));
    if (!Call(skelMeshComp, f)) return false;
    out = f.Get<FVector>(L"ReturnValue");
    return true;
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

FRotator GetActorRotation(void* actor) {
    FRotator rot;
    if (!actor || !ResolveActorFns() || !g_getRotFn) return rot;
    ParamFrame f(g_getRotFn);
    if (!Call(actor, f)) return rot;
    f.GetRaw(L"ReturnValue", &rot, sizeof(rot));
    return rot;
}

FVector GetActorVelocity(void* actor) {
    FVector vel;
    if (!actor || !ResolveActorFns() || !g_getVelFn) return vel;
    ParamFrame f(g_getVelFn);
    if (!Call(actor, f)) return vel;
    f.GetRaw(L"ReturnValue", &vel, sizeof(vel));
    return vel;
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
