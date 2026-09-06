// coop/session_manager.cpp -- the session actions behind the menus: host (listed, direct,
// hidden), join by lobby, direct and P2P connect, the listing transitions, the update check and
// the pending-start hand-off to the harness. Master-server HTTP runs on detached workers.

#include "coop/session/session_manager.h"

#include "coop/config/config.h"           // ResolveString -- the lobby password row
#include "coop/config/config_registry.h"  // T7: the my-name default constant
#include "coop/net/lobby_announcer.h"
#include "coop/net/protocol.h"  // kOfficialMasterUrl (the "DEFAULT" display mask) + kProtocolVersion (the b<N> build rev)
#include "coop/session/join_progress.h"
#include "coop/session/shutdown.h"
#include "coop/version.h"  // kGameTarget -- the game half of the Paper-pair identity (CMake-generated)
#include "ue_wrap/core/log.h"

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

// The pre-Configure seed: the harness calls Configure() at boot with the resolved master URL, so
// this only makes an earlier read reach the official endpoint instead of localhost. The one
// definition is protocol.h's.
constexpr const char* kDefaultMaster = coop::net::kOfficialMasterUrl;


// Leaked process-lifetime singletons: no thread join runs at static destruction or DLL unload
// (coop/shutdown.h forbids join-from-teardown, a loader-lock deadlock), and the detached HTTP
// workers' captures stay valid for the process life.
lobby::LobbyClient& Client() { static auto* c = new lobby::LobbyClient(); return *c; }
lobby::LobbyAnnouncer& Announcer() { static auto* a = new lobby::LobbyAnnouncer(); return *a; }

// ---- the announce a hidden lobby does not make ----
// A hidden DIRECT lobby is never announced: the announce would hand the master the name, world,
// lock flag, cap, listen port, identity and source address, and a DIRECT Config is built from
// the listen port alone, so the round trip bought nothing but an instant un-hide. The announce
// is deferred here instead, and the scoreboard's "Show in server browser" performs it when the
// player asks. P2P is not covered: there the master is the rendezvous, so hiding stays a flag.
struct DeferredAnnounce {
    bool armed = false;       // a DIRECT lobby is live that the master has not been told of
    std::string masterUrl;
    std::string name;
    std::string world;
    bool locked = false;
    int playersMax = 4;
    int directPort = 0;
};
std::mutex g_deferredMu;
DeferredAnnounce g_deferred;

// True while the live lobby is DIRECT: SetListed can then retract the lobby (/v1/leave) on an
// un-tick instead of settling for the visibility flag.
std::atomic<bool> g_hostIsDirect{false};

void ArmDeferredAnnounce(const std::string& masterUrl, const std::string& name,
                         const std::string& world, bool locked, int playersMax,
                         int directPort) {
    std::lock_guard<std::mutex> lk(g_deferredMu);
    g_deferred = DeferredAnnounce{true, masterUrl, name, world, locked, playersMax, directPort};
}

void DisarmDeferredAnnounce() {
    std::lock_guard<std::mutex> lk(g_deferredMu);
    g_deferred.armed = false;
}

bool PeekDeferredAnnounce(DeferredAnnounce& out) {
    std::lock_guard<std::mutex> lk(g_deferredMu);
    if (!g_deferred.armed) return false;
    out = g_deferred;
    return true;
}

// Config pushed from the harness at boot: the master URL and the host fallback Config (used when
// the announce fails); g_hostStatus is the last host-action result the UI shows. All under
// g_cfgMu.
std::mutex g_cfgMu;
std::string g_masterUrl = kDefaultMaster;  // overwritten by Configure
net::Config g_fallbackHostCfg;
std::string g_hostStatus;
std::string g_ownLobbyId;  // our own announced lobbyId -> we never list or join it (no self-join)
// The nickname default comes from the shared registry constant.
std::string g_nickname = coop::config_registry::kMyNameDefault;  // local display nickname (seeded from config; browser overwrites)

// One queued session start (last action wins until the harness consumes it).
std::mutex g_pendMu;
bool g_hasPending = false;
net::Config g_pending;

// One queued host-with-save: a {Config, SaveChoice} the harness drains, loads the world for, then
// starts. Separate from g_pending, which starts on the already loaded world.
std::mutex g_pendHostMu;
bool g_hasPendingHost = false;
PendingHost g_pendingHost;

// Serialises the session-start actions (host, join, direct connect): one in flight at a time.
// Refresh is not gated.
std::atomic<bool> g_actionBusy{false};

// The password for the join the player is about to make, set by the prompt just before Connect
// and consumed by whichever lane starts. Not a config row: writing someone else's lobby password
// into our ini would persist a secret the player was lent, and the ini is what people paste into
// bug reports.
std::mutex  g_joinPwMu;
std::string g_joinPassword;

// The one place a joiner's password is resolved, and it takes: the prompt's value wins and is
// cleared, so it can never ride into the next connection (connect to locked lobby A, then
// direct-connect to B, and B would receive a tag over A's secret). The fallback is
// net.join_password, never net.lobby_password: that is what the player's own hosted sessions
// require, and offering it to every locked host reached is a leak. The fallback serves the
// client with no prompt (scripted, LAN, dedicated).
std::string TakeJoinPassword() {
    {
        std::lock_guard<std::mutex> lk(g_joinPwMu);
        if (!g_joinPassword.empty()) {
            std::string taken;
            taken.swap(g_joinPassword);
            return taken;
        }
    }
    return ::coop::config::ResolveString(::coop::config_registry::rows::net_join_password);
}

void QueueStart(const net::Config& cfg) {
    std::lock_guard<std::mutex> lk(g_pendMu);
    g_pending = cfg;
    g_hasPending = true;
}

// "host" or "host:port" to host + port (kDefaultPort without one). IPv4 or a hostname; bracketed
// IPv6 is not parsed.
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

// What an unlisted DIRECT host can honestly promise: an address is enough, and a password if
// there is one, which is exactly what the joiner's window asks for. `why` is a parameter because
// the three callers are unlisted for two different reasons (the master could not be reached, or
// the host chose it), and a builder that hardcoded one told the other something false about its
// own network.
std::string UnlistedDirectStatus(const char* lead, const char* why, bool locked) {
    std::string s(lead);
    s += " -- ";
    s += why;
    s += "; friends use Direct Connect with your IP";
    if (locked) s += " and the password";
    return s;
}

// The user-visible form of a master URL: the official server prints as "DEFAULT"; a custom
// master prints as typed, since its operator needs to see it.
std::string DisplayMaster(const std::string& url) {
    return url == coop::net::kOfficialMasterUrl ? std::string("DEFAULT") : url;
}

}  // namespace

void Configure(const std::string& masterUrl, const net::Config& fallbackHostCfg) {
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        g_masterUrl = masterUrl.empty() ? std::string(kDefaultMaster) : masterUrl;
        g_fallbackHostCfg = fallbackHostCfg;
        UE_LOGI("session_manager: configured -- master='%s' fallback(signaling-set=%d)",
                DisplayMaster(g_masterUrl).c_str(),
                g_fallbackHostCfg.signalingUrl.empty() ? 0 : 1);
    }
    // No update check here: one at boot config time would tell the master every player's source
    // address at game launch, before any multiplayer decision exists. The check fires from
    // ui::server_browser_surface::Open(), because opening the browser is a request to talk to the
    // master, the same trigger the lobby list has; everything else the mod sends follows an action
    // the player took. The main-menu label falls back to DisplayVersion() when no check has landed,
    // so it is never empty, and the check is informational, never a gate.
}

std::string MasterUrl() {
    // Every caller is a post-boot action and the harness Configure()s at boot before any can run;
    // the static init already aliases the official endpoint, so a pre-Configure read still reaches
    // the right place. The env override is resolved once, in config.cpp's registry row.
    std::lock_guard<std::mutex> lk(g_cfgMu);
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

void SetJoinPassword(const std::string& password) {
    std::lock_guard<std::mutex> lk(g_joinPwMu);
    // An empty value is a clear, not an ignore (the opposite of SetNickname): a player who backs
    // out of the password prompt must not carry the last lobby's secret into the next connection.
    g_joinPassword = password;
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

const char* GameTarget() { return coop::version::kGameTarget; }

std::string DisplayVersion() {
    // The version identity is the pair (game target, build number), with no separate mod semver;
    // the build number is kProtocolVersion, which moves exactly when compatibility moves.
    // Function-static: the inputs are compile-time constants and the browser header calls this
    // every frame.
    static const std::string kLabel =
        std::string("Multivoid ") + coop::version::kGameTarget +
        " b" + std::to_string(static_cast<int>(net::kProtocolVersion));
    return kLabel;
}

namespace {
// Version-line state; the main-menu label polls the line. Re-polled on each main-menu entrance,
// made safe by two guards: at most one fetch worker alive, and a minimum interval between fetch
// starts (a burst of entrances coalesces to one fetch).
std::mutex g_latestMu;
std::string g_latestLine;          // empty until a check completes WITH a verdict
bool g_latestOutdated = false;     // amber tint when true
std::atomic<bool> g_latestInFlight{false};
std::atomic<uint64_t> g_latestFetchMs{0};  // GetTickCount64() of the last fetch START (0 = none yet)
constexpr uint64_t kLatestMinIntervalMs = 8000;  // no-DoS floor between /v1/latest fetches
}  // namespace

void RefreshLatestVersion() {
    // Debounce: skip if a fetch started within the floor, or one is already running.
    const uint64_t now = ::GetTickCount64();
    const uint64_t last = g_latestFetchMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kLatestMinIntervalMs) return;
    if (g_latestInFlight.exchange(true)) return;  // one worker at a time
    g_latestFetchMs.store(now, std::memory_order_relaxed);
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl] {
        try {
            if (coop::shutdown::IsShuttingDown()) { g_latestInFlight.store(false, std::memory_order_release); return; }
            const lobby::LatestInfo info = lobby::LobbyClient::FetchLatest(masterUrl, 8000);
            // proto <= 0: the master has no released-version record, so no verdict; the plain
            // identity label stays rather than a fabricated "latest" line.
            if (info.ok && info.proto > 0) {  // an unreachable master keeps the last known line
                const int ours = static_cast<int>(net::kProtocolVersion);
                std::string line;
                bool outdated = false;
                if (info.proto == ours) {
                    // The current build is the latest release: the compact "(latest)" tag.
                    line = DisplayVersion() + " (latest)";
                } else if (info.proto > ours) {
                    outdated = true;
                    // No URL: the label is a one-line UTextBlock, not a hyperlink, so an address
                    // would cost ~37 characters to deliver a string nobody can act on. What stays
                    // is which build supersedes yours.
                    line = DisplayVersion() + " -- UPDATE AVAILABLE: " +
                           (info.mod.empty() ? ("b" + std::to_string(info.proto)) : info.mod);
                } else {
                    // We are newer than the master's latest (a dev build); informational.
                    line = DisplayVersion() + " (dev; latest released b" +
                           std::to_string(info.proto) + ")";
                }
                UE_LOGI("session_manager: version check -- %s", line.c_str());
                std::lock_guard<std::mutex> lk(g_latestMu);
                g_latestLine = line;
                g_latestOutdated = outdated;
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: version check worker exception: %s", e.what());
        }
        g_latestInFlight.store(false, std::memory_order_release);
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
uint64_t RowsGeneration() { return Client().Generation(); }
std::string Status() { return Client().Status(); }

int FetchFailures() { return Client().ConsecutiveFailures(); }

uint64_t RowsDataGeneration() { return Client().DataGeneration(); }

void HostLobby(const std::string& name, const std::string& world, bool locked, int playersMax) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Host ignored"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, name, world, locked, playersMax] {
        // An exception escaping a detached thread is std::terminate; the store(false) is outside
        // the try so g_actionBusy clears on every path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, world, locked, playersMax, 8000);
            if (info.ok && !coop::shutdown::IsShuttingDown()) {
                net::Config cfg;
                cfg.role = net::Role::Host;
                cfg.topology = net::Topology::P2P;
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
                SetOwnLobbyId(info.lobbyId);  // never list or join our own lobby
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

extern std::atomic<bool> g_listedState;  // defined below at SetListed (UI mirror)

void AnnounceEnvHostHidden(const std::string& name, const std::string& world) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- env announce skipped"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, name, world] {
        // An exception escaping a detached thread is std::terminate; the store(false) is outside
        // the try so g_actionBusy clears on every path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, world,
                                 /*locked=*/false, /*playersMax=*/4, 8000);
            if (info.ok) {
                SetOwnLobbyId(info.lobbyId);  // never list or join our own lobby
                Announcer().SetListed(false); // the hide-from-list flag, immediately
                // Seed the UI mirror too, or the scoreboard's "Show in server browser" tick reads
                // on while hidden.
                g_listedState.store(false, std::memory_order_relaxed);
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

bool HostWithSave(const SaveChoice& choice, const std::string& name, bool locked,
                  const std::string& password, int playersMax,
                  coop::session::HostMode mode) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- HostWithSave ignored"); return false; }
    // The secret this session will require, resolved once and carried into whichever host path
    // runs: empty unless `locked`, so the caller's bool alone decides whether the lock has teeth.
    // The caller's string, not a re-read of the row it wrote (the config layer line-scans the ini
    // under a global mutex, file I/O the net thread must not do).
    std::string lobbyPw = locked ? password : std::string();
    // A lock with no secret is downgraded, not announced: the padlock in the browser is a promise,
    // so with nothing to check the session is honestly open and the player is told. The native flow
    // cannot reach this (its lock refuses to turn on without a password); the ImGui fallback's
    // checkbox and a hand-edited ini can.
    if (locked && lobbyPw.empty()) {
        UE_LOGW("session_manager: asked to host LOCKED with an EMPTY password -- hosting "
                "OPEN instead. A padlock nothing enforces is worse than no padlock: it "
                "tells the host they are protected.");
        SetHostStatus("No password is set, so this session is OPEN -- set one in the "
                      "hosting window's session settings.");
        locked = false;
    }
    using coop::session::Reachability;
    // Forced, not trusted: an unlisted brokered lobby is unreachable by anyone, so it is enforced
    // here rather than assumed of every caller.
    if (mode.reach == Reachability::Brokered) mode.listed = true;
    const bool directConnection = (mode.reach == Reachability::Direct);
    const bool hideFromBrowser  = !mode.listed;

    // A DIRECT session the master is never told about: nothing leaves the machine. This is also
    // what "LAN only" was, minus an accept filter that did the router's job (an unforwarded port is
    // local-only already), so there is no third connection type (coop/session/host_mode.h). The
    // announce is stashed; the scoreboard's "Show in server browser" performs it if the host asks.
    // No worker and no HTTP: a DIRECT Config is built from the listen port alone.
    if (directConnection && hideFromBrowser) {
        net::Config fallbackCfg;
        { std::lock_guard<std::mutex> lk(g_cfgMu); fallbackCfg = g_fallbackHostCfg; }
        const uint16_t directPort = fallbackCfg.port ? fallbackCfg.port : net::kDefaultPort;
        net::Config cfg;
        cfg.role = net::Role::Host;
        cfg.topology = net::Topology::LanDirect;
        cfg.port = directPort;
        {
            std::lock_guard<std::mutex> lk(g_pendHostMu);
            cfg.lobbyPassword = lobbyPw;
            g_pendingHost.cfg = cfg;
            g_pendingHost.save = choice;
            g_pendingHost.listed = false;
            g_hasPendingHost = true;
        }
        g_listedState.store(false, std::memory_order_relaxed);
        g_hostIsDirect.store(true, std::memory_order_relaxed);
        ArmDeferredAnnounce(MasterUrl(), name, choice.newGame ? choice.newName : choice.slot,
                            locked, playersMax, static_cast<int>(directPort));
        // Through the same builder as the two fallback lines, so the deliberately unlisted
        // configuration gets the same password wording.
        SetHostStatus(UnlistedDirectStatus(("Hosting '" + name + "' DIRECT").c_str(),
                                           "hidden by your choice, NOT listed", locked));
        UE_LOGI("session_manager: hosting DIRECT/HIDDEN '%s' port=%u -- NOT announced (the "
                "master is never told; the scoreboard's Show-in-browser tick announces it "
                "later if the host asks)", name.c_str(), directPort);
        g_actionBusy.store(false);
        return true;
    }
    const std::string masterUrl = MasterUrl();
    net::Config fallback;
    { std::lock_guard<std::mutex> lk(g_cfgMu); fallback = g_fallbackHostCfg; }
    // `hideFromBrowser` is not captured: the only branch that reads it returned above, before this
    // worker exists.
    std::thread([masterUrl, fallback, choice, name, locked, playersMax,
                 directConnection, lobbyPw] {
        // An exception escaping a detached thread is std::terminate; the store(false) is outside
        // the try so g_actionBusy clears on every path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            // Hosting never depends on a reachable master: the announce lists the lobby and
            // collects the master-issued signaling and TURN, but the boot is queued either way
            // (announce ok: the master's P2P Config, listed; announce failed: the local fallback,
            // unlisted but in-game). MTA precedent: the server runs regardless of the master list.
            // The harness loads the world, then starts.
            const std::string world = choice.newGame ? choice.newName : choice.slot;
            // DIRECT hosts a plain LanDirect UDP listen and announces it with the listen port (the
            // master records conn="direct" and the announce's source address, and /v1/join hands
            // joiners "ip:port"); AUTO announces the P2P lobby. An unreachable master blocks
            // neither.
            const uint16_t directPort =
                fallback.port ? fallback.port : net::kDefaultPort;
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, world, locked, playersMax,
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
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
            } else {
                // AUTO with no answer from the master falls back to a LanDirect listen,
                // unconditionally: a joiner needs the host's `gen:` identity to dial a P2P host,
                // that identity is published by /v1/join alone, and nothing else writes
                // net.host_identity, so an unlisted P2P host is unreachable even with a healthy
                // signaling server. LanDirect stays reachable with no master alive, through the
                // Direct Connect box that ships.
                cfg = fallback;
                cfg.role = net::Role::Host;        // belt-and-suspenders (fallback is already host)
                cfg.topology = net::Topology::LanDirect;
                cfg.port = directPort;
            }
            {
                std::lock_guard<std::mutex> lk(g_pendHostMu);
                cfg.lobbyPassword = lobbyPw;
                g_pendingHost.cfg = cfg;
                g_pendingHost.save = choice;
                g_pendingHost.listed = listed;
                g_hasPendingHost = true;
            }
            // Seed the scoreboard mirror; a hidden DIRECT lobby never reaches this worker.
            g_listedState.store(listed, std::memory_order_relaxed);
            // Derived from the transport actually configured, not from the player's pick: an AUTO
            // host whose master was unreachable fell back to a DIRECT listen just above, and this
            // flag chooses the hide semantics (retract via /v1/leave, or clear the visibility
            // flag). The three assignments to cfg.topology are the only producers.
            g_hostIsDirect.store(cfg.topology == net::Topology::LanDirect,
                                 std::memory_order_relaxed);
            if (listed) {
                SetOwnLobbyId(info.lobbyId);  // never list or join our own lobby
                // Arm the deferral even though we just announced: hiding a DIRECT lobby retracts it
                // (/v1/leave), so a Hide then Show cycle needs something to re-announce from. AUTO
                // stays un-hideable at host time: the master is a relay game's only rendezvous.
                if (directConnection)
                    ArmDeferredAnnounce(masterUrl, name, world, locked, playersMax,
                                        static_cast<int>(directPort));
                SetHostStatus(directConnection
                    ? "Hosting '" + name + "' DIRECT -- listed (UDP port must be forwarded!)"
                    : "Hosting '" + name + "' -- lobby listed");
                UE_LOGI("session_manager: HOST-WITH-SAVE ready (LISTED, %s) -- lobby=%s %s='%s'",
                        directConnection ? "DIRECT" : "P2P",
                        info.lobbyId.c_str(), choice.newGame ? "newGame" : "slot", world.c_str());
            } else if (directConnection) {
                SetHostStatus(UnlistedDirectStatus("Hosting DIRECT",
                                                   "master unreachable, NOT listed", locked));
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (DIRECT, UNLISTED -- master '%s' unreachable, port %u)",
                        DisplayMaster(masterUrl).c_str(), static_cast<unsigned>(directPort));
            } else {
                // The line describes what happened: a DIRECT listen, joinable by address.
                SetHostStatus(UnlistedDirectStatus("Hosting",
                                                   "master unreachable, NOT listed", locked));
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (UNLISTED -- master '%s' unreachable) "
                        "-- fell back to a DIRECT listen on port %u so the session stays joinable",
                        DisplayMaster(masterUrl).c_str(), static_cast<unsigned>(directPort));
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: HostWithSave worker exception: %s", e.what());
            SetHostStatus(std::string("Host failed: ") + e.what());
            // Drop the host-boot cover the picker raised, or the spinner hangs until the harness's
            // world-load timeout (~30 s), the wrong message for an HTTP throw; Reset() re-shows the
            // menu and the harness re-surfaces the browser.
            if (!coop::shutdown::IsShuttingDown()) {
                EndHostedLobby();            // /leave + stop heartbeat (worker-safe)
                coop::join_progress::Reset();
            }
        }
        g_actionBusy.store(false);
    }).detach();
    return true;  // accepted -- the picker raises the host-boot cover + closes
}

namespace {

// The version verdict: per-lobby equality on the pair, game target then build, each tier a hard
// refusal; the popup names the first mismatching axis and who updates. Empty or zero remote
// fields skip their tier (the Join wire gate and the header backstop cover them). Empty =
// compatible.
std::string VersionMismatchVerdict(const std::string& hostGame, int hostProto) {
    // Tier 1, the game cook: reachable with an equal build (a recook adaptation need not change the
    // wire), hence its own tier.
    if (!hostGame.empty() && hostGame != coop::version::kGameTarget) {
        return std::string("Host plays VOTV ") + hostGame + ", you have VOTV " +
               coop::version::kGameTarget + " -- game version mismatch.";
    }
    // Tier 2, the build (the wire revision).
    if (hostProto > 0 && hostProto != static_cast<int>(net::kProtocolVersion)) {
        const bool hostNewer = hostProto > static_cast<int>(net::kProtocolVersion);
        return std::string("Mod build mismatch: host runs b") + std::to_string(hostProto) +
               ", you run b" + std::to_string(net::kProtocolVersion) + " -- " +
               (hostNewer ? std::string("update: ") + net::kReleasesUrl
                          : "the host needs to update.");
    }
    return {};
}

}  // namespace

bool JoinLobby(const std::string& lobbyId, const std::string& displayName, int hostProto,
               const std::string& hostGame) {
    // Never connect to our own lobby (the host clicking its own listed server); rejected before any
    // loading state is raised.
    if (!lobbyId.empty() && lobbyId == OwnLobbyId()) {
        UE_LOGW("session_manager: refusing to join our OWN lobby '%s' -- you are the host", lobbyId.c_str());
        SetHostStatus("That's your own server -- you're already hosting it.");
        return false;
    }
    // The version gate, pre-flight from the browser row ("show normally, reject on Join"); the Join
    // wire gate re-validates live and the header close is the final backstop. Rejected through the
    // connect-failed popup, not the footer.
    {
        const std::string verdict = VersionMismatchVerdict(hostGame, hostProto);
        if (!verdict.empty()) {
            UE_LOGW("session_manager: JOIN rejected -- %s (host game='%s' b%d; ours %s b%u)",
                    verdict.c_str(), hostGame.c_str(), hostProto,
                    coop::version::kGameTarget, static_cast<unsigned>(net::kProtocolVersion));
            coop::join_progress::RefuseJoin(verdict);
            return false;
        }
    }
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Join ignored"); return false; }
    // Raise the browser-only loading state before the master round trip, so "Connecting to <name>"
    // shows at once; on a master failure the worker Fails it (drops the cover, reopens the
    // browser).
    coop::join_progress::BeginConnect(displayName.empty() ? std::string("the server") : displayName);
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, lobbyId] {
        try {
            // Shutdown race: BeginConnect raised the cover before this worker spawned, so every
            // exit drops it.
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
                    // A direct lobby: the master handed us the host's forwarded ip:port, a plain
                    // LanDirect dial, the browser's manual Direct Connect shape.
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
                    cfg.lobbyPassword = TakeJoinPassword();
                    // Which host is at that address, when the master says: the binding a locked
                    // DIRECT lobby needs before the joiner may send a password proof. Empty against
                    // an old master, deliberately: open direct lobbies join fine, locked ones
                    // refuse with a sentence.
                    cfg.hostIdentity = info.hostIdentity;
                    QueueStart(cfg);
                    UE_LOGI("session_manager: JOIN ready -- DIRECT lobby (LanDirect dial; session boot = harness Tier 2)");
                } else {
                cfg.topology = net::Topology::P2P;
                cfg.hostIdentity = info.hostIdentity;
                cfg.signalingUrl = info.signalingUrl;
                cfg.signalingToken = info.signalingToken;
                cfg.stunList = info.stun;
                cfg.turnList = info.turnUri;
                cfg.turnUser = info.turnUser;
                cfg.turnPass = info.turnPass;
                cfg.lobbyPassword = TakeJoinPassword();
                QueueStart(cfg);
                UE_LOGI("session_manager: JOIN ready -- host=%s (session boot = harness Tier 2)",
                        info.hostIdentity.c_str());
                }
            } else if (!info.ok) {
                UE_LOGW("session_manager: JoinLobby '%s' failed", lobbyId.c_str());
                coop::join_progress::Fail("could not reach the server (master unavailable?)");
            } else {
                // info.ok but shutdown raced true between the check and here: neither branch ran,
                // so drop the cover explicitly.
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
        cfg.lobbyPassword = TakeJoinPassword();
        // Which host we expect at that address, if the player was told: a direct connect names a
        // place, and the row gives a friend who was given the host's `gen:` line the binding an
        // AUTO joiner gets from the master. Empty stays empty and is true: an unbound joiner can
        // join any open server and is refused by a locked one with a sentence.
        cfg.hostIdentity =
            ::coop::config::ResolveString(::coop::config_registry::rows::net_host_identity);
        // This machine named this address (a typed box or its own configuration;
        // net::Config::selfAddressed). The only place in the tree that sets it.
        cfg.selfAddressed = true;
        // The browser-only loading state; a dead address fails asynchronously (GNS never reaches
        // Connected) and net_pump's connect-fail detector drops the cover.
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

bool ConnectP2PDirect(const std::string& hostIdentity, const net::Config& fallback) {
    // The P2P twin of ConnectDirect: dial a host by identity through a signaling server with no
    // master in the loop, for the env test client and for a dev dialling a `gen:` line copied from
    // a log. The signaling and ICE half comes from the caller's already resolved config:
    // FillP2PFields is the one place those fields are assembled.
    if (g_actionBusy.exchange(true)) {
        UE_LOGW("session_manager: action busy -- P2P connect ignored");
        return false;
    }
    bool ok = false;
    if (hostIdentity.empty()) {
        UE_LOGW("session_manager: P2P connect needs a host identity (`gen:<64 hex>`)");
    } else if (fallback.signalingUrl.empty()) {
        UE_LOGW("session_manager: P2P connect needs a signaling server");
    } else {
        net::Config cfg = fallback;
        cfg.role = net::Role::Client;
        cfg.topology = net::Topology::P2P;
        cfg.hostIdentity = hostIdentity;
        cfg.lobbyPassword = TakeJoinPassword();
        coop::join_progress::BeginConnect(hostIdentity);
        QueueStart(cfg);
        UE_LOGI("session_manager: P2P connect queued -> host '%s' via signaling %s "
                "(session boot = harness Tier 2)",
                hostIdentity.c_str(), cfg.signalingUrl.c_str());
        ok = true;
    }
    g_actionBusy.store(false);
    return ok;
}

// The lobby's listed state, mirrored for the UI (the scoreboard's toggle renders it; HostWithSave
// seeds it, SetListed flips it). True with no lobby.
std::atomic<bool> g_listedState{true};

// Serialises listing transitions: every branch either blocks (Announcer::Host is an 8 s round
// trip; Stop joins the heartbeat thread and POSTs /v1/leave) or depends on Announcer().active().
// The branch is decided inside the worker under this mutex, so an un-tick followed by a re-tick
// sees the state the retract actually left.
std::mutex g_listingMu;

void SetListed(bool listed) {
    g_listedState.store(listed, std::memory_order_relaxed);  // the UI mirror, immediately
    // Everything else on a worker: the scoreboard calls this on the game thread, and
    // lobby_announcer.h says Stop() must not run there.
    std::thread([listed] {
        if (coop::shutdown::IsShuttingDown()) return;
        std::lock_guard<std::mutex> lk(g_listingMu);
        try {
            // Show: if nothing was ever announced (a hidden DIRECT host), the tick is the announce;
            // there is no record for /v1/visibility to flip.
            if (listed && !Announcer().active()) {
                DeferredAnnounce d;
                if (!PeekDeferredAnnounce(d)) {
                    UE_LOGW("session_manager: Show ticked but nothing is armed to announce "
                            "-- no lobby is being hosted");
                    return;
                }
                const lobby::HostInfo info = Announcer().Host(
                    d.masterUrl, d.name, d.world, d.locked, d.playersMax, 8000, d.directPort);
                if (!info.ok) {
                    UE_LOGW("session_manager: deferred announce FAILED (master unreachable) "
                            "-- the lobby stays hidden and joinable by IP");
                    g_listedState.store(false, std::memory_order_relaxed);
                    SetHostStatus("Could not list the game -- master unreachable. "
                                  "Friends can still Direct Connect by IP.");
                    return;
                }
                // A re-announce mints a fresh lobbyId, so the self-join guard is re-pointed at it.
                SetOwnLobbyId(info.lobbyId);
                UE_LOGI("session_manager: deferred announce done -- lobby=%s is now listed "
                        "(the master learns this host's address at THIS moment, not at host "
                        "time)", info.lobbyId.c_str());
                return;
            }

            // Hide: /v1/visibility clears a flag while the heartbeat keeps the record, address
            // included, refreshed every 30 s. A DIRECT lobby is retracted instead and re-armed for
            // a later Show; a P2P lobby cannot be, since the master is its only rendezvous, and the
            // flag is the honest limit there.
            if (!listed && g_hostIsDirect.load(std::memory_order_relaxed) &&
                Announcer().active()) {
                DeferredAnnounce d;
                const bool rearm = PeekDeferredAnnounce(d);
                Announcer().Stop();       // POST /v1/leave + stop the heartbeat: record GOES
                SetOwnLobbyId(std::string());
                if (rearm) ArmDeferredAnnounce(d.masterUrl, d.name, d.world, d.locked,
                                               d.playersMax, d.directPort);
                UE_LOGI("session_manager: DIRECT lobby retracted (/v1/leave) -- the master no "
                        "longer holds a record for it%s",
                        rearm ? "; re-armed for a later Show" : "");
                return;
            }

            Announcer().SetListed(listed);
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: listing transition worker exception: %s", e.what());
        }
    }).detach();
}

bool ListedState() { return g_listedState.load(std::memory_order_relaxed); }

void SetPlayerCountSource(int (*fn)()) {
    // Announcer() lives for the process and the setter only stores the pointer, so installing it
    // before any lobby exists is correct.
    Announcer().SetPlayerCountFn(fn);
}

void EndHostedLobby() {
    // Clear the host-side lobby state before the blocking delist: Stop() blocks up to ~13 s, and a
    // re-host landing inside that window writes fresh pending state a post-Stop clear would wipe.
    {
        std::lock_guard<std::mutex> lk(g_pendHostMu);
        g_hasPendingHost = false;
    }
    SetOwnLobbyId(std::string());  // no longer hosting -> clear the own-lobby self-join guard
    g_listedState.store(true, std::memory_order_relaxed);  // back to the no-lobby default
    g_hostIsDirect.store(false, std::memory_order_relaxed);
    DisarmDeferredAnnounce();  // the lobby is over; a stale deferral must not outlive it
                               // and re-announce a world nobody is hosting any more
    Announcer().Stop();  // POST /v1/leave + stop the heartbeat thread (kills the listing)
    UE_LOGI("session_manager: EndHostedLobby -- lobby retired (/leave + heartbeat stopped)");
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
