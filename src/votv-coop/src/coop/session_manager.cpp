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
constexpr const char* kDefaultMaster = "127.0.0.1:10001";

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

std::string g_masterUrl;
std::once_flag g_masterOnce;

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

const std::string& MasterUrl() {
    std::call_once(g_masterOnce, [] {
        const std::string m = ReadEnvA("VOTVCOOP_MASTER_URL");
        g_masterUrl = m.empty() ? kDefaultMaster : m;
        UE_LOGI("session_manager: master server = %s", g_masterUrl.c_str());
    });
    return g_masterUrl;
}

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

void HostWithSave(const SaveChoice& choice, const std::string& name, bool locked, int playersMax) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- HostWithSave ignored"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, choice, name, locked, playersMax] {
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            // The P2P Config's signaling/TURN come from the master's announce response,
            // so we MUST announce first (to build the Config) -> queue {Config, choice}
            // -> the harness loads the world THEN StartCoopSession. The lobby is listed
            // during the host's brief world-load window (acceptable; a joiner just retries
            // until the host session is up).
            const std::string world = choice.newGame ? choice.newName : choice.slot;
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
                {
                    std::lock_guard<std::mutex> lk(g_pendHostMu);
                    g_pendingHost.cfg = cfg;
                    g_pendingHost.save = choice;
                    g_hasPendingHost = true;
                }
                UE_LOGI("session_manager: HOST-WITH-SAVE ready -- lobby=%s %s='%s' (harness loads then hosts)",
                        info.lobbyId.c_str(), choice.newGame ? "newGame" : "slot", world.c_str());
            } else if (!info.ok) {
                UE_LOGW("session_manager: HostWithSave failed (master announce -- master reachable?)");
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: HostWithSave worker exception: %s", e.what());
        }
        g_actionBusy.store(false);
    }).detach();
}

bool JoinLobby(const std::string& lobbyId, const std::string& displayName) {
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
