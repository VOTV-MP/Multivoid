// coop/session_manager.cpp -- see coop/session_manager.h.

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

// Pre-Configure seed only: the harness calls Configure() at boot with
// cfg::ReadMasterUrl() (the canonical source -> the built-in VPS endpoint or the
// net.master.custom gate), which overwrites g_masterUrl before any host/join. This
// VPS default just makes a read before Configure() (shouldn't happen) reach the right
// place instead of localhost. Aliases the ONE definition in protocol.h (was a
// duplicated string literal with a "keep in sync" comment -- retired 2026-07-16).
constexpr const char* kDefaultMaster = coop::net::kOfficialMasterUrl;


// LEAKED process-lifetime singletons (never destroyed): (a) no thread-join runs at
// static destruction / DLL unload -- the project forbids join-from-teardown (loader-
// lock deadlock; coop/shutdown.h), and a member-thread dtor join would do exactly
// that; (b) the detached HTTP workers' captures of these stay valid for the whole
// process life. The OS reclaims the memory at exit.
lobby::LobbyClient& Client() { static auto* c = new lobby::LobbyClient(); return *c; }
lobby::LobbyAnnouncer& Announcer() { static auto* a = new lobby::LobbyAnnouncer(); return *a; }

// ---- The announce a HIDDEN lobby does NOT make -------------------------------
//
// "Hidden" was implemented as ANNOUNCE-THEN-UNLIST: POST /v1/host with the name,
// world, lock flag, player cap, listen port and identity -- against which the
// master records the source IP it resolved -- and only THEN POST /v1/visibility
// to clear the list bit, with the heartbeat refreshing the record every 30 s for
// the lobby's life. So "unlisted" was implemented and "the master never hears of
// you" was not, while a player ticking a box labelled "Hide from server browser"
// reasonably reads the second.
//
// The fix is not to un-list harder. For a DIRECT lobby the announce buys nothing
// else: the returned signaling/STUN/TURN credentials are consumed only on the
// P2P branch, and the Direct branch builds its Config from the listen port
// alone. The whole round trip existed to make a LATER un-hide instant. That is
// what this state keeps instead -- the announce is DEFERRED, and the scoreboard's
// "Show in server browser" performs it the moment the player asks.
//
// P2P/AUTO is deliberately NOT covered: there the master IS the rendezvous, so a
// lobby that never announces is unjoinable. Hiding one stays a visibility flag.
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

// True while the live lobby is DIRECT. Read by SetListed to decide whether
// un-ticking can honestly RETRACT the lobby (/v1/leave) or must settle for the
// visibility flag because the master is this lobby's only rendezvous.
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

// Config pushed from the harness at boot (Configure): the master URL + the host
// fallback Config (used when the master announce fails). g_hostStatus is the last
// host-action result the browser surfaces. All under g_cfgMu (low contention --
// a boot write, then occasional worker-set / UI-read).
std::mutex g_cfgMu;
std::string g_masterUrl = kDefaultMaster;  // overwritten by Configure
net::Config g_fallbackHostCfg;
std::string g_hostStatus;
std::string g_ownLobbyId;  // our own announced lobbyId -> we never list or join it (no self-join)
// T7 (ini rework): MY-NAME default from the shared registry constant.
std::string g_nickname = coop::config_registry::kMyNameDefault;  // local display nickname (seeded from config; browser overwrites)

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

// THE PASSWORD FOR THE JOIN THE PLAYER IS ABOUT TO MAKE. Set by the prompt window
// just before Connect and consumed by whichever lane starts; it is NOT a config
// row, on purpose -- writing someone else's lobby password into our ini would
// persist a secret the player was lent, and the ini is what people paste into bug
// reports. It lives exactly as long as one join attempt.
std::mutex  g_joinPwMu;
std::string g_joinPassword;

// THE ONE PLACE A JOINER'S PASSWORD IS RESOLVED, and it TAKES -- the transient is
// consumed, not merely read.
//
// The PROMPT wins: it is what the player typed for the server in front of them, and it is
// cleared here so it can never ride along into the NEXT connection they make. Leaving it
// set was a real leak, not a tidiness point: connect to locked lobby A with password P,
// then Direct-connect to server B, and B (if locked, and if we bound to it) received a tag
// over P -- carrying the right secret to the wrong host, which is not something the
// binding gate was ever meant to cover.
//
// THE FALLBACK IS `net.join_password` AND NOT `net.lobby_password`, and that distinction
// is the other half of the same finding. `net.lobby_password` is what the player's OWN
// hosted sessions require; falling back to it meant anyone who had ever hosted a locked
// lobby offered their own lobby's secret to every locked host they reached. Two secrets,
// two rows. The fallback exists for the client with no prompt -- scripted, LAN, dedicated.
// (Both found by the post-ship audit, 2026-08-31.)
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
// WHAT AN UNLISTED DIRECT HOST CAN HONESTLY PROMISE.
//
// THIS SENTENCE WAS WRITTEN TWICE IN ONE DAY AND WAS WRONG BOTH TIMES, in opposite
// directions, which is worth recording rather than quietly fixing. It first promised
// "friends use Direct Connect with your IP" unconditionally, which was false for a LOCKED
// lobby -- the joiner had no identity to bind to and the password proof was refused. So a
// locked arm was added telling the host to dig `dial=` out of the log. Forty-five minutes
// later the same session DELETED that refusal (a typed address is now a self-addressed
// lane and may carry a password), and the new arm became the false one: it sends a host to
// a config row that no UI writes, for a problem that no longer exists.
//
// One arm again, and it is the true one: an address is enough, and a password if there is
// one, which is exactly what the joiner's window now asks for. (Post-ship audits,
// 2026-09-01, both halves.)
// `why` is a parameter because the three callers are unlisted for two DIFFERENT reasons --
// two because the master could not be reached, one because the host chose it -- and a
// builder that hardcoded "master unreachable" told the deliberate one something false about
// its own network.
std::string UnlistedDirectStatus(const char* lead, const char* why, bool locked) {
    std::string s(lead);
    s += " -- ";
    s += why;
    s += "; friends use Direct Connect with your IP";
    if (locked) s += " and the password";
    return s;
}

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
    // NO /v1/latest HERE ANY MORE (2026-08-30).
    //
    // This used to kick the first update check at boot config time, and
    // multiplayer_menu re-polled it on every main-menu entrance. Neither is
    // something the player asked for, and between them they meant the master
    // learned every player's source IP AT GAME LAUNCH -- before any multiplayer
    // decision existed. That made a promise we ship in player-facing text false
    // at the moment it is displayed: the host window's third row used to read
    // "Never contacts any Multivoid server". (That row is retired -- the promise now
    // belongs to DIRECT + Hidden, `coop::session::IsMasterFree`.)
    //
    // The check now fires from ui::server_browser_surface::Open(), because
    // opening the browser IS a request to talk to the master -- the same trigger
    // /v1/lobbies already has. Everything else the mod sends is a consequence of
    // an action the player took (Host, the visibility tick, clicking a server).
    //
    // Measured cost, not assumed: `multiplayer_menu.cpp:117-123` falls back to
    // DisplayVersion() when no check has landed, so the label is never empty --
    // a player who never opens the browser sees their own version with no update
    // verdict. The update check is documented informational-only, never a gate.
    //
    // Reported by an external source review of the public tree; the reviewer's
    // question was literally "can I run a server without sending my IP to the
    // master server".
}

std::string MasterUrl() {
    // All 6 callers are internal post-boot actions (host/join/refresh workers)
    // and the harness Configure()s at boot before any of them can run; the
    // static init already aliases the official endpoint, so a hypothetical
    // pre-Configure read still reaches the right place. The old !configured
    // env-fallback branch was a SECOND resolver of VOTVCOOP_MASTER_URL beside
    // config.cpp's registry row -- the F21 duplicate class (arc 3 T2b).
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
    // AN EMPTY VALUE IS A CLEAR, not an "ignore" -- the opposite of SetNickname
    // above, and deliberately so: a player who backs out of the password prompt
    // must not carry the last lobby's secret into the next connection.
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
    // Paper-Minecraft PAIR (user decision 2026-07-19, "Paper 1.20.4 #496" shape):
    // the identity is (game target, build number) -- no separate mod semver. The
    // build number IS kProtocolVersion: it moves exactly when compatibility moves
    // (the standing wire rule) and every release bumps it (release checklist).
    // Function-static: inputs are compile-time constants and the browser header
    // calls this every frame while open (perf audit LOW-2).
    static const std::string kLabel =
        std::string("Multivoid ") + coop::version::kGameTarget +
        " b" + std::to_string(static_cast<int>(net::kProtocolVersion));
    return kLabel;
}

namespace {
// Version-line state: the native main-menu label (coop::multiplayer_menu) polls the line.
// RE-POLLED on each main-menu entrance; the two guards below make that safe (no DoS):
//   g_latestInFlight -- at most ONE fetch worker alive at a time.
//   g_latestFetchMs  -- min interval floor between fetch STARTS (a burst of entrances
//                       within the floor is coalesced to one fetch).
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
            // proto<=0 = the master has no released-version record yet (no latest.json;
            // pre-release world) -- NO VERDICT, keep the plain identity label. Never
            // fabricate a "latest v0" line from an absent record.
            if (info.ok && info.proto > 0) {  // unreachable / pre-v59 master: keep the last known line
                const int ours = static_cast<int>(net::kProtocolVersion);
                std::string line;
                bool outdated = false;
                if (info.proto == ours) {
                    // Current build IS the latest release -> compact "(latest)" tag.
                    line = DisplayVersion() + " (latest)";
                } else if (info.proto > ours) {
                    outdated = true;
                    // NO URL (user 2026-08-31: "url не надо показывать, это длинно,
                    // достаточно лаконичного текста что обнова доступна"). It used to
                    // append the release page, which cost ~37 characters on a ONE-LINE
                    // main-menu label to deliver a string nobody can act on: the label is
                    // a UTextBlock, not a hyperlink, so the address was only ever
                    // retypeable, never clickable. What stays is the part that IS
                    // actionable -- WHICH build supersedes yours; a player who knows they
                    // are behind already knows where they got the mod.
                    line = DisplayVersion() + " -- UPDATE AVAILABLE: " +
                           (info.mod.empty() ? ("b" + std::to_string(info.proto)) : info.mod);
                } else {
                    // We are NEWER than the master's latest (a dev build) -- informational.
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
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
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

extern std::atomic<bool> g_listedState;  // defined below at SetListed (UI mirror)

void AnnounceEnvHostHidden(const std::string& name, const std::string& world) {
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- env announce skipped"); return; }
    const std::string masterUrl = MasterUrl();
    std::thread([masterUrl, name, world] {
        // try/catch: an exception escaping a detached thread is std::terminate. The
        // store(false) is OUTSIDE the try so g_actionBusy clears on EVERY path.
        try {
            if (coop::shutdown::IsShuttingDown()) { g_actionBusy.store(false); return; }
            const lobby::HostInfo info =
                Announcer().Host(masterUrl, name, world,
                                 /*locked=*/false, /*playersMax=*/4, 8000);
            if (info.ok) {
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                Announcer().SetListed(false); // the hide-from-list flag, immediately
                // seed the UI mirror too (same shape as HostWithSave) -- else the
                // scoreboard "Show in server browser" checkbox shows ON while hidden
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
    // THE SECRET THIS SESSION WILL REQUIRE, resolved ONCE and carried into whichever
    // of the three host paths runs. Empty unless `locked`, so the bool the caller
    // passes is the single thing that decides whether the lock has teeth -- an
    // announced `locked` with no password behind it is precisely the badge this
    // work exists to retire (security A2).
    //
    // It is read HERE rather than in `peer_admission` because the config layer opens
    // and line-scans the ini under a global mutex, and that is file I/O on the net
    // thread otherwise.
    // THE CALLER'S STRING, not a re-read of the row it wrote. See the header.
    std::string lobbyPw = locked ? password : std::string();
    // A LOCK WITH NO SECRET IS DOWNGRADED, NOT ANNOUNCED. The padlock in the browser is
    // a promise, and announcing one we cannot keep is precisely the badge-with-no-gate
    // this lane exists to retire -- so if there is nothing to check, the session is
    // honestly OPEN and the player is told.
    //
    // The native hosting flow cannot reach this state (its lock refuses to turn on
    // without a password). What can is the ImGui fallback's `locked` checkbox, which
    // predates the password entirely, and a hand-edited ini.
    if (locked && lobbyPw.empty()) {
        UE_LOGW("session_manager: asked to host LOCKED with an EMPTY password -- hosting "
                "OPEN instead. A padlock nothing enforces is worse than no padlock: it "
                "tells the host they are protected.");
        SetHostStatus("No password is set, so this session is OPEN -- set one in the "
                      "hosting window's session settings.");
        locked = false;
    }
    using coop::session::Reachability;
    // FORCED, NOT TRUSTED. An unlisted brokered lobby is unreachable by anyone, so a caller
    // that asked for one asked for a world nobody can join. The header promises this is
    // enforced here rather than assumed of every caller.
    if (mode.reach == Reachability::Brokered) mode.listed = true;
    const bool directConnection = (mode.reach == Reachability::Direct);
    const bool hideFromBrowser  = !mode.listed;

    // A DIRECT SESSION THE MASTER IS NEVER TOLD ABOUT -- nothing leaves the machine.
    //
    // THIS BRANCH ABSORBED "LAN ONLY" (2026-09-01). That mode was this branch plus an
    // accept filter refusing non-private remotes, and the filter was deleted whole: it did
    // the ROUTER's job, and if the port is not forwarded then local-only is what NAT
    // already gives you for free. What was left of LAN ONLY -- a Direct listen that never
    // contacts the master -- is exactly what this is, so the two are one and there is no
    // third connection type. See coop/session/host_mode.h.
    //
    // The old path announced FIRST (name, world, lock flag, cap, listen
    // port, identity, and the source IP the master resolves) and only then asked
    // to be un-listed, so a player who chose "hide" was registered for the lobby's
    // life with a heartbeat keeping the record warm. The announce is stashed
    // instead; the scoreboard's "Show in server browser" performs it if and when
    // they ask. No worker thread and no HTTP, exactly like LAN ONLY -- a DIRECT
    // Config is built from the listen port alone, so there is nothing in the
    // announce's reply this branch would have used.
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
        // THROUGH THE SAME BUILDER as the two fallback lines. This was the one
        // configuration a player DELIBERATELY chooses unlisted, and it was the one whose
        // sentence never mentioned the password -- so the audit fold that "fixed" the
        // wording fixed two of three sites and left the intentional one behind.
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
    // `hideFromBrowser` is deliberately NOT captured: the only branch that reads it
    // (directConnection && hideFromBrowser) returns above without ever creating this
    // worker, so carrying it in was a value the thread could not act on.
    std::thread([masterUrl, fallback, choice, name, locked, playersMax,
                 directConnection, lobbyPw] {
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
                // AUTO, AND THE MASTER DID NOT ANSWER. This used to keep P2P and dial
                // the fallback signaling server, which produced a world NOBODY COULD
                // REACH while the status line below told the player to use "LAN/direct"
                // -- the two things that configuration does not have. Start() is
                // either/or (`session_start.cpp`: P2P ? StartP2P : StartLanDirect), so
                // choosing P2P means no UDP listen socket exists at all.
                //
                // The reason it cannot work is DIRECTORY, not transport, and that is
                // what makes this unconditional rather than a signaling-reachability
                // test: a joiner needs the host's `gen:` identity to dial it, that
                // identity is published by /v1/join alone, and `net.host_identity` has
                // no UI that writes it. So an UNLISTED P2P host is unreachable even
                // when its signaling server is perfectly healthy -- and the official
                // signaling shares a BOX with the master anyway (protocol.h:1138-1139),
                // so it is usually down too.
                //
                // LanDirect is the one shape that stays reachable with no master alive:
                // a LAN friend finds it, a port-forwarded friend finds it, and both do
                // it through the Direct Connect box that already ships. RULE 1: pick
                // the configuration that can actually be joined, and say so honestly.
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
            // Seed the scoreboard mirror. No hidden term any more: a hidden DIRECT
            // lobby never reaches this worker (it returns above without announcing).
            g_listedState.store(listed, std::memory_order_relaxed);
            // DERIVED FROM THE TRANSPORT WE ACTUALLY CONFIGURED, not from what the
            // player picked: an AUTO host whose master was unreachable falls back to a
            // DIRECT listen just above, and this flag chooses the hide SEMANTICS
            // (retract via /v1/leave vs merely clearing the visibility flag). Keying it
            // on the player's choice would have made a genuinely-direct session take
            // the P2P branch. The three assignments to cfg.topology are the only
            // producers, so this reads exactly one of them.
            g_hostIsDirect.store(cfg.topology == net::Topology::LanDirect,
                                 std::memory_order_relaxed);
            if (listed) {
                SetOwnLobbyId(info.lobbyId);  // FIX 3: never list/join our own lobby
                // The announce-then-unlist branch that used to live here is GONE
                // (RULE 2). A hidden DIRECT lobby returns from HostWithSave before
                // this worker exists, so there is no longer any path that tells the
                // master about a lobby the player asked to hide. AUTO stays
                // un-hideable at host time for the reason it always was: the master
                // is a relay game's only rendezvous, so a hidden one is unjoinable
                // (user design call 2026-06-11).
                // ARM THE DEFERRAL EVEN THOUGH WE JUST ANNOUNCED. Hiding a DIRECT
                // lobby now RETRACTS it (/v1/leave) rather than clearing a flag, so
                // without this a Hide->Show cycle would have nothing to re-announce
                // from and the second tick would silently do nothing. Found by
                // re-reading my own Show path, not by a test.
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
                // The line now describes what actually happened. It used to promise
                // "LAN/direct only" for a P2P session that had neither.
                SetHostStatus(UnlistedDirectStatus("Hosting",
                                                   "master unreachable, NOT listed", locked));
                UE_LOGW("session_manager: HOST-WITH-SAVE ready (UNLISTED -- master '%s' unreachable) "
                        "-- fell back to a DIRECT listen on port %u so the session stays joinable",
                        DisplayMaster(masterUrl).c_str(), static_cast<unsigned>(directPort));
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
                EndHostedLobby();            // /leave + stop heartbeat (worker-safe)
                coop::join_progress::Reset();
            }
        }
        g_actionBusy.store(false);
    }).detach();
    return true;  // accepted -- the picker raises the host-boot cover + closes
}

namespace {

// The Minecraft-shape mismatch verdict (2026-07-19, user decision: the Paper
// PAIR -- game target + build number, no mod semver). Per-lobby EQUALITY gate,
// tier order game -> build; every tier hard-refuses; the popup names the FIRST
// mismatching axis and WHO updates (build direction is numeric). Empty/0 remote
// fields (old host / no row context) skip their tier -- the Join wire gate +
// header backstop cover. Returns empty = compatible.
std::string VersionMismatchVerdict(const std::string& hostGame, int hostProto) {
    // Tier 1 -- GAME cook. Reachable with an equal build by construction (a
    // new-cook adaptation need not change the wire), hence its own hard tier.
    if (!hostGame.empty() && hostGame != coop::version::kGameTarget) {
        return std::string("Host plays VOTV ") + hostGame + ", you have VOTV " +
               coop::version::kGameTarget + " -- game version mismatch.";
    }
    // Tier 2 -- BUILD (the wire revision; MC gates on the protocol number).
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
    // FIX 3 -- never connect to our OWN lobby (the 2026-06-08 repro: the host clicked its
    // own listed server + self-joined). Reject before raising any loading state.
    if (!lobbyId.empty() && lobbyId == OwnLobbyId()) {
        UE_LOGW("session_manager: refusing to join our OWN lobby '%s' -- you are the host", lobbyId.c_str());
        SetHostStatus("That's your own server -- you're already hosting it.");
        return false;
    }
    // VERSION GATE (v59 proto-only -> v122 game+build pair, 2026-07-19 Minecraft
    // shape; "show normally, reject on Join" browser policy). Pre-flight from the
    // browser row; the Join wire gate re-validates live and the header close stays
    // the final backstop. Reject via the connect-failed POPUP (RefuseJoin), not
    // the footer -- the user asked for a dialog that says the version is wrong.
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
                    cfg.lobbyPassword = TakeJoinPassword();
                    // WHICH HOST IS AT THAT ADDRESS, when the master tells us.
                    //
                    // THIS COMMENT USED TO CITE `lobby_client.cpp:284` AS PROOF THE VALUE
                    // WAS "guaranteed present here". It is not, and the citation was from
                    // the wrong branch: the direct path returns ~20 lines ABOVE that line
                    // and never reached the parse, and the master's direct response did not
                    // carry the field at all. So this assignment was a no-op writing an
                    // empty string, and the defect it claimed to fix -- a locked DIRECT
                    // lobby being unjoinable from the browser -- was still 100% live.
                    // Found by the audit of the commit that added it (2026-08-31); the real
                    // fix took a master change AND a client parse change, neither of which
                    // was in it.
                    //
                    // STILL EMPTY AGAINST AN OLD MASTER, deliberately -- see the parse
                    // site. Empty means unbound: open direct lobbies join fine, locked ones
                    // refuse with a sentence.
                    //
                    // Without it a joiner is unbound, and an unbound joiner may not send a
                    // password proof -- so a LOCKED lobby hosted in DIRECT mode refused
                    // every browser join even with the correct password typed. That is the
                    // exact defect the commit before this one claimed to have fixed: the
                    // fix landed in `ConnectDirect` and `ReadNetConfig` and missed the one
                    // path that never needed a fallback because it had the value in hand
                    // (post-ship audit, 2026-08-31).
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
        cfg.lobbyPassword = TakeJoinPassword();
        // WHICH HOST WE EXPECT TO FIND AT THAT ADDRESS, if the player was told.
        //
        // A direct connect names a PLACE, and until now that was all it named -- so the
        // joiner had nothing to bind the answering key to, and a LOCKED host was
        // unjoinable on this lane because the password proof refuses to be sent to an
        // unbound host (A65 + A2). Reading the row here is what gives a friend who was
        // given the host's `gen:` line the same binding an AUTO joiner gets from the
        // master, on the lane where there is no master to ask.
        //
        // Empty stays empty and that is a true statement, not a fallback: a player who
        // was given only an address is unbound, can still join any OPEN server, and is
        // refused by a locked one with a sentence saying why.
        cfg.hostIdentity =
            ::coop::config::ResolveString(::coop::config_registry::rows::net_host_identity);
        // THIS MACHINE NAMED THIS ADDRESS -- a typed box or its own configuration; see
        // `net::Config::selfAddressed` for why the wider wording is the accurate one. This
        // is the only place in the tree that sets it.
        cfg.selfAddressed = true;
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

bool ConnectP2PDirect(const std::string& hostIdentity, const net::Config& fallback) {
    // The P2P twin of ConnectDirect: dial a host BY IDENTITY through a signaling
    // server, with NO master in the loop. Two callers want exactly this and
    // neither can use JoinLobby -- the env test client (p2p_smoke, which had no
    // P2P path at all: `[V]` since 77225106, 2026-06-10, the env client has gone
    // through ConnectDirect regardless of net.topology, so the P2P CLIENT lane has
    // been unreachable from the rig for three months) and a dev dialling a host
    // whose `gen:` line they copied out of its log.
    //
    // The signaling/ICE half comes from the ALREADY-RESOLVED config the caller
    // holds, not from a second read: FillP2PFields is the one place those fields
    // are assembled, and re-deriving them here is how two paths drift.
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

// Mirror of the lobby's current listed state for the UI (the scoreboard's
// Hide-from-browser toggle renders it; HostWithSave seeds it, SetListed flips
// it). True when no lobby exists (harmless default).
std::atomic<bool> g_listedState{true};

// Serialises listing TRANSITIONS. Every branch below either blocks (Announcer::Host
// is an 8 s round trip; Announcer::Stop joins the heartbeat thread and POSTs
// /v1/leave) or depends on `Announcer().active()`, so they must not interleave.
//
// The first version of this decided the branch on the CALLING thread and dispatched
// the work asynchronously -- which meant an untick followed quickly by a re-tick read
// `active()` as still true, because the retract had not run yet, and took the
// visibility-flag branch against a lobby that was about to be retracted out from
// under it. Deciding INSIDE the worker, under this mutex, is what removes that: each
// transition sees the state the previous one actually left.
std::mutex g_listingMu;

void SetListed(bool listed) {
    g_listedState.store(listed, std::memory_order_relaxed);  // the UI mirror, immediately
    // Everything else on a worker: the scoreboard calls this from its click handler on
    // the game thread, and `lobby_announcer.h` says Stop() must not run there.
    std::thread([listed] {
        if (coop::shutdown::IsShuttingDown()) return;
        std::lock_guard<std::mutex> lk(g_listingMu);
        try {
            // SHOW. If nothing was ever announced (a hidden DIRECT host), the tick IS
            // the announce -- there is no listing to flip, because there is no record
            // at all, and /v1/visibility would post against a lobby the master has
            // never heard of.
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
                // A re-announce mints a FRESH sessionId/token/lobbyId, so the self-join
                // guard has to be re-pointed at the new one or it would still be
                // guarding the retracted lobby's id (FIX 3 would silently regress).
                SetOwnLobbyId(info.lobbyId);
                UE_LOGI("session_manager: deferred announce done -- lobby=%s is now listed "
                        "(the master learns this host's address at THIS moment, not at host "
                        "time)", info.lobbyId.c_str());
                return;
            }

            // HIDE. Which endpoint tells the truth depends on the topology, and choosing
            // wrong is what made this tick a ONE-WAY DOOR for the host's address:
            // /v1/visibility clears a flag while the heartbeat keeps the record -- and
            // the IP in it -- alive and refreshed every 30 s. A DIRECT lobby can be
            // properly RETRACTED instead, and re-armed so a later tick announces afresh.
            // A P2P lobby cannot: the master is its only rendezvous, so leaving would
            // cut it off from every future joiner, and the flag is the honest limit of
            // what "hide" can mean there.
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

uint16_t HostListenPort() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_fallbackHostCfg.port ? g_fallbackHostCfg.port : net::kDefaultPort;
}

void SetPlayerCountSource(int (*fn)()) {
    // Announcer() is the leaked process-lifetime singleton; the setter only stores
    // the pointer, so installing it before any lobby exists is correct and the
    // count is live from the first heartbeat of every lobby this process hosts.
    Announcer().SetPlayerCountFn(fn);
}

void EndHostedLobby() {
    // Clear the host-side lobby state BEFORE the blocking delist: Announcer().Stop()
    // blocks (heartbeat join up to ~8s + /leave POST up to 5s), and a re-host landing
    // inside that window writes FRESH pending/own-lobby state -- a post-Stop() clear
    // would silently wipe the new host request (audit on c8aec14c, item 2).
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
