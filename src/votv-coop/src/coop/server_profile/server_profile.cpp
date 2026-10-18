// coop/server_profile/server_profile.cpp -- the hosted server's folder: the id rule and the
// creation at a host session start. See server_profile.h.

#include "coop/server_profile/server_profile.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/mod_data/legacy_note.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <filesystem>
#include <system_error>

namespace coop::server_profile {
namespace {

constexpr size_t kMaxIdBytes = 24;
constexpr const char* kFallbackId = "server";

// A Windows device name opens the device, whatever folder it is asked for in.
bool IsDeviceName(std::string_view s) {
    if (s == "con" || s == "prn" || s == "aux" || s == "nul") return true;
    if (s.size() == 4 && (s.substr(0, 3) == "com" || s.substr(0, 3) == "lpt"))
        return s[3] >= '1' && s[3] <= '9';
    return false;
}

bool IsIdByte(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

}  // namespace

std::wstring ServersDir() {
    const std::wstring data = ue_wrap::paths::ModDataDir();
    if (data.empty()) return {};
    return data + L"\\multivoid_servers";
}

bool IsValidId(std::string_view id) {
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    if (id.front() == '-' || id.back() == '-') return false;
    for (char c : id)
        if (!IsIdByte(c)) return false;
    return !IsDeviceName(id);
}

std::string IdFromName(std::string_view nameUtf8) {
    std::string out;
    bool pendingDash = false;
    for (unsigned char u : nameUtf8) {
        char c = 0;
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u + ('a' - 'A'));
        else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) c = static_cast<char>(u);
        if (c == 0) {
            pendingDash = true;
            continue;
        }
        if (pendingDash && !out.empty()) out.push_back('-');
        pendingDash = false;
        out.push_back(c);
    }
    if (out.size() > kMaxIdBytes) out.resize(kMaxIdBytes);
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) return kFallbackId;
    if (IsDeviceName(out)) out += "-server";
    return out;
}

HostingId IdForHosting(std::string_view hostNickUtf8) {
    HostingId h;
    h.rowText = coop::config::ResolveString(coop::config_registry::rows::net_server);
    if (h.rowText.empty()) {
        h.id = IdFromName(hostNickUtf8);
        h.fromNick = true;
    } else if (IsValidId(h.rowText)) {
        h.id = h.rowText;
    } else {
        // MTA refuses an invalid name or falls back to a default (CMainConfig.cpp global_databases_path,
        // CResourceManager.cpp illegal resource names); we sanitise it for the session and still host:
        // the player's text stays theirs.
        h.id = IdFromName(h.rowText);
        h.handTyped = true;
    }
    return h;
}

std::wstring EnsureHosted(std::string_view hostNickUtf8) {
    // The server profile of an older build, beside the exe, is read by nothing now.
    coop::legacy_note::Once(coop::legacy_note::LegacyFile::Servers, L"multivoid_servers");

    const HostingId h = IdForHosting(hostNickUtf8);
    const std::string& id = h.id;

    if (h.fromNick) {
        switch (coop::config::SetValue(coop::config_registry::rows::net_server, id.c_str())) {
        case coop::config::SetResult::Saved:
            UE_LOGI("server: net.server set to '%s' (the first host start)", id.c_str());
            break;
        case coop::config::SetResult::HeldNotSaved:
            UE_LOGW("server: net.server='%s' could not be saved to multivoid.ini; this session uses it",
                    id.c_str());
            break;
        case coop::config::SetResult::Refused:
            UE_LOGE("server: net.server='%s' was refused by the config -- no server folder this session",
                    id.c_str());
            return {};
        }
    }
    if (h.handTyped) {
        UE_LOGW("server: net.server='%s' is not a server id (a-z, 0-9 and '-', at most 24); hosting '%s' "
                "-- write that in multivoid.ini to silence this",
                h.rowText.c_str(), id.c_str());
    }

    const std::wstring root = ServersDir();
    if (root.empty()) {
        UE_LOGE("server: the mod has no data folder, so no server folder for '%s' -- this session's "
                "server stores live in memory and are not saved",
                id.c_str());
        return {};
    }

    // The error_code overload: a refused write is a state to report, never an exception.
    const std::filesystem::path dir = std::filesystem::path(root) / std::filesystem::path(id);
    std::error_code ec;
    const bool created = std::filesystem::create_directories(dir, ec);
    if (ec) {
        UE_LOGE("server: could not create %ls (%s) -- this session's server stores will not persist",
                dir.c_str(), ec.message().c_str());
        return {};
    }
    UE_LOGI("server: hosting '%s' at %ls (%s)", id.c_str(), dir.c_str(), created ? "created" : "existing");
    return dir.wstring();
}

}  // namespace coop::server_profile
