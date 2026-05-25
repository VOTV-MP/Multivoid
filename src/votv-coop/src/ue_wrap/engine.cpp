#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

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

FVector GetActorLocation(void* actor) {
    FVector loc;
    if (!actor || !ResolveActorFns()) return loc;
    ParamFrame f(g_getLocFn);
    if (!Call(actor, f)) return loc;
    f.GetRaw(L"ReturnValue", &loc, sizeof(loc));
    return loc;
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

bool SetActorTickEnabled(void* actor, bool enabled) {
    if (!actor || !ResolveActorFns() || !g_setTickFn) {
        UE_LOGE("engine: SetActorTickEnabled unresolved (fn=%p)", g_setTickFn);
        return false;
    }
    ParamFrame f(g_setTickFn);
    f.Set<bool>(L"bEnabled", enabled);
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



// ---- Bug 2 / Plan B2: AnimBP satellite ----------------------------------------
// The puppet's AnimInstance fields (Movement, spd, IK targets, ...) are populated
// by BUA reading Pawn->GetMovementComponent()->Velocity. On a SkeletalMeshActor
// puppet, Pawn=NULL -> BUA early-outs -> the AnimInstance is vastly under-
// populated (confirmed empirically via the full-region dump 2026-05-23: 8 set
// fields on puppet vs ~26 on local). The state machine transition reads one or
// more of those fields and stays in idle.
//
// Fix: spawn a hidden satellite ACharacter, point puppet AnimInstance.Pawn at
// it, drive its Movement.Velocity from the network-streamed pose. BUA runs
// naturally, populates everything from the satellite. State machine sees
// valid Movement + correct velocity -> transitions on the same conditions
// the local mainPlayer's AnimInstance does.

void* SpawnSatelliteCharacter(const FVector& location) {
    void* characterClass = R::FindClass(L"Character");
    if (!characterClass) {
        UE_LOGE("engine: SpawnSatelliteCharacter -- Character class not found");
        return nullptr;
    }
    // inertPawn=true: clears AutoPossessPlayer/AI + sets bBlockInput in the
    // deferred window -> no PlayerController acquisition, no input theft.
    void* sat = SpawnActor(characterClass, location, /*inertPawn=*/true);
    if (!sat) {
        UE_LOGE("engine: SpawnSatelliteCharacter -- SpawnActor failed");
        return nullptr;
    }
    UE_LOGI("engine: SpawnSatelliteCharacter -> %p at (%.0f,%.0f,%.0f)",
            sat, location.X, location.Y, location.Z);
    return sat;
}



void WriteObjectField(void* target, size_t byteOffset, void* value) {
    if (!target) return;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(target) + byteOffset) = value;
}

namespace {
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
}  // namespace

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


}  // namespace ue_wrap::engine
