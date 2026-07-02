// harness/config.cpp -- env + ini configuration readers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).

#include "harness/config.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/skin_registry.h"  // IsValidSkinName + kDefaultSkinName (v93 player_skin=)
#include "ue_wrap/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace harness::config {

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

std::string ReadScenario() {
    // The TEST-launch signal is the PROCESS-SCOPED env var VOTVCOOP_SCENARIO
    // (set by mp.py / play-coop.bat / lan-test). A NATIVE launch (double-click /
    // Steam) inherits no such env -> it falls through to "menu": boot to VOTV's
    // own main menu, where the MULTIPLAYER button (server browser + Host-Game
    // save picker) drives coop. NO auto-load into gameplay on a native launch.
    //
    // RETIRED (2026-06-06, RULE 1 root cause / RULE 2 no leak-prone parallel
    // mechanism): the old on-disk `scenario.txt` fallback. A test launcher wrote
    // scenario.txt="play" INTO THE GAME DIR, and it survived on disk -- so the
    // NEXT native VotV.exe launch read the leftover file and auto-loaded straight
    // into gameplay (user-reported 2026-06-06). A per-launch mode MUST use a
    // per-launch signal (env), never a file that aliases later native launches.
    const std::string env = ReadEnv("VOTVCOOP_SCENARIO");
    return env.empty() ? "menu" : env;
}

// Trim leading/trailing whitespace (space, tab, CR, LF). The VALUE side of a key=value
// line keeps its INTERIOR spaces verbatim: audio device names ("Voicemeeter Out B1
// (VB-Audio ...)") are matched by substring against the enumerated device list, and the
// old strip-ALL-whitespace read mangled them into never-matching strings, silently
// falling back to the default device (the 2026-06-12 voice-inaudible root cause #1).
// Keys themselves never contain spaces, so edge-trimmed key equality is exact.
static std::string TrimEdges(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Split a raw ini line at the first '=' into an edge-trimmed key and an edge-trimmed,
// interior-verbatim value. False for lines without '=' or with an empty key (comments,
// blanks, section headers fall out naturally -- '#'/';' never equals a real key).
static bool ParseIniLine(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = TrimEdges(line.substr(0, eq));
    value = TrimEdges(line.substr(eq + 1));
    return !key.empty();
}

// One lock for every votv-coop.ini access in this process. Writers come from TWO
// threads (render: skins-panel RequestSkin / voice-panel device save; game: boot
// default-writes) -- an unserialized read-modify-write pair can interleave and one
// writer rebuilds the file from the other's half-written state. Readers take it too
// so a read never observes the (pre-atomic-rename) transition.
static std::mutex g_iniMutex;

std::string ReadIniValue(const char* key, const char* def) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return def;
    std::string result = def;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string k, v;
        if (ParseIniLine(line, k, v) && k == key) { result = v; break; }
    }
    std::fclose(f);
    return result;
}

void WriteIniValue(const char* key, const char* value) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    // Scrub CR/LF from the value (an embedded newline -- e.g. pasted into a text field --
    // would split the "key=value" line and corrupt the NEXT key on read-back), then
    // edge-trim. Interior spaces are part of the value (device names) and round-trip
    // verbatim through ReadIniValue's parse.
    std::string safe;
    for (const char* p = value; *p; ++p)
        if (*p != '\n' && *p != '\r') safe.push_back(*p);
    safe = TrimEdges(safe);
    const std::string newLine = std::string(key) + "=" + safe + "\n";
    // Read existing lines, replacing the key's line IN PLACE if present (so we keep
    // the rest of the ini -- sections, comments, other keys -- untouched).
    //
    // DESTRUCTION GUARDS (born 2026-07-02: the HOST's ini lost everything above its
    // last-appended keys -- [dev] header, devkeys=1, enabled=1 -- and the F1 dev menu
    // silently vanished; the file had been rebuilt from appends after an obliteration):
    //   1. if the ini EXISTS but cannot be opened for read (editor/AV/backup holding a
    //      lock), ABORT the write -- the old code carried on with an EMPTY line list
    //      and truncated the whole file down to the one new key;
    //   2. the new content goes to votv-coop.ini.new, then MoveFileExW REPLACE_EXISTING
    //      swaps it in ATOMICALLY -- the old truncate-then-write left a window (crash,
    //      kill, power) where the file on disk was empty/partial.
    std::vector<std::string> lines;
    bool found = false;
    {
        FILE* f = nullptr;
        errno_t rc = 1;
        for (int attempt = 0; attempt < 5; ++attempt) {   // transient sharing locks
            rc = _wfopen_s(&f, path.c_str(), L"r");
            if (rc == 0 && f) break;
            // Existence re-checked PER ATTEMPT (not a pre-loop snapshot): an ini
            // created between a stale snapshot and a transiently-failing open must
            // not take the "fresh file" path and get rebuilt down to one key.
            if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
            ::Sleep(20);
        }
        if ((rc != 0 || !f) &&
            ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            UE_LOGW("config: WriteIniValue('%s') SKIPPED -- votv-coop.ini exists but is "
                    "locked for read; refusing to rebuild the file blind", key);
            return;
        }
        if (rc == 0 && f) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                std::string s(line);
                std::string k, v;
                if (!found && ParseIniLine(s, k, v) && k == key) {
                    lines.push_back(newLine);
                    found = true;
                } else {
                    lines.push_back(s);
                }
            }
            std::fclose(f);
        }
    }
    if (!found) {
        // Make sure the appended key sits on its own line even if the file's last
        // line had no trailing newline.
        if (!lines.empty() && !lines.back().empty() && lines.back().back() != '\n')
            lines.back() += "\n";
        lines.push_back(newLine);
    }
    const std::wstring tmp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") != 0 || !f) {
        UE_LOGW("config: WriteIniValue('%s') could not open votv-coop.ini.new for write", key);
        return;
    }
    // Every write checked BEFORE the swap: a disk-full/IO-error .new must never be
    // atomically installed over the good ini (that would be the one data-loss path
    // this whole function exists to close -- audit 2026-07-02).
    bool wrote = true;
    for (const auto& l : lines)
        if (std::fputs(l.c_str(), f) == EOF) { wrote = false; break; }
    if (std::ferror(f)) wrote = false;
    if (std::fclose(f) != 0) wrote = false;
    if (!wrote) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: WriteIniValue('%s') writing votv-coop.ini.new FAILED (disk?) -- "
                "ini left unchanged", key);
        return;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        UE_LOGW("config: WriteIniValue('%s') atomic swap failed (err=%lu) -- ini left "
                "unchanged, votv-coop.ini.new kept", key, ::GetLastError());
        return;
    }
    UE_LOGI("config: persisted %s=%s", key, safe.c_str());
}

// ---- Built-in (hardcoded) public net endpoints -- our VPS -----------------------
// A fresh install with NO votv-coop.ini reaches these out of the box: the menu server
// browser + Host-Game flow hit the real master, which then mints the per-session
// signaling token + STUN + ephemeral TURN creds. These are PUBLIC connection endpoints
// (the master IP:port is advertised to every client), NOT secrets -- the signaling
// TOKEN, the TURN secret, and the SSH/ops creds are deliberately NOT compiled in (they
// stay in the local-only ini, or the master mints them per session; only net.master is
// strictly required for the normal flow). The `net.master.custom=1` ini gate opts OUT
// of these and uses the ini's own net.master / net.signaling (run-your-own-master).
// The constants live in coop/net/protocol.h (kOfficial*Url) -- shared with the UI
// display mask that prints "DEFAULT" instead of the raw VPS address.
static constexpr const char* kBuiltinMasterUrl    = coop::net::kOfficialMasterUrl;
static constexpr const char* kBuiltinSignalingUrl = coop::net::kOfficialSignalingUrl;

// The custom-master gate. net.master.custom = 1/true/yes/on opts out of the hardcoded
// VPS endpoints and uses the ini's net.master / net.signaling instead. Default OFF ->
// the built-in VPS endpoints win (a stale net.master in the ini is ignored unless the
// gate is set), which is what makes a no-config native install Just Work. An env
// override (VOTVCOOP_MASTER_URL / VOTVCOOP_NET_SIGNALING) always takes precedence over
// both (the dev / LAN-test framework).
static bool UseCustomNetMaster() {
    std::string g = ReadIniValue("net.master.custom", "0");
    for (char& c : g) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');  // case-insensitive
    return g == "1" || g == "true" || g == "yes" || g == "on";
}

// Fill the P2P (rungs 1-3) transport fields of `c` from env -> ini -> default.
// Shared by ReadNetConfig (when net.topology=p2p) AND ReadP2PHostFallback (the
// menu Host-Game master-unreachable fallback), so the env/ini key set lives in
// ONE place (RULE 2). Uses c.role to pick the identity default (host vs client).
static void FillP2PFields(coop::net::Config& c) {
    // Signaling rendezvous server. Both peers connect OUTBOUND -- no host port-forward.
    // Precedence mirrors the master: env -> custom-master gate (ini net.signaling) ->
    // the built-in VPS signaling. (The signaling TOKEN stays ini/master-minted -- never
    // hardcoded; in the normal master-up flow the master overrides this URL+token per
    // session, so this default only seeds the master-down fallback.)
    std::string sig = ReadEnv("VOTVCOOP_NET_SIGNALING");
    if (sig.empty())
        sig = UseCustomNetMaster() ? ReadIniValue("net.signaling", kBuiltinSignalingUrl)
                                   : std::string(kBuiltinSignalingUrl);
    c.signalingUrl = sig;
    std::string sigtok = ReadEnv("VOTVCOOP_NET_SIGNALING_TOKEN");
    c.signalingToken = sigtok.empty() ? ReadIniValue("net.signaling_token", "") : sigtok;

    // This peer's own signaling identity. Defaults give a working 2-peer
    // test out of the box (host="votvhost", client="votvclient"); a real
    // lobby with multiple clients MUST issue each client a UNIQUE identity
    // (the signaling server registers one connection per identity string).
    std::string ident = ReadEnv("VOTVCOOP_NET_IDENTITY");
    if (ident.empty()) ident = ReadIniValue("net.identity", "");
    if (ident.empty()) {
        if (c.role == coop::net::Role::Host) {
            ident = "votvhost";
        } else {
            // Unique per-process default so two un-configured clients don't
            // COLLIDE on the signaling server -- it registers one connection
            // per identity string, and a duplicate evicts the incumbent
            // (silently breaking the first client). The master server issues
            // real per-peer identities later; this keeps the default safe for
            // ad-hoc multi-client tests. "votvclient-XXXX" = 15 chars (<= the
            // 31-char SetGenericString cap).
            std::random_device rd;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "votvclient-%04x",
                          static_cast<unsigned>(rd() & 0xFFFFu));
            ident = buf;
        }
    }
    c.localIdentity = ident;

    // The host identity a client dials (must equal the host's localIdentity).
    std::string hostId = ReadEnv("VOTVCOOP_NET_HOST_IDENTITY");
    if (hostId.empty()) hostId = ReadIniValue("net.host_identity", "");
    if (hostId.empty()) hostId = "votvhost";
    c.hostIdentity = hostId;

    // ICE candidate sources. STUN (rung 2) defaults to a public server so a
    // real cross-NAT test works; for a same-machine test ICE also connects
    // via host/LAN candidates regardless. TURN (rung 3) is off by default
    // (the master mints ephemeral REST creds; static ini creds are dev-only).
    std::string stun = ReadEnv("VOTVCOOP_NET_STUN");
    c.stunList = stun.empty() ? ReadIniValue("net.stun", "stun.l.google.com:19302") : stun;
    std::string turn = ReadEnv("VOTVCOOP_NET_TURN");
    c.turnList = turn.empty() ? ReadIniValue("net.turn", "") : turn;
    std::string turnUser = ReadEnv("VOTVCOOP_NET_TURN_USER");
    c.turnUser = turnUser.empty() ? ReadIniValue("net.turn_user", "") : turnUser;
    std::string turnPass = ReadEnv("VOTVCOOP_NET_TURN_PASS");
    c.turnPass = turnPass.empty() ? ReadIniValue("net.turn_pass", "") : turnPass;

    // ICE candidate policy: "" / "all" (default) / "relay" / "disable" /
    // "default". "relay" forces the TURN relay path (privacy, or to validate
    // coturn end-to-end). Mapped to IceEnable in Session::StartP2P.
    std::string ice = ReadEnv("VOTVCOOP_NET_ICE");
    c.iceMode = ice.empty() ? ReadIniValue("net.ice", "") : ice;

    // Console-visible diagnostic: any endpoint on the OFFICIAL VPS host prints
    // as "DEFAULT" -- the connect console must not advertise the raw address
    // (the session_manager DisplayMaster twin; user 2026-06-10). A custom
    // endpoint prints verbatim (its operator debugs with it).
    auto maskOfficial = [](const std::string& v) -> std::string {
        std::string host = coop::net::kOfficialMasterUrl;
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host.resize(colon);
        return v.rfind(host, 0) == 0 ? std::string("DEFAULT") : v;
    };
    UE_LOGI("config: P2P fields -- identity='%s' host='%s' signaling='%s' stun='%s'",
            c.localIdentity.c_str(), c.hostIdentity.c_str(),
            maskOfficial(c.signalingUrl).c_str(), maskOfficial(c.stunList).c_str());
}

coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    std::string role = ReadEnv("VOTVCOOP_NET_ROLE");
    if (role.empty()) role = ReadIniValue("net.role", "");
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    std::string peer = ReadEnv("VOTVCOOP_NET_PEER");
    c.peerIp = peer.empty() ? ReadIniValue("net.peer", "127.0.0.1") : peer;

    std::string port = ReadEnv("VOTVCOOP_NET_PORT");
    if (port.empty()) port = ReadIniValue("net.port", "");
    if (!port.empty()) {
        // strtoul returns unsigned long; a cast to uint16_t silently wraps
        // values >65535 to the wrong port. Range-check before commit.
        const unsigned long raw = std::strtoul(port.c_str(), nullptr, 10);
        if (raw == 0 || raw > 65535) {
            UE_LOGW("config: VOTVCOOP_NET_PORT/net.port='%s' out of [1,65535] -- "
                    "ignoring (keeping default %u)", port.c_str(), c.port);
        } else {
            c.port = static_cast<uint16_t>(raw);
        }
    }

    // --- P2P (zero-open-ports) topology --------------------------------------
    // net.topology = "lan" (default, rung 0/1 IP) or "p2p" (rungs 1-3 ICE).
    std::string topo = ReadEnv("VOTVCOOP_NET_TOPOLOGY");
    if (topo.empty()) topo = ReadIniValue("net.topology", "lan");
    c.topology = (topo == "p2p" || topo == "P2P")
                     ? coop::net::Topology::P2P
                     : coop::net::Topology::LanDirect;

    if (c.topology == coop::net::Topology::P2P) FillP2PFields(c);

    return c;
}

std::string ReadMasterUrl() {
    // Master/lobby server "host:port". Precedence: env (LAN-test framework) -> the
    // custom-master gate (net.master.custom=1 -> ini net.master) -> the BUILT-IN VPS
    // endpoint. A native launch has no env and (by default) no custom gate, so it hits
    // the hardcoded VPS master, which drives the menu server browser + Host-Game flow
    // and mints the per-session signaling/STUN/TURN creds.
    std::string m = ReadEnv("VOTVCOOP_MASTER_URL");
    if (!m.empty()) return m;
    if (UseCustomNetMaster()) {
        std::string v = ReadIniValue("net.master", kBuiltinMasterUrl);
        // "DEFAULT" sentinel (the shipped release ini): resolves to the official
        // server even under the custom gate -- the ini never needs the raw VPS
        // address spelled out.
        std::string lower = v;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (v.empty() || lower == "default") return kBuiltinMasterUrl;
        return v;
    }
    return kBuiltinMasterUrl;
}

coop::net::Config ReadP2PHostFallback() {
    // The transport Config the menu Host-Game flow uses when the master announce
    // FAILS (master down) -- so hosting NEVER silently dies on an unreachable
    // master (RULE 1 decouple). Forced P2P host; signaling/identity/stun come
    // from the same env/ini keys as the normal P2P path (the deployed ini points
    // these at the VPS). Unlisted, but the host still boots + a configured peer
    // can still join. MTA precedent: the server runs regardless of the master list.
    coop::net::Config c;
    c.role = coop::net::Role::Host;
    c.topology = coop::net::Topology::P2P;
    FillP2PFields(c);
    return c;
}

std::wstring ReadNickname() {
    std::string nick = ReadEnv("VOTVCOOP_NET_NICK");
    if (nick.empty()) nick = ReadIniValue("net.nick", "Player");
    return std::wstring(nick.begin(), nick.end());
}

std::string ReadPlayerSkin() {
    // v93 skins: the persisted body-skin choice, stored NEXT TO the player identity
    // (votv-coop.ini "player_skin=", same file as player_guid -- user 2026-07-02).
    // v95: a NEW identity (absent/malformed key) rolls a RANDOM starter from the
    // curated converter-skin list (user: "для НОВЫХ пиров случайный скин из списка"),
    // filtered to paks present on this install -- hl_einstein_v1sc when none is.
    // Persisted immediately (like the guid), so the roll happens ONCE per identity.
    std::string skin = ReadIniValue("player_skin", "");
    if (!coop::skins::IsValidSkinName(skin)) {
        skin = coop::skins::PickRandomStarterSkin();
        WriteIniValue("player_skin", skin.c_str());
        UE_LOGI("config: player_skin absent/invalid -> random starter '%s' (persisted to "
                "votv-coop.ini)", skin.c_str());
    }
    return skin;
}

std::string ReadPlayerGuid() {
    // Durable per-INSTALL player identity for the host-side per-player inventory
    // (coop_players/<guid>.json). Read from votv-coop.ini "player_guid="; generate +
    // persist on first launch / if absent or malformed. 32 lowercase hex chars (128 bits).
    // Per-install identity is the accepted tradeoff (design 2.3 "go with guid"): a reinstall
    // or a different PC = a fresh inventory unless the player_guid= line is copied over.
    std::string guid = ReadIniValue("player_guid", "");
    bool ok = guid.size() == 32;
    if (ok) {
        for (char c : guid)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; }
    }
    if (!ok) {
        // std::random_device is CSPRNG-backed on MSVC -- ample for a stable identity (no
        // crypto guarantee needed). 4x32 bits -> 32 hex chars. Avoids a bcrypt include.
        std::random_device rd;
        static const char kHex[] = "0123456789abcdef";
        guid.clear();
        guid.reserve(32);
        for (int w = 0; w < 4; ++w) {
            const uint32_t r = rd();
            for (int n = 28; n >= 0; n -= 4) guid.push_back(kHex[(r >> n) & 0xF]);
        }
        WriteIniValue("player_guid", guid.c_str());
        UE_LOGI("config: generated new player_guid=%s (persisted to votv-coop.ini)", guid.c_str());
    }
    return guid;
}

}  // namespace harness::config
