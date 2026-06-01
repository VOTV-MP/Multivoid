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
#include "coop/players_registry.h"
#include "session_lanes.h"  // co-located private header (src tree, not include/)
#include "ue_wrap/log.h"

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

std::atomic<Session*> g_session{nullptr};

std::mutex g_initMutex;
bool g_inited = false;

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

bool EnsureGnsInit() {
    std::lock_guard<std::mutex> lk(g_initMutex);
    if (g_inited) return true;
    SteamNetworkingErrMsg err{};
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        UE_LOGE("net: GameNetworkingSockets_Init failed: %s", err);
        return false;
    }
    g_inited = true;
    UE_LOGI("net: GameNetworkingSockets_Init OK");
    return true;
}

// ConfigureLanesForPeer moved to session_status.cpp (M-1 2026-05-29).
// Used only by HandleConnStatusChanged which also moved.

}  // namespace

void Session::OnConnStatusChanged(void* info) {
    auto* self = g_session.load(std::memory_order_acquire);
    if (self) self->HandleConnStatusChanged(info);
}

namespace {
void ConnStatusTrampoline(SteamNetConnectionStatusChangedCallback_t* cb) {
    Session::OnConnStatusChanged(cb);
}
}  // namespace

// FindFreePeerSlotForClient / FindPeerSlotForConn / ResetPeerRemoteState /
// connectedPeerCount / HandleConnStatusChanged moved to session_status.cpp
// (M-1 2026-05-29) to bring this file under the 800-LOC soft cap.

Session::~Session() { Stop(); }

bool Session::Start(const Config& cfg) {
    if (running_.load()) {
        UE_LOGW("net: Session::Start ignored -- already running");
        return false;
    }
    cfg_ = cfg;

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

    auto* sockets = SteamNetworkingSockets();
    if (cfg_.role == Role::Host) {
        SteamNetworkingIPAddr addr{};
        addr.Clear();
        addr.m_port = cfg_.port;
        const HSteamListenSocket hListen = sockets->CreateListenSocketIP(addr, 0, nullptr);
        if (hListen == k_HSteamListenSocket_Invalid) {
            UE_LOGE("net: CreateListenSocketIP(port=%u) failed", cfg_.port);
            g_session.store(nullptr, std::memory_order_release);
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
            g_session.store(nullptr, std::memory_order_release);
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
            g_session.store(nullptr, std::memory_order_release);
            return false;
        }
        addr.m_port = cfg_.port;
        const HSteamNetConnection hConn = sockets->ConnectByIPAddress(addr, 0, nullptr);
        if (hConn == k_HSteamNetConnection_Invalid) {
            UE_LOGE("net: ConnectByIPAddress(%s:%u) failed", cfg_.peerIp.c_str(), cfg_.port);
            g_session.store(nullptr, std::memory_order_release);
            return false;
        }
        // Slot 0 = host (per the players::Registry indexing -- on a client,
        // the host occupies slot 0).
        peerConns_[0].store(hConn);
        UE_LOGI("net: client dialed %s:%u (hConn=0x%08x slot=0)",
                cfg_.peerIp.c_str(), cfg_.port, static_cast<unsigned>(hConn));
    }

    state_.store(ConnState::Handshaking);
    lastRttMs_.store(0);
    running_.store(true);
    thread_ = std::thread(&Session::NetThread, this);
    UE_LOGI("net: session started role=%s peer=%s:%u sendHz=%d",
            cfg_.role == Role::Host ? "host" : "client",
            cfg_.peerIp.c_str(), cfg_.port, cfg_.sendHz);
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

    state_.store(ConnState::Disconnected);
    g_session.store(nullptr, std::memory_order_release);
    UE_LOGI("net: session stopped (sent=%llu recv=%llu)",
            static_cast<unsigned long long>(sent_.load()),
            static_cast<unsigned long long>(recv_.load()));
}

void Session::SetLocalPose(const PoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    localPose_ = pose;
    hasLocal_ = true;
}

void Session::SetLocalPropPose(bool set, const PropPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalProp_ = set;
    if (set) localPropPose_ = pose;
}

void Session::SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalRagdoll_ = set;
    if (set) localRagdollPose_ = pose;
}

bool Session::TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemote_[peerSlot]) return false;
    out = remotePoses_[peerSlot];
    if (outIsNew) *outIsNew = (remoteStamp_[peerSlot] != lastReadStamp_[peerSlot]);
    lastReadStamp_[peerSlot] = remoteStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteProp_[peerSlot]) return false;
    out = remotePropPoses_[peerSlot];
    if (outIsNew) *outIsNew = (remotePropStamp_[peerSlot] != lastReadPropStamp_[peerSlot]);
    lastReadPropStamp_[peerSlot] = remotePropStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteRagdoll_[peerSlot]) return false;
    out = remoteRagdollPoses_[peerSlot];
    if (outIsNew) *outIsNew = (remoteRagdollStamp_[peerSlot] != lastReadRagdollStamp_[peerSlot]);
    lastReadRagdollStamp_[peerSlot] = remoteRagdollStamp_[peerSlot];
    return true;
}

bool Session::TryGetReliable(ReliableMessage& out) {
    std::lock_guard<std::mutex> lk(reliableInboxMutex_);
    if (reliableInbox_.empty()) return false;
    out = std::move(reliableInbox_.front());
    reliableInbox_.pop_front();
    return true;
}

bool Session::SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                 int len, uint8_t senderSlot) {
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliableToSlot rejected (slot=%d len=%d > %d)",
                peerSlot, len, kMaxReliablePayload);
        return false;
    }
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
        UE_LOGW("net: SendReliableToSlot(slot=%d) rc=%lld kind=%u",
                peerSlot, static_cast<long long>(outMsgNum), static_cast<unsigned>(kind));
        return false;
    }
    sent_.fetch_add(1);
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
        sent_.fetch_add(1);
        anySuccess = true;
    }
    return anySuccess;
}

bool Session::SendPropRelease(const WireKey& key,
                              float linVelX, float linVelY, float linVelZ,
                              float angVelX, float angVelY, float angVelZ) {
    PropReleasePayload p{};
    p.key = key;
    p.linVelX = linVelX; p.linVelY = linVelY; p.linVelZ = linVelZ;
    p.angVelX = angVelX; p.angVelY = angVelY; p.angVelZ = angVelZ;
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
    recv_.fetch_add(1);

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
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemote_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteSeq_[routeSlot]) <= 0) {
                break;  // stale/duplicate for this origin slot; still relayed? no -- a stale packet need not propagate
            }
            remotePoses_[routeSlot] = pkt.pose;
            lastRemoteSeq_[routeSlot] = seq;
            hasRemote_[routeSlot] = true;
            ++remoteStamp_[routeSlot];
        }
        // Host relay: forward this client's pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::PropPose: {
        if (len < static_cast<int>(sizeof(PropPosePacket))) return;
        PropPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        const float vals[6] = {pkt.pose.x, pkt.pose.y, pkt.pose.z,
                               pkt.pose.pitch, pkt.pose.yaw, pkt.pose.roll};
        for (float v : vals) if (!std::isfinite(v)) return;
        if (std::fabs(pkt.pose.x) > kMaxCoord ||
            std::fabs(pkt.pose.y) > kMaxCoord ||
            std::fabs(pkt.pose.z) > kMaxCoord) return;
        if (std::fabs(pkt.pose.pitch) > 180.f ||
            std::fabs(pkt.pose.yaw)   > 180.f ||
            std::fabs(pkt.pose.roll)  > 180.f) return;
        if (pkt.pose.key.len > 31) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteProp_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemotePropSeq_[routeSlot]) <= 0) {
                break;
            }
            remotePropPoses_[routeSlot] = pkt.pose;
            lastRemotePropSeq_[routeSlot] = seq;
            hasRemoteProp_[routeSlot] = true;
            ++remotePropStamp_[routeSlot];
        }
        // Host relay: forward this client's held-prop pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::RagdollPose: {
        if (len < static_cast<int>(sizeof(RagdollPosePacket))) return;
        RagdollPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary sanitize (same shape as PropPose): reject NaN/Inf and
        // out-of-bounds before storing -- a velocity write of a poisoned value
        // would corrupt the receiver's PhysX state. Velocities are unbounded in
        // principle but a finite + sane-magnitude check rejects garbage; the
        // rotation goes onto SetActorRotation so it must be a finite FRotator.
        const float vals[12] = {pkt.pose.x, pkt.pose.y, pkt.pose.z,
                                pkt.pose.pitch, pkt.pose.yaw, pkt.pose.roll,
                                pkt.pose.linVelX, pkt.pose.linVelY, pkt.pose.linVelZ,
                                pkt.pose.angVelX, pkt.pose.angVelY, pkt.pose.angVelZ};
        for (float v : vals) if (!std::isfinite(v)) return;
        if (std::fabs(pkt.pose.x) > kMaxCoord ||
            std::fabs(pkt.pose.y) > kMaxCoord ||
            std::fabs(pkt.pose.z) > kMaxCoord) return;
        // Rotation must be in the canonical FRotator range (the sender normalizes
        // via NormalizeAxis) -- same guard the PropPose case applies. SetActorRotation
        // normalizes internally so an out-of-range value wouldn't crash, but reject it
        // at the trust boundary for parity with PropPose (a finite-but-huge angle from
        // a malformed/hostile datagram has no legitimate sender).
        if (std::fabs(pkt.pose.pitch) > 180.f ||
            std::fabs(pkt.pose.yaw)   > 180.f ||
            std::fabs(pkt.pose.roll)  > 180.f) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteRagdoll_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteRagdollSeq_[routeSlot]) <= 0) {
                break;
            }
            remoteRagdollPoses_[routeSlot] = pkt.pose;
            lastRemoteRagdollSeq_[routeSlot] = seq;
            hasRemoteRagdoll_[routeSlot] = true;
            ++remoteRagdollStamp_[routeSlot];
        }
        // Host relay: forward this client's ragdoll pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::Reliable: {
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
        ReliableHeader rh;
        std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
        // payloadLen is uint16_t, can't be negative -- only the upper bound is
        // a real guard.
        const int payloadLen = static_cast<int>(rh.payloadLen);
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

    auto* sockets = SteamNetworkingSockets();

    while (running_.load()) {
        // 1) Pump GNS internal timers + dispatch any pending status callbacks
        // (the trampoline runs inline on THIS thread).
        sockets->RunCallbacks();

        // 2) Drain inbound messages.
        SteamNetworkingMessage_t* msgs[16]{};
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

        // 3) Connected: stream the local pose at sendHz, fan out to all peers.
        const auto now = std::chrono::steady_clock::now();
        if (state_.load() == ConnState::Connected && now >= nextSend) {
            PoseSnapshot local;
            bool have;
            PropPoseSnapshot localProp;
            bool haveProp;
            RagdollPoseSnapshot localRagdoll;
            bool haveRagdoll;
            { std::lock_guard<std::mutex> lk(localMutex_);
              local = localPose_; have = hasLocal_;
              localProp = localPropPose_; haveProp = hasLocalProp_;
              localRagdoll = localRagdollPose_; haveRagdoll = hasLocalRagdoll_; }
            if (have || haveProp || haveRagdoll) {
                for (int i = 0; i < kMaxPeers; ++i) {
                    const uint32_t hConn = peerConns_[i].load();
                    if (hConn == 0) continue;
                    if (have) {
                        PosePacket pkt{};
                        WriteHeader(pkt.header, MsgType::PoseSnapshot,
                                    sendSeq_.fetch_add(1), ownEpoch_);
                        pkt.pose = local;
                        const EResult rc = sockets->SendMessageToConnection(
                            hConn, &pkt, sizeof(pkt),
                            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                        if (rc == k_EResultOK) sent_.fetch_add(1);
                    }
                    if (haveProp) {
                        PropPosePacket pkt{};
                        WriteHeader(pkt.header, MsgType::PropPose,
                                    sendSeq_.fetch_add(1), ownEpoch_);
                        pkt.pose = localProp;
                        const EResult rc = sockets->SendMessageToConnection(
                            hConn, &pkt, sizeof(pkt),
                            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                        if (rc == k_EResultOK) sent_.fetch_add(1);
                    }
                    if (haveRagdoll) {
                        RagdollPosePacket pkt{};
                        WriteHeader(pkt.header, MsgType::RagdollPose,
                                    sendSeq_.fetch_add(1), ownEpoch_);
                        pkt.pose = localRagdoll;
                        const EResult rc = sockets->SendMessageToConnection(
                            hConn, &pkt, sizeof(pkt),
                            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                        if (rc == k_EResultOK) sent_.fetch_add(1);
                    }
                }
            }
            nextSend = now + sendInterval;
        }

        // 4) Sample RTT every second from GNS. The HUD shows ONE number, but
        // sampling only peer 0 means lastRttMs_ freezes if peer 0 disconnects
        // while peer 1/2/3 remain connected. Take the minimum across all live
        // peers so the HUD reflects the best available round-trip.
        if (state_.load() == ConnState::Connected && now >= nextRttSample) {
            int bestPing = -1;
            for (int i = 0; i < kMaxPeers; ++i) {
                const uint32_t hConn = peerConns_[i].load();
                if (hConn == 0) continue;
                SteamNetConnectionRealTimeStatus_t st{};
                if (sockets->GetConnectionRealTimeStatus(hConn, &st, 0, nullptr) == k_EResultOK) {
                    if (st.m_nPing >= 0 && st.m_nPing < 60000) {
                        if (bestPing < 0 || st.m_nPing < bestPing) {
                            bestPing = st.m_nPing;
                        }
                    }
                }
            }
            if (bestPing >= 0) lastRttMs_.store(bestPing);
            nextRttSample = now + std::chrono::milliseconds(1000);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace coop::net
