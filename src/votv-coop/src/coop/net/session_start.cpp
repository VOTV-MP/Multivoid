// coop/net/session_start.cpp -- the session lifecycle and the topology dispatch: the one-time
// GNS init, the status-callback bridge, Session::Start and Session::Stop. Start is the only
// place the transport differs (a direct IP listen or dial, or the zero-open-ports P2P path with
// a signaling client); everything downstream operates on connection handles and is
// topology-blind.

#include "coop/net/session.h"

#include "coop/config/config.h"           // ResolveInt for the fakelink drill knob
#include "coop/config/config_registry.h"  // rows::net_fakelink_kbs, the connect-cap rows
#include "coop/net/connect_history.h"
#include "coop/net/master_slots.h"       // an empty relay is the chosen master's
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
    // NOTHING GLOBAL SETS THE SEND RATE ANY MORE. This is where a 1 MiB/s floor and a 25 MiB/s
    // ceiling used to be written for every connection on both topologies, on the premise that a
    // raised floor protects the unreliable pose stream from a saturated reliable burst. Both were
    // measured wrong: the floor was the RATE on every link a player has, since the transport writes
    // its estimate once at connect and thereafter only clamps it, and it shields nothing -- what
    // starves a pose datagram is the shared send buffer, and a reliable retransmission is gathered
    // ahead of the lane-priority loop. The ceiling needed an init ping under 0.17 ms that nothing
    // ever measured. The rate is now per connection and measured: `coop/net/send_rate_control`
    // decides it and `coop/net/connection_tuning` opens it, so a global write here would be a
    // second writer of a quantity that has an owner. docs/send-path.md carries the measurements.
    //
    // The overdrive drill knob is the one thing still written globally, and correctly so: it
    // simulates a thin outbound link with the transport's send policer, which silently drops
    // packets beyond the token budget. That is the PHYSICS of the box's uplink, not a policy about
    // a link. 0 is off, the shipped default.
    if (auto* utils = SteamNetworkingUtils()) {
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
    UE_LOGI("net: GameNetworkingSockets_Init OK (no global send-rate pin -- the rate is measured "
            "per connection)");
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
    // The measured-rate controller, unless the drill has pinned a fixed rate -- that knob is an
    // instrument, and an instrument that the controller overrides measures the controller.
    {
        const long pinKbs =
            coop::config::ResolveInt(coop::config_registry::rows::net_sendrate_kbs);
        const bool pinned = pinKbs > 0;
        const bool want = coop::config::ResolveFlag(coop::config_registry::rows::net_ratecontrol);
        rateControl_.Reset(want && !pinned, pinKbs);   // and its per-link measurements
        if (want && pinned)
            UE_LOGW("net: send-rate control OVERRIDDEN by net.sendrate_kbs -- the link is pinned, "
                    "not measured");
        else if (!want)
            UE_LOGW("net: send-rate control OFF (net.ratecontrol=0) -- nothing writes a rate, so "
                    "links run at the transport's own stock 256 KB/s, fixed for their whole life");
    }
    admission_.Reset();         // and every slot's send-buffer occupancy
    // The time bound's budget, resolved once per session for the same reason the rate pin is:
    // ResolveInt re-reads the ini on every call, so asking per send would let an ini edited
    // mid-session change the rule under a stream already running against it.
    {
        const long capMs =
            coop::config::ResolveInt(coop::config_registry::rows::bulk_queue_cap_ms);
        admission_.SetQueueCapMs(capMs);
        if (capMs <= 0)
            UE_LOGW("net: bulk queue cap OFF (bulk_queue_cap_ms=0) -- a world blob may stand "
                    "behind the whole send buffer again, which is minutes of queue on a slow link");
    }

    // This peer's per-process session epoch, minted non-zero (0 is the receiver's "not yet latched"
    // sentinel) from a random device, so it is unpredictable off-path and differs between the
    // generations of a disconnect and reconnect cycle.
    {
        std::random_device rd;
        do { ownEpoch_ = rd(); } while (ownEpoch_ == 0);
    }
    // The per-slot occupancy generations: a reused Session must not open with slots that look
    // occupied. The counter is not reset, so generations stay unique across cycles and a stale
    // captured token can never alias a fresh occupant.
    for (int i = 0; i < kMaxPeers; ++i) peerGenBySlot_[i].store(0, std::memory_order_relaxed);
    // The local-stream "has published" flags and the host's one-shot samples too (no lock: the net
    // thread is not spawned yet). A Session reused after a Stop would otherwise fan out the prior
    // session's last pose, pelvis, hand, cursor or host sample on its first send, before the game
    // thread publishes this session's.
    hasLocal_ = false;
    hasLocalProp_ = false;
    hasLocalRagdoll_ = false;
    hasLocalHand_ = false;
    hasLocalDeskCursor_ = false;
    hasLocalDeskSim_ = false;
    hostClockDue_ = false;
    dishPoseDirty_ = false;
    reelPoseDirty_ = false;

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

    // The host's connection cap, MTA's join-flood shape at the accept edge: its two ini rows
    // resolved once here and never per connection, since a resolve line-scans the ini under a
    // mutex and the accept edge is attacker-timed. Applying the policy also empties the table.
    if (cfg_.role == Role::Host) {
        connect_history::ConfigureConnects(
            coop::config::ResolveInt(coop::config_registry::rows::net_connect_cap),
            coop::config::ResolveInt(coop::config_registry::rows::net_connect_window_s));
    }

    g_session.store(this, std::memory_order_release);
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &ConnStatusTrampoline);

    // Nothing carries across from a previous attempt: the host close reason is first-writer-wins,
    // so a reason parked by an attempt whose consumers never fired (an env or autotest client)
    // would otherwise be shown to the player as the explanation for the next browser join that
    // failed. Before the dial, which publishes the first link stage: a reset after it wiped
    // that stage before the pump could read it.
    { std::lock_guard<std::mutex> lk(hostCloseMutex_); hostClose_ = HostClose{}; }
    linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);

    // The topology dispatch, the only place the transport differs; the net thread, the poll-group
    // receive, the relay, the lanes, the epoch latch and the inbox drain operate on connection
    // handles regardless of how they were established.
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
        linkStage_.store(static_cast<uint8_t>(LinkStage::Dialing), std::memory_order_release);
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
    // after the identity install, so its greeting carries our identity. A session from a lobby
    // carries its master's relay; one dialled with no master and no net.signaling uses the relay on
    // the chosen master's host.
    if (cfg_.signalingUrl.empty()) cfg_.signalingUrl = master_slots::DefaultSignalingUrl();
    if (cfg_.signalingUrl.empty()) {
        UE_LOGE("net: P2P requires a signaling server: net.signaling is empty and no master is "
                "chosen to take the relay from");
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
        // Which host this dial is for, told to the transport BEFORE the first signal leaves, so
        // the lines GNS produces are counted from the first one. Without it a dial that fails can
        // only be reported as the transport's own timeout, which names neither the relay nor the
        // host (coop/net/signaling_client.h, DialReport).
        signaling_->NoteDialing(hostId);
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
        linkStage_.store(static_cast<uint8_t>(LinkStage::Dialing), std::memory_order_release);
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
    // The pose batches die with the session too: their sequence trackers are per host process, and
    // a client rejoining a fresh host would otherwise drop every batch as stale until the new
    // host's sequence climbed past the old one's.
    ResetPoseBatches();
    // And every slot's receive state and epoch latch, for the same reason: the close path resets a
    // slot when its peer closes or the link fails, but a connection this side closes is neither, and
    // the loop below empties its slot first, so a session that ends here would carry its last
    // senders' sequences into the next.
    { std::lock_guard<std::mutex> lk(remoteMutex_);
      for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }

    auto* sockets = SteamNetworkingSockets();
    if (sockets) {
        for (int i = 0; i < kMaxPeers; ++i) {
            // GEN: clear -- session teardown empties every slot. A Session sits stopped between
            // Stop and the next Start, and a generation left live across that window would read
            // as an occupied slot with no session behind it.
            const uint32_t hConn = peerConns_[i].exchange(0);
            peerGenBySlot_[i].store(0, std::memory_order_release);
            backlog_.FreeSlot(i);      // queued state dies with the session
            rateControl_.FreeSlot(i);  // and so do its byte counters, or the next link starts in debt
            admission_.FreeSlot(i);    // and the occupancy estimate, for the same reason
            relayEligible_[i].store(0, std::memory_order_release);
            if (hConn != 0) {
                // The code names who stopped: a host ending the session, or a client leaving
                // it (the leaver never sees its own code; the host logs it).
                sockets->CloseConnection(hConn,
                                         ToTransportEnd(cfg_.role == Role::Host
                                                            ? EndReason::HostStopped
                                                            : EndReason::LeftSession),
                                         "session stop", true);
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
    linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);
    g_session.store(nullptr, std::memory_order_release);
    // Rates to zero for the net-stats panel's offline state; the totals stay visible until the next
    // Start resets them.
    net_stats::PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
    UE_LOGI("net: session stopped (sent=%llu recv=%llu)",
            static_cast<unsigned long long>(net_stats::PacketsSent()),
            static_cast<unsigned long long>(net_stats::PacketsRecv()));
}

}  // namespace coop::net
