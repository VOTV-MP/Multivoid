// ue_wrap/engine/save_browser.cpp -- see ue_wrap/engine/save_browser.h.

#include "ue_wrap/engine/save_browser.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/save_folder.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
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

// lib_C::gameVersion(prefix, suffix) with both empty: the project version, the string the game's
// slot menu compares a save's metadata version against. The body is pure and never reads
// __WorldContext, so the CDO is a valid target. The out FString is minted engine-side and handed to
// the caller, who owns it. Game thread.
bool CallGameVersion(R::FString& out) {
    out = R::FString{};
    void* libCdo = R::FindClassDefaultObject(L"lib_C");
    void* libCls = libCdo ? R::ClassOf(libCdo) : nullptr;
    // The CXX dump renders GameVersion, the asset dump gameVersion. FindFunction compares
    // insensitively, so one lookup answers whichever spelling this process interned first.
    void* verFn = libCls ? R::FindFunction(libCls, L"GameVersion") : nullptr;
    if (!verFn) return false;
    ParamFrame f(verFn);
    f.Set<void*>(L"__WorldContext", libCdo);
    if (!Call(libCdo, f)) return false;
    return f.GetRaw(L"Version", &out, static_cast<int32_t>(sizeof(out))) && out.Data;
}

// The running game's version, resolved once per process on the game thread and kept for readers
// on any thread (GameVersion).
std::mutex   g_versionMu;
std::wstring g_gameVersion;

std::wstring ResolveGameVersion() {
    {
        std::lock_guard<std::mutex> lk(g_versionMu);
        if (!g_gameVersion.empty()) return g_gameVersion;
    }
    R::FString ver{};
    if (!CallGameVersion(ver)) {
        UE_LOGW("save_browser: lib_C.gameVersion unresolved -- no save is flagged as a version conflict");
        return {};
    }
    const std::wstring out = FStrToW(ver);
    R::EngineFree(ver.Data);  // ours: read once, kept as a copy
    UE_LOGI("save_browser: the running game's version is '%ls'", out.c_str());
    std::lock_guard<std::mutex> lk(g_versionMu);
    g_gameVersion = out;
    return out;
}

// One field of a loaded metadata object, by name; `fallback` when the class does not declare it.
template <class T>
T MetaField(void* meta, const wchar_t* prop, T fallback = T{}) {
    void* cls = meta ? R::ClassOf(meta) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    if (off < 0) return fallback;
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(meta) + off);
}

// The scan: every folder of SaveGames/ holding a main.meta, its metadata read through the game's
// own loader, sorted by the metadata's last-saved date, newest first. Game thread. False when the
// saved directory is not resolvable yet.
bool Scan(std::vector<SaveInfo>& out) {
    out.clear();
    const std::wstring root = save_folder::FolderPath(L"");
    if (root.empty()) return false;
    const std::wstring gameVersion = ResolveGameVersion();
    std::error_code ec;
    for (std::filesystem::directory_iterator it{std::filesystem::path(root), ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::wstring name = it->path().filename().wstring();
        if (!std::filesystem::exists(it->path() / L"main.meta", ec)) continue;  // not a save folder
        void* meta = save_folder::LoadCompressed(L"main", save_folder::FolderPath(name), L".meta");
        if (!meta) {
            UE_LOGW("save_browser: '%ls' has an unreadable main.meta -- not listed", name.c_str());
            continue;
        }
        SaveInfo info;
        info.slot = name;
        info.displayName = name;
        void* metaCls = R::ClassOf(meta);
        const int32_t modeOff = metaCls ? R::FindPropertyOffset(metaCls, L"gamemode_type") : -1;
        info.mode = modeOff >= 0 ? *(reinterpret_cast<const uint8_t*>(meta) + modeOff) : -1;
        const wchar_t* label = ue_wrap::game_mode::Name(info.mode);
        info.modeLabel = label ? label : L"";
        // savedtime is an FIntVector {h, m, day}; the slot row shows day + 1.
        const int32_t timeOff = metaCls ? R::FindPropertyOffset(metaCls, L"savedtime") : -1;
        info.day = (timeOff >= 0 ? *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(meta) +
                                                                     timeOff + 8)
                                 : 0) + 1;
        info.version = FStrToW(MetaField<R::FString>(meta, L"version"));
        // The game's compare is FString ==, which ignores case. An unresolved game version flags nothing.
        info.versionConflict =
            !gameVersion.empty() && ::_wcsicmp(info.version.c_str(), gameVersion.c_str()) != 0;
        info.lastPlayedTicks = MetaField<int64_t>(meta, L"lastSaved");
        out.push_back(std::move(info));
    }
    if (ec) {
        UE_LOGW("save_browser: SaveGames listing failed (%s)", ec.message().c_str());
        return false;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SaveInfo& a, const SaveInfo& b) { return a.lastPlayedTicks > b.lastPlayedTicks; });
    for (const SaveInfo& s : out)
        UE_LOGI("save_browser:   '%ls' mode=%d(%ls) day=%d ver='%ls'", s.slot.c_str(), s.mode,
                s.modeLabel.c_str(), s.day, s.version.c_str());
    return true;
}

// ---- async cache (render thread reads; the game thread fills) -------------------
std::mutex g_mu;
std::vector<SaveInfo> g_cache;
uint64_t g_rev = 0;
std::string g_status = "No save scan yet";
std::atomic<bool> g_scanning{false};

}  // namespace

bool EnumerateSaves(std::vector<SaveInfo>& out) { return Scan(out); }

bool SlotExists(const std::wstring& slot) {
    if (slot.empty()) return false;
    const std::wstring path = save_folder::FolderPath(slot);
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(std::filesystem::path(path), ec);
}

bool CreateNamedSave(const std::wstring& name, uint8_t mode, std::wstring& outSlot) {
    outSlot.clear();
    if (name.empty()) { UE_LOGW("save_browser: CreateNamedSave -- empty name"); return false; }
    if (SlotExists(name)) {
        UE_LOGW("save_browser: CreateNamedSave -- save '%ls' already exists (name taken)", name.c_str());
        return false;
    }
    void* gi = world_singleton::GameInstance();
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"createSaveSlotTreeDirectory") : nullptr;
    if (!gi || !fn) {
        UE_LOGW("save_browser: CreateNamedSave -- unresolved (gi=%p createSaveSlotTreeDirectory=%p)", gi, fn);
        return false;
    }
    // The game stamps the new metadata's gamemode_type from the GameInstance's mode, which its menu
    // set when the player picked the mode; the same is done here.
    ue_wrap::game_mode::WriteTo(gi, mode);
    std::wstring buf = name;
    R::FString fs{buf.data(), static_cast<int32_t>(buf.size()) + 1, static_cast<int32_t>(buf.size()) + 1};
    ParamFrame f(fn);
    f.SetRaw(L"SlotName", &fs, sizeof(fs));
    f.Set<void*>(L"__WorldContext", gi);
    if (!Call(lib, f) || !f.Get<bool>(L"return")) {
        UE_LOGW("save_browser: CreateNamedSave -- createSaveSlotTreeDirectory('%ls') failed", name.c_str());
        return false;
    }
    outSlot = name;
    UE_LOGI("save_browser: CreateNamedSave -- created + persisted save folder '%ls' (mode=%u)", name.c_str(),
            static_cast<unsigned>(mode));
    return true;
}

bool CreateNamedSaveUnique(const std::wstring& baseName, uint8_t mode, std::wstring& outSlot) {
    outSlot.clear();
    if (baseName.empty()) {
        UE_LOGW("save_browser: CreateNamedSaveUnique -- empty base name");
        return false;
    }
    // Numbered from two, the way a save list numbers a repeat: the first world keeps the bare
    // name. The cap refuses rather than wrapping, since reusing a name at the end of the range
    // would overwrite a world somebody played.
    for (int n = 1; n <= 99; ++n) {
        std::wstring name = baseName;
        if (n > 1) name += L" " + std::to_wstring(n);
        if (SlotExists(name)) continue;
        return CreateNamedSave(name, mode, outSlot);
    }
    UE_LOGW("save_browser: CreateNamedSaveUnique -- '%ls' and 98 numbered variants all exist", baseName.c_str());
    return false;
}

void RefreshAsync() {
    if (g_scanning.exchange(true)) return;  // a scan is already in flight
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = "Scanning saves...";
    }
    GT::Post([] {
        std::vector<SaveInfo> v;
        const bool ok = Scan(v);
        std::lock_guard<std::mutex> lk(g_mu);
        if (ok) {
            g_cache.swap(v);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu save%s", g_cache.size(), g_cache.size() == 1 ? "" : "s");
            g_status = buf;
        } else {
            g_status = "Save system not ready (try again)";
        }
        ++g_rev;
        // Cleared inside the lock, after rev/cache/status are coherent, so a render-thread
        // RefreshAsync that sees g_scanning==false also sees the finished scan.
        g_scanning.store(false, std::memory_order_release);
    });
}

uint64_t CopySaves(std::vector<SaveInfo>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_cache;
    return g_rev;
}

std::wstring GameVersion() {
    std::lock_guard<std::mutex> lk(g_versionMu);
    return g_gameVersion;
}

std::string Status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

}  // namespace ue_wrap::save_browser
