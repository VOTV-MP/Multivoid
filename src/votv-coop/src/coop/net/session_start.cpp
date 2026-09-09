// coop/net/session_start.cpp -- the session lifecycle and the topology dispatch: the one-time
// GNS init, the status-callback bridge, Session::Start and Session::Stop. Start is the only
// place the transport differs (a direct IP listen or dial, or the zero-open-ports P2P path with
// a signaling client); everything downstream operates on connection handles and is
// topology-blind.

#include "coop/net/session.h"

#include "coop/config/config.h"           // ResolveInt for the fakelink drill knob
#include "coop/config/config_registry.h"  // rows::net_fakelink_kbs
#include "coop/net/peer_admission.h"
#include "coop/net/peer_identity.h"
#include "coop/player/nickname_arbiter.h"
#include "coop/text/case_fold.h"
#include "coop/text/repertoire.h"
#include "coop/text/novelty_ledger.h"
#include "coop/text/utf8_codec.h"

#include "ice_config.h"          // co-located: ICE STUN/TURN config (P2P)
#include "signaling_client.h"    // co-located: P2P signaling transport
#include "ue_wrap/core/log.h"

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

// The P2P virtual port, an internal demux key: one listen socket per host, so 0 on both ends.
constexpr int kP2PVirtualPort = 0;


// The single live Session, published for the GNS global status callback, which is a C function
// pointer with no user data. Set in Start, cleared in Stop and on every Start failure.
std::atomic<Session*> g_session{nullptr};

// The GNS library init is process-global; one call, behind a latch, so repeated Start and Stop
// cycles do not re-init.
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
    // The send-rate ceiling raised. GNS stock defaults the send rate to 256 KB/s, which a session's
    // reliable bursts saturate (the connect snapshot, the re-seed that re-sends it when the host's
    // world mass-purges props); during a saturated burst the unreliable pose stream is starved, so
    // the remote player lags while a client's own edits stay real-time. This GNS build has no rate
    // adaptation: the estimate is written once at connection init from the ping and then only
    // clamped into the range, so any internet ping above a few milliseconds runs at the minimum
    // for the session's life, and a host uplink slower than the minimum is overdriven with pure
    // loss and retransmits; the per-connection net.sendrate_kbs knob is the remedy. Global, so
    // every connection on both topologies.
    if (auto* utils = SteamNetworkingUtils()) {
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, 1 * 1024 * 1024);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, 25 * 1024 * 1024);
        // The overdrive drill knob: a thin outbound link simulated with GNS's send policer, which
        // silently drops packets beyond the token budget. 0 is off, the shipped default.
        const long fakeKbs =
            coop::config::ResolveInt(coop::config_registry::rows::net_fakelink_kbs);
        if (fakeKbs > 0) {
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate,
                                             static_cast<int32>(fakeKbs) * 1024);
            UE_LOGW("net: FAKE LINK LIMIT %ld KB/s outbound (drill knob net.fakelink_kbs) -- "
                    "packets beyond the budget are silently dropped",
                    fakeKbs);
        }
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

    // This peer's per-process session epoch, minted non-zero (0 is the receiver's "not yet latched"
    // sentinel) from a random device, so it is unpredictable off-path and differs between the
    // generations of a disconnect and reconnect cycle.
    {
        std::random_device rd;
        do { ownEpoch_ = rd(); } while (ownEpoch_ == 0);
    }
    // Stale latches from a previous cycle on this Session instance cleared.
    for (int i = 0; i < kMaxPeers; ++i) expectedEpoch_[i] = 0;
    // The per-slot occupancy generations too: a reused Session must not open with slots that look
    // occupied. The counter is not reset, so generations stay unique across cycles and a stale
    // captured token can never alias a fresh occupant.
    for (int i = 0; i < kMaxPeers; ++i) peerGenBySlot_[i].store(0, std::memory_order_relaxed);
    // The local-stream "has published" flags too (no lock: the net thread is not spawned yet). A
    // Session reused after a Stop mid-ragdoll would otherwise fan out the prior session's pelvis or
    // prop pose on its first send, before the game thread republishes.
    hasLocal_ = false;
    hasLocalProp_ = false;
    hasLocalRagdoll_ = false;

    if (!EnsureGnsInit()) return false;

    // This install's durable identity goes into GNS before any socket exists: the public key is the
    // identity (GNS's 32-byte GenericBytes form), so every connection this process makes or
    // accepts carries a name its holder can be asked to prove at the Connected edge. A failure is
    // fatal to the session: starting anyway would present an identity we cannot sign for.
    if (!peer_identity::InstallInto(SteamNetworkingSockets())) {
        UE_LOGE("net: refusing to start -- the durable identity could not be installed");
        return false;
    }

    // The link classifier's self-test, once per process, here because this is the first point GNS
    // is initialised, and the two kinds it proves (direct, relayed) are unreachable by any LAN
    // drill. A function-local static makes it exactly once; the smoke greps its one PASS line.
    static const bool kLinkClassifyOk = RunLinkClassifySelftest();
    (void)kLinkClassifyOk;

    // The nickname arbiter's self-test, the same discipline: its policy is a pure function, and its
    // interesting cases (a suffix displacing stem characters at the cap, a variant colliding with a
    // name another player holds) would need four peers with chosen names to stage.
    static const bool kNickArbiterOk = coop::nickname_arbiter::RunNicknameArbiterSelftest();
    (void)kNickArbiterOk;

    // The UTF-8 codec's self-test: its interesting cases are an ill-formed byte sequence a peer
    // would have to send deliberately, and a cap landing mid-character.
    static const bool kCodecOk = coop::text::RunUtf8CodecSelftest();
    (void)kCodecOk;

    // The repertoire table's self-test: the table is generated, and a generated table is a claim
    // about a build step nobody watches. It asserts the shape the binary search needs (sorted,
    // disjoint) and the membership facts the fold depends on.
    static const bool kRepertoireOk = coop::text::RunRepertoireSelftest();
    (void)kRepertoireOk;

    // The case table beside it, generated in the same run. Its rows are positive: a generated table
    // that arrives empty folds nothing, and "no two names collided" is also what a healthy lobby
    // looks like, so there is no negative symptom to grep for.
    static const bool kCaseFoldOk = coop::text::RunCaseFoldSelftest();
    (void)kCaseFoldOk;

    // The receive-boundary novelty cap's self-test: its interesting case is a peer sending a
    // deliberately diverse alphabet, which no LAN drill stages, and its first draft passed by
    // construction for every budget above 32.
    static const bool kNoveltyOk = coop::text::RunNoveltyLedgerSelftest();
    (void)kNoveltyOk;

    g_session.store(this, std::memory_order_release);
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &ConnStatusTrampoline);

    // The topology dispatch, the only place the transport differs; the net thread, the poll-group
    // receive, the relay, the lanes, the epoch latch and the inbox drain operate on connection
    // handles regardless of how they were established.
    const bool ok = (cfg_.topology == Topology::P2P) ? StartP2P() : StartLanDirect();
    if (!ok) {
        g_session.store(nullptr, std::memory_order_release);
        return false;
    }

    // Nothing carries across from a previous attempt: the host close reason is first-writer-wins,
    // so a reason parked by an attempt whose consumers never fired (an env or autotest client)
    // would otherwise be shown to the player as the explanation for the next browser join that
    // failed.
    { std::lock_guard<std::mutex> lk(hostCloseMutex_); hostCloseReason_.clear(); }
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

// The direct IP transport: the host binds a UDP listen socket on the configured port, the
// client dials the peer address. False on failure; Start clears the published session.
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

        // A poll group lets the net thread drain messages from every accepted connection in one
        // call; AcceptConnection adds each new client to it.
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
        // GEN: none -- a client never mints an occupancy generation. Slot 0 is the host; the
        // generation is the host's authority over slot recycling and a client's roster is
        // wire-driven, so minting here would fight the wire.
        peerConns_[0].store(hConn);
        UE_LOGI("net: client dialed %s:%u (hConn=0x%08x slot=0)",
                cfg_.peerIp.c_str(), cfg_.port, static_cast<unsigned>(hConn));
    }
    return true;
}

// The zero-open-ports P2P transport: the ICE configuration (STUN and TURN), the
// signaling-server transport, then a P2P listen (host) or a custom-signaling connect (client).
// ICE hole-punches or relays through TURN; once the connection handle exists, everything
// downstream is as for the direct transport.
bool Session::StartP2P() {
    auto* sockets = SteamNetworkingSockets();

    // 1. The signaling identity is the durable identity Start installed. One identity serves both
    // the rendezvous and the proof: a second, per-session identity installed here once replaced the
    // durable one and the admission challenge would have had nothing to verify against on P2P.
    if (peer_identity::LocalIdentityString().empty()) {
        UE_LOGE("net: P2P requires the durable identity -- it was not installed");
        return false;
    }

    // 2. The ICE configuration: global values, one session per process.
    IceConfig ice;
    ice.stunList = cfg_.stunList;
    ice.turnList = cfg_.turnList;
    ice.turnUser = cfg_.turnUser;
    ice.turnPass = cfg_.turnPass;
    // The candidate policy: all (host, reflexive and relay) by default; relay-only forces the TURN
    // path (privacy, or to validate the relay end to end); disable turns ICE off; default leaves
    // GNS's own.
    if (cfg_.iceMode == "relay")        ice.enable = IceEnable::RelayOnly;
    else if (cfg_.iceMode == "disable") ice.enable = IceEnable::Disable;
    else if (cfg_.iceMode == "default") ice.enable = IceEnable::Default;
    else                                ice.enable = IceEnable::All;   // "" / "all"
    ApplyGlobalIceConfig(ice);

    // 3. The signaling transport, the out-of-band rendezvous for the opaque ICE blobs; constructed
    // after the identity install, so its greeting carries our identity.
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

    // 4. Listen (host) or connect (client).
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
                peer_identity::LocalIdentityString().c_str(), cfg_.signalingUrl.c_str(),
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
        // ParseString, not SetGenericString: a host publishes its durable identity, which renders
        // as `gen:<64 hex>` (68 characters), over the generic string's cap of 31, and a generic
        // string would dial the literal text rather than the key. No fallback to any other shape:
        // join compatibility is byte equality on the version pair, so a client of this build only
        // ever dials a host of this build, which always publishes `gen:`.
        if (!hostId.ParseString(cfg_.hostIdentity.c_str())) {
            UE_LOGE("net: hostIdentity '%s' is not a parseable identity",
                    cfg_.hostIdentity.c_str());
            signaling_.reset();
            return false;
        }
        // The per-connection signaling object; GNS takes ownership in ConnectP2PCustomSignaling and
        // releases it if the call fails.
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
        // GEN: none -- client dial; see the LanDirect site above for the reason. Slot 0 is the
        // host, as for the direct transport.
        peerConns_[0].store(hConn);
        UE_LOGI("net: P2P client dialing '%s' via signaling %s (hConn=0x%08x slot=0)",
                cfg_.hostIdentity.c_str(), cfg_.signalingUrl.c_str(),
                static_cast<unsigned>(hConn));
    }
    return true;
}

void Session::Stop() {
    if (!running_.exchange(false)) return;
    // The linger flush needs RunCallbacks pumping, so connections are closed after the net thread
    // is joined and the callbacks pumped by hand: signal exit and join, close every peer with
    // linger, pump for about 200 ms so GNS flushes the queued reliable data, then destroy the poll
    // group and the listen socket.
    if (thread_.joinable()) thread_.join();

    // After the join, never before: the client's admission state dies with the session (a stale
    // proved flag would let the next connection's slot assignment through unchallenged), and
    // peer_admission owns it without a lock on the claim that only the net thread touches it;
    // cleared above the join, this write raced a pass still in flight during the few milliseconds
    // until the thread exited.
    peer_admission::ClientReset();

    auto* sockets = SteamNetworkingSockets();
    if (sockets) {
        for (int i = 0; i < kMaxPeers; ++i) {
            // GEN: clear -- session teardown empties every slot. A Session sits stopped between
            // Stop and the next Start, and a generation left live across that window would read
            // as an occupied slot with no session behind it.
            const uint32_t hConn = peerConns_[i].exchange(0);
            peerGenBySlot_[i].store(0, std::memory_order_release);
            backlog_.FreeSlot(i);  // queued state dies with the session
            relayEligible_[i].store(0, std::memory_order_release);
            if (hConn != 0) {
                sockets->CloseConnection(hConn, 0, "session stop", true);
            }
        }
        for (int i = 0; i < 20; ++i) {
            sockets->RunCallbacks();
            // P2P: closing a connection may need a final rendezvous signal, so the signaling
            // transport is polled through the linger window too. Null on the direct transport.
            if (signaling_) signaling_->Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const uint32_t hPoll = hPollGroup_.exchange(0);
        if (hPoll != 0) sockets->DestroyPollGroup(static_cast<HSteamNetPollGroup>(hPoll));
        const uint32_t hListen = hListen_.exchange(0);
        if (hListen != 0) sockets->CloseListenSocket(static_cast<HSteamListenSocket>(hListen));
    }

    // The pump's per-peer disconnect edge gates on the lanes-configured flags, normally cleared by
    // the peer-closed status callbacks, which may not have dispatched during the linger loop above;
    // left true after a Stop with peers connected, the edge never fired and the puppet leaked until
    // full teardown. Cleared here beside the zeroed connections.
    for (int i = 0; i < kMaxPeers; ++i) peerLanesConfigured_[i].store(false);

    // The signaling transport torn down after the net thread has joined (no Poll racing us) and the
    // close signals have flushed; its destructor closes the socket. Null on the direct transport.
    signaling_.reset();

    state_.store(ConnState::Disconnected);
    g_session.store(nullptr, std::memory_order_release);
    // Rates to zero for the net-stats panel's offline state; the totals stay visible until the next
    // Start resets them.
    net_stats::PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
    UE_LOGI("net: session stopped (sent=%llu recv=%llu)",
            static_cast<unsigned long long>(net_stats::PacketsSent()),
            static_cast<unsigned long long>(net_stats::PacketsRecv()));
}

}  // namespace coop::net
