// coop/net/session_start.cpp -- Session lifecycle + topology dispatch.
//
// Extracted from session.cpp (2026-06-05) to bring that file back under the
// 800-LOC soft cap before the P2P branch lands, and to give the topology
// dispatch a clean home. Holds the GNS one-time init, the status-callback
// bridge, and Session::Start / Session::Stop.
//
// Topology dispatch lives in Start(): today only LanDirect (CreateListenSocketIP
// host / ConnectByIPAddress client). The zero-open-ports P2P branch
// (CreateListenSocketP2P / ConnectP2PCustomSignaling + a signaling client) is
// added here as a sibling -- everything downstream (PollGroup, status callback,
// receive loop, relay, lanes) is topology-blind and stays in the other TUs.
// See research/findings/votv-zero-ports-connectivity-ladder-design-2026-06-05.md.

#include "coop/net/session.h"

#include "ice_config.h"          // co-located: ICE STUN/TURN config (P2P)
#include "signaling_client.h"    // co-located: P2P signaling transport
#include "ue_wrap/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <chrono>
#include <random>
#include <thread>

namespace coop::net {

namespace {

// P2P virtual port: an internal demux key on the P2P listen/connect calls. One
// listen socket per host -> 0 on both ends (host listens on it, client dials it).
constexpr int kP2PVirtualPort = 0;


// The single live Session, published for the GNS global status callback to
// route back into (GNS callbacks are C function pointers with no user-data on
// the GLOBAL callback). Set in Start(), cleared in Stop() and on every Start()
// failure path.
std::atomic<Session*> g_session{nullptr};

// GNS library init is process-global + refcounted by GNS itself; we gate our
// one call behind this latch so repeated Start()/Stop() cycles (test harnesses
// reuse the Session) don't re-init.
std::mutex g_initMutex;
bool g_inited = false;

bool EnsureGnsInit() {
    std::lock_guard<std::mutex> lk(g_initMutex);
    if (g_inited) return true;
    SteamNetworkingErrMsg err{};
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        UE_LOGE("net: GameNetworkingSockets_Init failed: %s", err);
        return false;
    }
    // Raise the send-rate ceiling. GNS defaults SendRateMax to ~256 KB/s -- which a coop
    // session's RELIABLE bursts saturate: the ~368 KB connect-snapshot, and the world-change
    // re-seed that re-sends the full snapshot whenever the host's churning world mass-purges
    // props. At 256 KB/s each burst takes ~0.8-1.4 s of fully-saturated outbound, during which
    // the unreliable POSE stream is starved of bandwidth -- so the REMOTE PLAYER lags while a
    // client's prop edits (which never send big reliable bursts that direction) stay real-time
    // (the exact user-reported asymmetry; measured 2026-06-06: net-diag SEND BACKLOG
    // pendRel=204KB @ sendRate=262144B/s, puppet trail spiking to 256 cm). On LAN there is zero
    // reason to cap at 256 KB/s. Min 1 MB/s = an immediate floor so the snapshot doesn't crawl
    // during GNS slow-start; Max 25 MB/s lifts the ceiling for a fast path. GNS still adapts the
    // rate DOWN toward Min on a lossy link, so this only raises the headroom. Global default ->
    // every connection (LanDirect + P2P). [P2P-over-slow-internet follow-up: make Min topology-
    // aware so a genuinely thin uplink isn't forced to 1 MB/s.]
    if (auto* utils = SteamNetworkingUtils()) {
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, 1 * 1024 * 1024);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, 25 * 1024 * 1024);
    }
    g_inited = true;
    UE_LOGI("net: GameNetworkingSockets_Init OK (send rate raised: min 1 MB/s, max 25 MB/s)");
    return true;
}

void ConnStatusTrampoline(SteamNetConnectionStatusChangedCallback_t* cb) {
    Session::OnConnStatusChanged(cb);
}

}  // namespace

void Session::OnConnStatusChanged(void* info) {
    auto* self = g_session.load(std::memory_order_acquire);
    if (self) self->HandleConnStatusChanged(info);
}

bool Session::Start(const Config& cfg) {
    if (running_.load()) {
        UE_LOGW("net: Session::Start ignored -- already running");
        return false;
    }
    cfg_ = cfg;
    net_stats::ResetSession();  // a new session's traffic totals start at zero

    // PR-FOUNDATION-1b v16: mint this peer's per-process session epoch.
    // Non-zero is required (0 is the receiver-side "not yet latched"
    // sentinel in expectedEpoch_), so re-roll on the 1/2^32 zero. Random
    // device gives us a value that's unpredictable to off-path attackers
    // and effectively guaranteed to differ between the previous and next
    // generation after a disconnect/reconnect cycle (vs the v14/v15
    // monotonic 8-bit counter that aliased at 256 cycles).
    {
        std::random_device rd;
        do { ownEpoch_ = rd(); } while (ownEpoch_ == 0);
    }
    // Clear any stale latches from a previous Start()/Stop() cycle on
    // this same Session instance (test harnesses reuse the object).
    for (int i = 0; i < kMaxPeers; ++i) expectedEpoch_[i] = 0;
    // Clear stale LOCAL-stream "has published" flags too. The net thread isn't
    // spawned yet (no concurrency here), so no lock needed -- same as the epoch
    // clear above. Without this, a Session reused after a Stop() that happened
    // mid-ragdoll (or mid-hold) would carry hasLocalRagdoll_/hasLocalProp_=true
    // into the new session and the first net-thread send would fan out the PRIOR
    // session's stale pelvis/prop pose before the game thread's first pump tick
    // republishes current state. (v22 fixes the new ragdoll flag + the pre-existing
    // pose/prop ones at the root -- a fresh session has published nothing yet.)
    hasLocal_ = false;
    hasLocalProp_ = false;
    hasLocalRagdoll_ = false;

    if (!EnsureGnsInit()) return false;

    g_session.store(this, std::memory_order_release);
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &ConnStatusTrampoline);

    // Topology dispatch -- the ONLY place the transport differs. Everything
    // downstream (net thread, PollGroup receive, session_relay fan-out, lanes,
    // epoch latch, inbox drain) is topology-blind: it operates on
    // HSteamNetConnection handles regardless of how they were established.
    const bool ok = (cfg_.topology == Topology::P2P) ? StartP2P() : StartLanDirect();
    if (!ok) {
        g_session.store(nullptr, std::memory_order_release);
        return false;
    }

    state_.store(ConnState::Handshaking);
    for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
    running_.store(true);
    thread_ = std::thread(&Session::NetThread, this);
    UE_LOGI("net: session started role=%s topology=%s sendHz=%d",
            cfg_.role == Role::Host ? "host" : "client",
            cfg_.topology == Topology::P2P ? "P2P" : "LanDirect",
            cfg_.sendHz);
    return true;
}

// rung 0/1: the original IP transport. Host binds a UDP listen socket on
// cfg_.port (port-forwarded or LAN-reachable); client dials cfg_.peerIp:port.
// Returns false on failure; the caller (Start) clears g_session + returns false.
bool Session::StartLanDirect() {
    auto* sockets = SteamNetworkingSockets();
    if (cfg_.role == Role::Host) {
        SteamNetworkingIPAddr addr{};
        addr.Clear();
        addr.m_port = cfg_.port;
        const HSteamListenSocket hListen = sockets->CreateListenSocketIP(addr, 0, nullptr);
        if (hListen == k_HSteamListenSocket_Invalid) {
            UE_LOGE("net: CreateListenSocketIP(port=%u) failed", cfg_.port);
            return false;
        }
        hListen_.store(hListen);

        // PR-4: a PollGroup lets the net thread drain messages from ALL
        // accepted client connections in one call. AcceptConnection adds the
        // new client to this group via SetConnectionPollGroup.
        const HSteamNetPollGroup hPoll = sockets->CreatePollGroup();
        if (hPoll == k_HSteamNetPollGroup_Invalid) {
            UE_LOGE("net: CreatePollGroup failed");
            sockets->CloseListenSocket(hListen);
            hListen_.store(0);
            return false;
        }
        hPollGroup_.store(hPoll);
        UE_LOGI("net: host listening on port %u (hListen=0x%08x hPoll=0x%08x), capacity=%d clients",
                cfg_.port, static_cast<unsigned>(hListen),
                static_cast<unsigned>(hPoll), kMaxPeers - 1);
    } else {  // Client
        SteamNetworkingIPAddr addr{};
        if (!addr.ParseString(cfg_.peerIp.c_str())) {
            UE_LOGE("net: client peer IP '%s' did not parse", cfg_.peerIp.c_str());
            return false;
        }
        addr.m_port = cfg_.port;
        const HSteamNetConnection hConn = sockets->ConnectByIPAddress(addr, 0, nullptr);
        if (hConn == k_HSteamNetConnection_Invalid) {
            UE_LOGE("net: ConnectByIPAddress(%s:%u) failed", cfg_.peerIp.c_str(), cfg_.port);
            return false;
        }
        // Slot 0 = host (per the players::Registry indexing -- on a client,
        // the host occupies slot 0).
        peerConns_[0].store(hConn);
        UE_LOGI("net: client dialed %s:%u (hConn=0x%08x slot=0)",
                cfg_.peerIp.c_str(), cfg_.port, static_cast<unsigned>(hConn));
    }
    return true;
}

// rungs 1-3: the zero-open-ports P2P transport. Sets our signaling identity,
// applies the ICE (STUN/TURN) config, stands up the signaling-server transport,
// then host CreateListenSocketP2P / client ConnectP2PCustomSignaling. ICE then
// hole-punches (rung 2) or relays via TURN (rung 3). Once the connection handle
// exists, everything downstream is identical to LanDirect.
bool Session::StartP2P() {
    auto* sockets = SteamNetworkingSockets();

    // 1) Our concrete signaling identity. P2P requires a non-localhost identity;
    //    ResetIdentity post-init sets it cleanly without threading identity
    //    through the latched, process-global GameNetworkingSockets_Init.
    if (cfg_.localIdentity.empty()) {
        UE_LOGE("net: P2P requires a non-empty localIdentity");
        return false;
    }
    SteamNetworkingIdentity self;
    self.Clear();
    if (!self.SetGenericString(cfg_.localIdentity.c_str())) {
        UE_LOGE("net: localIdentity '%s' too long (max 31 chars)", cfg_.localIdentity.c_str());
        return false;
    }
    sockets->ResetIdentity(&self);
    UE_LOGI("net: P2P identity set to '%s'", cfg_.localIdentity.c_str());

    // 2) ICE config (STUN/TURN + which candidate types to share). Global config
    //    values -- one session per process.
    IceConfig ice;
    ice.stunList = cfg_.stunList;
    ice.turnList = cfg_.turnList;
    ice.turnUser = cfg_.turnUser;
    ice.turnPass = cfg_.turnPass;
    // ICE candidate policy from config (default All = share host+reflexive+relay).
    // "relay" forces the TURN relay path (privacy, or to validate coturn
    // end-to-end); "disable" = no ICE; "default" = leave GNS's default.
    if (cfg_.iceMode == "relay")        ice.enable = IceEnable::RelayOnly;
    else if (cfg_.iceMode == "disable") ice.enable = IceEnable::Disable;
    else if (cfg_.iceMode == "default") ice.enable = IceEnable::Default;
    else                                ice.enable = IceEnable::All;   // "" / "all"
    ApplyGlobalIceConfig(ice);

    // 3) Signaling transport (out-of-band rendezvous for the opaque ICE blobs).
    //    Constructed AFTER ResetIdentity so its greeting carries our identity.
    if (cfg_.signalingUrl.empty()) {
        UE_LOGE("net: P2P requires a signalingUrl");
        return false;
    }
    if (cfg_.signalingToken.empty()) {
        UE_LOGE("net: P2P requires a signalingToken (shared signaling-server auth token)");
        return false;
    }
    signaling_ = SignalingClient::Create(cfg_.signalingUrl, cfg_.signalingToken, sockets);
    if (!signaling_) {
        UE_LOGE("net: failed to create signaling client for '%s'", cfg_.signalingUrl.c_str());
        return false;
    }

    // 4) Listen (host) / Connect (client).
    if (cfg_.role == Role::Host) {
        const HSteamListenSocket hListen =
            sockets->CreateListenSocketP2P(kP2PVirtualPort, 0, nullptr);
        if (hListen == k_HSteamListenSocket_Invalid) {
            UE_LOGE("net: CreateListenSocketP2P failed");
            signaling_.reset();
            return false;
        }
        hListen_.store(hListen);

        const HSteamNetPollGroup hPoll = sockets->CreatePollGroup();
        if (hPoll == k_HSteamNetPollGroup_Invalid) {
            UE_LOGE("net: CreatePollGroup failed");
            sockets->CloseListenSocket(hListen);
            hListen_.store(0);
            signaling_.reset();
            return false;
        }
        hPollGroup_.store(hPoll);
        UE_LOGI("net: P2P host listening as '%s' via signaling %s "
                "(hListen=0x%08x hPoll=0x%08x), capacity=%d clients",
                cfg_.localIdentity.c_str(), cfg_.signalingUrl.c_str(),
                static_cast<unsigned>(hListen), static_cast<unsigned>(hPoll),
                kMaxPeers - 1);
    } else {  // Client
        if (cfg_.hostIdentity.empty()) {
            UE_LOGE("net: P2P client requires hostIdentity (the host to dial)");
            signaling_.reset();
            return false;
        }
        SteamNetworkingIdentity hostId;
        hostId.Clear();
        if (!hostId.SetGenericString(cfg_.hostIdentity.c_str())) {
            UE_LOGE("net: hostIdentity '%s' too long (max 31 chars)", cfg_.hostIdentity.c_str());
            signaling_.reset();
            return false;
        }
        // Per-connection signaling object. GNS takes ownership in
        // ConnectP2PCustomSignaling (and Release()s it if the call fails).
        ISteamNetworkingConnectionSignaling* connSig =
            signaling_->CreateSignalingForConnection(hostId);
        if (!connSig) {
            UE_LOGE("net: CreateSignalingForConnection failed");
            signaling_.reset();
            return false;
        }
        const HSteamNetConnection hConn = sockets->ConnectP2PCustomSignaling(
            connSig, &hostId, kP2PVirtualPort, 0, nullptr);
        if (hConn == k_HSteamNetConnection_Invalid) {
            UE_LOGE("net: ConnectP2PCustomSignaling to '%s' failed", cfg_.hostIdentity.c_str());
            signaling_.reset();
            return false;
        }
        // Slot 0 = host (players::Registry indexing -- on a client the host is
        // slot 0), exactly like LanDirect.
        peerConns_[0].store(hConn);
        UE_LOGI("net: P2P client dialing '%s' via signaling %s (hConn=0x%08x slot=0)",
                cfg_.hostIdentity.c_str(), cfg_.signalingUrl.c_str(),
                static_cast<unsigned>(hConn));
    }
    return true;
}

void Session::Stop() {
    if (!running_.exchange(false)) return;
    // The linger flush needs RunCallbacks pumping. Closing connections
    // AFTER joining the net thread leaves linger=true inoperative -- no
    // one pumps callbacks once the thread is gone. Sequence is:
    //   1) signal exit + join (~<=5ms; thread exits its sleep window)
    //   2) CloseConnection(linger=true) on every peer
    //   3) RunCallbacks pump loop (~200ms) so GNS flushes lingering data
    //   4) DestroyPollGroup + CloseListenSocket
    // This way queued reliable PropSpawn/ItemActivate/TeleportClient at shutdown
    // get out instead of being silently dropped.
    if (thread_.joinable()) thread_.join();

    auto* sockets = SteamNetworkingSockets();
    if (sockets) {
        for (int i = 0; i < kMaxPeers; ++i) {
            const uint32_t hConn = peerConns_[i].exchange(0);
            if (hConn != 0) {
                sockets->CloseConnection(hConn, 0, "session stop", true);
            }
        }
        for (int i = 0; i < 20; ++i) {
            sockets->RunCallbacks();
            // P2P: closing a connection may need to send a final rendezvous
            // signal; keep pumping the signaling transport during the linger
            // window so those flush. No-op (nullptr) for LanDirect.
            if (signaling_) signaling_->Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const uint32_t hPoll = hPollGroup_.exchange(0);
        if (hPoll != 0) sockets->DestroyPollGroup(static_cast<HSteamNetPollGroup>(hPoll));
        const uint32_t hListen = hListen_.exchange(0);
        if (hListen != 0) sockets->CloseListenSocket(static_cast<HSteamListenSocket>(hListen));
    }

    // Make net_pump::Tick's per-peer disconnect edge reliable after ANY Stop()
    // (not just a peer-initiated close). That edge gates on IsSlotReady() ==
    // peerLanesConfigured_[slot]; those flags are normally cleared by the GNS
    // ClosedByPeer status callbacks, but those may not have dispatched during the
    // linger RunCallbacks loop above. Without an explicit clear, a Stop() while peers
    // were connected (e.g. the local-death disconnect) leaves the flag true ->
    // IsSlotReady() stays true -> the puppet-destroy edge never fires -> the puppet
    // actor leaks until full teardown. peerConns_ was already zeroed above; pair the
    // lanes flags with it. (Audit 2026-06-01, death-handling change.)
    for (int i = 0; i < kMaxPeers; ++i) peerLanesConfigured_[i].store(false);

    // Tear down the P2P signaling transport AFTER the net thread has joined (no
    // more Poll() racing us) and the connections have lingered (close-signals
    // flushed above). ~SignalingClient closes the socket + WSACleanup. No-op for
    // LanDirect (signaling_ is null).
    signaling_.reset();

    state_.store(ConnState::Disconnected);
    g_session.store(nullptr, std::memory_order_release);
    // Rates -> zero for the ui net-stats panel (its "offline" state); totals stay
    // visible until the next Session::Start resets them.
    net_stats::PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
    UE_LOGI("net: session stopped (sent=%llu recv=%llu)",
            static_cast<unsigned long long>(net_stats::PacketsSent()),
            static_cast<unsigned long long>(net_stats::PacketsRecv()));
}

}  // namespace coop::net
