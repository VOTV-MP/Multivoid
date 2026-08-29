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

#include "coop/dev/wire_census.h"
#include "coop/element/element.h"
#include "coop/net/peer_admission.h"   // the identity challenge every peer passes
#include "coop/net/peer_identity.h"    // GuidForPublicKey -- the proved storage name
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

namespace {
// R-4b: build the complete on-wire reliable packet (PacketHeader +
// ReliableHeader + payload) into `buf` (caller-sized). ONE builder for the
// guaranteed path, the try path and the fan-out -- the backlog stores and
// retries exactly these bytes.
int BuildReliableWire_(uint8_t* buf, ReliableKind kind, const void* payload, int len,
                       uint32_t seq, uint32_t ownEpoch, uint8_t senderSlot) {
    auto* hdr = reinterpret_cast<PacketHeader*>(buf);
    WriteHeader(*hdr, MsgType::Reliable, seq, ownEpoch, senderSlot);
    auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
    std::memset(rh, 0, sizeof(*rh));
    rh->kind = static_cast<uint8_t>(kind);
    rh->payloadLen = static_cast<uint16_t>(len);
    // len=0 with payload=nullptr is a legitimate control packet; memcpy of a
    // null source is UB pre-C++20, hence the guard.
    if (len > 0 && payload) {
        std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), payload, len);
    }
    return static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;
}
}  // namespace

bool Session::SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                 int len, uint8_t senderSlot) {
    // R-4b D2: the save-stream family is the pump's PACING lane -- it must not
    // mix with the backlog (a bypassing chunk would overtake a queued Begin in
    // the same Bulk lane). The pump calls TrySendReliableToSlot directly; this
    // routing keeps any stray caller correct rather than silently wrong.
    if (kind == ReliableKind::SaveTransferBegin || kind == ReliableKind::SaveTransferChunk)
        return TrySendReliableToSlot(peerSlot, kind, payload, len, senderSlot);
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliableToSlot rejected (slot=%d len=%d > %d)",
                peerSlot, len, kMaxReliablePayload);
        return false;
    }
    // v56 pre-world gate (B2, the MTA invariant): world-mutating kinds don't
    // flow to a slot that hasn't announced world-ready (a menu-mode joiner is
    // connected ~30-60 s before it has a world); the world-ready connect replay
    // reconstructs all of it. Allowlist: handshake/identity + the save transfer.
    // Deliberately NOT absorbed by the backlog: queueing a gate-skip would
    // deliver stale pre-world mutations at ready-time and DUPE the replay.
    if (!IsSlotWorldReady(peerSlot) && !IsPreWorldSendableKind(kind)) return false;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return false;

    uint8_t wire[sizeof(PacketHeader) + sizeof(ReliableHeader) + kMaxReliablePayload];
    const int total = BuildReliableWire_(wire, kind, payload, len,
                                         sendSeq_.fetch_add(1), ownEpoch_, senderSlot);
    // Delivery contract: entered the stream or the backlog -> true; a dying
    // connection -> false (teardown owns cleanup). See send_backlog.h.
    return backlog_.SendOrQueue(peerSlot, static_cast<int>(LaneForKind(kind)),
                                hConn, wire, total);
}

bool Session::TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                    int len, uint8_t senderSlot) {
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    // v56: SaveTransferChunk is the one BULK kind -- it bypasses the 228B inbox on
    // the receiver (bulk sink), so its real bound is ReliableHeader.payloadLen's
    // uint16 (kSaveChunkBytes + 4 fits with headroom). Everything else keeps the
    // tight event-datagram cap.
    const int cap = (kind == ReliableKind::SaveTransferChunk) ? 65000 : kMaxReliablePayload;
    if (len < 0 || len > cap) {
        UE_LOGW("net: TrySendReliableToSlot rejected (slot=%d len=%d > %d)",
                peerSlot, len, cap);
        return false;
    }
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
        UE_LOGW("net: TrySendReliableToSlot AllocateMessage(%d) returned null", total);
        return false;
    }
    BuildReliableWire_(static_cast<uint8_t*>(msg->m_pData), kind, payload, len,
                       seq, ownEpoch_, senderSlot);
    msg->m_conn = hConn;
    msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
    msg->m_idxLane = static_cast<uint16>(laneIdx);

    int64 outMsgNum = 0;
    sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
    if (outMsgNum < 0) {
        // Save-family sends fail ROUTINELY under send-buffer backpressure -- that
        // IS the pump's pacing signal (it retries next tick); don't spam.
        return false;
    }
    net_stats::AddSent(static_cast<uint32_t>(total));
    return true;
}

bool Session::SendRawReliableToConn(uint32_t hConn, ReliableKind kind,
                                    const void* payload, int len) {
    if (hConn == 0 || len < 0 || len > kMaxReliablePayload) return false;
    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();
    if (!sockets || !utils) return false;
    const int total = static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;
    SteamNetworkingMessage_t* msg = utils->AllocateMessage(total);
    if (!msg) return false;
    // senderSlot 0: the admission exchange predates any slot assignment on both
    // ends, and the receiver of these three kinds never reads it (the host routes
    // by the connection's pending tag, the client has exactly one peer).
    BuildReliableWire_(static_cast<uint8_t*>(msg->m_pData), kind, payload, len,
                       sendSeq_.fetch_add(1), ownEpoch_, /*senderSlot*/0);
    msg->m_conn = static_cast<HSteamNetConnection>(hConn);
    msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
    // Lane 0 explicitly. ConfigureConnectionLanes has NOT run on this connection
    // yet -- it is part of finishing the link, which admission gates -- and GNS
    // gives every connection lane 0 by default.
    msg->m_idxLane = 0;
    int64 outMsgNum = 0;
    sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
    if (outMsgNum < 0) return false;
    net_stats::AddSent(static_cast<uint32_t>(total));
    return true;
}

void Session::FinishClientLink(uint32_t hConn) {
    if (cfg_.role != Role::Client) return;
    if (hConn == 0 || peerConns_[0].load() != hConn) return;
    if (peerLanesConfigured_[0].load(std::memory_order_acquire)) return;  // idempotent
    FinishPeerConnected(0, hConn);
}

bool Session::SendReliable(ReliableKind kind, const void* payload, int len) {
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliable rejected (len=%d > %d)", len, kMaxReliablePayload);
        return false;
    }
    // ONE wire build, ONE seq for the whole fan-out (the pre-R-4b behavior);
    // the backlog copies the bytes per slot as needed.
    uint8_t wire[sizeof(PacketHeader) + sizeof(ReliableHeader) + kMaxReliablePayload];
    const int total = BuildReliableWire_(wire, kind, payload, len,
                                         sendSeq_.fetch_add(1), ownEpoch_, /*senderSlot*/0);
    const int laneIdx = static_cast<int>(LaneForKind(kind));

    bool anySuccess = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // v56 pre-world gate (B2) -- same rule as SendReliableToSlot, per slot.
        if (!IsSlotWorldReady(i) && !IsPreWorldSendableKind(kind)) continue;
        // R-4b delivery contract per slot (stream or backlog -> counted sent).
        if (backlog_.SendOrQueue(i, laneIdx, hConn, wire, total)) anySuccess = true;
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

// ---- ADMISSION: what an UNADMITTED connection is allowed to do ------------
// Security A2/A57/A15. A parked connection holds no player seat, so it cannot
// reach any handler that takes a senderSlot -- which is every handler. This is
// the ONLY code an unproved peer can run.
//
// THE ADMISSION TEST IS THE IDENTITY CHALLENGE (v144, 2026-08-29). A peer is
// seated when, and only when, it has signed our nonce with the private key its
// GNS identity names -- see coop/net/peer_admission.h for the exchange and for
// why the library's own checks cannot substitute for it.
//
// WHAT THIS REPLACED, AND WHY THE INTERMEDIATE STEP EXISTED. The 2026-08-26 gate
// admitted on "sent one well-formed packet of our protocol version", which was
// honest about buying only the SEAT half of A57: a silent socket or a wrong-build
// socket could no longer hold the lobby shut, but a peer that merely spoke could
// still pull the host's entire world. `[V]` It could not have been tightened to
// "send a Join" -- that draft was measured to deadlock every honest join, because
// a joining client is in MENU MODE with no world and its FIRST message is
// `SaveTransferRequest` (kind 42); it must fetch the save and load it before it
// can Join at all. That measurement is why the gate needed its own wire pair
// UPSTREAM of the save rather than a field on an existing packet.
void Session::HandlePendingMessage(int pendIdx, uint32_t hConn, const void* data, int len) {
    // THE BAND IS THE AUTHORITY ON WHETHER THIS INDEX IS STILL LIVE, and this
    // guard is what makes a refusal cost O(1) instead of O(packets already
    // queued). GNS hands the drain up to 256 messages at once, each carrying the
    // user data it had when it was received -- so after we refuse and close a
    // connection, every message of ITS that was already in the same batch still
    // routes here. Without this test each one re-ran the refusal: another
    // `CloseConnection`, and another `UE_LOGW`, and `[V]` a WARN does a
    // synchronous `fflush` under a lock the game thread shares (`log.cpp:225-227`,
    // whose own comment records ~50 flushes/sec "visibly tanking FPS"). One junk
    // burst from an UNAUTHENTICATED peer was therefore 256 disk syncs.
    // Found by a post-ship audit, 2026-08-29. The pre-arc gate could not have this
    // shape: it admitted on the first well-formed packet, so a pending connection
    // logged at most once.
    if (pendIdx < 0 || pendIdx >= kMaxPending) return;
    if (pendingConns_[pendIdx].load(std::memory_order_acquire) != hConn) return;
    MsgType type;
    uint32_t seq, senderEpoch;
    uint8_t headerSenderSlot;
    if (!ParseHeader(data, len, type, seq, senderEpoch, headerSenderSlot)) {
        // Protocol mismatch is worth SAYING to an unadmitted peer too -- the
        // pre-fix silent-hang failure mode (handshake fine, every packet
        // dropped, connection "Connected" forever) is exactly as confusing here.
        const uint16_t peerVer = PeekProtocolVersion(data, len);
        if (peerVer != 0 && peerVer != kProtocolVersion) {
            char reason[64];
            std::snprintf(reason, sizeof(reason), "protocol mismatch: peer=v%u, ours=v%u",
                          static_cast<unsigned>(peerVer), static_cast<unsigned>(kProtocolVersion));
            UE_LOGW("net: %s -- closing PENDING %d", reason, pendIdx);
            RetirePending(pendIdx, hConn, reason);
        }
        return;
    }
    if (type != MsgType::Reliable) return;
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
    ReliableHeader rh;
    std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
    const int payloadLen = len - static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader));
    const void* payload = static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                          sizeof(ReliableHeader);
    // The declared length must match what actually arrived before anything reads
    // the body: this is the one parser an unauthenticated peer can reach.
    if (payloadLen < 0 || rh.payloadLen != static_cast<uint16_t>(payloadLen)) {
        UE_LOGW("net: PENDING %d sent a length-inconsistent packet -- closing", pendIdx);
        RetirePending(pendIdx, hConn, "malformed packet");
        return;
    }

    const auto res = peer_admission::HostOnPendingReliable(
        *this, pendIdx, hConn, static_cast<ReliableKind>(rh.kind), payload, payloadLen);
    if (res.verdict == peer_admission::Verdict::Continue) return;
    if (res.verdict == peer_admission::Verdict::Refuse) {
        UE_LOGW("net: PENDING %d REFUSED on kind=%u -- %s",
                pendIdx, static_cast<unsigned>(rh.kind), res.reason);
        RetirePending(pendIdx, hConn, res.reason);
        return;
    }

    // SUPERSESSION: one identity, one seat. A peer that has PROVED possession of
    // the key already sitting in a slot is that person, so the older connection
    // goes and the new one is seated -- synchronously, here, before the seat is
    // asked for, or the returning player would be refused by a lobby that its own
    // ghost is filling.
    //
    // THE TRIGGER IS NOT AN ATTACK, IT IS A DROPPED LINK. GNS takes seconds to
    // tens of seconds to time out a dead connection; a player who rejoins inside
    // that window used to take a SECOND seat under the same identity -- and `[V]`
    // `PlayerFilePath` (`player_inventory_sync.cpp:88-102`) keys the stored
    // inventory by GUID, not by slot, so both seats then marked the same
    // `coop_players/<slot>/<guid>.json` dirty and persisted it: last writer wins,
    // silent inventory loss. Found by a post-ship audit, 2026-08-29; PLAN_01 §2.3
    // specified this and part 2 shipped without it.
    const std::string guid = peer_identity::GuidForPublicKey(res.provedKey);
    for (int s = 1; s < kMaxPeers; ++s) {
        if (peerConns_[s].load() == 0) continue;
        if (ProvedGuidForSlot(s) != guid) continue;
        UE_LOGW("net: slot %d already holds identity %s -- superseding it with the "
                "new connection (the holder proved the same key)", s, guid.c_str());
        Kick(s, "superseded by a new connection from your own identity");
        break;  // one seat per identity is the invariant; there cannot be a second
    }

    const int slot = AdmitPending(pendIdx, hConn);
    if (slot < 0) {
        UE_LOGW("net: lobby full of admitted players -- refusing PENDING %d", pendIdx);
        RetirePending(pendIdx, hConn, "host full");
        return;
    }
    // The peer's storage guid is DERIVED from the key it just proved, and it is
    // published here -- on the net thread, into a net-owned store -- because the
    // roster row is game-thread-only (`roster_ledger.cpp:210` asserts it). The
    // Join handler reads it back on the game thread. This is the whole point of
    // the arc: the guid that names a player's stored inventory is now a fact about
    // a key, not a 32-char string the peer asked to be called.
    SetProvedGuidForSlot(slot, guid);
    UE_LOGI("net: PENDING %d ADMITTED -> slot %d (identity-bound, guid %s)",
            pendIdx, slot, guid.c_str());
    peer_admission::HostForgetPending(pendIdx);
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

    // DEV wire census (VOTVCOOP_WIRE_CENSUS=1; the D2 wire-window probe):
    // count every non-reliable inbound by logical origin; reliables are logged
    // individually inside their case below once the kind is parsed.
    if (type != MsgType::Reliable && dev::wire_census::Enabled())
        dev::wire_census::NoteStream(routeSlot, static_cast<unsigned>(type));

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
        if (dev::wire_census::Enabled())
            dev::wire_census::NoteReliable(routeSlot, static_cast<unsigned>(rh.kind));
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
        // --- ADMISSION, CLIENT SIDE (v144) ---------------------------------
        // Handled HERE, on the net thread, and not through the inbox: a joining
        // client is in menu mode with no world and its game thread may be inside
        // a multi-second save load, so an exchange that waited on the game tick
        // would stall behind the very thing the exchange must precede. It also
        // touches no engine object, which is what makes that possible.
        if (cfg_.role == Role::Client && peerSlot == 0) {
            const uint32_t hostConn = peerConns_[0].load();
            const void* body = static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                               sizeof(ReliableHeader);
            const char* closeWhy = nullptr;
            if (peer_admission::ClientOnReliable(*this, hostConn,
                                                 static_cast<ReliableKind>(rh.kind),
                                                 body, payloadLen, &closeWhy)) {
                if (closeWhy) {
                    UE_LOGE("net: leaving this host -- %s", closeWhy);
                    {   std::lock_guard<std::mutex> lk(hostCloseMutex_);
                        hostCloseReason_ = closeWhy; }
                    if (auto* sockets = SteamNetworkingSockets())
                        sockets->CloseConnection(hostConn,
                                                 k_ESteamNetConnectionEnd_App_Generic,
                                                 closeWhy, false);
                }
                return;  // consumed by the exchange; never reaches the game thread
            }
            // THE HOST'S AssignPeerSlot IS THE ADMISSION SIGNAL -- peeked, not
            // consumed: it still carries the slot + hostElementId the game thread
            // needs, so it falls through to the inbox below. `[V]`
            // FinishPeerConnected sends it only when role == Host, and on a host it
            // runs only from AdmitPending, so its arrival means we were seated.
            // Refusing it before the host has proved itself is the point: otherwise
            // an impostor could skip the challenge and seat us anyway.
            if (static_cast<ReliableKind>(rh.kind) == ReliableKind::AssignPeerSlot) {
                if (!peer_admission::ClientProvedHost()) {
                    static const char* kWhy =
                        "the host tried to seat us without proving its identity";
                    UE_LOGE("net: %s -- leaving", kWhy);
                    {   std::lock_guard<std::mutex> lk(hostCloseMutex_);
                        hostCloseReason_ = kWhy; }
                    if (auto* sockets = SteamNetworkingSockets())
                        sockets->CloseConnection(hostConn,
                                                 k_ESteamNetConnectionEnd_App_Generic,
                                                 kWhy, false);
                    return;
                }
                FinishClientLink(hostConn);
            }
        }
        // The admission kinds are NET-THREAD-TERMINAL in both directions: the host
        // answers them in HandlePendingMessage (before a slot exists) and the client
        // just above. Arriving here means an ALREADY-ADMITTED peer replayed one --
        // it has no consumer, and letting it reach the inbox costs an event_feed
        // "unknown ReliableKind" warning per copy.
        if (IsAdmissionKind(static_cast<ReliableKind>(rh.kind))) return;
        // W3 (docs/security/PLAN_02_WIRE_HARDENING.md): divert the save-blob ANNOUNCE to the net
        // thread too, so it lands on the same thread and in the same lane order as the chunks above.
        // It is small enough for the inbox -- it is diverted for ORDERING, not for size, and both
        // length guards above still applied. This is the sole Begin path (the event_feed game-thread
        // case was retired with it, RULE 2): two paths for one message is what created the window.
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::SaveTransferBegin) {
            if (BulkSinkFn sink = saveBeginSink_.load(std::memory_order_acquire)) {
                sink(peerSlot,
                     static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                     payloadLen);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            // SECURITY W10: the 8192 hard cap that used to DROP here is gone (RULE 2).
            // A silent drop on an in-order reliable lane is permanent state divergence,
            // and it discarded whichever message happened to arrive at the cap -- not
            // necessarily the flooder's. Growth is now bounded upstream instead, by the
            // NetThread pause both roles share: the drain loop stops receiving at
            // kReliableInboxSoftPause (6144) and GNS buffers losslessly beneath it.
            //
            // The pause is evaluated once per loop iteration and each iteration receives
            // at most one 256-wide batch, so the depth cannot exceed 6144 + 256 = 6400
            // before the next evaluation -- with 1792 messages of margin under the value
            // this branch used to fire at. HandleMessage has exactly ONE caller (the drain
            // loop below), so there is no second path that could grow the inbox unchecked.
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
            // Exact depth high-water for the net-diag sample (see the member's comment).
            const auto depth = static_cast<uint32_t>(reliableInbox_.size());
            if (depth > reliableInboxPeak_.load(std::memory_order_relaxed))
                reliableInboxPeak_.store(depth, std::memory_order_relaxed);
        }
        // Seeds arc (2026-08-23): stamp the slot RELAY-ELIGIBLE at the net-thread
        // RECEIPT of ClientWorldReady, hConn-stamped (the send_backlog anti-recycle
        // idiom). Placed AFTER the inbox accepted the announce (audit F-5: stamping
        // a hard-cap-DROPPED announce would open relays while the GT flip + seed
        // never run). The GT flip lags this instant by one drain; a peer reliable
        // received in that gap was relay-SKIPPED for the joiner AND applied to the
        // host array after the ready-edge seed's cur-read -- lost (the /qf R1
        // micro-window). With the stamp, received-after rows relay directly (the
        // joiner IS world-ready) and stay out of the seed; received-before rows
        // drain ahead of ClientWorldReady in the FIFO inbox and ride the seed.
        // Exactly once, by ordering. Design doc par.2 (votv-signal-email-ready-seeds).
        if (cfg_.role == Role::Host &&
            static_cast<ReliableKind>(rh.kind) == ReliableKind::ClientWorldReady &&
            peerSlot >= 1 && peerSlot < kMaxPeers) {
            relayEligible_[peerSlot].store(peerConns_[peerSlot].load(),
                                           std::memory_order_release);
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
    // W10 pause accounting, per trigger, since the last status sample. Kept SEPARATE
    // because the two triggers mean different things: a depth pause says the game thread
    // is behind, an apply-park pause says one lane asked for backpressure.
    uint64_t pauseDepthHits = 0, pauseParkHits = 0;
    size_t   pauseWorstDepth = 0;

    while (running_.load()) {
        // 0) P2P: pump the signaling transport -- drain inbound ICE rendezvous
        // blobs (-> ReceivedP2PCustomSignal, which advances the handshake) and
        // flush outbound. Before RunCallbacks so a connection-state advance
        // triggered by a received signal is dispatched in the SAME iteration.
        // No-op (nullptr) for LanDirect. signaling_ is set before this thread
        // spawned and reset only after it joins, so the lock-free read is safe
        // (same discipline as ownEpoch_).
        if (signaling_) signaling_->Poll();

        // DEV wire census: flush the aggregated stream counters at 1 Hz from the
        // LOOP (not from arrivals), so the final second before a peer disconnect
        // still lands in the log -- that tail IS the wire-window measurement.
        if (dev::wire_census::Enabled()) dev::wire_census::Tick();

        // 1) Pump GNS internal timers + dispatch any pending status callbacks
        // (the trampoline runs inline on THIS thread).
        sockets->RunCallbacks();

        // 1b) Close pending sockets that never proved an identity. Eight relaxed
        // loads; the band's own deadline, finally enforced (see SweepPending).
        if (cfg_.role == Role::Host) SweepPending();

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
        // R-4b D10 + SECURITY W10 (docs/security/TRACKER.md): the reliable inbox at its
        // threshold is BACKPRESSURE, not drop -- pause this pass's receive so GNS buffers
        // underneath (measured lossless-by-stall: reassembly overflow does not ACK, and
        // decoded-queue overflow refuses without advancing the stream, so the sender
        // retransmits). The pause is bounded by the game-thread stall that caused the
        // pile-up, since the inbox drains on the game tick; unreliable pose staleness
        // during it is the channel's normal contract.
        //
        // W10: THIS USED TO BE CLIENT-ONLY, and that was the whole finding. The host had
        // no depth check at all, so its inbox grew to a hard 8192 cap in HandleMessage and
        // then SILENTLY DROPPED a reliable message -- permanent state divergence on an
        // in-order lane, and the arriving message need not even be the flooder's. The
        // comment here used to justify that with "its poll group cannot pause one
        // connection selectively", which is doubly wrong: [V] pausing is not per-connection
        // at all (not draining the group pauses every source, which is exactly what the
        // client's pause does with its one source), and [V] selective pausing WOULD have
        // been available anyway via SetConnectionPollGroup, which session_status.cpp:229
        // already calls. The fix is therefore not a cap, a share, or a terminal: the host
        // simply gets the pause the client already had, and the drop becomes unreachable.
        //
        // Both triggers are load-bearing and both apply to both roles: inbox depth, AND a
        // lane's apply park escalating (the seeds arc -- pause-not-drop at both caps).
        constexpr size_t kReliableInboxSoftPause = 6144;
        SteamNetworkingMessage_t* msgs[256]{};
        int drained = 0;
        for (;;) {
            size_t inboxDepth = 0;
            {
                std::lock_guard<std::mutex> lk(reliableInboxMutex_);
                inboxDepth = reliableInbox_.size();
            }
            const bool inboxFull = inboxDepth >= kReliableInboxSoftPause;
            const bool parked    = applyBackpressureCount_.load(std::memory_order_acquire) > 0;
            if (inboxFull || parked) {
                // Attribute the pause to ONE trigger, depth first. Conflating them would let
                // a seeds-arc apply park read as a depth pause -- the W10 drill would then
                // "pass" on a fire it did not cause.
                if (inboxFull) {
                    ++pauseDepthHits;
                    if (inboxDepth > pauseWorstDepth) pauseWorstDepth = inboxDepth;
                } else {
                    ++pauseParkHits;
                }
                break;
            }
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
                    // SECURITY A2/A57: a PENDING (unadmitted) connection is
                    // tagged deliberately outside [1, kMaxPeers), so it lands in
                    // the drop below by construction. Fork it to the admission
                    // handler FIRST; everything that is neither a seat nor a
                    // pending tag keeps falling into the same drop as before.
                    if (IsPendingUserData(ud)) {
                        HandlePendingMessage(PendingIndexOf(ud),
                                             static_cast<uint32_t>(msgs[i]->m_conn),
                                             msgs[i]->m_pData,
                                             static_cast<int>(msgs[i]->m_cbSize));
                        msgs[i]->Release();
                        continue;
                    }
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

        // 3b) R-4b: drain the reliable-send backlogs (the delivery guarantee) --
        // one pass per loop iteration per live slot. The GNS rc is the headroom
        // read; the D8 reserve keeps the UnreliableNoDelay pose/voice streams
        // flowing during a drain episode. A slot whose backlog trips a fatal
        // bound (no progress / byte cap) is closed honestly, never trimmed.
        for (int i = 0; i < kMaxPeers; ++i) {
            const uint32_t hConn = peerConns_[i].load();
            if (hConn == 0) continue;
            backlog_.Drain(i, hConn, sendBufBytes_);
            const char* fatalReason = nullptr;
            if (backlog_.CheckFatal(i, &fatalReason)) FatalCloseSlot(i, fatalReason);
        }

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
                        "sendRate=%dB/s pendRel=%dB pendUnrel=%dB unacked=%dB queue=%lldms "
                        "backlog=%zuB",
                        i, st.m_nPing, st.m_flConnectionQualityLocal * 100.f,
                        st.m_flConnectionQualityRemote * 100.f, st.m_flInPacketsPerSec,
                        st.m_flOutPacketsPerSec, st.m_nSendRateBytesPerSecond,
                        st.m_cbPendingReliable, st.m_cbPendingUnreliable,
                        st.m_cbSentUnackedReliable, queueMs, backlog_.DepthBytes(i));
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
            // SECURITY W10 instrument. Reported EVERY sample, including the quiet case, and
            // with its INPUT: the peak depth is what the pause compares, so "no pause fired"
            // is only meaningful next to how close the depth actually came. Without this the
            // drill cannot tell a working pause from a depth that never got there.
            const uint32_t inboxPeak = reliableInboxPeak_.exchange(0, std::memory_order_relaxed);
            UE_LOGI("net-diag: reliable inbox peak %u/%zu since last sample (%s); pauses: "
                    "depth=%llu (worst %zu) park=%llu",
                    inboxPeak, kReliableInboxSoftPause,
                    cfg_.role == Role::Host ? "host" : "client",
                    static_cast<unsigned long long>(pauseDepthHits), pauseWorstDepth,
                    static_cast<unsigned long long>(pauseParkHits));
            if (pauseDepthHits > 0)
                UE_LOGW("net-diag: reliable receive PAUSED %llu time(s) on depth since last "
                        "sample (worst %zu >= %zu) on the %s net thread -- the game thread is "
                        "behind the inbox; GNS buffers losslessly beneath (no drop)",
                        static_cast<unsigned long long>(pauseDepthHits), pauseWorstDepth,
                        kReliableInboxSoftPause, cfg_.role == Role::Host ? "host" : "client");
            sendFails = 0;
            worstDrain = 0;
            pauseDepthHits = pauseParkHits = 0;
            pauseWorstDepth = 0;
            nextRttSample = now + std::chrono::milliseconds(1000);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace coop::net
