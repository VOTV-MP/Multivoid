#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

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
// Cached across the harness's boot retry loop (LoadStorySave is polled until we're
// in gameplay). The CDO + UFunctions never move; the slot is loaded from disk ONCE.
void* g_storyGsCdo = nullptr;
void* g_loadGameFn = nullptr;
void* g_setSaveSlotFn = nullptr;
void* g_storySave = nullptr;  // cached USaveGame* (LoadGameFromSlot once)

// Story/sandbox GameMode fix (2026-06-03). VOTV stores a save's game mode ONLY in
// the slot-name PREFIX: getSavePrefix(mode) (on Uui_saveSlots_C) yields the prefix
// VOTV uses for that mode, and a slot whose name starts with that prefix IS that
// mode. The menu sets mainGameInstance.GameMode @0x01E1 from this on load; our
// LoadStorySave bypass did NOT, so a story save loaded in the default (sandbox)
// mode (user-flagged 2026-06-02). Resolved once, then re-used across the boot poll.
void* g_saveSlotsUiCdo  = nullptr;  // Uui_saveSlots_C CDO (getSavePrefix is a pure mode->prefix map)
void* g_getSavePrefixFn = nullptr;  // Uui_saveSlots_C::getSavePrefix(enum_gamemode) -> FString prefix
bool  g_gameModeApplied = false;    // latch: derive + write GameMode once per session
constexpr uint8_t kEnumGamemodeCount = 8;  // enum_gamemode::enum_MAX (enum_gamemode_enums.hpp)

// Derive the slot's game mode from its name prefix (exactly as VOTV's menu does --
// NO hardcoded enum value, so it is correct for story AND sandbox AND any future
// mode) and write mainGameInstance.GameMode @0x01E1. Retried each boot poll until
// Uui_saveSlots_C is loaded; runs the derivation once it is, then latches. Game
// thread only. Logs every getSavePrefix(mode) result so the mapping is verifiable.
void ApplyGameModeFromSlot(void* gi, const wchar_t* slot) {
    if (g_gameModeApplied || !gi || !slot) return;
    if (!g_saveSlotsUiCdo) g_saveSlotsUiCdo = R::FindClassDefaultObject(L"ui_saveSlots_C");
    if (g_saveSlotsUiCdo && !g_getSavePrefixFn) {
        if (void* c = R::ClassOf(g_saveSlotsUiCdo))
            g_getSavePrefixFn = R::FindFunction(c, L"getSavePrefix");
    }
    if (!g_saveSlotsUiCdo || !g_getSavePrefixFn) {
        // Menu save-slots widget not loaded yet at this boot stage. Retry next poll
        // (do NOT latch). If it never loads, the warning persists + GameMode stays
        // as-is (current behaviour) -- a visible signal to pivot.
        UE_LOGW("engine: ApplyGameModeFromSlot -- ui_saveSlots_C cdo=%p getSavePrefix=%p not loaded yet; "
                "GameMode left as-is (will retry)", g_saveSlotsUiCdo, g_getSavePrefixFn);
        return;
    }
    const std::wstring slotStr(slot);
    int    bestMode = -1;
    size_t bestLen  = 0;
    for (uint8_t mode = 0; mode < kEnumGamemodeCount; ++mode) {
        ParamFrame f(g_getSavePrefixFn);
        f.Set<uint8_t>(L"Index", mode);  // TEnumAsByte<enum_gamemode::Type> = 1 byte
        if (!Call(g_saveSlotsUiCdo, f)) continue;
        const R::FString pre = f.Get<R::FString>(L"ReturnValue");
        std::wstring preStr;
        if (pre.Data && pre.Num > 1) preStr.assign(pre.Data, pre.Data + (pre.Num - 1));  // Num counts the null
        UE_LOGI("engine: getSavePrefix(%u) = '%ls'", static_cast<unsigned>(mode), preStr.c_str());
        // Longest matching prefix wins (defends against one prefix being a prefix of
        // another, e.g. "" matching everything).
        if (!preStr.empty() && slotStr.rfind(preStr, 0) == 0 && preStr.size() > bestLen) {
            bestLen  = preStr.size();
            bestMode = static_cast<int>(mode);
        }
    }
    // The widget is loaded + getSavePrefix is deterministic -> this is our one shot;
    // latch regardless of match (re-running would give the identical result).
    g_gameModeApplied = true;
    uint8_t* gm = reinterpret_cast<uint8_t*>(gi) + profile::off::mainGameInstance_GameMode;
    if (bestMode >= 0) {
        const uint8_t old = *gm;
        *gm = static_cast<uint8_t>(bestMode);
        UE_LOGI("engine: ApplyGameModeFromSlot -- slot '%ls' prefix-matched GameMode=%d (was %u, prefix len %zu); "
                "set @0x01E1 (story-loads-as-sandbox fix)",
                slot, bestMode, static_cast<unsigned>(old), bestLen);
    } else {
        UE_LOGW("engine: ApplyGameModeFromSlot -- NO getSavePrefix prefix matched slot '%ls' (GameMode stays %u); "
                "inspect the getSavePrefix dump above", slot, static_cast<unsigned>(*gm));
    }
}
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

    // Set the GameMode (story / sandbox / ...) from the slot prefix BEFORE the
    // travel -- VOTV's menu does this on load; our bypass didn't, so a story save
    // loaded as sandbox. Retried each poll until the save-slots widget is loaded.
    ApplyGameModeFromSlot(gi, slot);

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: LoadStorySave -- at preLoad/menu; (re)issuing '%ls' (save '%ls' registered)",
            openCmd.c_str(), slot);
    ExecuteConsoleCommand(openCmd.c_str());
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Travel to VOTV's MAIN MENU via the game's own level-travel verb,
// AmainGamemode_C::transition(FName "/Game/menu"). The FULL package path is required
// -- the short name "menu" does NOT resolve (probed 2026-06-01), unlike `open
// untitled_1`. Used by the local-death flee: it works regardless of player state (a
// direct gamemode call -- NO pause menu needed, so a dead/ragdolling player can travel)
// and, with the ProcessEvent detour held in transparent bypass for the duration, it
// reaches the menu and tears down the gameplay world WITHOUT our layer hanging the
// teardown or churning at the menu (validated: RSS flat + decreasing for 160 s).
// Game thread only.
bool ReturnToMainMenu() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) {
        UE_LOGW("engine: ReturnToMainMenu -- no live mainGamemode_C");
        return false;
    }
    void* fn = R::FindFunction(R::ClassOf(gm), P::name::MainGamemodeTransitionFn);
    if (!fn) {
        UE_LOGW("engine: ReturnToMainMenu -- mainGamemode_C::transition UFunction not resolved");
        return false;
    }
    R::FName ln = ue_wrap::fname_utils::StringToFName(L"/Game/menu");
    if (ln.ComparisonIndex == 0 && ln.Number == 0) {
        // StringToFName failed (Conv_StringToName unresolved) -> NAME_None. Do NOT pass
        // a None level name to transition (unprobed; could no-op or travel to nowhere).
        UE_LOGW("engine: ReturnToMainMenu -- StringToFName(\"/Game/menu\") returned NAME_None; abort");
        return false;
    }
    ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) {
        UE_LOGW("engine: ReturnToMainMenu -- transition SetRaw(LevelName) failed");
        return false;
    }
    const bool ok = Call(gm, f);
    UE_LOGI("engine: ReturnToMainMenu -- mainGamemode_C::transition(\"/Game/menu\") dispatched=%d",
            ok ? 1 : 0);
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

namespace { void* g_setScaleFn = nullptr; }

bool SetActorScale3D(void* actor, const FVector& scale) {
    if (!actor || !ResolveActorFns()) return false;
    if (!g_setScaleFn) g_setScaleFn = R::FindFunction(g_actorClass, P::name::SetActorScale3DFn);
    if (!g_setScaleFn) { UE_LOGE("engine: SetActorScale3D unresolved"); return false; }
    ParamFrame f(g_setScaleFn);
    f.SetRaw(L"NewScale3D", &scale, sizeof(scale));
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

namespace {
// Cached UFunction pointers + UClass for SpawnSoundAttenuation. The 3
// resolve attempts run lazily on first call; once non-null they stay
// (GameplayStatics CDO + SoundAttenuation UClass are process-stable).
void* g_gsCdoForAtt    = nullptr;
void* g_spawnObjectFn  = nullptr;
void* g_attClass       = nullptr;

bool ResolveAttSpawn() {
    if (!g_gsCdoForAtt) g_gsCdoForAtt = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdoForAtt && !g_spawnObjectFn) {
        if (void* gsCls = R::ClassOf(g_gsCdoForAtt)) {
            g_spawnObjectFn = R::FindFunction(gsCls, P::name::SpawnObjectFn);
        }
    }
    if (!g_attClass) g_attClass = R::FindClass(P::name::SoundAttenuationClass);
    return g_gsCdoForAtt && g_spawnObjectFn && g_attClass;
}
}  // namespace

void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg) {
    if (!ResolveAttSpawn()) return nullptr;

    // 1) SpawnObject(objectClass, Outer) -> UObject*. CASE-SENSITIVE param
    //    names per UE reflection: lowercase 'objectClass' + 'Outer' (the
    //    existing pattern in engine_widget.cpp's widget spawn). An
    //    uppercase-O `ObjectClass` would FName-mismatch -> SetRaw fail ->
    //    SpawnObject sees a null class -> returns null. Outer = the
    //    GameplayStatics CDO, which is process-stable.
    void* obj = nullptr;
    {
        ParamFrame f(g_spawnObjectFn);
        f.Set<void*>(L"objectClass", g_attClass);
        f.Set<void*>(L"Outer", g_gsCdoForAtt);
        if (!Call(g_gsCdoForAtt, f)) return nullptr;
        obj = f.Get<void*>(L"ReturnValue");
    }
    if (!obj) return nullptr;

    // 2) Configure via raw memory writes. UE exposes no setter
    //    UFunctions for these fields (they are edit-time UProperties
    //    on USoundAttenuation). Offsets cataloged in sdk_profile.h
    //    `att::` namespace -- the wrapper hides them from gameplay code.
    auto* p = reinterpret_cast<uint8_t*>(obj);
    *reinterpret_cast<uint8_t*>(p + P::off::att::AttenuationShape)  = cfg.shape;
    *reinterpret_cast<uint8_t*>(p + P::off::att::DistanceAlgorithm) = cfg.distanceAlgorithm;
    *reinterpret_cast<uint8_t*>(p + P::off::att::FalloffMode)       = cfg.falloffMode;
    float* extents = reinterpret_cast<float*>(p + P::off::att::AttenuationShapeExtents);
    extents[0] = cfg.extents[0];
    extents[1] = cfg.extents[1];
    extents[2] = cfg.extents[2];
    *reinterpret_cast<float*>(p + P::off::att::FalloffDistance)    = cfg.falloffDistance;
    *reinterpret_cast<float*>(p + P::off::att::ConeOffset)         = cfg.coneOffset;
    *reinterpret_cast<float*>(p + P::off::att::dBAttenuationAtMax) = cfg.dBAttenuationAtMax;
    uint8_t& flags = *reinterpret_cast<uint8_t*>(p + P::off::att::FlagsByte);
    if (cfg.attenuate)  flags |= 0x01; else flags &= ~static_cast<uint8_t>(0x01);
    if (cfg.spatialize) flags |= 0x02; else flags &= ~static_cast<uint8_t>(0x02);

    // 3) AddToRoot so UE GC keeps this object alive across collections.
    //    Caller will hold the pointer in a C++ static (invisible to UE's
    //    reachability scan) -- without rooting, the object is reaped on
    //    the next GC pass and PlaySoundAtLocation crashes reading freed
    //    memory on the next call (2026-05-26 F-spam crash root cause).
    R::AddToRoot(obj);
    return obj;
}

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
