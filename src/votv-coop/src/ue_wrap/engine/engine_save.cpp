// ue_wrap/engine/engine_save.cpp -- the save load, the campaign-scoped save cache, the fresh New
// Game boot and the return to the main menu. Declared in ue_wrap/engine/engine_save.h, which the
// engine.h umbrella includes; everything here is game-thread only. The load path is a boot poll:
// LoadStorySave and StartFreshGame are retried until the save classes and the world are live.
//
// Both boots do what the game's slot menu does on its way into a world (ui_saveSlots_C's Apply,
// then tryLaunch and lib_C::loadLevel), since neither passes through that menu: register the save
// objects on the GameInstance, put the save's day, rules and mode in force, name the level, set
// loadObjectsFromSlot, and open the level.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/save_folder.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap::engine {

namespace P = profile;
namespace R = reflection;

namespace {

// One live UObject the cache holds, with its GUObjectArray index for the liveness guard.
struct Held {
    void* obj = nullptr;
    int32_t idx = -1;
    void Set(void* o) { obj = o; idx = o ? R::InternalIndexOf(o) : -1; }
    bool Live() const { return obj && R::IsLiveByIndex(obj, idx); }
    void Clear() { obj = nullptr; idx = -1; }
};

// The campaign's save objects: a campaign is one continuous poll sequence targeting one save.
// Within it the GameInstance properties setSaveSlotObject writes keep them alive; once it ends the
// game may replace those references and the objects can be purged at any world transition. So a
// new campaign reloads from disk, and the liveness guard covers the reuse within one. Caching
// across campaigns once planted a dangling pointer into the GameInstance and the world was rebuilt
// from freed memory.
Held g_save;        // saveSlot_C: main.sav, or the area's slot once createAreaSlot swapped it in
Held g_persistent;  // save_persistent_C: persistent.sav; null for a fresh game (the gamemode makes one)
Held g_metadata;    // save_metadata_C: main.meta
std::wstring g_campaignSave;    // campaign identity axis 1: the save folder name (or the fresh name)
std::wstring g_loadedSlotName;  // what setSaveSlotObject is told is loaded: "main", the area, or ""
// The second axis: the non-gameplay world the campaign polls in. Every return to the menu
// creates a new menu world, so a changed polling world means a world round-trip happened since
// the cache was built, whether or not any poll saw the gameplay in between.
void* g_campaignWorld = nullptr;
int32_t g_campaignWorldIdx = -1;
bool g_areaSwapped = false;  // createAreaSlot ran for this campaign

// StartFreshGame's campaign identity. No folder carries it, and setSaveSlotObject is told no slot
// is loaded, so the game's own save (saveSlot_C::save returns on an empty loadedSlotName) never
// writes this world anywhere.
constexpr const wchar_t* kFreshCampaign = L"<fresh>";

// The coop inventory layer registers this to overwrite a freshly loaded or created save's
// player inventory before the native loadObjects materialises it.
SaveObjectReadyHook g_saveObjectReadyHook = nullptr;
void FireSaveObjectReadyHook(void* saveObj) {
    if (!g_saveObjectReadyHook || !saveObj) return;
    UE_LOGI("engine: firing SaveObjectReadyHook on save object %p (pre-materialize)", saveObj);
    g_saveObjectReadyHook(saveObj);
}

std::vector<std::pair<std::string, std::string>> g_freshGameRules;  // read on the game thread only

std::wstring ReadFString(void* obj, const wchar_t* prop) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    if (off < 0) return {};
    const R::FString& s = *reinterpret_cast<const R::FString*>(reinterpret_cast<uint8_t*>(obj) + off);
    return (s.Data && s.Num > 1) ? std::wstring(s.Data, s.Data + (s.Num - 1)) : std::wstring();
}

// An FName property: its address, or null when the class does not declare it.
R::FName* FNameField(void* obj, const wchar_t* prop) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    return off >= 0 ? reinterpret_cast<R::FName*>(reinterpret_cast<uint8_t*>(obj) + off) : nullptr;
}

// An FString property set to `value` when it holds something else, the engine minting the new
// buffer and freeing the old one, as the game's own assignment does. True when it was rewritten.
bool AssignFString(void* obj, const wchar_t* prop, const std::wstring& value) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    if (off < 0) return false;
    R::FString* f = reinterpret_cast<R::FString*>(reinterpret_cast<uint8_t*>(obj) + off);
    if (ReadFString(obj, prop) == value) return false;
    void* old = f->Data;
    *f = R::FString{};
    if (!ue_wrap::fstring_utils::MintFString(value, f)) {
        UE_LOGW("engine: %ls could not be rewritten", prop);
        return false;
    }
    if (old) R::EngineFree(old);
    return true;
}

// The three objects of a folder save name the folder they write back to: saveSlot_C.savePath (the
// folder's path), save_persistent_C.pathToFile (the same), save_metadata_C.saveSlot (the folder's
// name) with metadataName "main". The game sets them when it creates the folder; a folder that was
// renamed or copied -- a joiner's coop folder carries the host's -- still names the old one, and the
// game's own saves would write there. They are pointed at the folder they were loaded from.
void PointAtFolder(void* save, void* persistent, void* metadata, const std::wstring& folder,
                   const std::wstring& path) {
    int rewritten = 0;
    rewritten += AssignFString(save, L"savePath", path) ? 1 : 0;
    rewritten += AssignFString(persistent, L"pathToFile", path) ? 1 : 0;
    rewritten += AssignFString(metadata, L"saveSlot", folder) ? 1 : 0;
    rewritten += AssignFString(metadata, L"metadataName", L"main") ? 1 : 0;
    if (rewritten > 0)
        UE_LOGI("engine: LoadStorySave -- %d save path field(s) pointed at '%ls'", rewritten, path.c_str());
}

void ResetCampaign() {
    g_save.Clear();
    g_persistent.Clear();
    g_metadata.Clear();
    g_campaignSave.clear();
    g_loadedSlotName.clear();
    g_campaignWorld = nullptr;
    g_campaignWorldIdx = -1;
    g_areaSwapped = false;
}

// The campaign scope of the cache, one owner: called at the top of the boot phase with the target
// and the current non-gameplay world. A changed polling world or target is a new campaign and
// resets everything, reloading from disk since an autosave may have rewritten the folder. Within a
// campaign a purge between polls drops the objects and the next poll reloads them.
void ValidateCachedSaveForCampaign(const std::wstring& target, void* curWorld) {
    const bool worldChanged =
        g_campaignWorld &&
        (g_campaignWorld != curWorld || !R::IsLiveByIndex(g_campaignWorld, g_campaignWorldIdx));
    const bool targetChanged = !g_campaignSave.empty() && g_campaignSave != target;
    if (worldChanged || targetChanged) {
        UE_LOGI("engine: save cache -- NEW load campaign (target '%ls', cached '%ls'%s%s) -> full reset",
                target.c_str(), g_campaignSave.c_str(), worldChanged ? ", polling world changed" : "",
                targetChanged ? ", target changed" : "");
        ResetCampaign();
    }
    if (!g_campaignWorld && curWorld) {
        g_campaignWorld = curWorld;
        g_campaignWorldIdx = R::InternalIndexOf(curWorld);
    }
    const bool lost = (g_save.obj && !g_save.Live()) || (g_persistent.obj && !g_persistent.Live()) ||
                      (g_metadata.obj && !g_metadata.Live());
    if (lost) {
        UE_LOGW("engine: save cache -- a cached save object of '%ls' was GC-purged mid-campaign; reloading",
                g_campaignSave.c_str());
        g_save.Clear();
        g_persistent.Clear();
        g_metadata.Clear();
        g_areaSwapped = false;
    }
}

// The boot poll's two "where are we?" reads, one owner for LoadStorySave and StartFreshGame.
// Adopted from archhn0madd's Multifoid fork; see docs/credits.md.
// After a quit to the menu the dying gameplay world and its player corpse stay in GUObjectArray for
// tens of seconds and their kill flags can lag, so a class search found the corpse off the origin
// and answered "in gameplay" while the process sat at the menu. This keys on world identity
// instead: the current world comes from the GameInstance's local player chain, which a dying world
// cannot hold alive, and the pawn must be live and belong to it. The first-World scan answers only
// the mid-travel window, where that chain cannot.
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
            FVector p{};
            if (TryGetActorLocation(lp, p) && std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) > 100.f) {
                UE_LOGI("engine: %s -- in gameplay (mainPlayer @ %.0f,%.0f,%.0f)", who, p.X, p.Y, p.Z);
                v.inGameplay = true;
                return v;
            }
            // A live current-world pawn at the origin, or one whose location cannot be read, is not
            // yet placed: fall through to (b).
        } else {
            // Throttled: the poll re-runs every second or so and hits the same corpse every time,
            // so the first three and then a periodic heartbeat, keyed on the corpse's identity so a
            // second stale corpse in one process is reported too.
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
                        who, lp, pawnLive ? 1 : 0, pawnWorld, v.curWorld, readerUp ? 1 : 0, sStaleSkips);
            }
        }
    }
    // (b) The gameplay map already up or loading: never re-open, just wait for the player to spawn.
    const ue_wrap::world_identity::WorldKind kind =
        readerUp ? ue_wrap::world_identity::CurrentWorldKind() : ue_wrap::world_identity::WorldKind::Unknown;
    if (kind == ue_wrap::world_identity::WorldKind::Gameplay) {
        v.gameplayLoading = true;
    } else if (kind == ue_wrap::world_identity::WorldKind::Unknown) {
        // Mid-travel, or a degraded chain: the first-World reader, where the dying gameplay world is
        // the right "wait" answer.
        v.curWorld = R::FindObjectByClass(P::name::WorldClass);
        if (v.curWorld && R::ToString(R::NameOf(v.curWorld)).find(L"ntitled") != std::wstring::npos)
            v.gameplayLoading = true;
    }
    // The menu or preLoad: fall through, and the open is issued.
    return v;
}

// mainGameInstance_C::setSaveSlotObject(save, slotName, folderSlotName, metadata, persistent): the
// five references the gamemode's BeginPlay and the game's own saves read.
void RegisterSave(void* gi, void* save, const std::wstring& slotName, const std::wstring& folder,
                  void* metadata, void* persistent, const char* who) {
    void* fn = R::FindFunction(R::ClassOf(gi), P::name::SetSaveSlotObjectFn);
    if (!fn) {
        UE_LOGW("engine: %s -- setSaveSlotObject unresolved", who);
        return;
    }
    std::wstring a = slotName, b = folder;
    R::FString fa{a.data(), static_cast<int32_t>(a.size()) + 1, static_cast<int32_t>(a.size()) + 1};
    R::FString fb{b.data(), static_cast<int32_t>(b.size()) + 1, static_cast<int32_t>(b.size()) + 1};
    ParamFrame f(fn);
    f.Set<void*>(L"save_gameInst", save);
    f.SetRaw(L"SlotName", &fa, sizeof(fa));
    f.SetRaw(L"folderSlotName", &fb, sizeof(fb));
    f.Set<void*>(L"metadataObject", metadata);
    f.Set<void*>(L"save_persistent", persistent);
    Call(gi, f);
}

// The save's day handed to the gamemode, as tryLaunch does: mainGameInstance.startDay =
// saveSlot.savedtime.Z. The gamemode reads it once on its BeginPlay, marks every scheduled event
// dated before that day as passed WITHOUT running it, sets its clock's day, and zeroes the field.
// Re-asserted on every poll (the boots stop polling once the world is loading, before BeginPlay).
void PutSaveStartDayInForce(void* gi, void* save, const char* who) {
    static void* sLastSave = nullptr;
    void* giClass = R::ClassOf(gi);
    void* saveClass = (save && R::IsLive(save)) ? R::ClassOf(save) : nullptr;
    const int32_t dayOff = giClass ? R::FindPropertyOffset(giClass, L"startDay") : -1;
    const int32_t timeOff = saveClass ? R::FindPropertyOffset(saveClass, L"savedtime") : -1;
    if (dayOff < 0 || timeOff < 0) {
        if (save != sLastSave) UE_LOGW("engine: %s -- startDay / savedtime unresolved; the save's day was not handed over", who);
        sLastSave = save;
        return;
    }
    // savedtime is an FIntVector; Z is the day.
    const int32_t day = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(save) + timeOff + 8);
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(gi) + dayOff) = day;
    if (save != sLastSave) UE_LOGI("engine: %s -- the save's day %d handed to the gamemode (startDay)", who, day);
    sLastSave = save;
}

// The metadata's world rules put in force on the GameInstance, which is where the game reads nearly
// all of them; logged when a metadata object is first seen and whenever the copy changed something.
void PutSaveRulesInForce(void* gi, void* metadata, const char* who) {
    static void* sLast = nullptr;
    const int changed = ue_wrap::game_rules::ApplySavedToProcess(gi, metadata);
    if (changed < 0) {
        UE_LOGW("engine: %s -- the save's world rules could not be put in force (struct unresolved)", who);
    } else if (changed > 0 || metadata != sLast) {
        UE_LOGI("engine: %s -- the save's world rules are in force (%d rule(s) changed on the GameInstance)",
                who, changed);
    }
    sLast = metadata;
}

// The mode: named outright, else the metadata's gamemode_type (the byte the game's slot list
// filters on). Re-asserted each poll; logged when it changes.
void PutModeInForce(void* gi, void* metadata, int forceGameMode, const char* who) {
    int mode = forceGameMode;
    if (!ue_wrap::game_mode::IsValid(mode) && metadata) {
        void* cls = R::ClassOf(metadata);
        const int32_t off = cls ? R::FindPropertyOffset(cls, L"gamemode_type") : -1;
        if (off >= 0) mode = *(reinterpret_cast<uint8_t*>(metadata) + off);
    }
    if (!ue_wrap::game_mode::IsValid(mode)) {
        UE_LOGW("engine: %s -- no game mode named or saved; GameMode stays %d", who,
                ue_wrap::game_mode::ReadFrom(gi));
        return;
    }
    const int old = ue_wrap::game_mode::WriteTo(gi, mode);
    if (old != mode)
        UE_LOGI("engine: %s -- GameMode=%d %s (was %d)", who, mode,
                ue_wrap::game_mode::NameOrOrdinal(mode).c_str(), old);
}

// lib_C::loadLevel's writes, then the travel: currentLevel on the GameInstance and the metadata,
// baseLevel for a save that starts on its starter map, newVar_1 = the option tryLaunch passes (the
// gamemode's BeginPlay quits the game on a level outside its built-in list without it), the
// loadObjectsFromSlot flag tryLaunch raises, and `open`, re-issued each poll since an open fired
// during preLoad is dropped.
void TravelTo(void* gi, void* metadata, const std::wstring& level, bool isBaseLevel, const char* who) {
    const R::FName lvl = ue_wrap::fname_utils::StringToFName(level);
    if (R::FName* f = FNameField(gi, L"currentLevel")) *f = lvl;
    if (isBaseLevel)
        if (R::FName* f = FNameField(gi, L"baseLevel")) *f = lvl;
    if (R::FName* f = FNameField(metadata, L"currentLevel")) *f = lvl;
    void* giClass = R::ClassOf(gi);
    const int32_t optOff = giClass ? R::FindPropertyOffset(giClass, L"NewVar_1") : -1;
    if (optOff >= 0) {
        R::FString* opt = reinterpret_cast<R::FString*>(reinterpret_cast<uint8_t*>(gi) + optOff);
        const std::wstring want = P::name::LoadLevelOption;
        if (!(opt->Data && opt->Num == static_cast<int32_t>(want.size()) + 1 &&
              std::wmemcmp(opt->Data, want.c_str(), want.size()) == 0)) {
            if (opt->Data) R::EngineFree(opt->Data);
            *opt = R::FString{};
            ue_wrap::fstring_utils::MintFString(want, opt);
        }
    }
    const int32_t flagOff = giClass ? R::FindPropertyOffset(giClass, L"loadObjectsFromSlot") : -1;
    if (flagOff >= 0) *(reinterpret_cast<uint8_t*>(gi) + flagOff) = 1;
    else UE_LOGW("engine: %s -- loadObjectsFromSlot unresolved; the save's objects will not load", who);

    const std::wstring openCmd = L"open " + level;
    UE_LOGI("engine: %s -- at preLoad/menu; (re)issuing '%ls'", who, openCmd.c_str());
    ExecuteConsoleCommand(openCmd.c_str());
}

// The pending new-game rules written into the blank metadata's rules; a rule that did not take is
// named, and the list is spent either way.
void WriteFreshGameRules(void* metadata) {
    namespace GR = ue_wrap::game_rules;
    int written = 0;
    for (const auto& kv : g_freshGameRules) {
        switch (GR::WriteSavedRule(metadata, kv.first, kv.second)) {
            case GR::WriteResult::Written: ++written; break;
            case GR::WriteResult::UnknownKey:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s' names no gameRules member", kv.first.c_str());
                break;
            case GR::WriteResult::BadValue:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s' cannot take '%s'", kv.first.c_str(),
                        kv.second.c_str());
                break;
            case GR::WriteResult::Unresolved:
                UE_LOGW("engine: StartFreshGame -- new-game rule '%s': the blank metadata's rules are unresolved",
                        kv.first.c_str());
                break;
        }
    }
    if (!g_freshGameRules.empty())
        UE_LOGI("engine: StartFreshGame -- %d of %zu new-game rule(s) written to the blank metadata", written,
                g_freshGameRules.size());
    g_freshGameRules.clear();
}

void* CreateBlank(const wchar_t* className) {
    void* gs = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* fn = gs ? R::FindFunction(R::ClassOf(gs), L"CreateSaveGameObject") : nullptr;
    void* cls = R::FindClass(className);
    if (!fn || !cls) return nullptr;
    ParamFrame f(fn);
    f.Set<void*>(L"SaveGameClass", cls);
    return Call(gs, f) ? f.Get<void*>(L"ReturnValue") : nullptr;
}

}  // namespace

void SetFreshGameRules(const std::vector<std::pair<std::string, std::string>>& rules) {
    g_freshGameRules = rules;
}

// Called repeatedly by the boot loop. True only once a mainPlayer_C is in the level; while still at
// preLoad, the warning screen or the menu it re-issues the open each call. It does not re-open once
// the gameplay world is loading, which would restart the load.
bool LoadStorySave(const wchar_t* folderName, int forceGameMode) {
    if (!folderName || !*folderName) return false;
    const BootWorldView w = SurveyBootWorld("LoadStorySave");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    const std::wstring folder(folderName);
    ValidateCachedSaveForCampaign(folder, w.curWorld);
    void* gi = world_singleton::GameInstance();
    if (!gi) {
        UE_LOGW("engine: LoadStorySave -- no GameInstance yet; retry");
        return false;
    }

    // The folder's three files, read from disk once per campaign, as ui_saveSlots_C's Apply reads
    // them: main.sav, persistent.sav, and main.meta (which the slot row loaded).
    if (!g_save.obj) {
        const std::wstring path = save_folder::FolderPath(folder);
        if (path.empty()) {
            UE_LOGW("engine: LoadStorySave -- the saved directory is unresolved; retry");
            return false;
        }
        void* save = save_folder::LoadCompressed(L"main", path, L".sav");
        void* persistent = save_folder::LoadCompressed(L"persistent", path, L".sav");
        void* metadata = save_folder::LoadCompressed(L"main", path, L".meta");
        if (!save || !persistent || !metadata) {
            UE_LOGW("engine: LoadStorySave -- save '%ls' incomplete at '%ls' (main.sav=%p persistent.sav=%p "
                    "main.meta=%p)", folderName, path.c_str(), save, persistent, metadata);
            return false;
        }
        PointAtFolder(save, persistent, metadata, folder, path);
        g_save.Set(save);
        g_persistent.Set(persistent);
        g_metadata.Set(metadata);
        g_campaignSave = folder;
        const std::wstring area = ReadFString(metadata, L"area");
        g_loadedSlotName = area.empty() ? L"main" : area;
        UE_LOGI("engine: LoadStorySave -- loaded save '%ls' from '%ls' (slot %p, persistent %p, metadata %p%s%ls)",
                folderName, path.c_str(), save, persistent, metadata, area.empty() ? "" : ", area ",
                area.c_str());
        // The save's inventory arrays are present and the world is not yet built from them: the one
        // moment a coop client can substitute its per-player inventory. Fires once, on the save the world
        // is built from -- main.sav, or the area's slot once createAreaSlot has swapped it in below.
        if (area.empty()) FireSaveObjectReadyHook(save);
    }

    // Registered on the persistent GameInstance and re-asserted each poll, since the menu's own
    // gamemode clears it on its BeginPlay.
    RegisterSave(gi, g_save.obj, g_loadedSlotName, folder, g_metadata.obj, g_persistent.obj, "LoadStorySave");
    const bool inArea = g_loadedSlotName != L"main";
    if (inArea && !g_areaSwapped) {
        // A save left inside an area: lib_C::createAreaSlot loads areas/<area>.sav and registers it
        // in place of main.sav; that object is what later polls re-register.
        void* lib = R::FindClassDefaultObject(L"lib_C");
        void* fn = lib ? R::FindFunction(R::ClassOf(lib), P::name::CreateAreaSlotFn) : nullptr;
        if (!fn) {
            UE_LOGW("engine: LoadStorySave -- lib_C::createAreaSlot unresolved; retry");
            return false;
        }
        std::wstring a = g_loadedSlotName;
        R::FString fa{a.data(), static_cast<int32_t>(a.size()) + 1, static_cast<int32_t>(a.size()) + 1};
        ParamFrame f(fn);
        f.SetRaw(L"areaName", &fa, sizeof(fa));
        f.Set<void*>(L"__WorldContext", gi);
        if (!Call(lib, f) || !f.Get<bool>(L"success")) {
            UE_LOGW("engine: LoadStorySave -- createAreaSlot('%ls') failed", a.c_str());
            return false;
        }
        void* giClass = R::ClassOf(gi);
        const int32_t off = giClass ? R::FindPropertyOffset(giClass, L"save_gameInst") : -1;
        if (off >= 0) g_save.Set(*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gi) + off));
        g_areaSwapped = true;
        UE_LOGI("engine: LoadStorySave -- area '%ls' slot %p swapped in", a.c_str(), g_save.obj);
        FireSaveObjectReadyHook(g_save.obj);
    }
    PutSaveStartDayInForce(gi, g_save.obj, "LoadStorySave");
    PutSaveRulesInForce(gi, g_metadata.obj, "LoadStorySave");
    PutModeInForce(gi, g_metadata.obj, forceGameMode, "LoadStorySave");

    // tryLaunch's level: the starter map (None meaning untitled_1) for a save outside an area, the
    // area's current level inside one.
    std::wstring level;
    if (!inArea) {
        const R::FName* starter = FNameField(g_metadata.obj, L"starterMap");
        level = (starter && !R::NameEquals(*starter, L"None")) ? R::ToString(*starter)
                                                               : std::wstring(P::name::GameplayLevel);
    } else {
        const R::FName* cur = FNameField(g_metadata.obj, L"currentLevel");
        if (!cur || R::NameEquals(*cur, L"None")) {
            UE_LOGW("engine: LoadStorySave -- save '%ls' is in area '%ls' with no current level", folderName,
                    g_loadedSlotName.c_str());
            return false;
        }
        level = R::ToString(*cur);
    }
    TravelTo(gi, g_metadata.obj, level, !inArea, "LoadStorySave");
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Content invalidation: the next poll reloads the folder from disk even mid-campaign, for files
// that changed under the same name (a rejoin re-downloads the host's world into the same folder).
// Staleness across campaigns is handled by the campaign identity.
void ResetCachedSave() {
    if (g_save.obj) UE_LOGI("engine: ResetCachedSave -- dropping cached save %p ('%ls')", g_save.obj,
                            g_campaignSave.c_str());
    ResetCampaign();
}

void SetSaveObjectReadyHook(SaveObjectReadyHook hook) {
    // Idempotent: the inventory layer re-calls this every pump tick, so it acts and logs only when
    // the hook changes.
    if (g_saveObjectReadyHook == hook) return;
    g_saveObjectReadyHook = hook;
    UE_LOGI("engine: SaveObjectReadyHook %s", hook ? "armed" : "disarmed");
}

// The fresh New Game boot: as LoadStorySave, with a blank saveSlot_C and a blank save_metadata_C
// created in memory instead of read from a folder, and no persistent save (the gamemode creates one,
// as it does for any load without). A fresh client has only the level-default props, so the host's
// connect snapshot mirrors its whole world onto it with nothing to reconcile away. Polled like
// LoadStorySave. Game thread.
bool StartFreshGame(int gameMode) {
    const BootWorldView w = SurveyBootWorld("StartFreshGame");
    if (w.inGameplay) return true;
    if (w.gameplayLoading) return false;  // in/loading the gameplay world -> wait, no re-open

    ValidateCachedSaveForCampaign(kFreshCampaign, w.curWorld);
    void* gi = world_singleton::GameInstance();
    if (!gi) {
        UE_LOGW("engine: StartFreshGame -- no GameInstance yet; retry");
        return false;
    }
    if (!g_save.obj) {
        void* save = CreateBlank(P::name::SaveSlotClass);
        void* metadata = CreateBlank(L"save_metadata_C");
        if (!save || !metadata) {
            UE_LOGW("engine: StartFreshGame -- blank saveSlot_C=%p save_metadata_C=%p not created; retry", save,
                    metadata);
            return false;
        }
        g_save.Set(save);
        g_metadata.Set(metadata);
        g_campaignSave = kFreshCampaign;
        g_loadedSlotName.clear();
        UE_LOGI("engine: StartFreshGame -- created BLANK saveSlot_C %p and metadata %p (fresh New Game baseline)",
                save, metadata);
        WriteFreshGameRules(metadata);
        // A fresh client join still gets its per-player inventory applied onto the blank save
        // before loadObjects materialises it.
        FireSaveObjectReadyHook(save);
    } else if (!g_freshGameRules.empty()) {
        UE_LOGW("engine: StartFreshGame -- %zu new-game rule(s) named but the blank save already exists; "
                "not written", g_freshGameRules.size());
        g_freshGameRules.clear();
    }

    RegisterSave(gi, g_save.obj, L"", L"", g_metadata.obj, nullptr, "StartFreshGame");
    PutSaveStartDayInForce(gi, g_save.obj, "StartFreshGame");
    PutSaveRulesInForce(gi, g_metadata.obj, "StartFreshGame");
    int mode = gameMode;
    if (!ue_wrap::game_mode::IsValid(mode)) {
        UE_LOGE("engine: StartFreshGame -- %d names no game mode; opening in story", gameMode);
        mode = ue_wrap::game_mode::kStory;
    }
    PutModeInForce(gi, g_metadata.obj, mode, "StartFreshGame");
    // A blank save has empty object and trigger arrays, so loading it yields the level defaults
    // through the same load path as a real save.
    TravelTo(gi, g_metadata.obj, P::name::GameplayLevel, true, "StartFreshGame");
    return false;  // not in gameplay yet -> caller keeps retrying
}

// Travel to the main menu through the game's own level-travel verb, mainGamemode_C::transition
// with the full package path (the short name does not resolve). A direct gamemode call, so a
// dead or ragdolling player can travel with no pause menu; with the ProcessEvent detour in
// bypass for the duration it reaches the menu and tears the world down without our layer
// hanging the teardown. Game thread only.
bool ReturnToMainMenu() {
    void* gm = world_singleton::Gamemode();
    if (!gm) {
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
    UE_LOGI("engine: ReturnToMainMenu -- mainGamemode_C::transition(\"/Game/menu\") dispatched=%d", ok ? 1 : 0);
    return ok;
}

}  // namespace ue_wrap::engine
