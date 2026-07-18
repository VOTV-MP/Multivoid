// coop/net/session.cpp -- PR-4 multi-peer GNS implementation.
//
// Lifecycle (host):
//   Start():    Init GNS (refcounted) -> register status callback ->
//               CreateListenSocketIP -> CreatePollGroup -> spin NetThread.
//   On Connecting (None->Connecting status callback): find lowest free
//               client slot in [1..kMaxPeers-1], AcceptConnection,
//               SetConnectionUserData(slot), SetConnectionPollGroup ->
//               wait for Connected.
//   On Connected: ConfigureConnectionLanes per connection. Aggregate
//               state_=Connected if it isn't already.
//   On Closed:  free the slot; if any peers remain, stay Connected;
//               otherwise downgrade aggregate state_=Handshaking, reset
//               all remote state.
//   Stop():     CloseConnection on every active peer, DestroyPollGroup,
//               CloseListenSocket, join NetThread.
//
// Lifecycle (client):
//   Start():    Init GNS -> register status callback ->
//               ConnectByIPAddress -> store hConn at peerConns_[0] ->
//               spin NetThread.
//   On Connected: ConfigureConnectionLanes, aggregate state_=Connected.
//   On Closed:  clear peerConns_[0], state_=Handshaking, reset remote.
//   Stop():     CloseConnection(peerConns_[0]), join.
//
// Receive (net thread):
//   Host:   ReceiveMessagesOnPollGroup(hPollGroup_, ...).
//           Per-msg peerSlot = msg->m_nConnUserData (set at AcceptConnection).
//   Client: ReceiveMessagesOnConnection(peerConns_[0], ...).
//           peerSlot = 0.
//   Dispatch through HandleMessage(peerSlot, data, len).
//
// Send (net thread, pose stream @ sendHz):
//   Host:   iterate peerConns_[1..kMaxPeers-1], SendMessageToConnection for
//           each (UnreliableNoDelay; per-peer m_idxLane=0 implicit).
//   Client: SendMessageToConnection(peerConns_[0], ...).
//
// Send (game thread, SendReliable):
//   Host:   allocate one SteamNetworkingMessage_t PER connected client
//           (GNS owns each), set m_idxLane=LaneForKind(kind), SendMessages.
//   Client: single message to peerConns_[0].
//
// Wire format inside each GNS message is unchanged from PR-2/PR-3:
// PacketHeader (20 B) + the per-MsgType body (PoseSnapshot / PropPosePacket /
// ReliableHeader+payload). The header's token field is always 0 (GNS auth
// replaces it).

#include "coop/net/session.h"

#include "coop/element/element.h"
#include "coop/player/players_registry.h"
#include "session_lanes.h"      // co-located private header (src tree, not include/)
#include "signaling_client.h"   // co-located: complete type for the shared_ptr<SignalingClient> dtor + Poll()
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <cstring>
#include <chrono>
#include <random>

namespace coop::net {

namespace {

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr int kSendStaging = kMaxPacketBytes;

// PR-3 priority lanes + the T2-3 host-relay kind whitelist now live in the
// shared internal header coop/net/session_lanes.h so session_relay.cpp can
// reuse LaneForKind without duplicating the switch (T2-3 extraction). Pin
// Lane::Count to session_status.cpp's hard-coded kLaneCount=3 at compile
// time: a 4th lane here would flow through LaneForKind but
// ConfigureConnectionLanes would still pass 3, silently dropping reliables
// on the new lane.
static_assert(static_cast<int>(Lane::Count) == 3,
              "Lane::Count changed -- update kLaneCount in session_status.cpp::ConfigureLanesForPeer");

// ConfigureLanesForPeer moved to session_status.cpp (M-1 2026-05-29).
// Used only by HandleConnStatusChanged which also moved.

}  // namespace

// OnConnStatusChanged + ConnStatusTrampoline + the g_session bridge moved to
// session_start.cpp (2026-06-05) alongside Start/Stop.

// FindFreePeerSlotForClient / FindPeerSlotForConn / ResetPeerRemoteState /
// connectedPeerCount / HandleConnStatusChanged moved to session_status.cpp
// (M-1 2026-05-29) to bring this file under the 800-LOC soft cap.

Session::~Session() { Stop(); }

// Session::Start (topology dispatch) + Session::Stop moved to
// session_start.cpp (2026-06-05) to bring this file under the 800-LOC cap
// and give the upcoming P2P branch a clean home.

// The 9 SCALAR stream channels (pose/prop/ragdoll/hand/deskCursor/hostClock/
// deskSim/dishPose/reelPose) -- Set* publishers, TryGet* readers, the
// HandleMessage receive-store (StoreStreamPacket) and the NetThread step-3
// fan-out (SendStreamsTick) -- moved to session_streams.cpp (2026-07-18, the
// 800-LOC cap; bodies verbatim).

// SetLocalNpcPoseBatch / TakeRemoteNpcBatch / SerializeLocalNpcBatch /
// StoreRemoteNpcBatch -> session_npc.cpp (v37 NPC pose batch path; extracted
// 2026-06-07 per the 800-LOC soft cap).

bool Session::TryGetReliable(ReliableMessage& out) {
    std::lock_guard<std::mutex> lk(reliableInboxMutex_);
    if (reliableInbox_.empty()) return false;
    out = std::move(reliableInbox_.front());
    reliableInbox_.pop_front();
    return true;
}

// (v66 voice send/inbox live in session_voice.cpp -- the session_npc.cpp
// extraction precedent; session.cpp had crossed the 800-LOC soft cap.)

bool Session::SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                 int len, uint8_t senderSlot) {
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    // v56: SaveTransferChunk is the one BULK kind -- it bypasses the 228B inbox on
    // the receiver (bulk sink), so its real bound is ReliableHeader.payloadLen's
    // uint16 (kSaveChunkBytes + 4 fits with headroom). Everything else keeps the
    // tight event-datagram cap.
    const int cap = (kind == ReliableKind::SaveTransferChunk) ? 65000 : kMaxReliablePayload;
    if (len < 0 || len > cap) {
        UE_LOGW("net: SendReliableToSlot rejected (slot=%d len=%d > %d)",
                peerSlot, len, cap);
        return false;
    }
    // v56 pre-world gate (B2, the MTA invariant): world-mutating kinds don't
    // flow to a slot that hasn't announced world-ready (a menu-mode joiner is
    // connected ~30-60 s before it has a world); the world-ready connect replay
    // reconstructs all of it. Allowlist: handshake/identity + the save transfer.
    if (!IsSlotWorldReady(peerSlot) && !IsPreWorldSendableKind(kind)) return false;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return false;

    const int total = static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;
    const uint32_t seq = sendSeq_.fetch_add(1);
    const int laneIdx = static_cast<int>(LaneForKind(kind));

    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();

    SteamNetworkingMessage_t* msg = utils->AllocateMessage(total);
    if (!msg) {
        UE_LOGW("net: SendReliableToSlot AllocateMessage(%d) returned null", total);
        return false;
    }
    auto* buf = static_cast<uint8_t*>(msg->m_pData);
    auto* hdr = reinterpret_cast<PacketHeader*>(buf);
    WriteHeader(*hdr, MsgType::Reliable, seq, ownEpoch_, senderSlot);
    auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
    std::memset(rh, 0, sizeof(*rh));
    rh->kind = static_cast<uint8_t>(kind);
    rh->payloadLen = static_cast<uint16_t>(len);
    if (len > 0 && payload) {
        std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), payload, len);
    }

    msg->m_conn = hConn;
    msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
    msg->m_idxLane = static_cast<uint16>(laneIdx);

    int64 outMsgNum = 0;
    sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
    if (outMsgNum < 0) {
        // Chunk sends fail ROUTINELY under send-buffer backpressure -- that IS the
        // save-transfer pump's pacing signal (it retries next tick); don't spam.
        if (kind != ReliableKind::SaveTransferChunk) {
            UE_LOGW("net: SendReliableToSlot(slot=%d) rc=%lld kind=%u",
                    peerSlot, static_cast<long long>(outMsgNum), static_cast<unsigned>(kind));
        }
        return false;
    }
    net_stats::AddSent(static_cast<uint32_t>(total));
    return true;
}

bool Session::SendReliable(ReliableKind kind, const void* payload, int len) {
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliable rejected (len=%d > %d)", len, kMaxReliablePayload);
        return false;
    }
    const int total = static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;
    const uint32_t seq = sendSeq_.fetch_add(1);
    const int laneIdx = static_cast<int>(LaneForKind(kind));

    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();

    // Fan-out: allocate one GNS message per connected peer (GNS takes ownership
    // of each; we cannot share a single message across SendMessages calls).
    bool anySuccess = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // v56 pre-world gate (B2) -- same rule as SendReliableToSlot, per slot.
        if (!IsSlotWorldReady(i) && !IsPreWorldSendableKind(kind)) continue;

        SteamNetworkingMessage_t* msg = utils->AllocateMessage(total);
        if (!msg) {
            UE_LOGW("net: AllocateMessage(%d) returned null", total);
            continue;
        }
        auto* buf = static_cast<uint8_t*>(msg->m_pData);
        auto* hdr = reinterpret_cast<PacketHeader*>(buf);
        WriteHeader(*hdr, MsgType::Reliable, seq, ownEpoch_);
        auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
        std::memset(rh, 0, sizeof(*rh));
        rh->kind = static_cast<uint8_t>(kind);
        rh->payloadLen = static_cast<uint16_t>(len);
        // Symmetric with SendReliableToSlot: caller may legitimately pass
        // len=0 (control packet, payload-less kind) with payload=nullptr.
        // memcpy of a null source is UB pre-C++20; the guard makes both
        // entry points safe under the same contract.
        if (len > 0 && payload) {
            std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), payload, len);
        }

        msg->m_conn = hConn;
        msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
        msg->m_idxLane = static_cast<uint16>(laneIdx);

        int64 outMsgNum = 0;
        sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
        if (outMsgNum < 0) {
            UE_LOGW("net: SendMessages(slot=%d) rc=%lld kind=%u",
                    i, static_cast<long long>(outMsgNum), static_cast<unsigned>(kind));
            continue;
        }
        net_stats::AddSent(static_cast<uint32_t>(total));
        anySuccess = true;
    }
    return anySuccess;
}

bool Session::SendPropRelease(const WireKey& key,
                              float linVelX, float linVelY, float linVelZ,
                              float angVelX, float angVelY, float angVelZ,
                              uint32_t elementId, uint8_t ctx) {
    PropReleasePayload p{};
    p.key = key;
    p.linVelX = linVelX; p.linVelY = linVelY; p.linVelZ = linVelZ;
    p.angVelX = angVelX; p.angVelY = angVelY; p.angVelZ = angVelZ;
    p.elementId = elementId;  // v82: a keyless trash clump is routed by eid (key=None can't disambiguate)
    p.ctx = ctx;              // v82: stamp the host's per-eid generation so a stale throw can't re-apply post-transition
    return SendReliable(ReliableKind::PropRelease, &p, sizeof(p));
}

bool Session::SendPropSpawn(const PropSpawnPayload& payload) {
    return SendReliable(ReliableKind::PropSpawn, &payload, sizeof(payload));
}

bool Session::SendPropDestroy(const PropDestroyPayload& payload) {
    return SendReliable(ReliableKind::PropDestroy, &payload, sizeof(payload));
}

bool Session::SendEntitySpawn(const EntitySpawnPayload& payload) {
    return SendReliable(ReliableKind::EntitySpawn, &payload, sizeof(payload));
}

bool Session::SendEntityDestroy(uint32_t elementId) {
    EntityDestroyPayload p{};
    p.elementId = elementId;
    return SendReliable(ReliableKind::EntityDestroy, &p, sizeof(p));
}

void Session::HandleMessage(int peerSlot, const void* data, int len) {
    MsgType type;
    uint32_t seq;
    uint32_t senderEpoch;
    uint8_t headerSenderSlot;
    if (!ParseHeader(data, len, type, seq, senderEpoch, headerSenderSlot)) {
        // Distinguish "random garbage / spoofed packet" (silent drop) from
        // "a peer running an older/newer protocol" (close cleanly with a
        // human-readable reason so both ends see WHY they got dropped --
        // pre-fix this was a silent hang: handshake succeeds, every
        // application packet drops, connection stays "Connected" forever).
        const uint16_t peerVer = PeekProtocolVersion(data, len);
        if (peerVer != 0 && peerVer != kProtocolVersion &&
            peerSlot >= 0 && peerSlot < kMaxPeers) {
            const uint32_t hConn = peerConns_[peerSlot].load();
            if (hConn != 0) {
                char reason[64];
                std::snprintf(reason, sizeof(reason),
                              "protocol mismatch: peer=v%u, ours=v%u",
                              static_cast<unsigned>(peerVer),
                              static_cast<unsigned>(kProtocolVersion));
                UE_LOGW("net: %s -- closing peer slot %d", reason, peerSlot);
                if (auto* sockets = SteamNetworkingSockets()) {
                    sockets->CloseConnection(hConn,
                                             k_ESteamNetConnectionEnd_App_Generic,
                                             reason,
                                             /*bEnableLinger*/false);
                }
            }
        }
        return;
    }
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return;
    net_stats::AddRecv(static_cast<uint32_t>(len));

    // PR-FOUNDATION-1b v16: per-peer stale-generation defense. The first
    // packet from this slot establishes the expected epoch; subsequent
    // packets must match exactly or are dropped. ResetPeerRemoteState
    // clears expectedEpoch_[peerSlot] to 0 on disconnect so the next
    // connection at the same slot re-latches. Two edge cases:
    //  - senderEpoch == 0: pre-v16 sender (impossible at v16 since ParseHeader
    //    rejects mismatched version) OR a buggy sender forgot to mint --
    //    drop it; never latch 0.
    //  - expectedEpoch_[slot] == 0 + senderEpoch != 0: first packet from this
    //    slot, latch it.
    // Lock ordering: this matches every per-peer state update below (all
    // take remoteMutex_), so the lock is acquired once here, checked, and
    // released before falling into the per-type switch which re-acquires
    // it. Doing the check under the lock keeps the latch atomic with
    // ResetPeerRemoteState's clear.
    {
        std::lock_guard<std::mutex> lk(remoteMutex_);
        if (senderEpoch == 0) {
            UE_LOGW("net: dropping packet from slot %d with senderEpoch=0 (malformed sender)",
                    peerSlot);
            return;
        }
        const uint32_t expected = expectedEpoch_[peerSlot];
        if (expected == 0) {
            expectedEpoch_[peerSlot] = senderEpoch;
            UE_LOGI("net: latched senderEpoch=0x%08x for peer slot %d",
                    static_cast<unsigned>(senderEpoch), peerSlot);
        } else if (expected != senderEpoch) {
            // Logged at INFO not WARN: the most common cause is a clean
            // reconnect race (in-flight packets from the old connection
            // arrive after the new connection's first packet relatches),
            // which is benign and self-corrects. A WARN spam during
            // reconnect churn would be misleading.
            UE_LOGI("net: stale-gen drop slot=%d expected=0x%08x got=0x%08x kind=%u",
                    peerSlot,
                    static_cast<unsigned>(expected),
                    static_cast<unsigned>(senderEpoch),
                    static_cast<unsigned>(type));
            return;
        }
    }

    // PR-FOUNDATION Tier 2 T2-2 (host-relay): determine the LOGICAL origin
    // slot used to ROUTE pose data into the per-puppet store, distinct from
    // the connection slot `peerSlot` used for the epoch latch above.
    //  - HOST: the connection IS the origin (GNS-authenticated m_nConnUserData);
    //    trust it, ignore the header's (spoofable) senderSlot.
    //  - CLIENT: all packets arrive on the single host connection (peerSlot 0),
    //    so the connection can't distinguish originators -- route by the
    //    host-stamped header senderSlot. The host is trusted to have set it.
    int routeSlot = peerSlot;
    if (cfg_.role == Role::Client) {
        routeSlot = static_cast<int>(headerSenderSlot);
        if (routeSlot < 0 || routeSlot >= kMaxPeers) {
            UE_LOGW("net: client received packet with out-of-range senderSlot=%d "
                    "-- dropping", routeSlot);
            return;
        }
    }

    switch (type) {
    case MsgType::PoseSnapshot:
    case MsgType::PropPose:
    case MsgType::RagdollPose:
    case MsgType::HandPose:
    case MsgType::DeskCursorPose:
    case MsgType::ClockPose:
    case MsgType::DeskSimPose:
    case MsgType::DishPose:
    case MsgType::ReelPose:
        // -> session_streams.cpp: the 9 scalar per-channel stream cases
        // (validate + newest-wins store + host relay; bodies verbatim).
        StoreStreamPacket(type, routeSlot, peerSlot, data, len, seq);
        break;
    case MsgType::EntityPose:
        StoreRemoteNpcBatch(data, len, seq);  // -> session_npc.cpp (parse + newest-wins store)
        break;
    case MsgType::WorldActorPose:
        StoreRemoteWorldActorBatch(data, len, seq);  // v80 (B3b) -> session_worldactor.cpp (parse + newest-wins store)
        break;
    case MsgType::TrashCarryPose:
        StoreRemoteTrashCarryBatch(data, len, seq);  // v85 (Increment 2) -> session_trashcarry.cpp (parse + newest-wins store)
        break;
    case MsgType::VoiceFrame:
        // v66 voice: a STREAM -- queue every arrival (no header-seq stale-drop;
        // the per-payload voice seq orders at the jitter buffer). Store + host
        // relay live in session_voice.cpp.
        StoreVoiceFrame(routeSlot, peerSlot, data, len);
        break;
    case MsgType::Reliable: {
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
        ReliableHeader rh;
        std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
        // payloadLen is uint16_t, can't be negative -- only the upper bound is
        // a real guard.
        const int payloadLen = static_cast<int>(rh.payloadLen);
        // v56: the save-blob chunk exceeds the fixed inbox payload BY DESIGN --
        // divert it whole to the registered bulk sink (coop/save_transfer's heap
        // assembler) right here on the net thread; it never enters the 228B
        // ReliableMessage ring (and is never relayed -- host->one-client only).
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::SaveTransferChunk) {
            if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
            if (BulkSinkFn sink = bulkSink_.load(std::memory_order_acquire)) {
                sink(peerSlot,
                     static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                     payloadLen);
            }
            return;
        }
        if (payloadLen > kMaxReliablePayload) return;
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
        {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            // Cap the inbox so a flooding peer can't grow it unboundedly on
            // the host's net thread. 8192 provides ~4x headroom over an
            // observed worst-case connect-time snapshot burst (~1700
            // PropSpawn for a fully-populated VOTV world). At ~232 B per
            // inline message that's ~1.9 MB worst case -- bounded for DoS
            // while still fitting legitimate fan-outs.
            constexpr size_t kReliableInboxCap = 8192;
            if (reliableInbox_.size() >= kReliableInboxCap) {
                UE_LOGW("net: reliableInbox full (%zu) -- dropping kind=%u from peer slot %d",
                        kReliableInboxCap, static_cast<unsigned>(rh.kind), peerSlot);
                return;
            }
            // emplace + memcpy avoids the per-receive heap alloc
            // (vector::assign). ReliableMessage now holds an inline 228 B
            // payload buffer. Stamp senderPeerSlot = routeSlot so drainers
            // route per-sender: on the host routeSlot is the authenticated
            // connection slot; on a client it is the host-stamped origin
            // (a relayed reliable from peer A carries senderSlot=A so B's
            // event_feed applies it to A's puppet, not the host's).
            reliableInbox_.emplace_back();
            ReliableMessage& m = reliableInbox_.back();
            m.kind = static_cast<ReliableKind>(rh.kind);
            m.senderPeerSlot = routeSlot;
            m.payloadLen = static_cast<uint16_t>(payloadLen);
            std::memcpy(m.payload,
                        static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                        static_cast<size_t>(payloadLen));
        }
        // Host relay (T2-3): forward peer-originated gameplay reliables to
        // every OTHER client so cross-peer item/prop actions are seen by
        // all. Host-authoritative kinds (Weather/RedSky/Lightning/Entity*)
        // and handshake kinds (Join/AssignPeerSlot/PlayerJoined) are NOT
        // relayed -- they either originate on the host (fanned out via
        // SendReliable already) or are point-to-point handshake. The host
        // also processes the reliable locally (above) so its own view of
        // the origin peer's puppet updates too.
        if (cfg_.role == Role::Host &&
            IsClientRelayableReliableKind(static_cast<ReliableKind>(rh.kind))) {
            RelayReliableToOtherClients(peerSlot,
                                        static_cast<ReliableKind>(rh.kind),
                                        data, len);
        }
        break;
    }
    default:
        break;
    }
}

// RelayUnreliableToOtherClients + RelayReliableToOtherClients are defined in
// session_relay.cpp (the host-relay subsystem TU, extracted at T2-3 when this
// file crossed the 800-LOC soft cap).

void Session::NetThread() {
    const auto sendInterval = std::chrono::milliseconds(
        cfg_.sendHz > 0 ? 1000 / cfg_.sendHz : 33);
    auto nextSend = std::chrono::steady_clock::now();
    auto nextRttSample = std::chrono::steady_clock::now();
    // v109 (design F): the host world-clock snapshot streams on its OWN ~500 ms cadence,
    // independent of (and far slower than) the pose sendHz -- one game-minute of real time
    // is well over 500 ms at any plausible day length, so this keeps the frozen client
    // mirror within a minute of the host without loading the wire. Net-thread-local (only
    // this loop touches it), like nextSend.
    auto nextClockSend = std::chrono::steady_clock::now();
    auto nextDeskSimSend = std::chrono::steady_clock::now();

    auto* sockets = SteamNetworkingSockets();

    // --- Net diagnostics (PERMANENT; user request 2026-06-06: "log all rate-limiting +
    // high-PING events, now and for future"). The per-peer status block below reads GNS's
    // real-time telemetry every ~1 s and (a) logs an INFO summary, (b) WARNs on threshold
    // breach -- so a real user's log self-flags a slow link or a send-side rate-limit stall.
    // The two net-thread-local counters accumulate between samples. ---
    constexpr int kHighPingMs      = 250;    // LAN ~1 ms; 250+ = a real link/relay problem
    constexpr int kHighPendingBytes = 65536; // 64 KB outbound PENDING = a real send backlog
    uint64_t sendFails  = 0;  // SendMessageToConnection rejections since the last status sample
    int      worstDrain = 0;  // worst single-pass receive drain since the last status sample

    while (running_.load()) {
        // 0) P2P: pump the signaling transport -- drain inbound ICE rendezvous
        // blobs (-> ReceivedP2PCustomSignal, which advances the handshake) and
        // flush outbound. Before RunCallbacks so a connection-state advance
        // triggered by a received signal is dispatched in the SAME iteration.
        // No-op (nullptr) for LanDirect. signaling_ is set before this thread
        // spawned and reset only after it joins, so the lock-free read is safe
        // (same discipline as ownEpoch_).
        if (signaling_) signaling_->Poll();

        // 1) Pump GNS internal timers + dispatch any pending status callbacks
        // (the trampoline runs inline on THIS thread).
        sockets->RunCallbacks();

        // 2) Drain inbound messages -- to EMPTY, every iteration. A full batch means
        // more may be queued, so loop the receive until it returns a PARTIAL batch;
        // only then does the idle sleep at the bottom run. ROOT-CAUSE FIX (3 converging
        // audit agents, 2026-06-06) for the long-standing "remote player lags ~10000 ms"
        // bug: the old 16-wide batch + the UNCONDITIONAL 5 ms sleep capped intake at
        // 16/5ms = 3200 msg/sec -- BELOW the connect-snapshot's ~6000 reliable
        // PropSpawn/sec burst (prop_snapshot DrainChunk 100/tick x 60 Hz). So the
        // client's GNS receive queue backed up by SECONDS, and the unreliable pose
        // stream interleaved behind it was delivered ~10 s stale -> the puppet froze
        // then snapped. A 256-wide batch drained to empty clears a ~2300-msg snapshot
        // in ~9 receive calls within one loop pass, so poses are never starved behind it.
        // Per-pass drain cap (audit 2026-06-06): break after kMaxDrainPerPass even if the
        // batch stays full, so the outer `while (running_)` re-checks the stop flag within a
        // bounded number of messages. Without it a SUSTAINED flood (a buggy/hostile peer, or
        // the 4-peer host PollGroup under a reconnect storm) keeps n==256 forever -> the inner
        // loop never breaks -> Session::Stop()'s thread join HANGS. 4096/5ms = ~819k msg/sec is
        // far above any real rate AND above the ~2300 one-shot snapshot (which clears in one
        // pass via the n<256 break first), so this cap is invisible in normal operation.
        constexpr int kMaxDrainPerPass = 4096;
        SteamNetworkingMessage_t* msgs[256]{};
        int drained = 0;
        for (;;) {
            int n = 0;
            if (cfg_.role == Role::Host) {
                const uint32_t hPoll = hPollGroup_.load();
                if (hPoll != 0) {
                    n = sockets->ReceiveMessagesOnPollGroup(
                        static_cast<HSteamNetPollGroup>(hPoll), msgs,
                        static_cast<int>(std::size(msgs)));
                }
            } else {
                const uint32_t hConn = peerConns_[0].load();
                if (hConn != 0) {
                    n = sockets->ReceiveMessagesOnConnection(
                        hConn, msgs, static_cast<int>(std::size(msgs)));
                }
            }
            for (int i = 0; i < n; ++i) {
                // Host: peerSlot was stashed via SetConnectionUserData at accept
                // time; PollGroup messages carry it forward as m_nConnUserData.
                // Client: only ever receives from peerConns_[0] (host).
                int peerSlot;
                if (cfg_.role == Role::Host) {
                    // m_nConnUserData defaults to 0 (or -1 on some GNS versions)
                    // before SetConnectionUserData lands. Narrowing a default of
                    // 0 here would corrupt slot 0 (the host's own local-self
                    // slot) and then the backward-compat 0-arg TryGetRemotePose
                    // would permanently return it. Validate bounds AND reject
                    // slot 0 on host before narrowing.
                    const int64 ud = msgs[i]->m_nConnUserData;
                    if (ud < 1 || ud >= kMaxPeers) {
                        UE_LOGW("net: dropping msg from unregistered conn (ud=%lld)",
                                static_cast<long long>(ud));
                        msgs[i]->Release();
                        continue;
                    }
                    peerSlot = static_cast<int>(ud);
                } else {
                    peerSlot = 0;
                }
                HandleMessage(peerSlot, msgs[i]->m_pData, static_cast<int>(msgs[i]->m_cbSize));
                msgs[i]->Release();
            }
            drained += n;
            // Stop when the queue is drained (partial batch) OR the per-pass cap is hit
            // (the latter guarantees the outer running_ re-check -- Stop() liveness).
            if (n < static_cast<int>(std::size(msgs)) || drained >= kMaxDrainPerPass) break;
        }
        if (drained > worstDrain) worstDrain = drained;  // net-diag: receive-backlog high-water
        if (drained >= kMaxDrainPerPass)
            UE_LOGW("net-diag: receive drain hit the per-pass cap (%d) on the %s net thread -- "
                    "sustained inbound flood; the rest is queued for the next pass",
                    kMaxDrainPerPass, cfg_.role == Role::Host ? "host" : "client");

        // 3) Connected: stream the local pose at sendHz, fan out to all peers.
        //    -> session_streams.cpp (SendStreamsTick; the step-3 body verbatim,
        //    incl. the npc/wa/tc batch stamps). `now` is computed ONCE here and
        //    shared with step 4's net-diag below -- one timestamp per iteration.
        const auto now = std::chrono::steady_clock::now();
        SendStreamsTick(now, sendInterval, nextSend, nextClockSend, nextDeskSimSend, sendFails);

        // 4) Per-peer NET DIAGNOSTICS every ~1 s (RTT + send-queue + rate-limit telemetry).
        // GNS GetConnectionRealTimeStatus exposes the SEND-side state that explains a laggy
        // peer: m_usecQueueTime (how long the NEXT outbound packet will wait before it hits the
        // wire), m_cbPendingReliable/Unreliable (bytes already queued to send), and
        // m_nSendRateBytesPerSecond (the rate GNS is currently allowing). When our outbound
        // demand (the ~2300-msg connect-snapshot + the 60 Hz pose stream) exceeds the allowed
        // send rate, packets pile up in the send queue and the pose stream is delivered SECONDS
        // late -- which is invisible without this telemetry. Logged as an INFO summary; WARNs
        // fire on high ping / send-queue latency / pending backlog so the events stand out (and
        // flush to disk). rttMsBySlot_[i] gets each peer's ping for the nameplate + scoreboard.
        if (state_.load() != ConnState::Connected && now >= nextRttSample) {
            // Not connected: publish zeroed rates so the ui net-stats panel reads
            // "offline" instead of the last live sample frozen forever (totals stay).
            net_stats::PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
            nextRttSample = now + std::chrono::milliseconds(1000);
        }
        if (state_.load() == ConnState::Connected && now >= nextRttSample) {
            // ui net-stats: sum the GNS real-time view across live conns (wire-level
            // bytes/pkts per sec incl. acks/retransmits) + the worst ping among them.
            float sumInBps = 0.f, sumOutBps = 0.f, sumInPktps = 0.f, sumOutPktps = 0.f;
            int livePeers = 0, pingMax = -1;
            for (int i = 0; i < kMaxPeers; ++i) {
                const uint32_t hConn = peerConns_[i].load();
                if (hConn == 0) { rttMsBySlot_[i].store(-1, std::memory_order_relaxed); continue; }
                SteamNetConnectionRealTimeStatus_t st{};
                if (sockets->GetConnectionRealTimeStatus(hConn, &st, 0, nullptr) != k_EResultOK) continue;
                sumInBps    += st.m_flInBytesPerSec;
                sumOutBps   += st.m_flOutBytesPerSec;
                sumInPktps  += st.m_flInPacketsPerSec;
                sumOutPktps += st.m_flOutPacketsPerSec;
                ++livePeers;
                if (st.m_nPing >= 0 && st.m_nPing < 60000 && st.m_nPing > pingMax) pingMax = st.m_nPing;
                // m_usecQueueTime ("usec until the next send") returns a huge sentinel (~INT64_MAX)
                // whenever the estimate is undefined -- which is MOST of the time, even WITH a little
                // pending data -- so clamp anything absurd to 0. The reliable send-backlog signal is
                // the PENDING BYTES (m_cbPendingReliable/Unreliable), not this field.
                long long queueMs = static_cast<long long>(st.m_usecQueueTime / 1000);
                if (queueMs < 0 || queueMs > 60000) queueMs = 0;  // sentinel / no estimate -> 0
                // Store THIS slot's RTT for the per-peer nameplate + scoreboard ping
                // (event_feed fans it to the slot's puppet; roster reads it per row).
                rttMsBySlot_[i].store((st.m_nPing >= 0 && st.m_nPing < 60000) ? st.m_nPing : -1,
                                      std::memory_order_relaxed);
                UE_LOGI("net-diag[slot %d]: ping=%dms qual=%.0f/%.0f%% in=%.0f out=%.0f pkt/s "
                        "sendRate=%dB/s pendRel=%dB pendUnrel=%dB unacked=%dB queue=%lldms",
                        i, st.m_nPing, st.m_flConnectionQualityLocal * 100.f,
                        st.m_flConnectionQualityRemote * 100.f, st.m_flInPacketsPerSec,
                        st.m_flOutPacketsPerSec, st.m_nSendRateBytesPerSecond,
                        st.m_cbPendingReliable, st.m_cbPendingUnreliable,
                        st.m_cbSentUnackedReliable, queueMs);
                if (st.m_nPing > kHighPingMs)
                    UE_LOGW("net-diag[slot %d]: HIGH PING %d ms (> %d) -- the link/relay is slow",
                            i, st.m_nPing, kHighPingMs);
                // Send backlog = PENDING BYTES over threshold (the reliable signal; queueMs is
                // sentinel-prone). A real rate-limit / slow-link stall shows here as KB+ pending.
                if (st.m_cbPendingReliable > kHighPendingBytes ||
                    st.m_cbPendingUnreliable > kHighPendingBytes)
                    UE_LOGW("net-diag[slot %d]: SEND BACKLOG pendRel=%dB pendUnrel=%dB (> %d) -- the "
                            "outbound queue is building (rate limit / slow link / burst); "
                            "sendRate=%dB/s queue=%lldms",
                            i, st.m_cbPendingReliable, st.m_cbPendingUnreliable, kHighPendingBytes,
                            st.m_nSendRateBytesPerSecond, queueMs);
            }
            net_stats::PublishRates(sumInBps, sumOutBps, sumInPktps, sumOutPktps,
                                    livePeers, pingMax, true);
            if (sendFails > 0)
                UE_LOGW("net-diag: %llu outbound send(s) REJECTED by GNS since last sample "
                        "(send buffer full / rate-limited)", static_cast<unsigned long long>(sendFails));
            if (worstDrain > static_cast<int>(std::size(msgs)))
                UE_LOGW("net-diag: receive backlog -- worst single-pass drain %d msgs (> one 256 "
                        "batch) since last sample; an inbound burst exceeded the batch", worstDrain);
            sendFails = 0;
            worstDrain = 0;
            nextRttSample = now + std::chrono::milliseconds(1000);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace coop::net
