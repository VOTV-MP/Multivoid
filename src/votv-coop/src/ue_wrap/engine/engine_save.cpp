// ue_wrap/engine_save.cpp -- story/sandbox save LOAD + GameMode derivation + the
// campaign-scoped save cache + fresh New-Game + return-to-main-menu. Split out of
// engine.cpp (2026-07-07 modularization Tier C -- engine.cpp was the residual
// catch-all beside its 10 engine_*.cpp siblings). Public API is declared in
// ue_wrap/engine.h; all functions here are ue_wrap::engine and game-thread only.
//
// The load path is a boot-poll: LoadStorySave / StartFreshGame are retried until the
// save class + world are live. The cache is CAMPAIGN-scoped (slot + polling world),
// GC-guarded by IsLiveByIndex, to prevent the re-host dangling-save crash (see the
// long comment on g_storySave below + research/crash_2026-07-03_rehost_wispkill/).

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
// Cached across the harness's boot retry loop (LoadStorySave is polled until we're
// in gameplay). g_storyGsCdo/g_loadGameFn are NATIVE (GameplayStatics CDO + its
// UFunction -- rooted, never move); g_setSaveSlotFn belongs to mainGameInstance_C,
// whose class lives as long as the persistent GameInstance instance.
void* g_storyGsCdo = nullptr;
void* g_loadGameFn = nullptr;
void* g_setSaveSlotFn = nullptr;

// The cached save object is CAMPAIGN-scoped, NOT process-scoped. A "campaign" is
// one continuous LoadStorySave/StartFreshGame poll sequence targeting one slot
// (menu -> open untitled_1 -> gameplay). Within a campaign the cache is protected
// from GC by the gameInstance.saveSlotObject UPROPERTY we register it into; the
// moment the campaign ends the game owns/replaces that reference and the object
// can be GC-purged at any world transition.
//
// ROOT-CAUSED CRASH (2026-07-03 + 2026-07-04, research/crash_2026-07-03_rehost_wispkill/):
// this cache used to be process-scoped. A RE-HOST (second story load in one
// process) reused the FIRST session's purged save object -- setSaveSlotObject
// then planted a dangling UObject* into the GameInstance UPROPERTY; the world
// built from freed memory (per-frame absorbed BP-VM AVs) and the GC mark phase
// AV'd on the garbage InternalIndex from a TaskGraph worker (fatal, identical
// stack both days, faulting slot = gameInstance+0x1A8). Hence: campaign identity
// (slot + polling-world) forces a full disk reload on every new campaign
// (fresh pointer AND fresh content -- an autosave may have rewritten the slot),
// and IsLiveByIndex guards the reuse WITHIN a campaign.
void* g_storySave = nullptr;          // cached USaveGame* (one disk load per campaign)
int32_t g_storySaveIdx = -1;          // its GUObjectArray index (IsLiveByIndex guard)
std::wstring g_storySaveSlot;         // campaign identity axis 1: the target slot
// Campaign identity axis 2: the non-gameplay WORLD the campaign polls in. Every
// return to the menu creates a NEW menu-world object, so "the polling world
// changed" == "a world round-trip happened since this cache was built" == new
// campaign -- regardless of whether any poll observed the gameplay in between
// (a poll-side "done" latch would miss a session that reached gameplay after
// the boot-poll timeout, or a native-menu load our polls never saw).
void* g_campaignWorld = nullptr;
int32_t g_campaignWorldIdx = -1;

// StartFreshGame's pseudo-slot: the blank save is registered under this name and
// it doubles as that path's campaign identity in the shared cache.
constexpr const wchar_t* kFreshSlotName = L"coop_client_fresh";

// Inventory Inc 4: the coop inventory layer registers this to overwrite a freshly
// loaded/created saveSlot's player inventory BEFORE the native loadObjects() materialize.
SaveObjectReadyHook g_saveObjectReadyHook = nullptr;
// Fire the hook ONCE per loaded/created save object (guard against the boot poll re-firing it,
// though the load/create blocks already run once). The hook self-gates to a no-op off a join.
void FireSaveObjectReadyHook(void* saveObj) {
    if (!g_saveObjectReadyHook || !saveObj) return;
    UE_LOGI("engine: firing SaveObjectReadyHook on save object %p (pre-materialize)", saveObj);
    g_saveObjectReadyHook(saveObj);
}

// Story/sandbox GameMode fix (2026-06-03). VOTV stores a save's game mode ONLY in
// the slot-name PREFIX: getSavePrefix(mode) (on Uui_saveSlots_C) yields the prefix
// VOTV uses for that mode, and a slot whose name starts with that prefix IS that
// mode. The menu sets mainGameInstance.GameMode @0x01E1 from this on load; our
// LoadStorySave bypass did NOT, so a story save loaded in the default (sandbox)
// mode (user-flagged 2026-06-02). Resolved once, then re-used across the boot poll.
void* g_saveSlotsUiCdo  = nullptr;  // Uui_saveSlots_C CDO (getSavePrefix is a pure mode->prefix map)
int32_t g_saveSlotsUiCdoIdx = -1;   // its GUObjectArray index (BP CDO -- can be GC'd with its class)
void* g_getSavePrefixFn = nullptr;  // Uui_saveSlots_C::getSavePrefix(enum_gamemode) -> FString prefix
bool  g_gameModeApplied = false;    // latch: derive + write GameMode once per load campaign
constexpr uint8_t kEnumGamemodeCount = 8;  // enum_gamemode::enum_MAX (enum_gamemode_enums.hpp)

// Resolve the Uui_saveSlots_C CDO + its getSavePrefix UFunction (cached). The widget
// loads on the first menu / gameplay-level transition; before that this returns false
// and the caller retries. getSavePrefix is a pure mode->prefix map (no side effects),
// so the CDO is a valid call target (no live instance needed).
bool ResolveSavePrefixFn() {
    // ui_saveSlots_C is a BP class: it (and its CDO, and its UFunctions) can be
    // GC'd with the menu world. A later campaign (re-host) must re-resolve the
    // reloaded class instead of calling into freed memory.
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

// Derive the slot's game mode from its name prefix (exactly as VOTV's menu does -- NO
// hardcoded enum value, correct for story AND sandbox AND any future mode) and write
// mainGameInstance.GameMode @0x01E1. Shares the prefix source with the save browser via
// DeriveModeFromSlot (RULE 2). Retried each boot poll until Uui_saveSlots_C is loaded;
// runs once then latches. Game thread only.
void ApplyGameModeFromSlot(void* gi, const wchar_t* slot, int forceGameMode = -1) {
    if (g_gameModeApplied || !gi || !slot) return;
    // v56 save-transfer: the coop slot is `zcoop_<pid>` -- a prefix the game's
    // mode map can't match. The wire carried the HOST's mode
    // (SaveTransferBeginPayload.gameMode); write it directly, no derive.
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
        // Menu save-slots widget not loaded yet at this boot stage. Retry next poll
        // (do NOT latch). If it never loads, the warning persists + GameMode stays
        // as-is (current behaviour) -- a visible signal to pivot.
        UE_LOGW("engine: ApplyGameModeFromSlot -- ui_saveSlots_C cdo=%p getSavePrefix=%p not loaded yet; "
                "GameMode left as-is (will retry)", g_saveSlotsUiCdo, g_getSavePrefixFn);
        return;
    }
    const int bestMode = DeriveModeFromSlot(slot);
    // The widget is loaded + getSavePrefix is deterministic -> this is our one shot;
    // latch regardless of match (re-running would give the identical result).
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

// Enforce the campaign scope of the save cache (see the block comment above
// g_storySave). Called at the top of the boot phase of LoadStorySave /
// StartFreshGame with the target slot + the current (non-gameplay) world. ONE
// owner for the whole cache-lifetime axis -- callers never touch the cache
// fields themselves. A new campaign (the polling world changed = a world
// round-trip happened, or a different target slot) gets a FULL reset: reload
// from disk = fresh pointer AND fresh content (an autosave may have rewritten
// the same slot name), plus a fresh GameMode derive. Within a campaign,
// IsLiveByIndex catches a GC purge between polls (drop just the object; the
// campaign's GameMode latch stays -- the persistent GameInstance already
// carries the applied byte).
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
// ---------------------------------------------------------------------------
// REJOIN FIX (2026-08-28) -- the boot poll's two "where are we?" reads, ONE owner
// for LoadStorySave + StartFreshGame (they were two verbatim copies of the same
// unguarded reads). Fixes the SirWilliam field row ("rejoining requires a full
// game relaunch", docs/CREDITS.md).
//
// AUTHOR: archhn0madd (github.com/archhn0madd/Multifoid, commit feae7730),
// adopted 2026-08-31. Only the doc pointer above was retargeted -- this tree
// keeps field reports in CREDITS.md, not in a FIELD_REPORTS.md.
//
// THE BUG. After a death-flee (or any quit-to-menu), the DYING gameplay world and
// its ragdolled mainPlayer_C corpse stay in GUObjectArray until the GC purge --
// measured 44+ s ([[lesson-dying-world-actors-not-killflagged-at-menu]]); the kill
// FLAGS can lag GC too (LESSONS 2026-08-25: 311 dying-world piles "passed IsLive's
// kill-flag check" AT the menu). Both old checks then answered from the dead world:
//   (a) FindObjectByClass(mainPlayer_C) found the CORPSE, non-origin position
//       -> "in gameplay" while the process sat at the MENU. The join "booted" into
//       a world that was never loaded: ClientWorldReady never announced (its
//       Registry::Local() gate is world-stamped and correctly answers null there),
//       the host never streamed, and the only way out was a relaunch.
//   (b) FindObjectByClass(World) returned the dying gameplay world (lower
//       GUObjectArray index than the menu world), name still contains "ntitled"
//       -> "already loading, wait" -> the `open` was never issued.
//   The same two lies also stranded a HOST at the menu after a death-flee: its
//   re-host polls could neither see the old world gone nor issue the new open.
//
// THE FIX -- the 2026-08-23 world-identity lesson verbatim ("world gates must key
// on WORLD IDENTITY / travel-start signals, never per-object liveness"):
//   - the current world comes from world_identity (GameInstance -> LocalPlayers[0]
//     -> PlayerController -> OwningWorld), which a dying world cannot hold alive;
//   - the pawn must be LIVE (fresh-pointer guard BEFORE the WorldOf deref) AND
//     WorldOf(lp) == the current world -- both terms, per the lesson's "carry both";
//   - "gameplay loading?" keys on CurrentWorldKind(): Gameplay -> wait; Other
//     (menu/preLoad) -> boot; Unknown (the ~1 s mid-travel window, or a Degraded
//     chain) -> the legacy first-World scan = the pre-fix behaviour inside exactly
//     the one window where the chain cannot answer;
//   - Degraded() keeps the legacy path (fail-open): a recook that broke the chain
//     must not additionally break the boot.
struct BootWorldView {
    bool inGameplay;       // (a): a LIVE mainPlayer_C of the CURRENT world, off-origin
    bool gameplayLoading;  // (b): a gameplay world is up/loading -> never re-open, wait
    void* curWorld;        // the world phase (c) stamps as the campaign's polling world
};
BootWorldView SurveyBootWorld(const char* who) {
    BootWorldView v{false, false, nullptr};
    const bool readerUp = !ue_wrap::world_identity::Degraded();
    v.curWorld = readerUp ? ue_wrap::world_identity::CurrentWorld() : nullptr;

    // (a) Already in gameplay? mainPlayer_C placed in the real level (non-origin).
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
            // A live current-world pawn parked at origin = pre-placement; fall
            // through to (b) exactly like the pre-fix code did.
        } else {
            // Throttle: the boot poll re-runs every ~1.5 s and hits the SAME corpse
            // every time -- first 3 + a periodic heartbeat, not one line per poll.
            //
            // Keyed on the corpse's IDENTITY, not on a bare count (post-ship audit,
            // 2026-08-31). This static is ONE instance shared by both callers and is
            // out of ResetCachedSave's scope, so a plain counter would carry across
            // campaigns: the SECOND stale corpse in one process would lose its
            // opening burst and log only every 16th skip -- muting exactly the field
            // diagnosability this fix exists to provide, in the rejoin case, which is
            // by definition the second world of the process.
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
    // (b) Gameplay map already loading? The gameplay world is "Untitled" (map
    // untitled_1); preLoad/menu are other worlds. If we're in/loading it, DON'T
    // re-open -- just wait for the player to spawn.
    const ue_wrap::world_identity::WorldKind kind =
        readerUp ? ue_wrap::world_identity::CurrentWorldKind()
                 : ue_wrap::world_identity::WorldKind::Unknown;
    if (kind == ue_wrap::world_identity::WorldKind::Gameplay) {
        v.gameplayLoading = true;
    } else if (kind == ue_wrap::world_identity::WorldKind::Unknown) {
        // Mid-travel (or Degraded): the chain cannot answer -- legacy reader, kept
        // verbatim. In the travel window the dying gameplay world is the CORRECT
        // "wait" answer.
        v.curWorld = R::FindObjectByClass(P::name::WorldClass);
        if (v.curWorld &&
            R::ToString(R::NameOf(v.curWorld)).find(L"ntitled") != std::wstring::npos)
            v.gameplayLoading = true;
    }
    // kind == Other (menu/preLoad): fall through -- the open must be (re)issued.
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
        // Longest matching prefix wins (defends against one prefix being a prefix of
        // another, e.g. "" matching everything).
        if (!pre.empty() && slotStr.rfind(pre, 0) == 0 && pre.size() > bestLen) {
            bestLen  = pre.size();
            bestMode = static_cast<int>(mode);
        }
    }
    return bestMode;
}

// Called repeatedly by the harness boot loop. Returns true ONLY once a mainPlayer_C
// is in the real level (non-origin) -- i.e. we've reached story gameplay. While
// still at preLoad / the OMEGA WARNING / the menu it (re)issues `open untitled_1`
// each call; the user confirmed `open` travels straight to gameplay from the OMEGA
// screen (Proceed only loads preLoad, which we DON'T want). A single early open
// fired during preLoad is silently dropped, hence the retry. It will NOT re-open
// once the gameplay world is already loading (that would restart the load).
bool LoadStorySave(const wchar_t* slot, int forceGameMode) {
    if (!slot || !*slot) return false;

    // (a)+(b): the world-aware boot survey -- ONE owner shared with StartFreshGame
    // (see SurveyBootWorld above for the rejoin root cause it fixes).
    const BootWorldView w = SurveyBootWorld("LoadStorySave");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still at preLoad / OMEGA / menu: register the save (once per campaign)
    // + (re)issue open. Campaign scope FIRST -- a re-host (second load in one
    // process) must never reuse the previous campaign's save object.
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
        g_storySaveIdx = R::InternalIndexOf(g_storySave);  // IsLiveByIndex guard for reuse
        g_storySaveSlot = slot;                            // campaign identity
        UE_LOGI("engine: LoadStorySave -- loaded save '%ls' = %p (idx %d)", slot, g_storySave, g_storySaveIdx);
        // Inc 4: the save's inventory arrays are now present but the world has NOT been built
        // from them yet -- the one moment a coop client can substitute its per-player inventory
        // (no-op off a join). Fires once (this block runs once; g_storySave then stays cached).
        FireSaveObjectReadyHook(g_storySave);
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
    ApplyGameModeFromSlot(gi, slot, forceGameMode);

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: LoadStorySave -- at preLoad/menu; (re)issuing '%ls' (save '%ls' registered)",
            openCmd.c_str(), slot);
    ExecuteConsoleCommand(openCmd.c_str());
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Content invalidation: force the next LoadStorySave/StartFreshGame poll to
// reload the slot from disk even MID-campaign. Needed when the slot FILE
// changed under the same name (v56 rejoin: save_transfer re-downloads the
// host's world into the same zcoop_<pid> slot). Lifetime staleness across
// campaigns is handled automatically by ValidateCachedSaveForCampaign -- this
// API exists solely for the disk-content case the campaign identity can't see.
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
    // Idempotent: player_inventory_sync::Install re-calls this EVERY net-pump tick (the standard
    // install-retry pattern), so logging on every call spammed the log at >10 Hz. Only act + log
    // when the hook actually changes.
    if (g_saveObjectReadyHook == hook) return;
    g_saveObjectReadyHook = hook;
    UE_LOGI("engine: SaveObjectReadyHook %s", hook ? "armed" : "disarmed");
}

// FRESH New-Game boot: identical to LoadStorySave but with a BLANK saveSlot
// (GameplayStatics::CreateSaveGameObject(saveSlot_C)) instead of a disk slot -> drops into
// a fresh New Game in untitled_1. This is the deterministic baseline for the ephemeral-client
// world snapshot (project-ephemeral-client-host-authoritative-world): a fresh client has only
// the level-default props, so the host's existing prop/trigger connect-snapshot mirrors the
// host's whole world onto it cleanly, with NO reconcile-remove (no client dynamic props to dedup).
// Polled like LoadStorySave (returns true once in gameplay). Game thread.
// NOTE (2026-06-04): this is the EMPIRICAL test of whether a fresh story New Game drops straight
// into gameplay or stalls on a day-0 intro -- run via the `fresh_boot` ini gate + read the log.
bool StartFreshGame(bool storyMode) {
    auto makeFStr = [](std::wstring& b) {
        R::FString fs{};
        fs.Data = b.data();
        fs.Num = static_cast<int32_t>(b.size()) + 1;
        fs.Max = fs.Num;
        return fs;
    };

    // (a)+(b): the world-aware boot survey -- ONE owner shared with LoadStorySave
    // (see SurveyBootWorld above for the rejoin root cause it fixes).
    const BootWorldView w = SurveyBootWorld("StartFreshGame");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    // (c) Still at preLoad / menu: create a blank saveSlot + register + travel.
    // Campaign scope FIRST (shared cache with LoadStorySave): a fresh-boot
    // campaign after any prior load must never reuse the old save object.
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

    // Create the blank saveSlot ONCE per campaign (cached in g_storySave like the loaded path).
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
        // Inc 4: a fresh client join (no host save) still gets its per-player inventory applied
        // onto the BLANK save here, before loadObjects() materializes it (no-op off a join).
        FireSaveObjectReadyHook(g_storySave);
    }

    // Register the blank save under a temp slot name. (Persistence suppression is a later
    // increment; for the isolated test nothing writes it back.)
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
    // A blank save has empty objectsData/triggers -> "restoring" it yields the level defaults
    // (a fresh New Game). loadObjects=1 runs the same load path as a real save.
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(gi) + P::off::mainGameInstance_loadObjects) = 1;
    // GameMode: drive it via the existing prefix logic -- 's_' => story (the coop target),
    // 'b_' => sandbox/default. (Later: driven by the host-sent GameMode on the handshake.)
    ApplyGameModeFromSlot(gi, storyMode ? L"s_coopFresh" : L"b_coopFresh");

    std::wstring openCmd = L"open ";
    openCmd += P::name::GameplayLevel;
    UE_LOGI("engine: StartFreshGame -- at preLoad/menu; issuing '%ls' (BLANK save registered, mode=%s)",
            openCmd.c_str(), storyMode ? "story" : "sandbox");
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

}  // namespace ue_wrap::engine
