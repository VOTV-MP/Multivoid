// ue_wrap/save_browser.cpp -- see ue_wrap/save_browser.h.

#include "ue_wrap/engine/save_browser.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"        // GetSavePrefix / DeriveModeFromSlot
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/engine/gvas_meta.h"     // worker-thread .sav metadata reads (no LoadGameFromSlot)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ue_wrap::save_browser {
namespace {

namespace R = reflection;
namespace GT = game_thread;

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
    int32_t savedtime = -1, points = -1, health = -1, maxHealth = -1, version = -1, lastDate = -1;
    bool tried = false;
};
SlotOffsets g_off;

void ResolveSlotOffsets() {
    if (g_off.tried) return;
    void* cls = R::FindClass(L"saveSlot_C");
    if (!cls) return;  // not loaded yet -- retry next call (tried stays false)
    // The DAY shown on the game's own save rows is savedtime.Z + 1 (uicomp_saveSlot::upd
    // bytecode: days := save.savedtime.Z; SetText(Conv_IntToText(Add_IntInt(days, 1)))).
    // `savedtime` is an FIntVector {h, m, day}. The float `Day` property is the raw
    // elapsed-TIME accumulator (== daynightCycle.totalTime) -- displaying it raw was the
    // "Day 3566" bug (2026-06-10 user report).
    g_off.savedtime = R::FindPropertyOffset(cls, L"savedtime");
    g_off.points    = R::FindPropertyOffset(cls, L"Points");
    g_off.health    = R::FindPropertyOffset(cls, L"health");
    g_off.maxHealth = R::FindPropertyOffset(cls, L"maxHealth");
    g_off.version   = R::FindPropertyOffset(cls, L"Version");
    g_off.lastDate  = R::FindPropertyOffset(cls, L"lastDate");
    // Latch ONLY when every field resolved, so a partial/recook miss (one field renamed)
    // doesn't stick at -1 forever and silently read 0 for that field (audit I-2). Until
    // then we retry each call (cheap; the class loads once on a gameplay/menu transition).
    const bool all = g_off.savedtime >= 0 && g_off.points >= 0 && g_off.health >= 0 &&
                     g_off.maxHealth >= 0 && g_off.version >= 0 && g_off.lastDate >= 0;
    if (all) {
        g_off.tried = true;
        UE_LOGI("save_browser: saveSlot_C offsets savedtime=%d Points=%d health=%d maxHealth=%d Version=%d lastDate=%d",
                g_off.savedtime, g_off.points, g_off.health, g_off.maxHealth, g_off.version, g_off.lastDate);
    } else {
        UE_LOGW("save_browser: saveSlot_C offsets incomplete savedtime=%d Points=%d health=%d maxHealth=%d "
                "Version=%d lastDate=%d -- will retry", g_off.savedtime, g_off.points, g_off.health,
                g_off.maxHealth, g_off.version, g_off.lastDate);
    }
}

template <class T>
T ReadField(void* obj, int32_t off, T fallback = T{}) {
    if (!obj || off < 0) return fallback;
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

// ---- the scan pipeline (2026-07-11 rework) ---------------------------------------
// The old scan drove VOTV's Uui_saveSlots_C::loadSlots, which LoadGameFromSlot's
// EVERY save -- each a full synchronous GVAS deserialize of a 15-20 MB world file ON
// THE GAME THREAD (measured 2026-07-11: 15 saves = a multi-second picker-open freeze
// + 15 transient UsaveSlot_C object graphs for GC). The picker only needs a handful
// of scalars per row, so the scan is now two stages:
//   STAGE A (game thread, cheap): resolve the SaveGames dir (native
//     GetProjectSavedDirectory -- honors -saveddirsuffix), list *.sav, classify
//     subsaves via VOTV's own lib_C::processSaveNameIntoSubsave (native filter
//     parity), derive mode/displayName from the slot prefix, and read the
//     saveSlot_C CDO defaults (delta-vs-CDO serialization omits default-valued
//     properties from the file -- the CDO supplies them, LoadGameFromSlot parity).
//   STAGE B (worker thread): stat + gvas_meta::ReadSlotMeta each file (tag-walk,
//     payload-skip -- no full deserialize), mtime-keyed cache so re-opens only
//     re-parse changed files, sort newest-first, publish.
// Filter parity with the native loadSlots list (ground-truthed against its logged
// output 2026-07-11): everything except subsaves and non-saveSlot_C classes
// (data.sav fails the native DynamicCast; here it fails the GVAS class check).
// b_* files are SANDBOX saves (the 'b_' SAVE PREFIX), not backups -- never filter
// by name. The gvas_meta.h header documents the format evidence.

struct ScanItem {
    SaveInfo     base;   // slot/mode/modeLabel/displayName pre-filled (stage A)
    std::wstring path;   // full path to the .sav
};

// saveSlot_C CDO defaults for properties the delta serializer omitted.
struct SlotCdoDefaults {
    bool    valid = false;
    int32_t savedTimeZ = 0;
    int32_t points = 0;
    float   health = 0.f;
    float   maxHealth = 0.f;
    std::wstring version;
};

// Resolve <ProjectSavedDir>/SaveGames/ once via the native UFunction (the engine
// honors -saveddirsuffix, so never rebuild this from the environment). The returned
// FString's engine-side buffer is read once and pinned (the fstring_utils pin/leak
// doctrine; one small allocation per process).
std::wstring ResolveSaveGamesDir() {
    static std::wstring s_dir;
    if (!s_dir.empty()) return s_dir;
    void* ksl = R::FindClassDefaultObject(L"KismetSystemLibrary");
    void* fn  = ksl ? R::FindFunction(R::ClassOf(ksl), L"GetProjectSavedDirectory") : nullptr;
    if (!fn) { UE_LOGW("save_browser: GetProjectSavedDirectory unresolved"); return {}; }
    ParamFrame f(fn);
    if (!Call(ksl, f)) return {};
    R::FString ret{};
    f.GetRaw(L"ReturnValue", &ret, static_cast<int32_t>(sizeof(ret)));
    std::wstring dir = FStrToW(ret);
    if (dir.empty()) return {};
    if (dir.back() != L'/' && dir.back() != L'\\') dir += L'/';
    dir += L"SaveGames/";
    s_dir = dir;
    UE_LOGI("save_browser: SaveGames dir = '%ls'", s_dir.c_str());
    return s_dir;
}

// VOTV's own subsave classifier (lib_C CDO; pure string logic). False on resolve
// failure = list rather than hide. The out mainSaveName FString is engine-minted
// into the frame and abandoned (pin doctrine; bytes per call, scans are on-demand).
bool IsSubsaveName(const std::wstring& slot) {
    static void* s_lib = nullptr;
    static void* s_fn  = nullptr;
    if (!s_lib) s_lib = R::FindClassDefaultObject(L"lib_C");
    if (s_lib && !s_fn) s_fn = R::FindFunction(R::ClassOf(s_lib), L"processSaveNameIntoSubsave");
    if (!s_lib || !s_fn) return false;
    std::wstring buf = slot;
    R::FString fs = MakeFStr(buf);
    ParamFrame f(s_fn);
    f.SetRaw(L"saveSlotName", &fs, static_cast<int32_t>(sizeof(fs)));
    f.Set<void*>(L"__WorldContext", s_lib);
    if (!Call(s_lib, f)) return false;
    return f.Get<bool>(L"isSubsave");
}

// STAGE A. Game thread. False = save system not resolvable yet (retry later).
bool BuildScanList(std::vector<ScanItem>& items, SlotCdoDefaults& def) {
    items.clear();
    const std::wstring dir = ResolveSaveGamesDir();
    if (dir.empty()) return false;

    ResolveSlotOffsets();
    void* cdo = R::FindClassDefaultObject(L"saveSlot_C");
    if (!cdo || g_off.savedtime < 0) {
        UE_LOGW("save_browser: saveSlot_C CDO/offsets unresolved -- cannot scan yet");
        return false;
    }
    def.valid      = true;
    def.savedTimeZ = ReadField<int32_t>(cdo, g_off.savedtime + 8);
    def.points     = ReadField<int32_t>(cdo, g_off.points);
    def.health     = ReadField<float>(cdo, g_off.health);
    def.maxHealth  = ReadField<float>(cdo, g_off.maxHealth);
    def.version    = FStrToW(ReadField<R::FString>(cdo, g_off.version));

    std::error_code ec;
    for (std::filesystem::directory_iterator it{std::filesystem::path(dir), ec}, end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring ext = it->path().extension().wstring();
        for (wchar_t& c : ext) c = static_cast<wchar_t>(::towlower(c));
        if (ext != L".sav") continue;
        const std::wstring slot = it->path().stem().wstring();
        if (slot.empty()) continue;
        if (IsSubsaveName(slot)) continue;  // native parity: subsaves never top-level

        ScanItem item;
        item.path = it->path().wstring();
        item.base.slot = slot;
        item.base.mode = engine::DeriveModeFromSlot(slot.c_str());
        item.base.modeLabel = ModeLabel(item.base.mode);
        std::wstring prefix;  // displayName = slot minus the mode prefix (cosmetic)
        if (item.base.mode >= 0 &&
            engine::GetSavePrefix(static_cast<uint8_t>(item.base.mode), prefix) &&
            !prefix.empty() && slot.rfind(prefix, 0) == 0) {
            item.base.displayName = slot.substr(prefix.size());
        } else {
            item.base.displayName = slot;
        }
        items.push_back(std::move(item));
    }
    if (ec) {
        UE_LOGW("save_browser: SaveGames listing failed (%s)", ec.message().c_str());
        return false;
    }
    return true;
}

// ---- async cache (render thread reads; stage B fills) ---------------------------
std::mutex g_mu;
std::vector<SaveInfo> g_cache;
uint64_t g_rev = 0;
std::string g_status = "No save scan yet";
std::atomic<bool> g_scanning{false};

// mtime-keyed per-slot metadata cache: an unchanged file's row is reused without
// re-opening it, so re-opening the picker is ~free. Guarded by g_metaMu (stage B
// runs on a worker; the synchronous EnumerateSaves path runs on the game thread).
struct CachedMeta {
    int64_t  mtime = 0;
    uint64_t size = 0;
    SaveInfo info;
};
std::mutex g_metaMu;
std::map<std::wstring, CachedMeta> g_metaCache;

// STAGE B. Any thread (pure file I/O). Fills `out` sorted newest-first.
void ParseScanList(const std::vector<ScanItem>& items, const SlotCdoDefaults& def,
                   std::vector<SaveInfo>& out) {
    struct Row { int64_t mtime; SaveInfo info; };
    std::vector<Row> rows;
    rows.reserve(items.size());
    for (const ScanItem& it : items) {
        std::error_code ec;
        const std::filesystem::path p{it.path};
        const uint64_t sz = std::filesystem::file_size(p, ec);
        if (ec) continue;
        const int64_t mt = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
        if (ec) continue;
        {
            std::lock_guard<std::mutex> lk(g_metaMu);
            auto c = g_metaCache.find(it.base.slot);
            if (c != g_metaCache.end() && c->second.mtime == mt && c->second.size == sz) {
                rows.push_back({mt, c->second.info});
                continue;
            }
        }
        gvas_meta::GvasSlotMeta m;
        if (!gvas_meta::ReadSlotMeta(it.path, m) || !m.isSaveSlotClass)
            continue;  // unreadable / not a saveSlot_C (data.sav): native-parity skip
        SaveInfo info = it.base;
        // Display formula parity (uicomp_saveSlot::upd): day = savedtime.Z + 1.
        info.day       = (m.hasSavedTimeZ ? m.savedTimeZ : def.savedTimeZ) + 1;
        info.points    = m.hasPoints ? m.points : def.points;
        info.health    = m.hasHealth ? m.health : def.health;
        info.maxHealth = m.hasMaxHealth ? m.maxHealth : def.maxHealth;
        info.version   = m.hasVersion ? m.version : def.version;
        info.lastPlayedTicks = m.hasLastSavedDate ? m.lastSavedDateTicks : 0;
        {
            std::lock_guard<std::mutex> lk(g_metaMu);
            g_metaCache[it.base.slot] = {mt, sz, info};
        }
        rows.push_back({mt, std::move(info)});
    }
    // Newest-first by file mtime. Deliberate divergence from loadSlots'
    // MaxOfDateTimeArray-over-save-dates: mtime is stamped by the same
    // SaveGameToSlot write those dates describe, gives the same ordering, and
    // costs zero extra parsing (lastSavedDate is delta-omitted on fresh slots).
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.mtime > b.mtime; });
    out.clear();
    out.reserve(rows.size());
    for (Row& r : rows) {
        UE_LOGI("save_browser:   '%ls' mode=%d(%ls) day=%d pts=%d hp=%.0f/%.0f ver='%ls'",
                r.info.slot.c_str(), r.info.mode, r.info.modeLabel.c_str(), r.info.day,
                r.info.points, r.info.health, r.info.maxHealth, r.info.version.c_str());
        out.push_back(std::move(r.info));
    }
}

}  // namespace

bool EnumerateSaves(std::vector<SaveInfo>& out) {
    out.clear();
    std::vector<ScanItem> items;
    SlotCdoDefaults def;
    if (!BuildScanList(items, def)) return false;
    ParseScanList(items, def, out);  // synchronous convenience path (dev probe)
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

    // Stamp Version exactly as the native create does (ui_saveSlots button_create
    // ubergraph @5177-5227: tempSave.version = Default__lib_C->gameVersion("","")).
    // A blank CDO-default object serializes an EMPTY Version -> every save list paints
    // the red "unk!" badge and the widget's launch-time version check reads a mismatch
    // (user report 2026-07-04). gameVersion's body is pure ([RD] lib bytecode:
    // Concat(prefix, GetProjectVersion(), suffix); __WorldContext unused), so the lib
    // CDO is a valid call target at the menu. The out FString's buffer is minted
    // ENGINE-side inside the call; we transfer its 16-byte header into the fresh
    // object's Version field (the fstring_utils pin doctrine: the engine's later
    // reassign/destroy frees it; ParamFrame is a raw byte arena that frees nothing,
    // so ownership moves cleanly -- one allocation, zero copies, zero leaks).
    // Best-effort: a resolve failure logs + still creates (the slot works; only the
    // badge is wrong -- same as the pre-fix behavior).
    ResolveSlotOffsets();
    do {
        void* libCdo = R::FindClassDefaultObject(L"lib_C");
        void* libCls = libCdo ? R::ClassOf(libCdo) : nullptr;
        // Live-FName case roulette: the CXX dump renders GameVersion, the asset dump
        // gameVersion -- FindFunction compares case-sensitively, so try both.
        void* verFn = libCls ? R::FindFunction(libCls, L"GameVersion") : nullptr;
        if (!verFn && libCls) verFn = R::FindFunction(libCls, L"gameVersion");
        if (!verFn || g_off.version < 0) {
            UE_LOGW("save_browser: CreateNamedSave -- Version stamp unavailable "
                    "(lib_C.gameVersion=%p VersionOff=%d); creating unversioned",
                    verFn, g_off.version);
            break;
        }
        ParamFrame f(verFn);  // Prefix/Suffix stay zeroed = valid empty FStrings
        f.Set<void*>(L"__WorldContext", save);
        if (!Call(libCdo, f)) {
            UE_LOGW("save_browser: CreateNamedSave -- gameVersion call failed; creating unversioned");
            break;
        }
        R::FString ver{};
        f.GetRaw(L"Version", &ver, static_cast<int32_t>(sizeof(ver)));
        std::memcpy(reinterpret_cast<uint8_t*>(save) + g_off.version, &ver, sizeof(ver));
        UE_LOGI("save_browser: CreateNamedSave -- stamped Version '%ls'", FStrToW(ver).c_str());
    } while (false);

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
        // STAGE A on the game thread (native dir resolve + subsave classify + CDO
        // defaults -- all cheap); STAGE B (file stat + GVAS tag-walk) on a worker so
        // no disk I/O ever runs under the game tick. The worker is detached: it only
        // touches the leak-safe pipeline statics above, runs for tens of ms, and a
        // scan can only be in flight while the picker is open (never during process
        // exit teardown).
        auto items = std::make_shared<std::vector<ScanItem>>();
        auto def   = std::make_shared<SlotCdoDefaults>();
        const bool ok = BuildScanList(*items, *def);
        if (!ok) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status = "Save system not ready (try again)";
            ++g_rev;
            // Clear the coalescing flag INSIDE the lock, after the rev/cache/status
            // are coherent -- so a render-thread RefreshAsync that observes
            // g_scanning==false also sees the completed scan's data (audit C-1).
            g_scanning.store(false, std::memory_order_release);
            return;
        }
        std::thread([items, def] {
            std::vector<SaveInfo> v;
            ParseScanList(*items, *def, v);
            std::lock_guard<std::mutex> lk(g_mu);
            g_cache.swap(v);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu save%s", g_cache.size(),
                          g_cache.size() == 1 ? "" : "s");
            g_status = buf;
            ++g_rev;
            g_scanning.store(false, std::memory_order_release);  // audit C-1: inside the lock
        }).detach();
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
