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
    // World context can become available later than the CDO; re-resolve until found.
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
    }
    return g_actorClass && g_getLocFn && g_setLocFn;
}

}  // namespace

void* SpawnActor(void* actorClass, const FVector& location) {
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

// Byte size of a UFunction's parameter named `name`, straight from reflection
// (ElementSize*ArrayDim). 0 if not found. We drive FText copies off this rather
// than hardcoding 16/24 -- the same FText must round-trip Conv's ReturnValue and
// SetText's Value, and an FText with a null shared-ref controller crashes the
// engine's refcount increment (write to controller+0x20).
int32_t ParamSize(void* fn, const wchar_t* name) {
    for (const auto& p : R::FunctionParams(fn)) {
        if (p.name == name) return p.size;
    }
    return 0;
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

void* SpawnTextMarker(const FVector& location, const wchar_t* text, float worldSize) {
    void* ktlCdo = R::FindClassDefaultObject(P::name::KismetTextLibraryClass);
    void* convFn = ktlCdo ? R::FindFunction(R::ClassOf(ktlCdo), P::name::ConvStringToTextFn) : nullptr;
    void* traClass = R::FindClass(P::name::TextRenderActorClass);
    void* trcClass = R::FindClass(P::name::TextRenderComponentClass);
    void* setTextFn = trcClass ? R::FindFunction(trcClass, P::name::SetTextFn) : nullptr;
    void* setSizeFn = trcClass ? R::FindFunction(trcClass, P::name::SetWorldSizeFn) : nullptr;
    if (!convFn || !traClass || !setTextFn) {
        UE_LOGE("engine: SpawnTextMarker unresolved (conv=%p tra=%p trc=%p setText=%p)",
                convFn, traClass, trcClass, setTextFn);
        return nullptr;
    }

    // 1) FString -> FText (UKismetTextLibrary::Conv_StringToText). Sizes come from
    //    reflection so Conv's ReturnValue and SetText's Value agree (both FText).
    const int32_t convRetSize = ParamSize(convFn, L"ReturnValue");
    const int32_t setValSize = ParamSize(setTextFn, L"Value");
    const int32_t ftextSize = (convRetSize > setValSize ? convRetSize : setValSize);
    if (ftextSize <= 0) {
        UE_LOGE("engine: SpawnTextMarker FText sizes unresolved (convRet=%d setVal=%d)",
                convRetSize, setValSize);
        return nullptr;
    }

    std::wstring sbuf(text);
    R::FString fs{sbuf.data(), static_cast<int32_t>(sbuf.size()) + 1,
                  static_cast<int32_t>(sbuf.size()) + 1};
    std::vector<uint8_t> ftext(static_cast<size_t>(ftextSize), 0);
    bool convOk = false;
    {
        ParamFrame conv(convFn);
        conv.SetRaw(L"inString", &fs, sizeof(fs));
        convOk = Call(ktlCdo, conv);
        conv.GetRaw(L"ReturnValue", ftext.data(), convRetSize);
    }
    // FText's TSharedRef is { ITextData* Object @0x00; FReferenceController* @0x08 }.
    // A null controller means Conv didn't populate -- passing it to SetText would
    // crash on the refcount increment, so verify before use.
    uint64_t ftObj = 0, ftCtrl = 0;
    std::memcpy(&ftObj, ftext.data(), sizeof(ftObj));
    std::memcpy(&ftCtrl, ftext.data() + 8, sizeof(ftCtrl));
    UE_LOGI("engine: SpawnTextMarker FText convOk=%d convRet=%d setVal=%d obj=%p ctrl=%p",
            convOk, convRetSize, setValSize, reinterpret_cast<void*>(ftObj),
            reinterpret_cast<void*>(ftCtrl));

    // 2) Spawn the TextRenderActor and find its TextRenderComponent.
    void* actor = SpawnActor(traClass, location);
    if (!actor) return nullptr;
    void* trc = nullptr;
    for (const auto& c : R::ChildObjectsOf(actor)) {
        if (c.className == P::name::TextRenderComponentClass) { trc = c.object; break; }
    }
    if (!trc) { UE_LOGW("engine: TextMarker has no TextRenderComponent"); return actor; }

    // 3) SetText(FText Value) + SetWorldSize(float Value) + ensure visible.
    //    Per-call logs (flushed) so a crash in any one of these is pinpointed by
    //    which "marker: before X" line is the last in the log.
    UE_LOGI("engine: marker trc=%p before SetText (setVal=%d)", trc, setValSize);
    if (ftCtrl == 0) {
        UE_LOGW("engine: TextMarker FText has null shared-ref controller; skipping "
                "SetText (would crash on refcount). Marker spawned but blank.");
    } else {
        ParamFrame f(setTextFn);
        f.SetRaw(L"Value", ftext.data(), setValSize);
        Call(trc, f);
    }
    UE_LOGI("engine: marker before SetWorldSize");
    if (setSizeFn) {
        ParamFrame f(setSizeFn);
        f.Set<float>(L"Value", worldSize);
        Call(trc, f);
    }
    UE_LOGI("engine: marker before SetComponentVisible");
    SetComponentVisible(trc);
    UE_LOGI("engine: SpawnTextMarker '%ls' actor=%p at (%.0f,%.0f,%.0f)",
            text, actor, location.X, location.Y, location.Z);
    return actor;
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

}  // namespace ue_wrap::engine
