// coop/net/session.cpp -- PR-2 GNS implementation.
//
// Lifecycle:
//   Start():  Init GNS lib (once, refcounted) + register status callback +
//             host CreateListenSocketIP or client ConnectByIPAddress +
//             spin NetThread.
//   NetThread: loop { RunCallbacks (drives the status callback inline);
//             ReceiveMessagesOnConnection -> HandleMessage; emit pose +
//             prop pose at sendHz if Connected; sleep(5ms) }
//   Status callback (called by GNS from RunCallbacks): host accepts
//             incoming connection, both sides transition state_, both sides
//             reset their remote tracking on disconnect.
//   Stop():   close connection + join thread. GNS lib stays inited across
//             Start/Stop cycles for fast reconnect; killed in OnDllUnload.
//
// Wire format inside each GNS message: the pre-PR-2 protocol.h packet
// structs are sent raw via SendMessageToConnection. The header's `token`
// field is now always 0 (GNS auth replaces it); `seq` is still used for
// stale-drop on the unreliable pose streams.

#include "coop/net/session.h"

#include "ue_wrap/log.h"

// GNS headers warn at /W4 (audit-prompt-perf-template item: localize the
// suppression to this TU, don't whitelist the warnings at project level).
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

// One Session per process (PR-3 plans 4 peers but still one Session). The C
// GNS callback finds the live instance via this pointer.
std::atomic<Session*> g_session{nullptr};

// GNS lib refcount. GameNetworkingSockets_Init spawns a background thread;
// per plan-doc §11.2, we MUST NOT call it from DllMain. We init lazily on
// first Start() and leave it inited so a Stop()+Start() reconnect is fast.
// Kill is queued for OnDllUnload (TODO once shutdown.cpp catches the
// DLL_PROCESS_DETACH for this).
std::mutex g_initMutex;
bool g_inited = false;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// MTU-sized scratch buffer for the host's outbound packets. PR-2 ships with
// 256 B max (matches our pre-PR-2 protocol constant); GNS itself supports
// ~1200 B per message, which PR-4 will exploit for batched reliable bursts.
constexpr int kSendStaging = kMaxPacketBytes;

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

}  // namespace

// === C callback adapter (GNS calls this from RunCallbacks on our net thread).
//
// Session::OnConnStatusChanged keeps a void* signature in the header so the
// public API doesn't pull in GNS types; the file-local trampoline below has
// the exact FnSteamNetConnectionStatusChanged signature GNS expects, casts
// to void*, and forwards.
void Session::OnConnStatusChanged(void* info) {
    auto* self = g_session.load(std::memory_order_acquire);
    if (self) self->HandleConnStatusChanged(info);
}

namespace {
void ConnStatusTrampoline(SteamNetConnectionStatusChangedCallback_t* cb) {
    Session::OnConnStatusChanged(cb);
}
}  // namespace

void Session::HandleConnStatusChanged(void* info) {
    auto* cb = static_cast<SteamNetConnectionStatusChangedCallback_t*>(info);
    const HSteamNetConnection hConn = cb->m_hConn;
    const auto oldState = cb->m_eOldState;
    const auto newState = cb->m_info.m_eState;
    auto* sockets = SteamNetworkingSockets();

    // --- Host: accept incoming connection requests (one at a time for PR-2;
    // PR-3 lifts to kMaxPeers=4 with a PollGroup).
    if (cfg_.role == Role::Host &&
        oldState == k_ESteamNetworkingConnectionState_None &&
        newState == k_ESteamNetworkingConnectionState_Connecting) {
        if (hConn_.load() != 0) {
            UE_LOGW("net: host rejecting extra connection (already have one peer)");
            sockets->CloseConnection(hConn, 0, "host already has a peer", false);
            return;
        }
        const EResult rc = sockets->AcceptConnection(hConn);
        if (rc != k_EResultOK) {
            UE_LOGW("net: AcceptConnection failed rc=%d", static_cast<int>(rc));
            sockets->CloseConnection(hConn, 0, "accept failed", false);
            return;
        }
        hConn_.store(hConn);
        state_.store(ConnState::Handshaking);
        UE_LOGI("net: host accepted incoming connection (h=0x%08x)",
                static_cast<unsigned>(hConn));
        return;  // wait for Connecting -> Connected on a later callback
    }

    // --- Both roles: client got here via ConnectByIPAddress (state was None on
    // the Session side but Connecting on the GNS side already). The handle
    // was stored in Start(); just track state transitions below.

    if (newState == k_ESteamNetworkingConnectionState_Connected) {
        if (state_.load() != ConnState::Connected) {
            state_.store(ConnState::Connected);
            UE_LOGI("net: CONNECTED (%s, h=0x%08x)",
                    cfg_.role == Role::Host ? "host" : "client",
                    static_cast<unsigned>(hConn));
        }
        return;
    }

    if (newState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        newState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        UE_LOGW("net: connection closed (oldState=%d reason='%s')",
                static_cast<int>(oldState), cb->m_info.m_szEndDebug);
        if (hConn_.load() == hConn) hConn_.store(0);
        // GNS requires us to release the handle ourselves on these terminal
        // states (per header doc on SteamNetConnectionStatusChangedCallback_t).
        sockets->CloseConnection(hConn, 0, nullptr, false);
        state_.store(ConnState::Handshaking);
        // Reset remote tracking so a reconnecting peer's seq=0 isn't stale-
        // dropped for the rest of the session (audit fix carried from pre-PR-2).
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          hasRemote_ = false; lastRemoteSeq_ = 0; remoteStamp_ = 0; lastReadStamp_ = 0;
          hasRemoteProp_ = false; lastRemotePropSeq_ = 0;
          remotePropStamp_ = 0; lastReadPropStamp_ = 0; }
        { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
        lastRttMs_.store(0);
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

    // Register the status callback (global, picked up by every connection).
    // Idempotent: GNS overwrites the existing pointer.
    g_session.store(this, std::memory_order_release);
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &ConnStatusTrampoline);

    auto* sockets = SteamNetworkingSockets();
    if (cfg_.role == Role::Host) {
        SteamNetworkingIPAddr addr{};
        addr.Clear();
        addr.m_port = cfg_.port;
        // Listen on all interfaces (IPv4 + IPv6); GNS treats AnyIP as
        // dual-stack. (addr.m_port==0 with everything else zero == AnyIP.)
        const HSteamListenSocket hListen = sockets->CreateListenSocketIP(addr, 0, nullptr);
        if (hListen == k_HSteamListenSocket_Invalid) {
            UE_LOGE("net: CreateListenSocketIP(port=%u) failed", cfg_.port);
            g_session.store(nullptr, std::memory_order_release);
            return false;
        }
        hListen_.store(hListen);
        UE_LOGI("net: host listening on port %u (hListen=0x%08x)",
                cfg_.port, static_cast<unsigned>(hListen));
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
        hConn_.store(hConn);
        UE_LOGI("net: client dialed %s:%u (hConn=0x%08x)",
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
    if (thread_.joinable()) thread_.join();

    auto* sockets = SteamNetworkingSockets();
    if (sockets) {
        const uint32_t hConn = hConn_.exchange(0);
        if (hConn != 0) {
            // Linger 200 ms so any queued reliable packets get out.
            sockets->CloseConnection(hConn, 0, "session stop", true);
        }
        const uint32_t hListen = hListen_.exchange(0);
        if (hListen != 0) sockets->CloseListenSocket(hListen);
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

bool Session::TryGetRemotePose(PoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemote_) return false;
    out = remotePose_;
    if (outIsNew) *outIsNew = (remoteStamp_ != lastReadStamp_);
    lastReadStamp_ = remoteStamp_;
    return true;
}

bool Session::TryGetRemotePropPose(PropPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteProp_) return false;
    out = remotePropPose_;
    if (outIsNew) *outIsNew = (remotePropStamp_ != lastReadPropStamp_);
    lastReadPropStamp_ = remotePropStamp_;
    return true;
}

bool Session::TryGetReliable(ReliableMessage& out) {
    std::lock_guard<std::mutex> lk(reliableInboxMutex_);
    if (reliableInbox_.empty()) return false;
    out = std::move(reliableInbox_.front());
    reliableInbox_.pop_front();
    return true;
}

bool Session::SendReliable(ReliableKind kind, const void* payload, int len) {
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliable rejected (len=%d > %d)", len, kMaxReliablePayload);
        return false;
    }
    const uint32_t hConn = hConn_.load();
    if (hConn == 0) return false;  // not connected yet; caller decides whether to retry

    // Frame the message the same way the pre-PR-2 reliable channel did:
    // PacketHeader + ReliableHeader + payload. GNS provides the auth +
    // ordering + retransmit; we keep the existing layout so the receiver's
    // dispatch code is unchanged.
    uint8_t buf[kSendStaging];
    auto* hdr = reinterpret_cast<PacketHeader*>(buf);
    WriteHeader(*hdr, MsgType::Reliable, sendSeq_.fetch_add(1), /*token*/0);
    auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
    std::memset(rh, 0, sizeof(*rh));
    rh->kind = static_cast<uint8_t>(kind);
    rh->payloadLen = static_cast<uint16_t>(len);
    std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), payload, len);
    const int total = static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;

    int64 outMsg = 0;
    const EResult rc = SteamNetworkingSockets()->SendMessageToConnection(
        hConn, buf, static_cast<uint32>(total),
        k_nSteamNetworkingSend_Reliable, &outMsg);
    if (rc != k_EResultOK) {
        UE_LOGW("net: SendReliable rc=%d kind=%u", static_cast<int>(rc),
                static_cast<unsigned>(kind));
        return false;
    }
    sent_.fetch_add(1);
    return true;
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

bool Session::SendPropDestroy(const WireKey& key) {
    PropDestroyPayload p{};
    p.key = key;
    return SendReliable(ReliableKind::PropDestroy, &p, sizeof(p));
}

bool Session::SendEntitySpawn(const EntitySpawnPayload& payload) {
    return SendReliable(ReliableKind::EntitySpawn, &payload, sizeof(payload));
}

bool Session::SendEntityDestroy(uint32_t sessionId) {
    EntityDestroyPayload p{};
    p.sessionId = sessionId;
    return SendReliable(ReliableKind::EntityDestroy, &p, sizeof(p));
}

void Session::HandleMessage(const void* data, int len) {
    MsgType type;
    uint32_t seq;
    uint64_t tokenUnused;
    if (!ParseHeader(data, len, type, seq, tokenUnused)) return;
    recv_.fetch_add(1);

    switch (type) {
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) return;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        if (hasRemote_ && static_cast<int32_t>(seq - lastRemoteSeq_) <= 0) return;
        remotePose_ = pkt.pose;
        lastRemoteSeq_ = seq;
        hasRemote_ = true;
        ++remoteStamp_;
        break;
    }
    case MsgType::PropPose: {
        if (len < static_cast<int>(sizeof(PropPosePacket))) return;
        PropPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary: finite floats + position AABB + rotation canonical +
        // key length. Carried verbatim from pre-PR-2; without these a hostile
        // or buggy sender's NaN/Inf reaches SetActorRotation -> PhysX UB.
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
        if (hasRemoteProp_ && static_cast<int32_t>(seq - lastRemotePropSeq_) <= 0) return;
        remotePropPose_ = pkt.pose;
        lastRemotePropSeq_ = seq;
        hasRemoteProp_ = true;
        ++remotePropStamp_;
        break;
    }
    case MsgType::Reliable: {
        // GNS gave us in-order delivery already; we just unwrap the
        // ReliableHeader and queue for the game thread.
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
        ReliableHeader rh;
        std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
        const int payloadLen = static_cast<int>(rh.payloadLen);
        if (payloadLen < 0 || payloadLen > kMaxReliablePayload) return;
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
        ReliableMessage m;
        m.kind = static_cast<ReliableKind>(rh.kind);
        m.payload.assign(
            static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
            static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader) + payloadLen);
        std::lock_guard<std::mutex> lk(reliableInboxMutex_);
        reliableInbox_.push_back(std::move(m));
        break;
    }
    default:
        // PR-2 dropped Hello / Bye / Ping / Pong / ReliableAck (GNS owns
        // handshake + RTT + acks). Unknown / dead MsgTypes ignored.
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
        // (which run inline on THIS thread).
        sockets->RunCallbacks();

        // 2) Drain inbound messages on our connection (if any).
        const uint32_t hConn = hConn_.load();
        if (hConn != 0) {
            SteamNetworkingMessage_t* msgs[16]{};
            const int n = sockets->ReceiveMessagesOnConnection(
                hConn, msgs, static_cast<int>(std::size(msgs)));
            for (int i = 0; i < n; ++i) {
                HandleMessage(msgs[i]->m_pData, static_cast<int>(msgs[i]->m_cbSize));
                msgs[i]->Release();  // critical: GNS owns the buffer
            }
        }

        // 3) Connected: stream the local pose at sendHz.
        const auto now = std::chrono::steady_clock::now();
        if (state_.load() == ConnState::Connected && now >= nextSend && hConn != 0) {
            PoseSnapshot local;
            bool have;
            PropPoseSnapshot localProp;
            bool haveProp;
            { std::lock_guard<std::mutex> lk(localMutex_);
              local = localPose_; have = hasLocal_;
              localProp = localPropPose_; haveProp = hasLocalProp_; }
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
            nextSend = now + sendInterval;
        }

        // 4) Sample RTT every second from GNS (replaces our Ping/Pong).
        if (state_.load() == ConnState::Connected && hConn != 0 && now >= nextRttSample) {
            SteamNetConnectionRealTimeStatus_t st{};
            if (sockets->GetConnectionRealTimeStatus(hConn, &st, 0, nullptr) == k_EResultOK) {
                if (st.m_nPing >= 0 && st.m_nPing < 60000) {
                    lastRttMs_.store(st.m_nPing);
                }
            }
            nextRttSample = now + std::chrono::milliseconds(1000);
        }

        // 5) Idle. ~5 ms keeps recv latency low while letting the 60 Hz send
        // cadence be paced by the timer above.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace coop::net
