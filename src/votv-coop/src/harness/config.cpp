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

    if (c.topology == coop::net::Topology::P2P) {
        // Signaling rendezvous server (the VPS, or a local test server). Both
        // peers connect OUTBOUND -- no host port-forward.
        std::string sig = ReadEnv("VOTVCOOP_NET_SIGNALING");
        c.signalingUrl = sig.empty() ? ReadIniValue("net.signaling", "127.0.0.1:10000") : sig;
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
        // via host/LAN candidates regardless. TURN (rung 3) is off by default.
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

        UE_LOGI("config: P2P topology -- identity='%s' host='%s' signaling='%s' stun='%s'",
                c.localIdentity.c_str(), c.hostIdentity.c_str(),
                c.signalingUrl.c_str(), c.stunList.c_str());
    }

    return c;
}

std::wstring ReadNickname() {
    std::string nick = ReadEnv("VOTVCOOP_NET_NICK");
    if (nick.empty()) nick = ReadIniValue("net.nick", "Player");
    return std::wstring(nick.begin(), nick.end());
}

}  // namespace harness::config
