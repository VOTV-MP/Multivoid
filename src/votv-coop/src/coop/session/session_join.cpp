// coop/session/session_join.cpp -- the session manager's join lane: a lobby join through its master,
// a direct connect to an address and a P2P connect to an identity, the join password the prompt lends
// them, and the version pre-flight a lobby join makes first. The host lane is session_manager.cpp.

#include "coop/session/session_manager.h"
#include "coop/game_build/game_build.h"

#include "session_manager_internal.h"  // co-located: the action latch, QueueStart, LobbyP2PConfig

#include "coop/config/config.h"           // Resolve* -- the join password and host identity rows
#include "coop/config/config_registry.h"
#include "coop/net/endpoint_log.h"
#include "coop/net/master_slots.h"  // DefaultSignalingUrl -- a P2P connect's relay when none is named
#include "coop/net/protocol.h"      // kProtocolVersion, kDefaultPort, kReleasesUrl
#include "coop/session/join_progress.h"
#include "coop/props/prop_key_index.h"  // Unavailable -- an install that cannot index starts no session
#include "coop/session/shutdown.h"
#include "coop/version.h"  // kGameTarget -- the game half of the pre-flight's pair
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"   // IsInstalled -- a script loop that could not be hooked starts none

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace coop::session_manager {

using namespace internal;

namespace {

namespace lobby = coop::net::lobby;
namespace slots = coop::net::master_slots;

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

// The version verdict: per-lobby equality on the pair, game target then build, each tier a hard
// refusal; the popup names the first mismatching axis and who updates. Empty or zero remote
// fields skip their tier (the Join wire gate and the header backstop cover them). Empty =
// compatible.
struct PreflightVerdict {
    net::EndReason code = net::EndReason::None;  // None = compatible
    std::string    text;                         // the two builds, for the notice's detail
};

PreflightVerdict VersionMismatchVerdict(const std::string& hostGame, int hostProto,
                                        const std::string& hostGameBuild) {
    // Tier 1, the game cook: reachable with an equal build (a recook adaptation need not change the
    // wire), hence its own tier.
    if (!hostGame.empty() && hostGame != coop::version::kGameTarget) {
        return {net::EndReason::GameVersionMismatch,
                std::string("host plays VOTV ") + hostGame + ", you have VOTV " +
                    coop::version::kGameTarget};
    }
    // Tier 1b, the game each side runs: one Multivoid on two different games. The row's key, when the
    // master lists it; the Join's own gate compares it either way.
    if (coop::game_build::DiffersFromOurs(hostGameBuild)) {
        return {net::EndReason::GameVersionMismatch,
                "host plays VOTV " + coop::game_build::VersionText(hostGameBuild) + ", you have VOTV " +
                    coop::game_build::VersionText(coop::game_build::Running().id.key)};
    }
    // Tier 2, the build (the wire revision).
    if (hostProto > 0 && hostProto != static_cast<int>(net::kProtocolVersion)) {
        const bool hostNewer = hostProto > static_cast<int>(net::kProtocolVersion);
        return {hostNewer ? net::EndReason::HostNewer : net::EndReason::HostOlder,
                std::string("host runs b") + std::to_string(hostProto) + ", you run b" +
                    std::to_string(net::kProtocolVersion) +
                    (hostNewer ? std::string(" -- update: ") + net::kReleasesUrl : "")};
    }
    return {};
}

}  // namespace

void SetJoinPassword(const std::string& password) {
    std::lock_guard<std::mutex> lk(g_joinPwMu);
    // An empty value is a clear, not an ignore (the opposite of SetNickname): a player who backs
    // out of the password prompt must not carry the last lobby's secret into the next connection.
    g_joinPassword = password;
}

net::Refusal SubstrateRefusal() {
    // MTA stops the whole process at load on a module it cannot use
    // (reference/mtasa-blue/Client/core/CCore.cpp:1095-1107,
    // reference/mtasa-blue/Server/core/CServerImpl.cpp:340-352); this mod augments a single-player game,
    // which keeps running, so only the session is refused. The script gate arms at boot or never, and the key index
    // installs on the first game-thread task after it (harness::Start), so both answers are in before any
    // action here can run.
    if (!ue_wrap::script_gate::IsInstalled())
        return {net::EndReason::ScriptHooksUnavailable, "the script loop could not be hooked"};
    if (coop::prop_key_index::Unavailable())
        return {net::EndReason::ScriptHooksUnavailable, "the prop key watches were refused"};
    return {};
}

namespace {
// A join this build cannot start, refused before any cover or connect, as the version gate refuses one;
// `out` (reset by the caller) gets the refusal.
bool RefusedBySubstrate(const char* what, net::Refusal* out) {
    const net::Refusal r = SubstrateRefusal();
    if (r.code == net::EndReason::None) return false;
    if (out) *out = r;
    UE_LOGW("session_manager: %s refused -- [%s] %s (%s)", what, net::Describe(r.code).id,
            net::Describe(r.code).text, r.detail.c_str());
    coop::join_progress::RefuseJoin(r.code, r.detail);
    return true;
}
}  // namespace

bool JoinLobby(const std::string& masterUrl, const std::string& lobbyId,
               const std::string& displayName, int hostProto, const std::string& hostGame,
               const std::string& hostGameBuild, net::Refusal* refused) {
    if (refused) *refused = net::Refusal{};
    // Never connect to our own lobby (the host clicking its own listed server); rejected before any
    // loading state is raised.
    if (!lobbyId.empty() && lobbyId == OwnLobbyId()) {
        UE_LOGW("session_manager: refusing to join our OWN lobby '%s' -- you are the host", lobbyId.c_str());
        SetHostStatus("That's your own server -- you're already hosting it.");
        return false;
    }
    if (RefusedBySubstrate("JOIN", refused)) return false;
    // The version gate, pre-flight from the browser row ("show normally, reject on Join"); the Join
    // wire gate re-validates live and the header close is the final backstop. Rejected through the
    // connect-failed popup, not the footer.
    {
        const PreflightVerdict verdict = VersionMismatchVerdict(hostGame, hostProto, hostGameBuild);
        if (verdict.code != net::EndReason::None) {
            UE_LOGW("session_manager: JOIN rejected -- [%s] %s (host game='%s' b%d; ours %s b%u)",
                    net::Describe(verdict.code).id, verdict.text.c_str(), hostGame.c_str(),
                    hostProto, coop::version::kGameTarget,
                    static_cast<unsigned>(net::kProtocolVersion));
            coop::join_progress::RefuseJoin(verdict.code, verdict.text);
            if (refused) *refused = net::Refusal{verdict.code, verdict.text};
            return false;
        }
    }
    if (g_actionBusy.exchange(true)) { UE_LOGW("session_manager: action busy -- Join ignored"); return false; }
    // Raise the browser-only loading state before the master round trip, so "Connecting to <name>"
    // shows at once; on a master failure the worker Fails it (drops the cover, reopens the
    // browser).
    const std::string label = displayName.empty() ? std::string("the server") : displayName;
    UE_LOGI("session_manager: JoinLobby '%s' -- connecting to lobby '%s'", lobbyId.c_str(),
            label.c_str());
    coop::join_progress::BeginConnect(label, coop::join_progress::Stage::FindingHost);
    std::thread([masterUrl, lobbyId] {
        try {
            // Shutdown race: BeginConnect raised the cover before this worker spawned, so every
            // exit drops it.
            if (coop::shutdown::IsShuttingDown()) {
                coop::join_progress::Fail(net::EndReason::ShuttingDown, "");
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
                                lobbyId.c_str(), ue_wrap::log::Addr(info.addr).c_str());
                        coop::join_progress::Fail(net::EndReason::BadAddress, "");
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
                    cfg = LobbyP2PConfig(net::Role::Client, info);
                    cfg.hostIdentity = info.hostIdentity;
                    cfg.lobbyPassword = TakeJoinPassword();
                    QueueStart(cfg);
                    UE_LOGI("session_manager: JOIN ready -- host=%s (session boot = harness Tier 2)",
                            info.hostIdentity.c_str());
                }
            } else if (!info.ok) {
                UE_LOGW("session_manager: JoinLobby '%s' failed", lobbyId.c_str());
                coop::join_progress::Fail(net::EndReason::MasterUnreachable, "");
            } else {
                // info.ok but shutdown raced true between the check and here: neither branch ran,
                // so drop the cover explicitly.
                coop::join_progress::Fail(net::EndReason::ShuttingDown, "");
            }
        } catch (const std::exception& e) {
            UE_LOGW("session_manager: JoinLobby worker exception: %s", e.what());
            coop::join_progress::Fail(net::EndReason::JoinError, e.what());
        }
        g_actionBusy.store(false);
    }).detach();
    return true;
}

bool ConnectDirect(const std::string& hostPort, net::Refusal* refused) {
    if (refused) *refused = net::Refusal{};
    if (RefusedBySubstrate("direct connect", refused)) return false;
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
        coop::join_progress::BeginConnect(host, coop::join_progress::Stage::Dialing);
        QueueStart(cfg);
        UE_LOGI("session_manager: DIRECT connect queued -> %s:%u (session boot = harness Tier 2)",
                ue_wrap::log::Addr(host).c_str(), static_cast<unsigned>(port));
    } else {
        UE_LOGW("session_manager: bad direct address '%s'",
                ue_wrap::log::Addr(hostPort).c_str());
    }
    g_actionBusy.store(false);
    return ok;
}

bool ConnectP2PDirect(const std::string& hostIdentity, const net::Config& p2pFields, net::Refusal* refused) {
    // The P2P twin of ConnectDirect: dial a host by identity through a signaling server with no
    // master in the loop, for the env test client and for a dev dialling a `gen:` line copied from
    // a log. The signaling and ICE half comes from the caller's already resolved config:
    // FillP2PFields is the one place those fields are assembled.
    if (refused) *refused = net::Refusal{};
    if (RefusedBySubstrate("P2P connect", refused)) return false;
    if (g_actionBusy.exchange(true)) {
        UE_LOGW("session_manager: action busy -- P2P connect ignored");
        return false;
    }
    bool ok = false;
    if (hostIdentity.empty()) {
        UE_LOGW("session_manager: P2P connect needs a host identity (`gen:<64 hex>`)");
    } else if (p2pFields.signalingUrl.empty() && slots::DefaultSignalingUrl().empty()) {
        // An empty relay is the chosen master's (the P2P entry resolves it); with no master
        // either, there is nowhere to rendezvous.
        UE_LOGW("session_manager: P2P connect needs a signaling server (net.signaling is empty "
                "and no master is chosen)");
    } else {
        net::Config cfg = p2pFields;
        cfg.role = net::Role::Client;
        cfg.topology = net::Topology::P2P;
        cfg.hostIdentity = hostIdentity;
        cfg.lobbyPassword = TakeJoinPassword();
        coop::join_progress::BeginConnect(hostIdentity, coop::join_progress::Stage::Dialing);
        QueueStart(cfg);
        UE_LOGI("session_manager: P2P connect queued -> host '%s' via signaling %s "
                "(session boot = harness Tier 2)", hostIdentity.c_str(),
                cfg.signalingUrl.empty()
                    ? "(the chosen master's)"
                    : coop::net::endpoint_log::LogEndpoint(cfg.signalingUrl).c_str());
        ok = true;
    }
    g_actionBusy.store(false);
    return ok;
}


}  // namespace coop::session_manager
