// ue_wrap/save_browser.cpp -- see ue_wrap/save_browser.h.

#include "ue_wrap/save_browser.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"        // GetSavePrefix / DeriveModeFromSlot / GetWorldContext
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ue_wrap::save_browser {
namespace {

namespace R = reflection;
namespace GT = game_thread;

// UE4 TArray header: { T* Data; int32 Num; int32 Max }. Same shape as R::FString.
struct TArrayHeader {
    void*   Data;
    int32_t Num;
    int32_t Max;
};

std::wstring FStrToW(const R::FString& s) {
    // FString::Num counts the null terminator.
    if (s.Data && s.Num > 1) return std::wstring(s.Data, s.Data + (s.Num - 1));
    return std::wstring();
}

// Build an FString that ALIASES `buf` (no copy) to pass into a UFunction. `buf` must
// outlive the call. UE reads it (const FString&) and does not take ownership.
R::FString MakeFStr(std::wstring& buf) {
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num  = static_cast<int32_t>(buf.size()) + 1;  // counts the null
    fs.Max  = fs.Num;
    return fs;
}

const wchar_t* ModeLabel(int mode) {
    switch (mode) {
        case 0: return L"Story";
        case 1: return L"Infinite";
        case 4: return L"Sandbox";
        case 5: return L"Halloween";
        case 6: return L"Ambience";
        case 7: return L"Solar";
        default: return L"";
    }
}

// ---- GameplayStatics save UFunctions (CreateSaveGameObject / SaveGameToSlot /
//      DoesSaveGameExist), resolved once on the CDO. -----------------------------
void* g_gsCdo        = nullptr;
void* g_createFn     = nullptr;
void* g_saveToSlotFn = nullptr;
void* g_doesExistFn  = nullptr;

bool ResolveGs() {
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(L"GameplayStatics");
    if (g_gsCdo) {
        void* c = R::ClassOf(g_gsCdo);
        if (c && !g_createFn)     g_createFn     = R::FindFunction(c, L"CreateSaveGameObject");
        if (c && !g_saveToSlotFn) g_saveToSlotFn = R::FindFunction(c, L"SaveGameToSlot");
        if (c && !g_doesExistFn)  g_doesExistFn  = R::FindFunction(c, L"DoesSaveGameExist");
    }
    return g_gsCdo && g_createFn && g_saveToSlotFn && g_doesExistFn;
}

// ---- saveSlot_C metadata offsets (reflection-resolved, recook-safe; cached). -----
struct SlotOffsets {
    int32_t day = -1, points = -1, health = -1, maxHealth = -1, version = -1, lastDate = -1;
    bool tried = false;
};
SlotOffsets g_off;

void ResolveSlotOffsets() {
    if (g_off.tried) return;
    void* cls = R::FindClass(L"saveSlot_C");
    if (!cls) return;  // not loaded yet -- retry next call (tried stays false)
    g_off.day       = R::FindPropertyOffset(cls, L"Day");
    g_off.points    = R::FindPropertyOffset(cls, L"Points");
    g_off.health    = R::FindPropertyOffset(cls, L"health");
    g_off.maxHealth = R::FindPropertyOffset(cls, L"maxHealth");
    g_off.version   = R::FindPropertyOffset(cls, L"Version");
    g_off.lastDate  = R::FindPropertyOffset(cls, L"lastDate");
    // Latch ONLY when every field resolved, so a partial/recook miss (one field renamed)
    // doesn't stick at -1 forever and silently read 0 for that field (audit I-2). Until
    // then we retry each call (cheap; the class loads once on a gameplay/menu transition).
    const bool all = g_off.day >= 0 && g_off.points >= 0 && g_off.health >= 0 &&
                     g_off.maxHealth >= 0 && g_off.version >= 0 && g_off.lastDate >= 0;
    if (all) {
        g_off.tried = true;
        UE_LOGI("save_browser: saveSlot_C offsets Day=%d Points=%d health=%d maxHealth=%d Version=%d lastDate=%d",
                g_off.day, g_off.points, g_off.health, g_off.maxHealth, g_off.version, g_off.lastDate);
    } else {
        UE_LOGW("save_browser: saveSlot_C offsets incomplete Day=%d Points=%d health=%d maxHealth=%d "
                "Version=%d lastDate=%d -- will retry", g_off.day, g_off.points, g_off.health,
                g_off.maxHealth, g_off.version, g_off.lastDate);
    }
}

template <class T>
T ReadField(void* obj, int32_t off, T fallback = T{}) {
    if (!obj || off < 0) return fallback;
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

// Resolve the live Uui_saveSlots_C the menu owns. VOTV constructs umg_saveSlots as a
// member of the live ui_menu_C; the save_enum probe proved it is LIVE at the menu
// (offset 0x488), which is the only context the picker enumerates from (opened from the
// MULTIPLAYER button at the main menu). We do NOT spawn a bare ui_saveSlots_C as a
// fallback: driving loadSlots on an un-Constructed widget is an untested speculative
// path (RULE 1/3). If umg_saveSlots is ever null (not the picker's context), enumerate
// returns false -> the picker shows "save system not ready" and a later RefreshAsync
// retries. Game thread.
void* ResolveSaveSlotsWidget() {
    void* menu = R::FindObjectByClass(L"ui_menu_C");
    if (!menu) {
        UE_LOGW("save_browser: no live ui_menu_C -- cannot enumerate (not at the menu?)");
        return nullptr;
    }
    static int32_t umgOff = -1;
    if (umgOff < 0) {
        const int32_t v = R::FindPropertyOffset(R::ClassOf(menu), L"umg_saveSlots");
        if (v >= 0) { umgOff = v; UE_LOGI("save_browser: ui_menu_C.umg_saveSlots offset = %d", v); }
    }
    if (umgOff < 0) return nullptr;  // property not resolved yet -- retry next call
    void* w = ReadField<void*>(menu, umgOff, nullptr);
    if (!w || !R::IsLive(w)) {
        UE_LOGW("save_browser: umg_saveSlots not constructed yet (null) -- open the menu / retry");
        return nullptr;
    }
    return w;
}

// ---- async cache (render thread reads; game thread fills) -----------------------
std::mutex g_mu;
std::vector<SaveInfo> g_cache;
uint64_t g_rev = 0;
std::string g_status = "No save scan yet";
std::atomic<bool> g_scanning{false};

}  // namespace

bool EnumerateSaves(std::vector<SaveInfo>& out) {
    out.clear();
    ResolveSlotOffsets();
    void* widget = ResolveSaveSlotsWidget();
    if (!widget) { UE_LOGW("save_browser: EnumerateSaves -- no save-slots widget"); return false; }

    void* cls = R::ClassOf(widget);
    void* loadSlotsFn = cls ? R::FindFunction(cls, L"loadSlots") : nullptr;
    if (!loadSlotsFn) { UE_LOGW("save_browser: loadSlots UFunction not found"); return false; }

    // Resolve the two array offsets, RETRYING while unresolved: only COMMIT a
    // successful (>=0) result, so a transient -1 (class loaded but property not yet
    // resolvable / a recook) doesn't latch and suppress every future call (audit I-1).
    static int32_t savesOff = -1, namesOff = -1;
    if (savesOff < 0) { const int32_t v = R::FindPropertyOffset(cls, L"saves");            if (v >= 0) savesOff = v; }
    if (namesOff < 0) { const int32_t v = R::FindPropertyOffset(cls, L"valid_savesNames"); if (v >= 0) namesOff = v; }
    if (savesOff < 0 || namesOff < 0) {
        UE_LOGW("save_browser: saves@%d / valid_savesNames@%d offset unresolved (will retry)", savesOff, namesOff);
        return false;
    }

    // Drive VOTV's own list builder (no params). It file-lists SaveGames, LoadGameFromSlot's
    // each, casts to saveSlot_C, filters subsaves/backups, sorts newest-first -- and the
    // engine frees that UFunction's transient frame (no manual TArray<FString> free for us).
    {
        ParamFrame f(loadSlotsFn);
        Call(widget, f);
    }

    const TArrayHeader saves = ReadField<TArrayHeader>(widget, savesOff);
    const TArrayHeader names = ReadField<TArrayHeader>(widget, namesOff);
    const int32_t n = (saves.Num < names.Num) ? saves.Num : names.Num;
    UE_LOGI("save_browser: loadSlots -> saves.Num=%d valid_savesNames.Num=%d (using %d)",
            saves.Num, names.Num, n);
    if (n <= 0 || !saves.Data || !names.Data) return true;  // resolved, just no saves

    auto* saveArr = reinterpret_cast<void**>(saves.Data);
    auto* nameArr = reinterpret_cast<R::FString*>(names.Data);
    out.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        void* so = saveArr[i];
        const std::wstring slot = FStrToW(nameArr[i]);
        if (slot.empty()) continue;

        SaveInfo info;
        info.slot = slot;
        info.mode = engine::DeriveModeFromSlot(slot.c_str());
        info.modeLabel = ModeLabel(info.mode);
        // displayName = slot minus the mode prefix (cosmetic).
        std::wstring prefix;
        if (info.mode >= 0 && engine::GetSavePrefix(static_cast<uint8_t>(info.mode), prefix) &&
            !prefix.empty() && slot.rfind(prefix, 0) == 0) {
            info.displayName = slot.substr(prefix.size());
        } else {
            info.displayName = slot;
        }
        if (so && R::IsLive(so)) {
            info.day            = static_cast<int>(ReadField<float>(so, g_off.day));
            info.points         = ReadField<int32_t>(so, g_off.points);
            info.health         = ReadField<float>(so, g_off.health);
            info.maxHealth      = ReadField<float>(so, g_off.maxHealth);
            info.lastPlayedTicks = ReadField<int64_t>(so, g_off.lastDate);
            if (g_off.version >= 0)
                info.version = FStrToW(ReadField<R::FString>(so, g_off.version));
        }
        UE_LOGI("save_browser:   [%d] '%ls' mode=%d(%ls) day=%d pts=%d hp=%.0f/%.0f ver='%ls'",
                i, info.slot.c_str(), info.mode, info.modeLabel.c_str(), info.day,
                info.points, info.health, info.maxHealth, info.version.c_str());
        out.push_back(std::move(info));
    }
    return true;
}

bool SlotExists(const std::wstring& slot) {
    if (slot.empty() || !ResolveGs() || !g_doesExistFn) return false;
    std::wstring buf = slot;
    R::FString fs = MakeFStr(buf);
    ParamFrame f(g_doesExistFn);
    f.SetRaw(L"SlotName", &fs, sizeof(fs));
    f.Set<int32_t>(L"UserIndex", 0);
    if (!Call(g_gsCdo, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

bool CreateNamedSave(const std::wstring& name, uint8_t mode, std::wstring& outSlot) {
    outSlot.clear();
    if (name.empty()) { UE_LOGW("save_browser: CreateNamedSave -- empty name"); return false; }
    if (!ResolveGs()) { UE_LOGW("save_browser: CreateNamedSave -- GameplayStatics not resolved"); return false; }

    std::wstring prefix;
    if (!engine::GetSavePrefix(mode, prefix)) {
        UE_LOGW("save_browser: CreateNamedSave -- getSavePrefix(%u) unresolved (widget not loaded?)",
                static_cast<unsigned>(mode));
        return false;
    }
    const std::wstring slot = prefix + name;
    if (SlotExists(slot)) {
        UE_LOGW("save_browser: CreateNamedSave -- slot '%ls' already exists (name taken)", slot.c_str());
        return false;
    }

    // CreateSaveGameObject(saveSlot_C) -> a blank UsaveSlot_C (the New-Game baseline).
    void* saveCls = R::FindClass(L"saveSlot_C");
    if (!saveCls) { UE_LOGW("save_browser: CreateNamedSave -- saveSlot_C class missing"); return false; }
    void* save = nullptr;
    {
        ParamFrame f(g_createFn);
        f.Set<void*>(L"SaveGameClass", saveCls);
        if (!Call(g_gsCdo, f)) { UE_LOGE("save_browser: CreateSaveGameObject call failed"); return false; }
        save = f.Get<void*>(L"ReturnValue");
    }
    if (!save) { UE_LOGW("save_browser: CreateSaveGameObject returned null"); return false; }

    // SaveGameToSlot(save, "<prefix><name>", 0) -> writes <slot>.sav NOW (persist at create).
    std::wstring buf = slot;
    R::FString fs = MakeFStr(buf);
    bool ok = false;
    {
        ParamFrame f(g_saveToSlotFn);
        f.Set<void*>(L"SaveGameObject", save);
        f.SetRaw(L"SlotName", &fs, sizeof(fs));
        f.Set<int32_t>(L"UserIndex", 0);
        if (!Call(g_gsCdo, f)) { UE_LOGE("save_browser: SaveGameToSlot call failed"); return false; }
        ok = f.Get<bool>(L"ReturnValue");
    }
    if (!ok) { UE_LOGW("save_browser: SaveGameToSlot('%ls') returned false", slot.c_str()); return false; }

    outSlot = slot;
    UE_LOGI("save_browser: CreateNamedSave -- created + persisted '%ls' (mode=%u)",
            slot.c_str(), static_cast<unsigned>(mode));
    return true;
}

void RefreshAsync() {
    if (g_scanning.exchange(true)) return;  // a scan is already in flight
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = "Scanning saves...";
    }
    GT::Post([] {
        std::vector<SaveInfo> v;
        const bool ok = EnumerateSaves(v);
        std::lock_guard<std::mutex> lk(g_mu);
        if (ok) {
            g_cache.swap(v);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu save%s", g_cache.size(),
                          g_cache.size() == 1 ? "" : "s");
            g_status = buf;
        } else {
            g_status = "Save system not ready (try again)";
        }
        ++g_rev;
        // Clear the coalescing flag INSIDE the lock, after the rev/cache/status are
        // coherent -- so a render-thread RefreshAsync that observes g_scanning==false
        // also sees the completed scan's data (audit C-1: clearing it outside the lock
        // left a window where g_rev bumped but a concurrent caller still saw scanning).
        g_scanning.store(false, std::memory_order_release);
    });
}

uint64_t CopySaves(std::vector<SaveInfo>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_cache;
    return g_rev;
}

std::string Status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

}  // namespace ue_wrap::save_browser
