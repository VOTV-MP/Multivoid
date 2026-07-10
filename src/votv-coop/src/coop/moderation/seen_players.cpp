// coop/moderation/seen_players.cpp -- see coop/moderation/seen_players.h.

#include "coop/moderation/seen_players.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/session/player_handshake.h"
#include "coop/config/config.h"                // ModuleDir -- the file lives next to the DLL
#include "ue_wrap/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace coop::seen_players {
namespace {

struct Record {
    std::string nick;
    std::string ip;
    long long   lastSeenUnix = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, Record> g_records;  // keyed by GUID; guarded by g_mutex
// slot -> guid of the peer currently online in that slot ("" = none). Guarded by
// g_mutex too: written by game-thread join/disconnect edges, read by GetSnapshot.
std::array<std::string, coop::players::kMaxPeers> g_onlineGuidBySlot{};

fs::path RegistryPath() {
    const std::wstring dir = coop::config::ModuleDir();
    if (dir.empty()) return {};
    return fs::path(dir) / L"votv-coop-players.txt";
}

// Field sanitizer for the pipe-separated line format: the separators and line
// breaks must never appear inside a field (nick is ASCII-sanitized upstream, but
// the format guards itself regardless).
std::string CleanField(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out)
        if (c == '|' || c == '\n' || c == '\r') c = '_';
    return out;
}

// Rewrite the whole file from memory (the ban_list discipline: the registry is
// small, a full rewrite keeps disk == memory exactly). Caller holds g_mutex.
// Best-effort: a write failure warns but leaves the in-memory record in force.
void WriteFileLocked() {
    const fs::path path = RegistryPath();
    if (path.empty()) {
        UE_LOGW("seen_players: module dir unresolved -- registry not persisted");
        return;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        UE_LOGW("seen_players: cannot open '%ls' for write -- registry not persisted",
                path.c_str());
        return;
    }
    f << "# VOTV coop seen-players registry -- one player per line: guid|nick|lastSeenUnix|ip\n";
    for (const auto& [guid, rec] : g_records)
        f << guid << '|' << rec.nick << '|' << rec.lastSeenUnix << '|' << rec.ip << '\n';
}

void FillEntryLocked(const std::string& guid, const Record& rec, Entry& e) {
    std::snprintf(e.guid, sizeof(e.guid), "%s", guid.c_str());
    std::snprintf(e.nick, sizeof(e.nick), "%s", rec.nick.c_str());
    std::snprintf(e.ip, sizeof(e.ip), "%s", rec.ip.c_str());
    e.lastSeenUnix = rec.lastSeenUnix;
    e.online = false;
    for (const auto& g : g_onlineGuidBySlot)
        if (!g.empty() && g == guid) { e.online = true; break; }
}

}  // namespace

void Load() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_records.clear();
    const fs::path path = RegistryPath();
    if (path.empty()) return;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        UE_LOGI("seen_players: no registry file yet (%ls) -- starting empty", path.c_str());
        return;
    }
    std::ifstream f(path);
    if (!f) {
        UE_LOGW("seen_players: cannot open '%ls' for read", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Parse "guid|nick|lastSeenUnix|ip". Lenient like ban_list: guid required,
        // the rest default empty/0.
        const size_t p1 = line.find('|');
        const std::string guid = line.substr(0, p1);
        if (guid.empty() || p1 == std::string::npos) continue;
        Record rec;
        const size_t p2 = line.find('|', p1 + 1);
        rec.nick = line.substr(p1 + 1, (p2 == std::string::npos) ? std::string::npos
                                                                 : p2 - (p1 + 1));
        if (p2 != std::string::npos) {
            const size_t p3 = line.find('|', p2 + 1);
            const std::string t = line.substr(
                p2 + 1, (p3 == std::string::npos) ? std::string::npos : p3 - (p2 + 1));
            if (!t.empty()) { try { rec.lastSeenUnix = std::stoll(t); } catch (...) {} }
            if (p3 != std::string::npos) rec.ip = line.substr(p3 + 1);
        }
        g_records[guid] = std::move(rec);
    }
    UE_LOGI("seen_players: loaded %zu known player(s) from %ls", g_records.size(),
            path.c_str());
}

void TouchOnJoin(coop::net::Session& session, int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // Game-thread reads (player_handshake owns these strings on the game thread).
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("seen_players: slot %d joined without a GUID -- not registered", peerSlot);
        return;
    }
    // Narrow the (already-sanitized, ASCII) nick.
    const std::wstring& wnick = coop::player_handshake::NicknameForSlot(peerSlot);
    std::string nick;
    nick.reserve(wnick.size());
    for (wchar_t c : wnick) nick.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '?');

    char ip[64] = {};
    session.GetPeerAddress(peerSlot, ip, sizeof(ip));

    std::lock_guard<std::mutex> lk(g_mutex);
    Record& rec = g_records[guid];
    rec.nick = CleanField(nick.c_str());
    if (ip[0]) rec.ip = CleanField(ip);  // keep the previous IP if unresolvable now
    rec.lastSeenUnix = static_cast<long long>(::time(nullptr));
    g_onlineGuidBySlot[peerSlot] = guid;
    WriteFileLocked();
    UE_LOGI("seen_players: slot %d registered (nick='%s' ip=%s) -- %zu known",
            peerSlot, rec.nick.c_str(), rec.ip.empty() ? "?" : rec.ip.c_str(),
            g_records.size());
}

void OnSlotDisconnected(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string& guid = g_onlineGuidBySlot[peerSlot];
    if (guid.empty()) return;  // never registered (client role / GUID-less peer)
    auto it = g_records.find(guid);
    if (it != g_records.end()) {
        it->second.lastSeenUnix = static_cast<long long>(::time(nullptr));
        WriteFileLocked();
    }
    guid.clear();
}

void OnSessionStart() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& g : g_onlineGuidBySlot) g.clear();
}

void GetSnapshot(std::vector<Entry>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_mutex);
    out.reserve(g_records.size());
    for (const auto& [guid, rec] : g_records) {
        Entry e;
        FillEntryLocked(guid, rec, e);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.lastSeenUnix > b.lastSeenUnix;  // most recently seen first
    });
}

bool FindByGuid(const char* guid, Entry& out) {
    if (!guid || !guid[0]) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_records.find(guid);
    if (it == g_records.end()) return false;
    FillEntryLocked(it->first, it->second, out);
    return true;
}

}  // namespace coop::seen_players
