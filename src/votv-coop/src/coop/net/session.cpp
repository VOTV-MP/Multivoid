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

#include "ue_wrap/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <cstring>
#include <chrono>

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

// PR-3 priority lanes (carried over). See session.cpp commentary in PR-3 commit
// or research/findings/votv-gns-integration-plan-2026-05-27.md §5.4.
enum class Lane : int {
    High = 0,
    Normal = 1,
    Bulk = 2,
    Count = 3,
};

Lane LaneForKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::TeleportClient: return Lane::High;
    case ReliableKind::RestoreVitals:  return Lane::High;
    case ReliableKind::ItemActivate:   return Lane::High;
    case ReliableKind::PropSpawn:      return Lane::Bulk;
    case ReliableKind::EntitySpawn:    return Lane::Bulk;
    default:                           return Lane::Normal;
    }
}

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

// PR-3 lane plumbing applied to a freshly-connected peer (called from the
// Connected status callback). On failure, reliable sends collapse to lane 0
// (still functional, just no priority routing).
void ConfigureLanesForPeer(HSteamNetConnection hConn) {
    constexpr int kLaneCount = static_cast<int>(Lane::Count);
    const int priorities[kLaneCount] = { 0, 1, 2 };
    const uint16 weights[kLaneCount] = { 4, 2, 1 };
    const EResult rc = SteamNetworkingSockets()->ConfigureConnectionLanes(
        hConn, kLaneCount, priorities, weights);
    if (rc != k_EResultOK) {
        UE_LOGW("net: ConfigureConnectionLanes(h=0x%08x) rc=%d",
                static_cast<unsigned>(hConn), static_cast<int>(rc));
    }
}

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

int Session::FindFreePeerSlotForClient() {
    // Host: scan client slots [1..kMaxPeers-1] for the lowest unoccupied one.
    // Slot 0 reserved for "host self" -- never holds a remote connection here.
    for (int i = 1; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() == 0) return i;
    }
    return -1;
}

int Session::FindPeerSlotForConn(uint32_t hConn) {
    for (int i = 0; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() == hConn) return i;
    }
    return -1;
}

void Session::ResetPeerRemoteState(int peerSlot) {
    // remoteMutex_ held by caller.
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return;
    hasRemote_[peerSlot] = false;
    lastRemoteSeq_[peerSlot] = 0;
    remoteStamp_[peerSlot] = 0;
    lastReadStamp_[peerSlot] = 0;
    hasRemoteProp_[peerSlot] = false;
    lastRemotePropSeq_[peerSlot] = 0;
    remotePropStamp_[peerSlot] = 0;
    lastReadPropStamp_[peerSlot] = 0;
}

int Session::connectedPeerCount() const {
    int n = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() != 0) ++n;
    }
    return n;
}

void Session::HandleConnStatusChanged(void* info) {
    auto* cb = static_cast<SteamNetConnectionStatusChangedCallback_t*>(info);
    const HSteamNetConnection hConn = cb->m_hConn;
    const auto oldState = cb->m_eOldState;
    const auto newState = cb->m_info.m_eState;
    auto* sockets = SteamNetworkingSockets();

    // --- Host: accept incoming clients up to kMaxPeers-1 of them.
    if (cfg_.role == Role::Host &&
        oldState == k_ESteamNetworkingConnectionState_None &&
        newState == k_ESteamNetworkingConnectionState_Connecting) {
        const int slot = FindFreePeerSlotForClient();
        if (slot < 0) {
            UE_LOGW("net: host full (%d/%d slots) -- rejecting incoming connection",
                    kMaxPeers - 1, kMaxPeers - 1);
            sockets->CloseConnection(hConn, 0, "host full", false);
            return;
        }
        const EResult rc = sockets->AcceptConnection(hConn);
        if (rc != k_EResultOK) {
            UE_LOGW("net: AcceptConnection rc=%d", static_cast<int>(rc));
            sockets->CloseConnection(hConn, 0, "accept failed", false);
            return;
        }
        // Tag the connection with its peer slot so ReceiveMessagesOnPollGroup
        // can recover the sender in O(1) via msg->m_nConnUserData.
        sockets->SetConnectionUserData(hConn, slot);
        // Add to the host's PollGroup so we drain all clients with one call.
        const uint32_t hPoll = hPollGroup_.load();
        if (hPoll != 0) {
            sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
        }
        peerConns_[slot].store(hConn);
        // Finding #1: only demote to Handshaking if currently Disconnected.
        // If peer-1 is already Connected and peer-2 starts connecting, the
        // aggregate state must remain Connected -- otherwise pose fan-out to
        // peer-1 pauses, TryGetRemotePose returns false, event_feed re-sends
        // Join, and harness teardown can trigger for ~10-200ms of handshake.
        if (state_.load() == ConnState::Disconnected) {
            state_.store(ConnState::Handshaking);
        }
        UE_LOGI("net: host accepted client at slot %d (h=0x%08x, %d/%d connected)",
                slot, static_cast<unsigned>(hConn),
                connectedPeerCount(), kMaxPeers - 1);
        return;
    }

    // --- Both roles: state transitions on an existing connection.

    if (newState == k_ESteamNetworkingConnectionState_Connected) {
        int slot = FindPeerSlotForConn(hConn);
        // Finding #6: GNS may skip the None->Connecting transition in rare cases
        // (per SteamNetConnectionStatusChangedCallback_t header doc). When that
        // happens on host, the slot is unregistered. Late-register here so the
        // connection has a known slot and SetConnectionUserData lands.
        if (slot < 0 && cfg_.role == Role::Host) {
            slot = FindFreePeerSlotForClient();
            if (slot < 0) {
                UE_LOGW("net: host full at Connected (Connecting was skipped) -- closing h=0x%08x",
                        static_cast<unsigned>(hConn));
                sockets->CloseConnection(hConn, 0, "host full", false);
                return;
            }
            sockets->SetConnectionUserData(hConn, slot);
            const uint32_t hPoll = hPollGroup_.load();
            if (hPoll != 0) {
                sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
            }
            peerConns_[slot].store(hConn);
            UE_LOGI("net: late-registered slot %d (Connecting was skipped, h=0x%08x)",
                    slot, static_cast<unsigned>(hConn));
        }
        if (slot < 0) {
            UE_LOGW("net: Connected on unknown connection h=0x%08x (role=%s)",
                    static_cast<unsigned>(hConn),
                    cfg_.role == Role::Host ? "host" : "client");
            return;
        }
        ConfigureLanesForPeer(hConn);
        // Order matters: lanes-configured flag flips ONLY after the
        // ConfigureConnectionLanes call returns, so IsSlotReady() readers
        // see the slot as ready only when the per-kind lane mapping is
        // live on the connection. Acquire/release pair below pairs with
        // the IsSlotReady() relaxed load (any subsequent send through
        // SendReliable etc. happens-before consumer dispatch).
        peerLanesConfigured_[slot].store(true, std::memory_order_release);
        if (state_.load() != ConnState::Connected) {
            state_.store(ConnState::Connected);
        }
        UE_LOGI("net: peer slot %d CONNECTED (%s, h=0x%08x)",
                slot, cfg_.role == Role::Host ? "host" : "client",
                static_cast<unsigned>(hConn));
        // Host tells the freshly-connected client which peer slot it was
        // assigned (clients no longer self-stamp peerSessionId=1; the
        // host is the only authority on slot assignment so two clients
        // can't silently collide). Status callback runs on the net
        // thread; SendReliableToSlot is thread-safe via GNS's queue.
        if (cfg_.role == Role::Host) {
            AssignPeerSlotPayload p{};
            p.slot = static_cast<uint8_t>(slot);
            if (!SendReliableToSlot(slot, ReliableKind::AssignPeerSlot, &p, sizeof(p))) {
                UE_LOGW("net: SendReliableToSlot(AssignPeerSlot=%d) failed", slot);
            } else {
                UE_LOGI("net: sent AssignPeerSlot=%d to client", slot);
            }
        }
        return;
    }

    if (newState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        newState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        const int slot = FindPeerSlotForConn(hConn);
        UE_LOGW("net: peer slot %d closed (oldState=%d reason='%s')",
                slot, static_cast<int>(oldState), cb->m_info.m_szEndDebug);
        if (slot >= 0) {
            peerConns_[slot].store(0);
            peerLanesConfigured_[slot].store(false, std::memory_order_release);
        }
        // Per the GNS header doc on the status callback, terminal states
        // require us to CloseConnection to release the handle.
        sockets->CloseConnection(hConn, 0, nullptr, false);

        // Per-slot reset so a reconnecting peer (whose seq restarts at 0) is
        // not stale-dropped.
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          if (slot >= 0) ResetPeerRemoteState(slot); }

        // Finding #11: drop reliable messages still queued from the departing
        // peer. Without this a PropSpawn from a ghost peer can land in the
        // game thread AFTER the slot has been cleared, and no future
        // PropDestroy can ever arrive.
        if (slot >= 0) {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
                if (it->senderPeerSlot == slot) it = reliableInbox_.erase(it);
                else ++it;
            }
        }

        // Aggregate state: stay Connected if any peer still up; otherwise
        // downgrade and clear everything.
        if (connectedPeerCount() == 0) {
            // Finding #2: full disconnect goes to Disconnected, not Handshaking.
            // Reconnect UI / harness polling state()==Disconnected was
            // permanently blocked when this said Handshaking.
            state_.store(ConnState::Disconnected);
            { std::lock_guard<std::mutex> lk(remoteMutex_);
              for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
            { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
            lastRttMs_.store(0);
            UE_LOGI("net: all peers gone -- session back to Disconnected");
        }
    }
}

Session::~Session() { Stop(); }

bool Session::Start(const Config& cfg) {
    if (running_.load()) {
        UE_LOGW("net: Session::Start ignored -- already running");
        return false;
    }
    cfg_ = cfg;

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

bool Session::TryGetReliable(ReliableMessage& out) {
    std::lock_guard<std::mutex> lk(reliableInboxMutex_);
    if (reliableInbox_.empty()) return false;
    out = std::move(reliableInbox_.front());
    reliableInbox_.pop_front();
    return true;
}

bool Session::SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload, int len) {
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
    WriteHeader(*hdr, MsgType::Reliable, seq, /*token*/0);
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
        WriteHeader(*hdr, MsgType::Reliable, seq, /*token*/0);
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
    uint64_t tokenUnused;
    if (!ParseHeader(data, len, type, seq, tokenUnused)) {
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

    switch (type) {
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) return;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        if (hasRemote_[peerSlot] &&
            static_cast<int32_t>(seq - lastRemoteSeq_[peerSlot]) <= 0) return;
        remotePoses_[peerSlot] = pkt.pose;
        lastRemoteSeq_[peerSlot] = seq;
        hasRemote_[peerSlot] = true;
        ++remoteStamp_[peerSlot];
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
        std::lock_guard<std::mutex> lk(remoteMutex_);
        if (hasRemoteProp_[peerSlot] &&
            static_cast<int32_t>(seq - lastRemotePropSeq_[peerSlot]) <= 0) return;
        remotePropPoses_[peerSlot] = pkt.pose;
        lastRemotePropSeq_[peerSlot] = seq;
        hasRemoteProp_[peerSlot] = true;
        ++remotePropStamp_[peerSlot];
        break;
    }
    case MsgType::Reliable: {
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
        ReliableHeader rh;
        std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
        // Finding #15: payloadLen is uint16_t, can't be negative. Only the
        // upper bound is a real guard.
        const int payloadLen = static_cast<int>(rh.payloadLen);
        if (payloadLen > kMaxReliablePayload) return;
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
        {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            // Finding #12: cap the inbox so a flooding peer can't grow it
            // unboundedly on the host's net thread. The 8192 figure provides
            // ~4x headroom over an observed worst-case connect-time snapshot
            // burst (~1700 PropSpawn for a fully-populated VOTV world). At
            // ~232 B per inline message that's ~1.9 MB worst case -- bounded
            // for DoS while still fitting legitimate fan-outs.
            constexpr size_t kReliableInboxCap = 8192;
            if (reliableInbox_.size() >= kReliableInboxCap) {
                UE_LOGW("net: reliableInbox full (%zu) -- dropping kind=%u from peer slot %d",
                        kReliableInboxCap, static_cast<unsigned>(rh.kind), peerSlot);
                return;
            }
            // Finding #4: emplace + memcpy avoids the per-receive heap alloc
            // (vector::assign). ReliableMessage now holds an inline 228 B
            // payload buffer. Finding #10: stamp senderPeerSlot so drainers
            // can route per-sender (3-peer correctness).
            reliableInbox_.emplace_back();
            ReliableMessage& m = reliableInbox_.back();
            m.kind = static_cast<ReliableKind>(rh.kind);
            m.senderPeerSlot = peerSlot;
            m.payloadLen = static_cast<uint16_t>(payloadLen);
            std::memcpy(m.payload,
                        static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                        static_cast<size_t>(payloadLen));
        }
        break;
    }
    default:
        break;
    }
}

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
                // Finding #5: m_nConnUserData defaults to 0 (or -1 on some
                // GNS versions) before SetConnectionUserData lands. Narrowing
                // a default of 0 here would corrupt slot 0 (the host's own
                // local-self slot) and then the backward-compat 0-arg
                // TryGetRemotePose would permanently return it. Validate
                // bounds AND reject slot 0 on host before narrowing.
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
            { std::lock_guard<std::mutex> lk(localMutex_);
              local = localPose_; have = hasLocal_;
              localProp = localPropPose_; haveProp = hasLocalProp_; }
            if (have || haveProp) {
                for (int i = 0; i < kMaxPeers; ++i) {
                    const uint32_t hConn = peerConns_[i].load();
                    if (hConn == 0) continue;
                    if (have) {
                        PosePacket pkt{};
                        WriteHeader(pkt.header, MsgType::PoseSnapshot,
                                    sendSeq_.fetch_add(1), /*token*/0);
                        pkt.pose = local;
                        const EResult rc = sockets->SendMessageToConnection(
                            hConn, &pkt, sizeof(pkt),
                            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                        if (rc == k_EResultOK) sent_.fetch_add(1);
                    }
                    if (haveProp) {
                        PropPosePacket pkt{};
                        WriteHeader(pkt.header, MsgType::PropPose,
                                    sendSeq_.fetch_add(1), /*token*/0);
                        pkt.pose = localProp;
                        const EResult rc = sockets->SendMessageToConnection(
                            hConn, &pkt, sizeof(pkt),
                            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                        if (rc == k_EResultOK) sent_.fetch_add(1);
                    }
                }
            }
            nextSend = now + sendInterval;
        }

        // 4) Sample RTT every second from GNS (carried from PR-2). Picks the
        // first connected peer's RTT -- good enough for HUD; per-peer RTT is
        // available via GetConnectionRealTimeStatus(peerConns_[i],...) if a
        // future HUD wants it.
        if (state_.load() == ConnState::Connected && now >= nextRttSample) {
            for (int i = 0; i < kMaxPeers; ++i) {
                const uint32_t hConn = peerConns_[i].load();
                if (hConn == 0) continue;
                SteamNetConnectionRealTimeStatus_t st{};
                if (sockets->GetConnectionRealTimeStatus(hConn, &st, 0, nullptr) == k_EResultOK) {
                    if (st.m_nPing >= 0 && st.m_nPing < 60000) {
                        lastRttMs_.store(st.m_nPing);
                    }
                }
                break;  // first connected peer wins for the HUD-level RTT
            }
            nextRttSample = now + std::chrono::milliseconds(1000);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace coop::net
