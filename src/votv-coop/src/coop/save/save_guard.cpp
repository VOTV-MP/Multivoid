// coop/save_guard.cpp -- see coop/save_guard.h.

#include "coop/save/save_guard.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace coop::save_guard {

fs::path SaveGamesDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf) / L"VotV" / L"Saved" / L"SaveGames";
}

namespace {

// Once-per-process latch: a process hosts at most one coop session in practice;
// if it ever re-hosts, the pre-session snapshot from the first host start is the
// meaningful one (taken before ANY coop state existed).
std::atomic<bool> g_done{false};

// Bound disk use: keep only the newest N timestamped backups.
constexpr int kKeepBackups = 3;

std::wstring TimestampNow() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u%02u%02u_%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Keep only the newest kKeepBackups timestamped dirs under coop_backup\.
// Names are zero-padded timestamps, so lexicographic sort == chronological.
void PruneOld(const fs::path& backupRoot) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::directory_iterator(backupRoot, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    if (dirs.size() <= static_cast<size_t>(kKeepBackups)) return;
    std::sort(dirs.begin(), dirs.end());
    const size_t toRemove = dirs.size() - static_cast<size_t>(kKeepBackups);
    for (size_t i = 0; i < toRemove; ++i) {
        std::error_code rec;
        fs::remove_all(dirs[i], rec);
    }
}

}  // namespace

void BackupSaveOnSessionStart() {
    bool expected = false;
    if (!g_done.compare_exchange_strong(expected, true)) return;  // once per process

    const fs::path src = SaveGamesDir();
    if (src.empty()) {
        UE_LOGW("save_guard: LOCALAPPDATA unresolved -- skipping pre-session save backup");
        return;
    }
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) {
        UE_LOGW("save_guard: SaveGames dir '%ls' not found -- skipping backup "
                "(new game / no save yet?)", src.c_str());
        return;
    }

    const fs::path backupRoot = src / L"coop_backup";
    const fs::path dst = backupRoot / TimestampNow();
    fs::create_directories(dst, ec);
    if (ec) {
        UE_LOGW("save_guard: cannot create backup dir '%ls' (%s) -- skipping",
                dst.c_str(), ec.message().c_str());
        return;
    }

    // Copy every top-level entry EXCEPT coop_backup itself (never recurse into
    // our own backups). Covers <slot>.sav, data.sav, b_*.sav, and the _bin\ asset
    // dir. Best-effort per entry: a single failed copy warns but does not abort
    // the rest -- a partial backup still beats none.
    size_t files = 0;
    uintmax_t bytes = 0;
    for (auto it = fs::directory_iterator(src, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (p.filename() == L"coop_backup") continue;
        std::error_code cec;
        if (it->is_directory(cec)) {
            fs::copy(p, dst / p.filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, cec);
        } else if (it->is_regular_file(cec)) {
            const uintmax_t sz = fs::file_size(p, cec);
            fs::copy_file(p, dst / p.filename(), fs::copy_options::overwrite_existing, cec);
            if (!cec) { ++files; bytes += sz; }
        }
        if (cec) {
            UE_LOGW("save_guard: copy of '%ls' failed (%s) -- backup may be incomplete",
                    p.filename().c_str(), cec.message().c_str());
        }
    }

    UE_LOGI("save_guard: pre-session save backup -> '%ls' (%zu file(s), %.1f MB)",
            dst.c_str(), files, static_cast<double>(bytes) / (1024.0 * 1024.0));
    PruneOld(backupRoot);
}

}  // namespace coop::save_guard
