// coop/moderation/ban_list.cpp -- see coop/moderation/ban_list.h.

#include "coop/moderation/ban_list.h"

#include "ue_wrap/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace coop::ban_list {
namespace {

struct Record {
    std::string nick;
    std::string reason;
    long long   bannedUnix = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, Record> g_bans;  // keyed by IP; guarded by g_mutex

// Banlist lives next to the deployed mod binary -- the same Binaries\Win64
// directory as votv-coop.dll / votv-coop.ini / votv-coop.log (resolved via our
// own module handle, exactly like coop::config::ModuleDir). Each game copy keeps
// its OWN banlist; the host copy's file is the canonical one (clients never
// load it). File name follows the votv-coop-* convention.
fs::path BanlistPath() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&BanlistPath), &self);
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(path).parent_path() / L"votv-coop-banlist.txt";
}

// Rewrite the whole file from the in-memory set. Caller holds g_mutex. The file
// is small (a banlist, not a log), so a full rewrite on each Add is simplest and
// keeps disk == memory exactly (no append-only drift / dup lines). Best-effort:
// a write failure warns but leaves the in-memory ban in force for the session.
void WriteFileLocked() {
    const fs::path path = BanlistPath();
    if (path.empty()) {
        UE_LOGW("ban_list: LOCALAPPDATA unresolved -- ban not persisted (in-memory only)");
        return;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);  // ok if it already exists
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        UE_LOGW("ban_list: cannot open '%ls' for write -- ban not persisted", path.c_str());
        return;
    }
    f << "# VOTV coop banlist -- one ban per line: ip|nick|unixtime|reason\n";
    for (const auto& [ip, rec] : g_bans) {
        f << ip << '|' << rec.nick << '|' << rec.bannedUnix << '|' << rec.reason << '\n';
    }
}

// Field sanitizer for the pipe-separated format: separators/line breaks must
// never appear inside a stored field (reason is admin-typed free text).
std::string CleanField(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out)
        if (c == '|' || c == '\n' || c == '\r') c = '_';
    return out;
}

}  // namespace

void Load() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_bans.clear();
    const fs::path path = BanlistPath();
    if (path.empty()) return;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        UE_LOGI("ban_list: no banlist file yet (%ls) -- starting empty", path.c_str());
        return;
    }
    std::ifstream f(path);
    if (!f) {
        UE_LOGW("ban_list: cannot open '%ls' for read", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Parse "ip|nick|unixtime|reason". Be lenient: nick/reason may be empty
        // (pre-reason files have 3 fields); a missing time defaults to 0. Only
        // the IP (field 0) is required.
        const size_t p1 = line.find('|');
        const std::string ip = line.substr(0, p1);
        if (ip.empty()) continue;
        std::string nick, reason;
        long long when = 0;
        if (p1 != std::string::npos) {
            const size_t p2 = line.find('|', p1 + 1);
            nick = line.substr(p1 + 1, (p2 == std::string::npos) ? std::string::npos : p2 - (p1 + 1));
            if (p2 != std::string::npos) {
                const size_t p3 = line.find('|', p2 + 1);
                const std::string t = line.substr(
                    p2 + 1, (p3 == std::string::npos) ? std::string::npos : p3 - (p2 + 1));
                if (!t.empty()) { try { when = std::stoll(t); } catch (...) { when = 0; } }
                if (p3 != std::string::npos) reason = line.substr(p3 + 1);
            }
        }
        g_bans[ip] = Record{ nick, reason, when };
    }
    UE_LOGI("ban_list: loaded %zu banned IP(s) from %ls", g_bans.size(), path.c_str());
}

bool IsBanned(const char* ip) {
    if (!ip || !ip[0]) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_bans.find(ip) != g_bans.end();
}

void Add(const char* ip, const char* nick, const char* reason) {
    if (!ip || !ip[0]) {
        UE_LOGW("ban_list: Add called with empty IP -- ignored");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    Record rec;
    rec.nick = CleanField(nick);
    rec.reason = CleanField(reason);
    rec.bannedUnix = static_cast<long long>(::time(nullptr));
    g_bans[ip] = rec;
    WriteFileLocked();
    UE_LOGI("ban_list: banned IP %s (nick='%s' reason='%s') -- %zu total",
            ip, rec.nick.c_str(), rec.reason.c_str(), g_bans.size());
}

bool Remove(const char* ip) {
    if (!ip || !ip[0]) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    const auto it = g_bans.find(ip);
    if (it == g_bans.end()) return false;
    UE_LOGI("ban_list: unbanned IP %s (nick='%s') -- %zu remain",
            ip, it->second.nick.c_str(), g_bans.size() - 1);
    g_bans.erase(it);
    WriteFileLocked();
    return true;
}

void GetSnapshot(std::vector<Entry>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_mutex);
    out.reserve(g_bans.size());
    for (const auto& [ip, rec] : g_bans) {
        Entry e;
        std::snprintf(e.ip, sizeof(e.ip), "%s", ip.c_str());
        std::snprintf(e.nick, sizeof(e.nick), "%s", rec.nick.c_str());
        std::snprintf(e.reason, sizeof(e.reason), "%s", rec.reason.c_str());
        e.bannedUnix = rec.bannedUnix;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.bannedUnix > b.bannedUnix;  // most recent ban first
    });
}

int Count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return static_cast<int>(g_bans.size());
}

}  // namespace coop::ban_list
