// ue_wrap/engine_save.cpp -- the save load, the game-mode derivation, the campaign-scoped save
// cache, the fresh New Game boot and the return to the main menu. Declared in
// ue_wrap/engine/engine_save.h, which the engine.h umbrella includes; everything here is
// game-thread only. The load path is a boot poll: LoadStorySave and
// StartFreshGame are retried until the save class and the world are live.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::engine {

namespace P = profile;
namespace R = reflection;

namespace {
// Cached across the boot poll. The GameplayStatics CDO and its UFunction are native and rooted;
// setSaveSlotObject belongs to mainGameInstance_C, whose class lives as long as the persistent
// GameInstance.
void* g_storyGsCdo = nullptr;
void* g_loadGameFn = nullptr;
void* g_setSaveSlotFn = nullptr;

// The cached save object is campaign-scoped, not process-scoped: a campaign is one continuous poll
// sequence targeting one slot. Within it the gameInstance.saveSlotObject property we register into
// keeps the object alive; once the campaign ends the game may replace that reference and the object
// can be purged at any world transition. So a new campaign forces a disk reload, and IsLiveByIndex
// guards the reuse within one. Caching across campaigns instead planted a dangling pointer into the
// GameInstance and the world was rebuilt from freed memory.
void* g_storySave = nullptr;          // cached USaveGame* (one disk load per campaign)
int32_t g_storySaveIdx = -1;          // its GUObjectArray index (IsLiveByIndex guard)
std::wstring g_storySaveSlot;         // campaign identity axis 1: the target slot
// The second axis: the non-gameplay world the campaign polls in. Every return to the menu
// creates a new menu world, so a changed polling world means a world round-trip happened since
// the cache was built, whether or not any poll saw the gameplay in between.
void* g_campaignWorld = nullptr;
int32_t g_campaignWorldIdx = -1;

// StartFreshGame's pseudo-slot: the blank save is registered under this name, which doubles as
// that path's campaign identity.
constexpr const wchar_t* kFreshSlotName = L"coop_client_fresh";

// The coop inventory layer registers this to overwrite a freshly loaded or created save's
// player inventory before the native loadObjects materialises it.
SaveObjectReadyHook g_saveObjectReadyHook = nullptr;
// Fired once per loaded or created save object; the hook self-gates to a no-op off a join.
void FireSaveObjectReadyHook(void* saveObj) {
    if (!g_saveObjectReadyHook || !saveObj) return;
    UE_LOGI("engine: firing SaveObjectReadyHook on save object %p (pre-materialize)", saveObj);
    g_saveObjectReadyHook(saveObj);
}

// The game stores a save's mode only in the slot-name prefix: getSavePrefix(mode) yields the
// prefix for a mode, and a slot whose name starts with it is that mode. The menu writes
// mainGameInstance.GameMode from this on load; the load bypass here has to do the same, or a
// story save loads in the default sandbox mode.
void* g_saveSlotsUiCdo  = nullptr;  // the ui_saveSlots_C CDO; getSavePrefix is a pure mode-to-prefix map
int32_t g_saveSlotsUiCdoIdx = -1;   // its GUObjectArray index; a BP CDO can be collected with its class
void* g_getSavePrefixFn = nullptr;  // ui_saveSlots_C::getSavePrefix(mode) -> FString prefix
bool  g_gameModeApplied = false;    // the GameMode is derived and written once per campaign
constexpr uint8_t kEnumGamemodeCount = 8;  // enum_gamemode::enum_MAX

// The ui_saveSlots_C CDO and its getSavePrefix, cached. The widget loads on the first menu or
// gameplay transition; before that this returns false and the caller retries. getSavePrefix is
// pure, so the CDO is a valid call target.
bool ResolveSavePrefixFn() {
    // A BP class, its CDO and its UFunctions can be collected with the menu world; a later campaign
    // re-resolves the reloaded class instead of calling into freed memory.
    if (g_saveSlotsUiCdo && !R::IsLiveByIndex(g_saveSlotsUiCdo, g_saveSlotsUiCdoIdx)) {
        UE_LOGI("engine: ResolveSavePrefixFn -- ui_saveSlots_C CDO was GC'd; re-resolving");
        g_saveSlotsUiCdo = nullptr;
        g_saveSlotsUiCdoIdx = -1;
        g_getSavePrefixFn = nullptr;
    }
    if (!g_saveSlotsUiCdo) {
        g_saveSlotsUiCdo = R::FindClassDefaultObject(L"ui_saveSlots_C");
        g_saveSlotsUiCdoIdx = g_saveSlotsUiCdo ? R::InternalIndexOf(g_saveSlotsUiCdo) : -1;
        g_getSavePrefixFn = nullptr;  // belongs to the (possibly reloaded) class
    }
    if (g_saveSlotsUiCdo && !g_getSavePrefixFn) {
        if (void* c = R::ClassOf(g_saveSlotsUiCdo))
            g_getSavePrefixFn = R::FindFunction(c, L"getSavePrefix");
    }
    return g_saveSlotsUiCdo && g_getSavePrefixFn;
}

// The slot's game mode from its name prefix, as the menu does, written to the GameInstance.
// Shares the prefix source with the save browser. Retried each poll until the widget is loaded,
// then runs once. Game thread only.
void ApplyGameModeFromSlot(void* gi, const wchar_t* slot, int forceGameMode = -1) {
    if (g_gameModeApplied || !gi || !slot) return;
    // The save-transfer slot carries a prefix the game's mode map cannot match; the wire carried
    // the host's mode, written directly.
    if (forceGameMode >= 0) {
        g_gameModeApplied = true;
        uint8_t* gm = reinterpret_cast<uint8_t*>(gi) + profile::off::mainGameInstance_GameMode;
        const uint8_t old = *gm;
        *gm = static_cast<uint8_t>(forceGameMode);
        UE_LOGI("engine: ApplyGameModeFromSlot -- slot '%ls' FORCED GameMode=%d (was %u; v56 coop slot)",
                slot, forceGameMode, static_cast<unsigned>(old));
        return;
    }
    if (!ResolveSavePrefixFn()) {
        // The widget is not loaded yet at this boot stage: retried next poll, not latched. Should
        // it never load, the warning persists and the mode stays as-is, a visible signal.
        UE_LOGW("engine: ApplyGameModeFromSlot -- ui_saveSlots_C cdo=%p getSavePrefix=%p not loaded yet; "
                "GameMode left as-is (will retry)", g_saveSlotsUiCdo, g_getSavePrefixFn);
        return;
    }
    const int bestMode = DeriveModeFromSlot(slot);
    // The widget is loaded and getSavePrefix is deterministic, so this latches whether or not a
    // prefix matched; a re-run would give the same answer.
    g_gameModeApplied = true;
    uint8_t* gm = reinterpret_cast<uint8_t*>(gi) + profile::off::mainGameInstance_GameMode;
    if (bestMode >= 0) {
        const uint8_t old = *gm;
        *gm = static_cast<uint8_t>(bestMode);
        UE_LOGI("engine: ApplyGameModeFromSlot -- slot '%ls' prefix-matched GameMode=%d (was %u); "
                "set @0x01E1 (story-loads-as-sandbox fix)", slot, bestMode, static_cast<unsigned>(old));
    } else {
        UE_LOGW("engine: ApplyGameModeFromSlot -- NO getSavePrefix prefix matched slot '%ls' "
                "(GameMode stays %u)", slot, static_cast<unsigned>(*gm));
    }
}

// The campaign scope of the cache, one owner: called at the top of the boot phase with the target
// slot and the current non-gameplay world. A changed polling world or slot is a new campaign and
// resets everything, reloading from disk since an autosave may have rewritten the slot. Within a
// campaign a purge between polls drops only the object; the mode latch stays, the GameInstance
// already holding the byte.
void ValidateCachedSaveForCampaign(const wchar_t* slot, void* curWorld) {
    const bool worldChanged =
        g_campaignWorld &&
        (g_campaignWorld != curWorld || !R::IsLiveByIndex(g_campaignWorld, g_campaignWorldIdx));
    const bool slotChanged = !g_storySaveSlot.empty() && g_storySaveSlot != slot;
    if (worldChanged || slotChanged) {
        UE_LOGI("engine: save cache -- NEW load campaign (target '%ls', cached '%ls'%s%s) -> "
                "full reset, (re)load from disk",
                slot, g_storySaveSlot.c_str(),
                worldChanged ? ", polling world changed" : "",
                slotChanged ? ", slot changed" : "");
        ResetCachedSave();
    }
    if (!g_campaignWorld && curWorld) {
        g_campaignWorld = curWorld;
        g_campaignWorldIdx = R::InternalIndexOf(curWorld);
    }
    if (g_storySave && !R::IsLiveByIndex(g_storySave, g_storySaveIdx)) {
        UE_LOGW("engine: save cache -- cached save %p ('%ls') was GC-purged mid-campaign; "
                "reloading from disk", g_storySave, g_storySaveSlot.c_str());
        g_storySave = nullptr;
        g_storySaveIdx = -1;
    }
}
// The boot poll's two "where are we?" reads, one owner for LoadStorySave and StartFreshGame.
// Adopted from archhn0madd's Multifoid fork; see docs/CREDITS.md.
// After a quit to the menu the dying gameplay world and its player corpse stay in GUObjectArray for
// tens of seconds and their kill flags can lag, so a class search found the corpse off the origin
// and answered "in gameplay" while the process sat at the menu. This keys on world identity
// instead: the current world comes from the GameInstance's local player chain, which a dying world
// cannot hold alive, and the pawn must be live and belong to it. The legacy first-World scan is
// kept only for the mid-travel window, where that chain cannot answer.
struct BootWorldView {
    bool inGameplay;       // (a): a LIVE mainPlayer_C of the CURRENT world, off-origin
    bool gameplayLoading;  // (b): a gameplay world is up/loading -> never re-open, wait
    void* curWorld;        // the world phase (c) stamps as the campaign's polling world
};
BootWorldView SurveyBootWorld(const char* who) {
    BootWorldView v{false, false, nullptr};
    const bool readerUp = !ue_wrap::world_identity::Degraded();
    v.curWorld = readerUp ? ue_wrap::world_identity::CurrentWorld() : nullptr;

    // (a) Already in gameplay: a mainPlayer_C placed in the level, off the origin.
    if (void* lp = R::FindObjectByClass(P::name::MainPlayerClass)) {
        const bool pawnLive = R::IsLive(lp);  // fresh-pointer guard BEFORE the WorldOf deref
        void* pawnWorld = (pawnLive && readerUp) ? ue_wrap::world_identity::WorldOf(lp) : nullptr;
        const bool currentWorldPawn =
            pawnLive && (!readerUp || (pawnWorld != nullptr && pawnWorld == v.curWorld));
        if (currentWorldPawn) {
            const FVector p = GetActorLocation(lp);
            if (std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) > 100.f) {
                UE_LOGI("engine: %s -- in gameplay (mainPlayer @ %.0f,%.0f,%.0f)",
                        who, p.X, p.Y, p.Z);
                v.inGameplay = true;
                return v;
            }
            // A live current-world pawn at the origin is pre-placement: fall through to (b).
        } else {
            // Throttled: the poll re-runs every second or so and hits the same corpse every time,
            // so the first three and then a periodic heartbeat. Keyed on the corpse's identity, not
            // a bare count: this static is shared by both callers and outlives the cache reset, so
            // a plain counter would mute the second stale corpse of a process, which in the rejoin
            // case is the one that matters.
            static void* sLastStaleActor = nullptr;
            static unsigned sStaleSkips = 0;
            if (lp != sLastStaleActor) {
                sLastStaleActor = lp;
                sStaleSkips = 0;
            }
            ++sStaleSkips;
            if (sStaleSkips <= 3 || sStaleSkips % 16 == 0) {
                UE_LOGW("engine: %s -- mainPlayer_C %p is a STALE other-world actor "
                        "(live=%d itsWorld=%p current=%p readerUp=%d) -- the quit-to-menu "
                        "corpse window; ignoring it, continuing the boot (skip #%u)",
                        who, lp, pawnLive ? 1 : 0, pawnWorld, v.curWorld,
                        readerUp ? 1 : 0, sStaleSkips);
            }
        }
    }
    // (b) The gameplay map already up or loading: never re-open, just wait for the player to spawn.
    const ue_wrap::world_identity::WorldKind kind =
        readerUp ? ue_wrap::world_identity::CurrentWorldKind()
                 : ue_wrap::world_identity::WorldKind::Unknown;
    if (kind == ue_wrap::world_identity::WorldKind::Gameplay) {
        v.gameplayLoading = true;
    } else if (kind == ue_wrap::world_identity::WorldKind::Unknown) {
        // Mid-travel, or a degraded chain: the legacy reader, where the dying gameplay world is the
        // right "wait" answer.
        v.curWorld = R::FindObjectByClass(P::name::WorldClass);
        if (v.curWorld &&
            R::ToString(R::NameOf(v.curWorld)).find(L"ntitled") != std::wstring::npos)
            v.gameplayLoading = true;
    }
    // The menu or preLoad: fall through, and the open is issued.
    return v;
}

}  // namespace

bool GetSavePrefix(uint8_t mode, std::wstring& out) {
    out.clear();
    if (!ResolveSavePrefixFn()) return false;
    ParamFrame f(g_getSavePrefixFn);
    f.Set<uint8_t>(L"Index", mode);  // TEnumAsByte<enum_gamemode::Type> = 1 byte
    if (!Call(g_saveSlotsUiCdo, f)) return false;
    const R::FString pre = f.Get<R::FString>(L"ReturnValue");
    if (pre.Data && pre.Num > 1) out.assign(pre.Data, pre.Data + (pre.Num - 1));  // Num counts the null
    return true;
}

int DeriveModeFromSlot(const wchar_t* slot) {
    if (!slot || !ResolveSavePrefixFn()) return -1;
    const std::wstring slotStr(slot);
    int    bestMode = -1;
    size_t bestLen  = 0;
    for (uint8_t mode = 0; mode < kEnumGamemodeCount; ++mode) {
        std::wstring pre;
        if (!GetSavePrefix(mode, pre)) continue;
        // The longest matching prefix wins, so one prefix that is a prefix of another (an empty
        // one, say) cannot win.
        if (!pre.empty() && slotStr.rfind(pre, 0) == 0 && pre.size() > bestLen) {
            bestLen  = pre.size();
            bestMode = static_cast<int>(mode);
        }
    }
    return bestMode;
}

// Called repeatedly by the boot loop. True only once a mainPlayer_C is in the level; while still
// at preLoad, the warning screen or the menu it re-issues the open each call, since a single
// open fired during preLoad is silently dropped. It does not re-open once the gameplay world is
// loading, which would restart the load.
bool LoadStorySave(const wchar_t* slot, int forceGameMode) {
    if (!slot || !*slot) return false;

    // The boot survey, shared with StartFreshGame.
    const BootWorldView w = SurveyBootWorld("LoadStorySave");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still before gameplay: register the save (once per campaign) and re-issue the open. The
    // campaign scope first, so a re-host never reuses the previous campaign's save object.
    ValidateCachedSaveForCampaign(slot, w.curWorld);
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

    // The slot loaded from disk once.
    if (!g_storySave) {
        std::wstring b(slot);
        R::FString fs = makeFStr(b);
        ParamFrame f(g_loadGameFn);
        f.SetRaw(L"SlotName", &fs, sizeof(fs));
        f.Set<int32_t>(L"UserIndex", 0);
        if (!Call(g_storyGsCdo, f)) { UE_LOGE("engine: LoadStorySave -- LoadGameFromSlot call failed"); return false; }
        g_storySave = f.Get<void*>(L"ReturnValue");
        if (!g_storySave) { UE_LOGW("engine: LoadStorySave -- slot '%ls' missing/empty", slot); return false; }
        g_storySaveIdx = R::InternalIndexOf(g_storySave);  // IsLiveByIndex guard for reuse
        g_storySaveSlot = slot;                            // campaign identity
        UE_LOGI("engine: LoadStorySave -- loaded save '%ls' = %p (idx %d)", slot, g_storySave, g_storySaveIdx);
        // The save's inventory arrays are present and the world is not yet built from them: the one
        // moment a coop client can substitute its per-player inventory. Fires once.
        FireSaveObjectReadyHook(g_storySave);
    }

    // Registered on the persistent GameInstance and flagged for the game to apply on BeginPlay;
    // re-asserted each retry (no disk) so it is fresh at the travel.
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

    // The game mode from the slot prefix before the travel, retried each poll until the widget is
    // loaded.
    ApplyGameModeFromSlot(gi, slot, forceGameMode);

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: LoadStorySave -- at preLoad/menu; (re)issuing '%ls' (save '%ls' registered)",
            openCmd.c_str(), slot);
    ExecuteConsoleCommand(openCmd.c_str());
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Content invalidation: the next poll reloads the slot from disk even mid-campaign, for a slot
// file that changed under the same name (a rejoin re-downloads the host's world into the same
// slot). Staleness across campaigns is handled by the campaign identity.
void ResetCachedSave() {
    if (g_storySave) UE_LOGI("engine: ResetCachedSave -- dropping cached save %p ('%ls')",
                             g_storySave, g_storySaveSlot.c_str());
    g_storySave = nullptr;
    g_storySaveIdx = -1;
    g_storySaveSlot.clear();
    g_campaignWorld = nullptr;
    g_campaignWorldIdx = -1;
    g_gameModeApplied = false;
}

void SetSaveObjectReadyHook(SaveObjectReadyHook hook) {
    // Idempotent: the inventory layer re-calls this every pump tick, so it acts and logs only when
    // the hook changes.
    if (g_saveObjectReadyHook == hook) return;
    g_saveObjectReadyHook = hook;
    UE_LOGI("engine: SaveObjectReadyHook %s", hook ? "armed" : "disarmed");
}

// The fresh New Game boot: as LoadStorySave, with a blank save object created in memory rather
// than a disk slot. A fresh client has only the level-default props, so the host's connect
// snapshot mirrors its whole world onto it with nothing to reconcile away. Polled like
// LoadStorySave. Game thread.
bool StartFreshGame(bool storyMode) {
    auto makeFStr = [](std::wstring& b) {
        R::FString fs{};
        fs.Data = b.data();
        fs.Num = static_cast<int32_t>(b.size()) + 1;
        fs.Max = fs.Num;
        return fs;
    };

    // The boot survey, shared with LoadStorySave.
    const BootWorldView w = SurveyBootWorld("StartFreshGame");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still before gameplay: create the blank save, register it and travel. The campaign scope
    // first, so a fresh boot after any prior load never reuses the old save object.
    ValidateCachedSaveForCampaign(kFreshSlotName, w.curWorld);
    if (!g_storyGsCdo) g_storyGsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gi = R::FindObjectByClass(P::name::GameInstanceClass);
    if (!g_storyGsCdo || !gi) {
        UE_LOGW("engine: StartFreshGame -- not up yet (cdo=%p gi=%p); retry", g_storyGsCdo, gi);
        return false;
    }
    void* gsCls = R::ClassOf(g_storyGsCdo);
    void* createFn = gsCls ? R::FindFunction(gsCls, L"CreateSaveGameObject") : nullptr;
    void* saveCls  = R::FindClass(L"saveSlot_C");
    if (!createFn || !saveCls) {
        UE_LOGW("engine: StartFreshGame -- CreateSaveGameObject=%p saveSlot_C=%p not resolved; retry", createFn, saveCls);
        return false;
    }
    if (!g_setSaveSlotFn) {
        if (void* gicls = R::ClassOf(gi)) g_setSaveSlotFn = R::FindFunction(gicls, P::name::SetSaveSlotObjectFn);
    }

    // The blank save created once per campaign, cached like the loaded one.
    if (!g_storySave) {
        ParamFrame f(createFn);
        f.Set<void*>(L"SaveGameClass", saveCls);
        if (!Call(g_storyGsCdo, f)) { UE_LOGE("engine: StartFreshGame -- CreateSaveGameObject call failed"); return false; }
        g_storySave = f.Get<void*>(L"ReturnValue");
        if (!g_storySave) { UE_LOGW("engine: StartFreshGame -- CreateSaveGameObject returned null"); return false; }
        g_storySaveIdx = R::InternalIndexOf(g_storySave);  // IsLiveByIndex guard for reuse
        g_storySaveSlot = kFreshSlotName;                  // campaign identity
        UE_LOGI("engine: StartFreshGame -- created BLANK saveSlot_C = %p (idx %d, fresh New Game baseline)",
                g_storySave, g_storySaveIdx);
        // A fresh client join still gets its per-player inventory applied onto the blank save
        // before loadObjects materialises it.
        FireSaveObjectReadyHook(g_storySave);
    }

    // The blank save registered under the pseudo-slot name.
    const wchar_t* freshSlot = kFreshSlotName;
    if (g_setSaveSlotFn) {
        std::wstring b(freshSlot);
        R::FString fs = makeFStr(b);
        ParamFrame f(g_setSaveSlotFn);
        f.Set<void*>(L"save_gameInst", g_storySave);
        f.SetRaw(L"SlotName", &fs, sizeof(fs));
        Call(gi, f);
    } else {
        UE_LOGW("engine: StartFreshGame -- setSaveSlotObject unresolved");
    }
    // A blank save has empty object and trigger arrays, so restoring it yields the level defaults
    // through the same load path as a real save.
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(gi) + P::off::mainGameInstance_loadObjects) = 1;
    // The game mode through the prefix logic: a story or sandbox pseudo-slot name.
    ApplyGameModeFromSlot(gi, storyMode ? L"s_coopFresh" : L"b_coopFresh");

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: StartFreshGame -- at preLoad/menu; issuing '%ls' (BLANK save registered, mode=%s)",
            openCmd.c_str(), storyMode ? "story" : "sandbox");
    ExecuteConsoleCommand(openCmd.c_str());
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Travel to the main menu through the game's own level-travel verb, mainGamemode_C::transition
// with the full package path (the short name does not resolve). A direct gamemode call, so a
// dead or ragdolling player can travel with no pause menu; with the ProcessEvent detour in
// bypass for the duration it reaches the menu and tears the world down without our layer
// hanging the teardown. Game thread only.
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
        // StringToFName failed and returned None; a None level name is not passed to transition.
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

}  // namespace ue_wrap::engine
