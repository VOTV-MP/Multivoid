#include "coop/net/session.h"

#include "ue_wrap/log.h"

#include <chrono>
#include <random>

namespace coop::net {

namespace {

// Cap on datagrams processed per net-thread iteration. This is the rate limiter:
// the inner recv-drain is bounded so a UDP flood can never make the thread spin a
// core or starve the send/handshake pacing -- it always falls through to the
// timers + sleep. ~64 >> the ~2-4 pkt/tick a healthy 30 Hz peer produces; with the
// post-Connected source-address filter, non-peer flood costs only recvfrom + a
// compare before being dropped.
constexpr int kMaxRecvPerTick = 64;

// A non-zero random session nonce. Not crypto-grade (adequate for LAN anti-spoof;
// a WAN phase would want a CSPRNG); seeded from random_device + the clock so two
// sessions on one machine don't collide.
uint64_t MintToken() {
    std::random_device rd;
    std::mt19937_64 gen(static_cast<uint64_t>(rd()) ^
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint64_t t = gen();
    return t ? t : 1;  // never 0 (0 means "unknown")
}

}  // namespace

Session::~Session() { Stop(); }

bool Session::Start(const Config& cfg) {
    if (running_.load()) {
        UE_LOGW("net: Session::Start ignored -- already running");
        return false;
    }
    cfg_ = cfg;

    // Host binds the well-known port; client binds an ephemeral port (it only
    // needs to reach the host, which learns the client's addr from recvfrom).
    const uint16_t bindPort = (cfg_.role == Role::Host) ? cfg_.port : 0;
    if (!transport_.Open(bindPort)) {
        UE_LOGE("net: Session::Start -- transport open failed (role=%s)",
                cfg_.role == Role::Host ? "host" : "client");
        return false;
    }

    // The client (or an explicit loopback initiator) knows its peer up front; a
    // real host leaves it invalid and learns it from the first client Hello. The
    // host mints the session token; the client starts at 0 and learns it from the
    // host's Hello.
    {
        std::lock_guard<std::mutex> lk(peerMutex_);
        peerLocked_ = false;
        if (cfg_.role == Role::Client || cfg_.initiate) {
            peer_ = Transport::Resolve(cfg_.peerIp, cfg_.port);
            peerLocked_ = (cfg_.role == Role::Host);  // loopback host: peer is fixed (self)
        }
        sessionToken_ = (cfg_.role == Role::Host) ? MintToken() : 0;
    }

    state_.store(ConnState::Handshaking);
    running_.store(true);
    thread_ = std::thread(&Session::NetThread, this);
    UE_LOGI("net: session started role=%s peer=%s:%u port=%u sendHz=%d",
            cfg_.role == Role::Host ? "host" : "client", cfg_.peerIp.c_str(),
            cfg_.port, cfg_.port, cfg_.sendHz);
    return true;
}

void Session::Stop() {
    if (!running_.exchange(false)) return;
    // The net thread is the SOLE owner of the socket: it observes running_==false,
    // sends a best-effort Bye in its teardown, and returns. We join FIRST, then
    // close -- so no thread touches transport_ concurrently (fixes the sock_ race
    // both audits flagged). No Bye is sent from this thread.
    if (thread_.joinable()) thread_.join();
    transport_.Close();
    state_.store(ConnState::Disconnected);
    UE_LOGI("net: session stopped (sent=%llu recv=%llu)",
            static_cast<unsigned long long>(sent_.load()),
            static_cast<unsigned long long>(recv_.load()));
}

void Session::SetLocalPose(const PoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    localPose_ = pose;
    hasLocal_ = true;
}

bool Session::SendReliable(ReliableKind kind, const void* payload, int len) {
    return reliable_.Send(kind, payload, len);
}

bool Session::TryGetReliable(ReliableChannel::Message& out) {
    return reliable_.TryDrain(out);
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

void Session::HandleDatagram(const void* data, int len, const Endpoint& from) {
    MsgType type;
    uint32_t seq;
    uint64_t token;
    if (!ParseHeader(data, len, type, seq, token)) return;  // not ours / malformed -- drop
    recv_.fetch_add(1);

    // Trust-boundary check, done ONCE up front: who may this datagram be from, and
    // does it carry the right session token? A Hello is the only message allowed to
    // carry token 0 (a client that hasn't learned the token yet) or to arrive from
    // an as-yet-unknown peer. Everything else must come from the locked peer and
    // bear the established token.
    {
        std::lock_guard<std::mutex> lk(peerMutex_);
        const bool fromPeer = peerLocked_ && peer_ == from;
        if (type == MsgType::Hello) {
            if (cfg_.role == Role::Host) {
                if (!peerLocked_) {                      // first contact: lock to this client
                    peer_ = from;
                    peerLocked_ = true;
                    UE_LOGI("net: host locked peer");
                } else if (!fromPeer) {
                    return;  // already bound to someone else -- ignore foreign Hello (no hijack)
                }
                // Host verifies the client only once the client echoes our token.
                if (token == sessionToken_ && state_.load() != ConnState::Connected) {
                    state_.store(ConnState::Connected);
                    UE_LOGI("net: CONNECTED (host)");
                }
            } else {  // client: adopt the token the host advertised
                if (!fromPeer) return;  // only the configured host may answer
                if (token != 0) {
                    sessionToken_ = token;
                    if (state_.load() != ConnState::Connected) {
                        state_.store(ConnState::Connected);
                        UE_LOGI("net: CONNECTED (client)");
                    }
                }
            }
            return;  // Hello carries no payload
        }
        // Non-Hello: must be the locked peer with the right token.
        if (!fromPeer || token == 0 || token != sessionToken_) return;
    }

    switch (type) {
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) return;  // reject NaN/Inf/out-of-bounds before the engine sees it

        std::lock_guard<std::mutex> lk(remoteMutex_);
        // Stale-drop with RFC-1982 serial comparison: accept only a strictly newer
        // seq. Signed wrap of (seq - last) handles the 32-bit counter rollover so a
        // wrap (or a hostile high seq) can't lock out the real peer forever.
        if (hasRemote_ && static_cast<int32_t>(seq - lastRemoteSeq_) <= 0) return;
        remotePose_ = pkt.pose;
        lastRemoteSeq_ = seq;
        hasRemote_ = true;
        ++remoteStamp_;
        break;
    }
    case MsgType::Reliable: {
        // Ack to the sender's address; deliver in order via the reliable channel.
        auto ack = [this, &from](const void* d, int n) { return transport_.SendTo(from, d, n); };
        reliable_.OnReliable(data, len, token, ack);
        break;
    }
    case MsgType::ReliableAck:
        reliable_.OnAck(seq);  // the header seq carries the acked relSeq
        break;
    case MsgType::Bye: {
        UE_LOGI("net: peer said Bye");
        state_.store(ConnState::Handshaking);
        // Reset remote tracking so a reconnecting peer (whose seq restarts at 0) is
        // not stale-dropped for the rest of the session, and re-learn the peer.
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          hasRemote_ = false; lastRemoteSeq_ = 0; remoteStamp_ = 0; lastReadStamp_ = 0; }
        reliable_.Reset();
        if (cfg_.role == Role::Host) {
            std::lock_guard<std::mutex> lk(peerMutex_);
            peerLocked_ = false;  // host re-learns the peer on the next Hello
        }
        break;
    }
    default:
        break;  // Hello handled above
    }
}

void Session::NetThread() {
    const auto sendInterval = std::chrono::milliseconds(
        cfg_.sendHz > 0 ? 1000 / cfg_.sendHz : 33);
    auto nextSend = std::chrono::steady_clock::now();
    auto nextHello = std::chrono::steady_clock::now();
    char buf[kMaxPacketBytes];

    while (running_.load()) {
        // 1) Drain pending datagrams, BOUNDED (kMaxRecvPerTick). The cap is the
        // rate limiter: a flood can never make this loop run forever / spin the
        // core or starve the send + handshake pacing below -- we always fall
        // through to the timers and the sleep.
        for (int i = 0; i < kMaxRecvPerTick; ++i) {
            Endpoint from;
            const int n = transport_.RecvFrom(buf, sizeof(buf), from);
            if (n <= 0) break;  // 0 = would-block (drained), -1 = error
            HandleDatagram(buf, n, from);
        }

        const auto now = std::chrono::steady_clock::now();

        // Snapshot the peer + token once per iteration (cheap; avoids re-locking).
        Endpoint peer;
        uint64_t token;
        { std::lock_guard<std::mutex> lk(peerMutex_); peer = peer_; token = sessionToken_; }

        // Reliable channel: retransmit the in-flight control message if its RTO
        // elapsed (sends to the current peer).
        auto sendToPeer = [this, &peer](const void* d, int n) {
            return peer.valid() && transport_.SendTo(peer, d, n);
        };
        reliable_.Tick(sendToPeer, token);

        // 2) Handshake: until connected, send a Hello carrying our token ~5x/s. The
        // host advertises its real token; the client sends 0 until it learns one. A
        // host with no locked peer simply waits (peer invalid).
        if (state_.load() != ConnState::Connected && now >= nextHello) {
            if (peer.valid()) {
                PacketHeader hello{};
                WriteHeader(hello, MsgType::Hello, sendSeq_.fetch_add(1), token);
                if (transport_.SendTo(peer, &hello, sizeof(hello))) sent_.fetch_add(1);
            }
            nextHello = now + std::chrono::milliseconds(200);
        }

        // 3) Connected: stream the local pose at sendHz (with the session token).
        if (state_.load() == ConnState::Connected && now >= nextSend) {
            PoseSnapshot local;
            bool have;
            { std::lock_guard<std::mutex> lk(localMutex_); local = localPose_; have = hasLocal_; }
            if (have && peer.valid()) {
                PosePacket pkt{};
                WriteHeader(pkt.header, MsgType::PoseSnapshot, sendSeq_.fetch_add(1), token);
                pkt.pose = local;
                if (transport_.SendTo(peer, &pkt, sizeof(pkt))) sent_.fetch_add(1);
            }
            nextSend = now + sendInterval;
        }

        // Idle a touch so we don't spin a core. ~5 ms keeps recv latency low while
        // the 30 Hz send / 200 ms hello cadence is paced by the timers above.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Teardown (we are the sole socket owner here): best-effort Bye to the peer,
    // then return so Stop() can join + close with no concurrent access.
    Endpoint peer;
    uint64_t token;
    { std::lock_guard<std::mutex> lk(peerMutex_); peer = peer_; token = sessionToken_; }
    if (peer.valid() && transport_.IsOpen()) {
        PacketHeader bye{};
        WriteHeader(bye, MsgType::Bye, sendSeq_.fetch_add(1), token);
        transport_.SendTo(peer, &bye, sizeof(bye));
    }
}

}  // namespace coop::net
