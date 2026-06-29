// coop/session_manager.cpp -- see coop/session_manager.h.

#include "coop/session/session_manager.h"

#include "coop/net/lobby_announcer.h"
#include "coop/net/protocol.h"  // kOfficialMasterUrl (the "DEFAULT" display mask)
#include "coop/session/join_progress.h"
#include "coop/session/shutdown.h"
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

// User-visible form of a master URL: the OFFICIAL server prints as "DEFAULT"
// -- the connect console / browser status / boot log never advertise the raw
// VPS address (user 2026-06-10). A genuinely custom master prints verbatim
// (its operator needs to see it for debugging).
std::string DisplayMaster(const std::string& url) {
    return url == coop::net::kOfficialMasterUrl ? std::string("DEFAULT") : url;
}

}  // namespace

void Configure(const std::string& masterUrl, const net::Config& fallbackHostCfg) {
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        g_masterUrl = masterUrl.empty() ? std::string(kDefaultMaster) : masterUrl;
        g_fallbackHostCfg = fallbackHostCfg;
        g_configured = true;
        UE_LOGI("session_manager: configured -- master='%s' fallback(signaling-set=%d identity='%s')",
                DisplayMaster(g_masterUrl).c_str(),
                g_fallbackHostCfg.signalingUrl.empty() ? 0 : 1,
                g_fallbackHostCfg.localIdentity.c_str());
    }
    // v59 LAUNCH TOAST: one /v1/latest check per process, kicked at boot config
    // time (the overlay polls LatestVersionLine and toasts the verdict).
    CheckLatestVersionAsync();
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

namespace {
// v59 launch-toast state: one check per process; the overlay polls the line.
std::mutex g_latestMu;
std::string g_latestLine;          // empty until the check completes WITH a verdict
bool g_latestOutdated = false;     // amber tint when true
std::atomic<bool> g_latestStarted{false};
}  // namespace

void CheckLatestVersionAsync() {
    if (g_latestStarted.exchange(true)) return;  // once per process
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl] {
        try {
            if (coop::shutdown::IsShuttingDown()) return;
            const lobby::LatestInfo info = lobby::LobbyClient::FetchLatest(masterUrl, 8000);
            if (!info.ok) return;  // unreachable / pre-v59 master: stay silent
            const int ours = static_cast<int>(net::kProtocolVersion);
            std::string line;
            bool outdated = false;
            if (info.proto == ours) {
                line = "VOTV-Coop is up to date (v" + std::to_string(ours) +
                       (info.mod.empty() ? std::string() : " / " + info.mod) + ")";
            } else if (info.proto > ours) {
                outdated = true;
                line = "VOTV-Coop UPDATE AVAILABLE: v" + std::to_string(info.proto) +
                       (info.mod.empty() ? std::string() : " (" + info.mod + ")") +
                       " -- you have v" + std::to_string(ours) + ". Get it: " +
                       (info.url.empty() ? "github.com/pelmentor/VOTV_MP/releases" : info.url);
            } else {
                // We are NEWER than the master's latest (a dev build) -- informational.
                line = "VOTV-Coop dev build v" + std::to_string(ours) +
                       " (latest released: v" + std::to_string(info.proto) + ")";
            }
            UE_LOGI("session_manager: version check -- %s", line.c_str());
            std::lock_guard<std::mutex> lk(g_latestMu);
            g_latestLine = line;
            g_latestOutdated = outdated;
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: version check worker exception: %s", e.what());
        }
    }).detach();
}

std::string LatestVersionLine(bool* outdated) {
    std::lock_guard<std::mutex> lk(g_latestMu);
    if (outdated) *outdated = g_latestOutdated;
    return g_latestLine;
}

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

extern std::atomic<bool> g_listedState;  // defined below at SetListed (UI mirror)

bool HostWithSave(const SaveChoice& choice, const std::string& name, bool locked, int playersMax,
                  bool directConnection, bool hideFromBrowser) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- HostWithSave ignored"); return false; }
    const std::string masterUrl = MasterUrl();
    net::Config fallback;
    { std::lock_guard<std::mutex> lk(g_cfgMu); fallback = g_fallbackHostCfg; }
    std::thread([masterUrl, fallback, choice, name, locked, playersMax,
                 directConnection, hideFromBrowser] {
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
            // DIRECT hosts a plain LanDirect UDP listen and announces it WITH the
            // listen port: the master records conn="direct" + the announce's
            // source ip, the browser lists it, /v1/join hands joiners "ip:port".
            // AUTO announces the normal P2P lobby. RULE 1 either way: an
            // unreachable master never blocks hosting (DIRECT falls back to
            // share-your-IP; AUTO to the local signaling fallback Config).
            const uint16_t directPort =
                fallback.port ? fallback.port : net::kDefaultPort;
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, ModVersion(), world, locked, playersMax,
                                 8000, directConnection ? static_cast<int>(directPort) : 0);
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }

            net::Config cfg;
            const bool listed = info.ok;
            if (directConnection) {
                cfg.role = net::Role::Host;
                cfg.topology = net::Topology::LanDirect;
                cfg.port = directPort;
            } else if (listed) {
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
                g_pendingHost.listed = listed && !(directConnection && hideFromBrowser);
                g_hasPendingHost = true;
            }
            g_listedState.store(listed && !(directConnection && hideFromBrowser),
                                std::memory_order_relaxed);  // seed the scoreboard mirror
            if (listed) {
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                if (directConnection && hideFromBrowser) {
                    // Hidden DIRECT lobby: heartbeat lives (creds fresh), the
                    // browser never lists it; friends Direct Connect by IP. The
                    // scoreboard's Hide toggle can re-list it any time. AUTO
                    // games are deliberately NOT hideable at host time -- the
                    // master is a relay game's ONLY rendezvous, a hidden one is
                    // unjoinable (user design call 2026-06-11).
                    Announcer().SetListed(false);
                    SetHostStatus("Hosting '" + name + "' DIRECT (hidden) -- friends use Direct Connect");
                    UE_LOGI("session_manager: HOST-WITH-SAVE ready (DIRECT/hidden, port %u) -- lobby=%s",
                            static_cast<unsigned>(directPort), info.lobbyId.c_str());
                } else {
                    SetHostStatus(directConnection
                        ? "Hosting '" + name + "' DIRECT -- listed (UDP port must be forwarded!)"
                        : "Hosting '" + name + "' -- lobby listed");
                    UE_LOGI("session_manager: HOST-WITH-SAVE ready (LISTED, %s) -- lobby=%s %s='%s'",
                            directConnection ? "DIRECT" : "P2P",
                            info.lobbyId.c_str(), choice.newGame ? "newGame" : "slot", world.c_str());
                }
            } else if (directConnection) {
                SetHostStatus("Hosting DIRECT -- master unreachable, NOT listed; friends use "
                              "Direct Connect with your IP");
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (DIRECT, UNLISTED -- master '%s' unreachable, port %u)",
                        DisplayMaster(masterUrl).c_str(), static_cast<unsigned>(directPort));
            } else {
                SetHostStatus("Hosting -- master server unreachable, lobby NOT listed (LAN/direct only)");
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (UNLISTED -- master '%s' unreachable) "
                        "-- hosting via local config (signaling-set=%d identity='%s')",
                        DisplayMaster(masterUrl).c_str(),
                        cfg.signalingUrl.empty() ? 0 : 1, cfg.localIdentity.c_str());
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: HostWithSave worker exception: %s", e.what());
            SetHostStatus(std::string("Host failed: ") + e.what());
            // Drop the host-boot cover the picker raised (audit F3): without
            // this the spinner hangs until the harness "world didn't load"
            // timeout (~30 s), which is the wrong message for an HTTP throw.
            // Reset() re-shows the menu; the harness re-surfaces the browser on
            // the next idle tick (it owns ui::server_browser).
            if (!coop::shutdown::IsShuttingDown()) {
                AbortHost();                 // /leave + stop heartbeat (worker-safe)
                coop::join_progress::Reset();
            }
        }
        g_actionBusy.store(false);
    }).detach();
    return true;  // accepted -- the picker raises the host-boot cover + closes
}

bool JoinLobby(const std::string& lobbyId, const std::string& displayName, int hostProto) {
    // FIX 3 -- never connect to our OWN lobby (the 2026-06-08 repro: the host clicked its
    // own listed server + self-joined). Reject before raising any loading state.
    if (!lobbyId.empty() && lobbyId == OwnLobbyId()) {
        UE_LOGW("session_manager: refusing to join our OWN lobby '%s' -- you are the host", lobbyId.c_str());
        SetHostStatus("That's your own server -- you're already hosting it.");
        return false;
    }
    // v59 VERSION GATE ("show normally, reject on Join" -- the user-chosen browser
    // policy): the row carries the host's announced kProtocolVersion. A mismatch
    // would otherwise only die LATE at the wire layer (session.cpp closes on the
    // first app packet's header version, reason log-only); reject HERE with a
    // human message instead. hostProto==0 = the host predates the field (or no
    // row context) -- not verifiable upfront; the wire-level close stays the
    // backstop.
    if (hostProto > 0 && hostProto != static_cast<int>(net::kProtocolVersion)) {
        const bool hostNewer = hostProto > static_cast<int>(net::kProtocolVersion);
        UE_LOGW("session_manager: JOIN rejected -- host protocol v%d != ours v%u",
                hostProto, static_cast<unsigned>(net::kProtocolVersion));
        SetHostStatus(hostNewer
            ? "Host runs a NEWER mod (protocol v" + std::to_string(hostProto) +
              " vs your v" + std::to_string(net::kProtocolVersion) +
              ") -- update: github.com/pelmentor/VOTV_MP/releases"
            : "Host runs an OLDER mod (protocol v" + std::to_string(hostProto) +
              " vs your v" + std::to_string(net::kProtocolVersion) +
              ") -- the host needs to update.");
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
            // Shutdown race: BeginConnect raised the loading cover on the render
            // thread before this worker spawned; bailing without Fail() would
            // strand it (audit F1). Drop it on every exit.
            if (coop::shutdown::IsShuttingDown()) {
                coop::join_progress::Fail("shutting down");
                g_actionBusy.store(false);
                return;
            }
            const lobby::JoinInfo info = lobby::LobbyClient::Join(masterUrl, lobbyId, 8000);
            if (info.ok && !coop::shutdown::IsShuttingDown()) {
                net::Config cfg;
                cfg.role = net::Role::Client;
                if (info.direct) {
                    // Direct lobby (2026-06-11): the master handed us the host's
                    // forwarded ip:port -- a plain LanDirect dial, same shape as
                    // the browser's manual Direct Connect.
                    std::string host;
                    uint16_t port = 0;
                    if (!ParseHostPort(info.addr, host, port)) {
                        UE_LOGW("session_manager: JoinLobby '%s' -- bad direct addr '%s'",
                                lobbyId.c_str(), info.addr.c_str());
                        coop::join_progress::Fail("server returned a bad address");
                        g_actionBusy.store(false);
                        return;
                    }
                    cfg.topology = net::Topology::LanDirect;
                    cfg.peerIp = host;
                    cfg.port = port;
                    QueueStart(cfg);
                    UE_LOGI("session_manager: JOIN ready -- DIRECT lobby (LanDirect dial; session boot = harness Tier 2)");
                } else {
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
                }
            } else if (!info.ok) {
                UE_LOGW("session_manager: JoinLobby '%s' failed", lobbyId.c_str());
                coop::join_progress::Fail("could not reach the server (master unavailable?)");
            } else {
                // info.ok but shutdown raced true between the check above and here:
                // neither branch ran, so drop the cover explicitly (audit F2).
                coop::join_progress::Fail("shutting down");
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

// Mirror of the lobby's current listed state for the UI (the scoreboard's
// Hide-from-browser toggle renders it; HostWithSave seeds it, SetListed flips
// it). True when no lobby exists (harmless default).
std::atomic<bool> g_listedState{true};

void SetListed(bool listed) {
    g_listedState.store(listed, std::memory_order_relaxed);
    Announcer().SetListed(listed);
}

bool ListedState() { return g_listedState.load(std::memory_order_relaxed); }

uint16_t HostListenPort() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_fallbackHostCfg.port ? g_fallbackHostCfg.port : net::kDefaultPort;
}

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
