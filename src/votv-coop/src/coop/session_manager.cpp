// coop/session_manager.cpp -- see coop/session_manager.h.

#include "coop/session_manager.h"

#include "coop/net/lobby_announcer.h"
#include "coop/join_progress.h"
#include "coop/shutdown.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <exception>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace coop::session_manager {
namespace {

namespace net = coop::net;
namespace lobby = coop::net::lobby;

constexpr const char* kModVersion = "0.9.0-n";
// Pre-Configure seed only: the harness calls Configure() at boot with
// cfg::ReadMasterUrl() (the canonical source -> the built-in VPS endpoint or the
// net.master.custom gate), which overwrites g_masterUrl before any host/join. This
// VPS default just makes a read before Configure() (shouldn't happen) reach the right
// place instead of localhost. Keep in sync with config.cpp kBuiltinMasterUrl.
constexpr const char* kDefaultMaster = "87.121.218.33:10001";

std::string ReadEnvA(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

// LEAKED process-lifetime singletons (never destroyed): (a) no thread-join runs at
// static destruction / DLL unload -- the project forbids join-from-teardown (loader-
// lock deadlock; coop/shutdown.h), and a member-thread dtor join would do exactly
// that; (b) the detached HTTP workers' captures of these stay valid for the whole
// process life. The OS reclaims the memory at exit.
lobby::LobbyClient& Client() { static auto* c = new lobby::LobbyClient(); return *c; }
lobby::LobbyAnnouncer& Announcer() { static auto* a = new lobby::LobbyAnnouncer(); return *a; }

// Config pushed from the harness at boot (Configure): the master URL + the host
// fallback Config (used when the master announce fails). g_hostStatus is the last
// host-action result the browser surfaces. All under g_cfgMu (low contention --
// a boot write, then occasional worker-set / UI-read).
std::mutex g_cfgMu;
std::string g_masterUrl = kDefaultMaster;  // overwritten by Configure
bool g_configured = false;
net::Config g_fallbackHostCfg;
std::string g_hostStatus;
std::string g_ownLobbyId;  // our own announced lobbyId -> we never list or join it (no self-join)
std::string g_nickname = "Player";  // local display nickname (seeded from config; browser overwrites)

// One queued session start (last action wins until the harness consumes it).
std::mutex g_pendMu;
bool g_hasPending = false;
net::Config g_pending;

// One queued HOST-WITH-SAVE (the Host-Game save picker): a {Config, SaveChoice} the
// harness drains, LOADS A WORLD for, then starts. Separate from g_pending (which starts
// immediately on the already-loaded world); host-with-save must load the chosen save (or
// create the new one) FIRST, so the harness needs the save choice alongside the Config.
std::mutex g_pendHostMu;
bool g_hasPendingHost = false;
PendingHost g_pendingHost;

// Serialize the session-start actions (Host/Join/ConnectDirect): only one in flight
// at a time (you can't start two sessions at once). Refresh is NOT gated.
std::atomic<bool> g_actionBusy{false};

void QueueStart(const net::Config& cfg) {
    std::lock_guard<std::mutex> lk(g_pendMu);
    g_pending = cfg;
    g_hasPending = true;
}

// "host" or "host:port" -> host + port (default kDefaultPort if no port). IPv4 /
// hostname only (matches the existing LanDirect path; bracketed IPv6 is not parsed).
bool ParseHostPort(const std::string& in, std::string& host, uint16_t& port) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) s.pop_back();
    if (s.empty()) return false;
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) { host = s; port = net::kDefaultPort; return true; }
    host = s.substr(0, colon);
    const unsigned long raw = std::strtoul(s.c_str() + colon + 1, nullptr, 10);
    if (host.empty() || raw == 0 || raw > 65535) return false;
    port = static_cast<uint16_t>(raw);
    return true;
}

}  // namespace

void Configure(const std::string& masterUrl, const net::Config& fallbackHostCfg) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_masterUrl = masterUrl.empty() ? std::string(kDefaultMaster) : masterUrl;
    g_fallbackHostCfg = fallbackHostCfg;
    g_configured = true;
    UE_LOGI("session_manager: configured -- master='%s' fallback(signaling='%s' identity='%s')",
            g_masterUrl.c_str(), g_fallbackHostCfg.signalingUrl.c_str(),
            g_fallbackHostCfg.localIdentity.c_str());
}

std::string MasterUrl() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    if (!g_configured) {
        // Not yet Configure()'d (e.g. a direct unit probe) -- fall back to the
        // env var / localhost default so the value is still sane.
        const std::string m = ReadEnvA("VOTVCOOP_MASTER_URL");
        return m.empty() ? std::string(kDefaultMaster) : m;
    }
    return g_masterUrl;
}

void SetHostStatus(const std::string& status) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_hostStatus = status;
}

std::string HostStatus() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_hostStatus;
}

std::string OwnLobbyId() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_ownLobbyId;
}

void SetNickname(const std::string& nick) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    if (!nick.empty()) g_nickname = nick;  // ignore empty (keep the last good name)
}

std::string Nickname() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_nickname;
}

namespace {
void SetOwnLobbyId(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_ownLobbyId = id;
}
}  // namespace

const char* ModVersion() { return kModVersion; }

void Refresh() {
    Client().RefreshAsync(MasterUrl(), /*versionFilter=*/std::string());  // show all
}

uint64_t CopyRows(std::vector<lobby::LobbyRow>& out) { return Client().CopyRows(out); }
std::string Status() { return Client().Status(); }

void HostLobby(const std::string& name, const std::string& world, bool locked, int playersMax) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Host ignored"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, name, world, locked, playersMax] {
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, ModVersion(), world, locked, playersMax, 8000);
            if (info.ok && !coop::shutdown::IsShuttingDown()) {
                net::Config cfg;
                cfg.role = net::Role::Host;
                cfg.topology = net::Topology::P2P;
                cfg.localIdentity = info.hostIdentity;
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                QueueStart(cfg);
                UE_LOGI("session_manager: HOST ready -- lobby=%s identity=%s (session boot = harness Tier 2)",
                        info.lobbyId.c_str(), info.hostIdentity.c_str());
            } else if (!info.ok) {
                UE_LOGW("session_manager: HostLobby failed (master announce)");
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: HostLobby worker exception: %s", e.what());
        }
        g_actionBusy.store(false);
    }).detach();
}

void AnnounceEnvHostHidden(const std::string& name, const std::string& world) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- env announce skipped"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, name, world] {
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, ModVersion(), world,
                                 /*locked=*/false, /*playersMax=*/4, 8000);
            if (info.ok) {
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                Announcer().SetListed(false); // the hide-from-list flag, immediately
                SetHostStatus("Hosting '" + name + "' -- announced (hidden from list)");
                UE_LOGI("session_manager: env host announced HIDDEN -- lobby=%s world='%s'",
                        info.lobbyId.c_str(), world.c_str());
            } else {
                UE_LOGW("session_manager: env hidden announce failed (master unreachable) -- "
                        "hosting direct-only");
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: env announce worker exception: %s", e.what());
        }
        g_actionBusy.store(false);
    }).detach();
}

bool HostWithSave(const SaveChoice& choice, const std::string& name, bool locked, int playersMax) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- HostWithSave ignored"); return false; }
    const std::string masterUrl = MasterUrl();
    net::Config fallback;
    { std::lock_guard<std::mutex> lk(g_cfgMu); fallback = g_fallbackHostCfg; }
    std::thread([masterUrl, fallback, choice, name, locked, playersMax] {
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            // RULE 1 -- hosting must NOT depend on a reachable master. We announce
            // (best-effort) to LIST the lobby + collect master-issued signaling/TURN,
            // but EITHER WAY we queue the boot: announce-ok -> the master's P2P Config
            // (listed); announce-fail -> the LOCAL fallback Config (the deployed ini
            // -> the VPS signaling, identity "votvhost"), UNLISTED but still in-game
            // (never a silent dead-end). MTA precedent: the server runs regardless of
            // the master list. The harness then loads the world THEN StartCoopSession.
            const std::string world = choice.newGame ? choice.newName : choice.slot;
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, ModVersion(), world, locked, playersMax, 8000);
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }

            net::Config cfg;
            const bool listed = info.ok;
            if (listed) {
                cfg.role = net::Role::Host;
                cfg.topology = net::Topology::P2P;
                cfg.localIdentity = info.hostIdentity;
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
            } else {
                cfg = fallback;
                cfg.role = net::Role::Host;        // belt-and-suspenders (fallback is already host)
                cfg.topology = net::Topology::P2P;
            }
            {
                std::lock_guard<std::mutex> lk(g_pendHostMu);
                g_pendingHost.cfg = cfg;
                g_pendingHost.save = choice;
                g_pendingHost.listed = listed;
                g_hasPendingHost = true;
            }
            if (listed) {
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                SetHostStatus("Hosting '" + name + "' -- lobby listed");
                UE_LOGI("session_manager: HOST-WITH-SAVE ready (LISTED) -- lobby=%s %s='%s' (harness loads then hosts)",
                        info.lobbyId.c_str(), choice.newGame ? "newGame" : "slot", world.c_str());
            } else {
                SetHostStatus("Hosting -- master server unreachable, lobby NOT listed (LAN/direct only)");
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (UNLISTED -- master '%s' unreachable) "
                        "-- hosting via local config (signaling='%s' identity='%s')",
                        masterUrl.c_str(), cfg.signalingUrl.c_str(), cfg.localIdentity.c_str());
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: HostWithSave worker exception: %s", e.what());
            SetHostStatus(std::string("Host failed: ") + e.what());
        }
        g_actionBusy.store(false);
    }).detach();
    return true;  // accepted -- the picker raises the host-boot cover + closes
}

bool JoinLobby(const std::string& lobbyId, const std::string& displayName) {
    // FIX 3 -- never connect to our OWN lobby (the 2026-06-08 repro: the host clicked its
    // own listed server + self-joined). Reject before raising any loading state.
    if (!lobbyId.empty() && lobbyId == OwnLobbyId()) {
        UE_LOGW("session_manager: refusing to join our OWN lobby '%s' -- you are the host", lobbyId.c_str());
        SetHostStatus("That's your own server -- you're already hosting it.");
        return false;
    }
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Join ignored"); return false; }
    // Raise the BROWSER-ONLY loading state NOW (before the master round-trip) so the user
    // gets immediate "Connecting to <name>" feedback while the worker talks to the master.
    // On a master/HTTP failure the worker Fails it (drops the cover + reopens the browser).
    coop::join_progress::BeginConnect(displayName.empty() ? std::string("the server") : displayName);
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, lobbyId] {
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::JoinInfo info = lobby::LobbyClient::Join(masterUrl, lobbyId, 8000);
            if (info.ok && !coop::shutdown::IsShuttingDown()) {
                net::Config cfg;
                cfg.role = net::Role::Client;
                cfg.topology = net::Topology::P2P;
                cfg.localIdentity = info.peerIdentity;
                cfg.hostIdentity = info.hostIdentity;
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
                QueueStart(cfg);
                UE_LOGI("session_manager: JOIN ready -- peer=%s host=%s (session boot = harness Tier 2)",
                        info.peerIdentity.c_str(), info.hostIdentity.c_str());
            } else if (!info.ok) {
                UE_LOGW("session_manager: JoinLobby '%s' failed", lobbyId.c_str());
                coop::join_progress::Fail("could not reach the server (master unavailable?)");
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: JoinLobby worker exception: %s", e.what());
            coop::join_progress::Fail("join error -- see the log");
        }
        g_actionBusy.store(false);
    }).detach();
    return true;
}

bool ConnectDirect(const std::string& hostPort) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Direct ignored"); return false; }
    std::string host;
    uint16_t port = 0;
    const bool ok = ParseHostPort(hostPort, host, port);
    if (ok) {
        net::Config cfg;
        cfg.role = net::Role::Client;
        cfg.topology = net::Topology::LanDirect;
        cfg.peerIp = host;
        cfg.port = port;
        // Browser-only loading state. A dead address fails async (GNS never reaches
        // Connected) -> net_pump's connect-fail detector drops the cover + reopens the browser.
        coop::join_progress::BeginConnect(host);
        QueueStart(cfg);
        UE_LOGI("session_manager: DIRECT connect queued -> %s:%u (session boot = harness Tier 2)",
                host.c_str(), static_cast<unsigned>(port));
    } else {
        UE_LOGW("session_manager: bad direct address '%s'", hostPort.c_str());
    }
    g_actionBusy.store(false);
    return ok;
}

void SetListed(bool listed) { Announcer().SetListed(listed); }

void AbortHost() {
    Announcer().Stop();  // POST /v1/leave + stop the heartbeat thread (kills the listing)
    {
        std::lock_guard<std::mutex> lk(g_pendHostMu);
        g_hasPendingHost = false;
    }
    SetOwnLobbyId(std::string());  // no longer hosting -> clear the own-lobby self-join guard
    UE_LOGI("session_manager: AbortHost -- announced lobby cancelled (/leave + heartbeat stopped)");
}

bool TakePendingStart(net::Config& out) {
    std::lock_guard<std::mutex> lk(g_pendMu);
    if (!g_hasPending) return false;
    out = g_pending;
    g_hasPending = false;
    return true;
}

bool TakePendingHostWithSave(PendingHost& out) {
    std::lock_guard<std::mutex> lk(g_pendHostMu);
    if (!g_hasPendingHost) return false;
    out = g_pendingHost;
    g_hasPendingHost = false;
    return true;
}

}  // namespace coop::session_manager
