#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cmath>
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
int32_t g_worldContextIdx = -1;  // GUObjectArray index of g_worldContext (for the safe staleness check)

void* ResolveWorldContext() {
    // The GameInstance persists across level loads and is a valid world context.
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) return gi;
    // Fall back to any live World (e.g. before the GameInstance is up).
    return R::FindObjectByClass(P::name::WorldClass);
}

// Return a VALID world context, dropping + re-resolving a stale one. Centralized so EVERY
// spawn/exec site shares the staleness guard -- the guard must not live at only some sites.
//
// bug2 ROOT CAUSE (2026-05-30): ResolveWorldContext prefers the persistent GameInstance but
// FALLS BACK to a World (which dies on a level reload). Resolve() dropped a dead context
// before reuse; SpawnActor + BeginDeferredSpawn only checked `!g_worldContext`, so a stale
// (freed) fallback World -- cached pre-GameInstance, then killed by the host's save-load
// level transition -- got reused, and BeginDeferredActorSpawnFromClass(World=null) returned
// null FOREVER (the host never spawned the connecting client's puppet; 128 consecutive
// failures observed in the 2026-05-30 smoke). Validate via IsLiveByIndex on the cached index,
// NOT IsLive(ptr): a GC-freed World pointer must not be dereferenced
// ([[feedback-islive-unsafe-on-freed-cached-pointer]]). After dropping a stale World the
// re-resolve prefers the GameInstance, which never dies -> the failure cannot recur.
void* EnsureWorldContext() {
    if (g_worldContext && !R::IsLiveByIndex(g_worldContext, g_worldContextIdx)) {
        UE_LOGW("engine: g_worldContext STALE (dead/recreated world, idx=%d) -- re-resolving "
                "[bug2 guard: prevents BeginDeferredActorSpawnFromClass null]", g_worldContextIdx);
        g_worldContext = nullptr;
        g_worldContextIdx = -1;
    }
    if (!g_worldContext) {
        g_worldContext = ResolveWorldContext();
        g_worldContextIdx = g_worldContext ? R::InternalIndexOf(g_worldContext) : -1;
        if (g_worldContext) {
            UE_LOGI("engine: world context resolved -> %p (class=%ls, idx=%d)",
                    g_worldContext, R::ClassNameOf(g_worldContext).c_str(), g_worldContextIdx);
        }
    }
    return g_worldContext;
}

bool Resolve() {
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(P::name::KismetSystemLibraryClass);
    if (g_kslCdo && !g_execFn) {
        if (void* cls = R::ClassOf(g_kslCdo)) {
            g_execFn = R::FindFunction(cls, P::name::ExecuteConsoleCommandFn);
        }
    }
    // World context can become available later than the CDO; the centralized
    // EnsureWorldContext re-resolves until found AND drops a destroyed one (a stale
    // World from the pre-GameInstance fallback would otherwise be reused after a
    // level reload -- see the helper's bug2 note).
    return g_kslCdo && g_execFn && EnsureWorldContext();
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
// World-pause verbs (coop pause_guard). GameplayStatics CDO + the two UFunctions are
// process-stable natives -- latch once. Separate from g_storyGsCdo below (that block's
// lifetime notes are save-boot-specific; sharing would tangle the concerns).
void* g_pauseGsCdo   = nullptr;
void* g_isPausedFn   = nullptr;  // UGameplayStatics::IsGamePaused(WorldContextObject)
void* g_setPausedFn  = nullptr;  // UGameplayStatics::SetGamePaused(WorldContextObject, bPaused)

bool ResolvePauseFns() {
    if (g_pauseGsCdo && g_isPausedFn && g_setPausedFn) return true;
    if (!g_pauseGsCdo) g_pauseGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_pauseGsCdo) {
        void* cls = R::ClassOf(g_pauseGsCdo);
        if (cls) {
            if (!g_isPausedFn)  g_isPausedFn  = R::FindFunction(cls, L"IsGamePaused");
            if (!g_setPausedFn) g_setPausedFn = R::FindFunction(cls, L"SetGamePaused");
        }
    }
    return g_pauseGsCdo && g_isPausedFn && g_setPausedFn;
}
}  // namespace

bool IsGamePaused() {
    // False on any resolution miss -- the pause guard then idles (never a false unpause).
    if (!ResolvePauseFns() || !EnsureWorldContext()) return false;
    ParamFrame f(g_isPausedFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", g_worldContext);
    if (!Call(g_pauseGsCdo, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool SetGamePaused(bool paused) {
    if (!ResolvePauseFns() || !EnsureWorldContext()) return false;
    ParamFrame f(g_setPausedFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", g_worldContext);
    f.Set<bool>(L"bPaused", paused);
    if (!Call(g_pauseGsCdo, f)) return false;
    return f.Get<bool>(L"ReturnValue");
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
void* g_getScaleFn = nullptr;
void* g_setScaleFn = nullptr;
void* g_setHiddenFn = nullptr;     // SetActorHiddenInGame (visual-only; instant-world deferred-hide)
void* g_setCollisionFn = nullptr;  // SetActorEnableCollision (paired with hide so a hidden mirror is not grabbable)

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
        if (!g_getScaleFn) g_getScaleFn = R::FindFunction(g_actorClass, P::name::GetActorScale3DFn);
        if (!g_setScaleFn) g_setScaleFn = R::FindFunction(g_actorClass, P::name::SetActorScale3DFn);
        if (!g_setHiddenFn) g_setHiddenFn = R::FindFunction(g_actorClass, P::name::SetActorHiddenInGameFn);
        if (!g_setCollisionFn) g_setCollisionFn = R::FindFunction(g_actorClass, P::name::SetActorEnableCollisionFn);
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
    EnsureWorldContext();  // drop+re-resolve a stale world context (bug2 guard)

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
        // 2026-05-25 audit fix (puppet audit IMPORTANT-6): also zero
        // APawn::AIControllerClass. AutoPossessAI=0 blocks AUTO-spawn of
        // an AI controller but does NOT prevent later code (other BP
        // systems iterating pawns + calling SpawnDefaultController) from
        // using the class default to acquire one. Nulling the class
        // pointer closes that path. Matches the documented invariant
        // "AI possession blocked at deferred-spawn (AutoPossessPlayer/AI
        // =Disabled, AIControllerClass=null)" in
        // [[project-coop-enemies-target-both]].
        *reinterpret_cast<void**>(a + P::off::APawn_AIControllerClass) = nullptr;
        UE_LOGI("engine: SpawnActor inertPawn -> no player possess, AIControllerClass=null, bBlockInput=1");
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

bool DebugCheckWorldContextRecovery() {
    EnsureWorldContext();  // baseline: ensure a valid context is cached first
    if (!g_worldContext) {
        UE_LOGW("worldctx_test: no world context resolved -- cannot self-test the guard");
        return false;
    }
    void* before = g_worldContext;
    const int32_t goodIdx = g_worldContextIdx;
    // Simulate a freed/recreated World after a level reload: the cached index no longer
    // matches g_worldContext's slot. Use an adjacent (in-range) index so IsLiveByIndex
    // returns false via a slot mismatch -- the same trigger the real stale World hits --
    // without any out-of-range read.
    g_worldContextIdx = goodIdx ^ 1;
    void* recovered = EnsureWorldContext();  // must DROP (IsLiveByIndex false) + re-resolve
    const bool ok = recovered != nullptr && R::IsLiveByIndex(recovered, g_worldContextIdx);
    UE_LOGI("worldctx_test: forced-stale guard check -- before=%p (idx=%d) corrupted->%d -> "
            "recovered=%p (class=%ls, idx=%d) live=%d -> %s",
            before, goodIdx, goodIdx ^ 1, recovered,
            recovered ? R::ClassNameOf(recovered).c_str() : L"<null>",
            g_worldContextIdx, ok ? 1 : 0, ok ? "PASS" : "FAIL");
    return ok;
}

namespace {
// Build a transform with rotation from FRotator (degrees -> quaternion). UE4
// uses Pitch=Y, Yaw=Z, Roll=X ordering for FRotator::Quaternion().
FTransform MakeTransform(const FVector& location, const FRotator& rotation) {
    FTransform t = MakeTransform(location);
    const float pitch = rotation.Pitch * 0.00872664625f;  // deg -> rad / 2
    const float yaw   = rotation.Yaw   * 0.00872664625f;
    const float roll  = rotation.Roll  * 0.00872664625f;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw),   cy = std::cos(yaw);
    const float sr = std::sin(roll),  cr = std::cos(roll);
    t.RotX =  cr * sp * sy - sr * cp * cy;
    t.RotY = -cr * sp * cy - sr * cp * sy;
    t.RotZ =  cr * cp * sy - sr * sp * cy;
    t.RotW =  cr * cp * cy + sr * sp * sy;
    return t;
}
}  // namespace

void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation) {
    if (!actorClass) return nullptr;
    if (!ResolveSpawn()) {
        UE_LOGE("engine: BeginDeferredSpawn unresolved (cdo=%p begin=%p)",
                g_gsCdo, g_beginSpawnFn);
        return nullptr;
    }
    EnsureWorldContext();  // drop+re-resolve a stale world context (bug2 guard)
    const FTransform xform = MakeTransform(location, rotation);
    ParamFrame begin(g_beginSpawnFn);
    begin.Set<void*>(L"WorldContextObject", g_worldContext);
    begin.Set<void*>(L"ActorClass", actorClass);
    begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
    begin.Set<void*>(L"Owner", nullptr);
    if (!Call(g_gsCdo, begin)) {
        UE_LOGE("engine: BeginDeferredSpawn call failed");
        return nullptr;
    }
    return begin.Get<void*>(L"ReturnValue");
}

bool FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation) {
    if (!actor || !ResolveSpawn()) return false;
    const FTransform xform = MakeTransform(location, rotation);
    ParamFrame finish(g_finishSpawnFn);
    finish.Set<void*>(L"Actor", actor);
    finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    if (!Call(g_gsCdo, finish)) {
        UE_LOGE("engine: FinishDeferredSpawn call failed");
        return false;
    }
    return true;
}

FVector GetActorLocation(void* actor) {
    FVector loc;
    if (!actor || !ResolveActorFns()) return loc;
    ParamFrame f(g_getLocFn);
    if (!Call(actor, f)) return loc;
    f.GetRaw(L"ReturnValue", &loc, sizeof(loc));
    return loc;
}

FVector GetActorScale3D(void* actor) {
    // Unit scale on failure -- callers stamp it straight into a spawn
    // transform, where (0,0,0) would collapse the mirror invisibly.
    FVector scl{1.f, 1.f, 1.f};
    if (!actor || !ResolveActorFns() || !g_getScaleFn) return scl;
    ParamFrame f(g_getScaleFn);
    if (!Call(actor, f)) return scl;
    f.GetRaw(L"ReturnValue", &scl, sizeof(scl));
    return scl;
}

bool IsChildActor(void* actor) {
    // See engine.h. Offset states: -2 = not yet resolved (retry while the Actor class
    // loads), -1 = property missing (layout drift; exclusion goes inert LOUDLY once).
    if (!actor) return false;
    static std::atomic<int32_t> sOff{-2};
    int32_t off = sOff.load(std::memory_order_acquire);
    if (off == -2) {
        void* cls = R::FindClass(P::name::ActorClassName);
        if (cls) {
            off = R::FindPropertyOffset(cls, L"ParentComponent");
            if (off < 0) {
                off = -1;
                UE_LOGW("engine: Actor.ParentComponent not found (engine layout drift?) -- "
                        "child-actor exclusion INERT");
            }
            sOff.store(off, std::memory_order_release);
        } else {
            return false;  // class not loaded yet; retry next call
        }
    }
    if (off < 0) return false;
    struct { int32_t idx; int32_t serial; } weak{};
    std::memcpy(&weak, reinterpret_cast<uint8_t*>(actor) + off, sizeof(weak));
    return weak.idx >= 0 && weak.serial != 0;
}

bool ForceGarbageCollection() {
    // UKismetSystemLibrary::CollectGarbage -- schedules a full GC purge at
    // the end of the current frame (the engine's own post-level-transition
    // pattern). Added 2026-06-10: the adoption sweep destroys ~1k actors at
    // SnapshotComplete after a ~3k-spawn bracket; UE's default 61 s purge
    // cadence held that pending-kill garbage at a ~10.6 GB client plateau
    // (smoke-measured) until the periodic purge freed 4.6 GB -- pairing the
    // mass destruction with the engine's purge collapses the plateau to
    // seconds. Game thread only (we run in the event_feed drain).
    static void* sCdo = nullptr;
    static void* sFn = nullptr;
    if (!sCdo) sCdo = R::FindClassDefaultObject(P::name::KismetSystemLibraryClass);
    if (sCdo && !sFn) {
        if (void* cls = R::ClassOf(sCdo)) {
            sFn = R::FindFunction(cls, P::name::CollectGarbageFn);
        }
    }
    if (!sCdo || !sFn) return false;
    ParamFrame f(sFn);
    return Call(sCdo, f);
}



float GetActorCharacterHalfHeight(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return 0.f;
    void* capsule = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayerPawn) + P::off::ACharacter_CapsuleComponent);
    if (!capsule) return 0.f;
    return *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(capsule) + P::off::UCapsuleComponent_CapsuleHalfHeight);
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

bool SetActorScale3D(void* actor, const FVector& scale) {
    if (!actor || !ResolveActorFns() || !g_setScaleFn) {
        UE_LOGE("engine: SetActorScale3D unresolved (fn=%p)", g_setScaleFn);
        return false;
    }
    // AActor::SetActorScale3D(FVector NewScale3D) -> void (no ReturnValue); success
    // is the success of the ProcessEvent call.
    ParamFrame f(g_setScaleFn);
    f.SetRaw(L"NewScale3D", &scale, sizeof(scale));
    return Call(actor, f);
}

FRotator GetComponentWorldRotation(void* component) {
    FRotator rot;
    if (!component || !R::IsLive(component)) return rot;
    static void* fn = nullptr;
    if (!fn) {
        if (void* sc = R::FindClass(P::name::SceneComponentClass))
            fn = R::FindFunction(sc, P::name::GetComponentRotationFn);
    }
    if (!fn) return rot;
    ParamFrame f(fn);
    if (!Call(component, f)) return rot;
    f.GetRaw(L"ReturnValue", &rot, sizeof(rot));
    return rot;
}

bool SetComponentWorldRotation(void* component, const FRotator& rotation) {
    if (!component || !R::IsLive(component)) return false;
    static void* fn = nullptr;
    if (!fn) {
        if (void* sc = R::FindClass(P::name::SceneComponentClass))
            fn = R::FindFunction(sc, P::name::SetWorldRotationFn);
    }
    if (!fn) return false;
    ParamFrame f(fn);
    f.SetRaw(L"NewRotation", &rotation, sizeof(rotation));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);  // K2_SetWorldRotation's FHitResult& out-param frame space is allocated by ParamFrame
    return Call(component, f);
}

bool ProjectWorldToScreen(void* playerController, const FVector& world,
                          FVector2D& outScreen, bool viewportRelative) {
    if (!playerController || !R::IsLive(playerController)) return false;
    static void* fn = nullptr;
    if (!fn) {
        // ProjectWorldLocationToScreen is defined on APlayerController; the local
        // player's controller is a VOTV subclass but inherits it, so resolve the
        // UFunction off the engine base class once + ProcessEvent dispatches it on
        // the real controller.
        if (void* pc = R::FindClass(P::name::PlayerControllerClassName))
            fn = R::FindFunction(pc, P::name::ProjectWorldToScreenFn);
    }
    if (!fn) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("engine: ProjectWorldToScreen unresolved -- PlayerController / "
                    "ProjectWorldLocationToScreen not found; nameplates won't project");
        }
        return false;
    }
    ParamFrame f(fn);
    f.SetRaw(L"WorldLocation", &world, sizeof(world));
    f.Set<bool>(L"bPlayerViewportRelative", viewportRelative);
    if (!Call(playerController, f)) return false;
    f.GetRaw(L"ScreenLocation", &outScreen, sizeof(outScreen));
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

bool SetActorHiddenInGame(void* actor, bool hidden) {
    // AActor::SetActorHiddenInGame -- VISUAL ONLY (bHidden @0x58; collision is the
    // SEPARATE bActorEnableCollision @0x5C). The instant-world deferred-spawn upper
    // layer hides a mirror until reconcile resolves, then reveals it. Game thread.
    if (!actor || !ResolveActorFns() || !g_setHiddenFn) {
        UE_LOGE("engine: SetActorHiddenInGame unresolved (fn=%p)", g_setHiddenFn);
        return false;
    }
    ParamFrame f(g_setHiddenFn);
    f.Set<bool>(L"bNewHidden", hidden);
    return Call(actor, f);
}

bool SetActorEnableCollision(void* actor, bool enabled) {
    // AActor::SetActorEnableCollision -- paired with SetActorHiddenInGame(true) on a
    // deferred-hidden mirror so it is not grab-trace-hittable / physics-active while
    // invisible (the SDK proves hide alone leaves collision on). Game thread.
    if (!actor || !ResolveActorFns() || !g_setCollisionFn) {
        UE_LOGE("engine: SetActorEnableCollision unresolved (fn=%p)", g_setCollisionFn);
        return false;
    }
    ParamFrame f(g_setCollisionFn);
    f.Set<bool>(L"bNewActorEnableCollision", enabled);
    return Call(actor, f);
}



namespace { void* g_teleportToFn = nullptr; void* g_getActorBoundsFn = nullptr; }

bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent) {
    if (!actor || !ResolveActorFns()) return false;
    if (!g_getActorBoundsFn) g_getActorBoundsFn = R::FindFunction(g_actorClass, P::name::GetActorBoundsFn);
    if (!g_getActorBoundsFn) { UE_LOGE("engine: GetActorBounds unresolved"); return false; }
    ParamFrame f(g_getActorBoundsFn);
    f.Set<bool>(L"bOnlyCollidingComponents", onlyColliding);
    f.Set<bool>(L"bIncludeFromChildActors", false);
    if (!Call(actor, f)) return false;
    f.GetRaw(L"Origin", &outOrigin, sizeof(outOrigin));
    f.GetRaw(L"BoxExtent", &outBoxExtent, sizeof(outBoxExtent));
    return true;
}

bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation) {
    if (!actor || !ResolveActorFns()) return false;
    if (!g_teleportToFn) g_teleportToFn = R::FindFunction(g_actorClass, P::name::TeleportToFn);
    if (!g_teleportToFn) { UE_LOGE("engine: TeleportTo unresolved"); return false; }
    ParamFrame f(g_teleportToFn);
    f.SetRaw(L"DestLocation", &location, sizeof(location));
    f.SetRaw(L"DestRotation", &rotation, sizeof(rotation));
    if (!Call(actor, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}



void WriteObjectField(void* target, size_t byteOffset, void* value) {
    if (!target) return;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(target) + byteOffset) = value;
}

void LogClassProperties(const wchar_t* className) {
    void* cls = R::FindClass(className);
    if (!cls) {
        UE_LOGW("LogClassProperties: class '%ls' not found", className);
        return;
    }
    UE_LOGI("LogClassProperties: %ls FProperty chain (own props only, no super):", className);
    auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(cls) +
                                               P::off::UStruct_ChildProperties);
    int idx = 0;
    while (field) {
        const auto name = R::ToString(*reinterpret_cast<const R::FName*>(field + P::off::FField_NamePrivate));
        const int32_t off = *reinterpret_cast<int32_t*>(field + P::off::FProperty_Offset_Internal);
        const int32_t sz = *reinterpret_cast<int32_t*>(field + P::off::FProperty_ElementSize) *
                           *reinterpret_cast<int32_t*>(field + P::off::FProperty_ArrayDim);
        UE_LOGI("  [%d] %ls @ +0x%X size=%d", idx, name.c_str(), off, sz);
        ++idx;
        field = *reinterpret_cast<uint8_t**>(field + P::off::FField_Next);
    }
    UE_LOGI("LogClassProperties: %ls -- %d properties listed", className, idx);
}

void* GetWorldContext() {
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) return gi;
    return R::FindObjectByClass(P::name::WorldClass);
}

// SpawnSoundAttenuation + PlaySoundAtLocation extracted to ue_wrap/engine_audio.cpp
// (2026-06-06 modularity audit: engine.cpp had grown past the 800-LOC soft cap; the
// audio block is self-contained so it lifts into its own file). Declarations remain in
// engine.h, so callers are unchanged.

void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw) {
    constexpr float kHalfDegToRad = 0.0087266462599716478846184538424431f;  // (pi/180)/2
    const float sp = std::sin(pitchDeg * kHalfDegToRad);
    const float cp = std::cos(pitchDeg * kHalfDegToRad);
    const float sy = std::sin(yawDeg   * kHalfDegToRad);
    const float cy = std::cos(yawDeg   * kHalfDegToRad);
    const float sr = std::sin(rollDeg  * kHalfDegToRad);
    const float cr = std::cos(rollDeg  * kHalfDegToRad);
    qx =  cr * sp * sy - sr * cp * cy;
    qy = -cr * sp * cy - sr * cp * sy;
    qz =  cr * cp * sy - sr * sp * cy;
    qw =  cr * cp * cy + sr * sp * sy;
}

}  // namespace ue_wrap::engine
