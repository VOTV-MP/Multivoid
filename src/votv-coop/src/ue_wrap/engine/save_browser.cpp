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
    // The row's DAY is savedtime.Z + 1 (uicomp_saveSlot::upd). `savedtime` is an FIntVector
    // {h, m, day}; the float `Day` property is the raw elapsed-time accumulator and is not it.
    g_off.savedtime = R::FindPropertyOffset(cls, L"savedtime");
    g_off.points    = R::FindPropertyOffset(cls, L"Points");
    g_off.health    = R::FindPropertyOffset(cls, L"health");
    g_off.maxHealth = R::FindPropertyOffset(cls, L"maxHealth");
    g_off.version   = R::FindPropertyOffset(cls, L"Version");
    g_off.lastDate  = R::FindPropertyOffset(cls, L"lastDate");
    // Latch only once every field resolved, so a recook that renames one does not stick the rest
    // at -1 and read 0 forever; until then each call retries.
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

// ---- the scan pipeline ------------------------------------------------------------
// The game's own loadSlots deserializes every .sav on the game thread; this scan reads the
// few scalars a row needs instead, in two stages. STAGE A (game thread) resolves the
// SaveGames dir, lists *.sav, drops subsaves through the game's own classifier and reads the
// saveSlot_C CDO defaults. STAGE B (worker) tag-walks each file for its metadata, caches by
// mtime and sorts newest-first. Slot names are never filtered by hand: the mode prefixes come
// from the game's getSavePrefix.

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

// Resolve <ProjectSavedDir>/SaveGames/ once through the native UFunction, which honors
// -saveddirsuffix; never rebuild it from the environment. The returned FString's engine-side
// buffer is read once and pinned.
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
    // Newest-first by file mtime rather than loadSlots' MaxOfDateTimeArray over the saved dates:
    // the same write stamps both, and lastSavedDate is delta-omitted on a fresh slot.
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

    // Stamp Version as the native create does (lib_C::gameVersion), because a blank CDO-default
    // object serializes an empty one and every save row then paints the red "unk!" badge. The
    // out FString's buffer is minted engine-side in the call and its header moves into the fresh
    // object's field, so ownership transfers with no copy. A resolve failure still creates the
    // slot; only the badge is wrong.
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

bool CreateNamedSaveUnique(const std::wstring& baseName, uint8_t mode, std::wstring& outSlot) {
    outSlot.clear();
    if (baseName.empty()) {
        UE_LOGW("save_browser: CreateNamedSaveUnique -- empty base name");
        return false;
    }
    std::wstring prefix;
    if (!ResolveGs() || !engine::GetSavePrefix(mode, prefix)) {
        UE_LOGW("save_browser: CreateNamedSaveUnique -- save system unresolved (mode=%u)",
                static_cast<unsigned>(mode));
        return false;
    }

    // Numbered from two, the way a save list numbers a repeat: the first world keeps the bare
    // name. The cap refuses rather than wrapping, since reusing a slot at the end of the range
    // would overwrite a world somebody played.
    for (int n = 1; n <= 99; ++n) {
        std::wstring name = baseName;
        if (n > 1) name += L" " + std::to_wstring(n);
        if (SlotExists(prefix + name)) continue;
        return CreateNamedSave(name, mode, outSlot);
    }
    UE_LOGW("save_browser: CreateNamedSaveUnique -- '%ls' and 98 numbered variants all exist",
            baseName.c_str());
    return false;
}

void RefreshAsync() {
    if (g_scanning.exchange(true)) return;  // a scan is already in flight
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = "Scanning saves...";
    }
    GT::Post([] {
        // STAGE A here on the game thread, STAGE B on a detached worker, so no disk I/O runs under
        // the game tick. The worker touches only the statics above and a scan can be in flight only
        // while the picker is open.
        auto items = std::make_shared<std::vector<ScanItem>>();
        auto def   = std::make_shared<SlotCdoDefaults>();
        const bool ok = BuildScanList(*items, *def);
        if (!ok) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status = "Save system not ready (try again)";
            ++g_rev;
            // Clear the coalescing flag inside the lock, after rev/cache/status are coherent, so a
            // render-thread RefreshAsync that sees g_scanning==false also sees the finished scan.
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
            g_scanning.store(false, std::memory_order_release);  // inside the lock
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
