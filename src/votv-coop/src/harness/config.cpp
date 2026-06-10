// harness/config.cpp -- env + ini configuration readers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).

#include "harness/config.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::string ReadIniValue(const char* key, const char* def) {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return def;
    const std::string prefix = std::string(key) + "=";
    std::string result = def;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
                s.end());
        if (s.rfind(prefix, 0) == 0) { result = s.substr(prefix.size()); break; }
    }
    std::fclose(f);
    return result;
}

void WriteIniValue(const char* key, const char* value) {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    const std::string prefix = std::string(key) + "=";
    // Scrub CR/LF from the value: an embedded newline (e.g. pasted into the name field)
    // would split the "key=value" line and corrupt the NEXT key on the read-back. ReadIniValue
    // already strips whitespace on read, so the value can't legitimately contain a newline.
    std::string safe;
    for (const char* p = value; *p; ++p)
        if (*p != '\n' && *p != '\r') safe.push_back(*p);
    value = safe.c_str();
    // Read existing lines, replacing the key's line IN PLACE if present (so we keep
    // the rest of the ini -- sections, comments, other keys -- untouched).
    std::vector<std::string> lines;
    bool found = false;
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"r") == 0 && f) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                std::string s(line);
                std::string stripped = s;
                stripped.erase(std::remove_if(stripped.begin(), stripped.end(),
                                              [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
                               stripped.end());
                if (!found && stripped.rfind(prefix, 0) == 0) {
                    lines.push_back(prefix + value + "\n");
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
        lines.push_back(prefix + value + "\n");
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
        UE_LOGW("config: WriteIniValue('%s') could not open votv-coop.ini for write", key);
        return;
    }
    for (const auto& l : lines) std::fputs(l.c_str(), f);
    std::fclose(f);
    UE_LOGI("config: persisted %s=%s", key, value);
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
static constexpr const char* kBuiltinMasterUrl    = "87.121.218.33:10001";
static constexpr const char* kBuiltinSignalingUrl = "87.121.218.33:10000";

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

    UE_LOGI("config: P2P fields -- identity='%s' host='%s' signaling='%s' stun='%s'",
            c.localIdentity.c_str(), c.hostIdentity.c_str(),
            c.signalingUrl.c_str(), c.stunList.c_str());
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
    if (UseCustomNetMaster()) return ReadIniValue("net.master", kBuiltinMasterUrl);
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

}  // namespace harness::config
