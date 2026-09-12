// coop/net/session.cpp -- the GameNetworkingSockets session: the reliable send paths and the net
// thread's loop (the signaling poll, callbacks, the pending sweep, the inbound drain with its
// backpressure pause, the stream fan-out, the backlog drain, the per-second diagnostics). The
// receive switch and the admission handler live in session_receive.cpp, Start/Stop and the status
// callbacks in session_start.cpp and session_status.cpp; the scalar streams, the NPC, world-actor,
// trash-carry and driven-prop batches, voice and the relay each have their own file beside this.

#include "coop/net/session.h"

#include "coop/dev/wire_census.h"
#include "coop/element/element.h"
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

// The lanes and the relayable-kind list live in session_lanes.h; Lane::Count is pinned to the
// kLaneCount ConfigureLanesForPeer passes, or a fourth lane would flow through LaneForKind while
// the connection still configured three.
static_assert(static_cast<int>(Lane::Count) == 3,
              "Lane::Count changed -- update kLaneCount in session_status.cpp::ConfigureLanesForPeer");

}  // namespace

Session::~Session() { Stop(); }

bool Session::TryGetReliable(ReliableMessage& out) {
    std::lock_guard<std::mutex> lk(reliableInboxMutex_);
    if (reliableInbox_.empty()) return false;
    out = std::move(reliableInbox_.front());
    reliableInbox_.pop_front();
    return true;
}

namespace {
// Build the complete on-wire reliable packet (PacketHeader + ReliableHeader + payload) into
// `buf`: one builder for the guaranteed path, the try path and the fan-out, so the backlog retries
// exactly these bytes.
int BuildReliableWire_(uint8_t* buf, ReliableKind kind, const void* payload, int len,
                       uint32_t seq, uint32_t ownEpoch, uint8_t senderSlot) {
    auto* hdr = reinterpret_cast<PacketHeader*>(buf);
    WriteHeader(*hdr, MsgType::Reliable, seq, ownEpoch, senderSlot);
    auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
    std::memset(rh, 0, sizeof(*rh));
    rh->kind = static_cast<uint8_t>(kind);
    rh->payloadLen = static_cast<uint16_t>(len);
    // len=0 with a null payload is a legitimate control packet; memcpy from null is UB.
    if (len > 0 && payload) {
        std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), payload, len);
    }
    return static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + len;
}
}  // namespace

bool Session::SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                 int len, uint8_t senderSlot) {
    // The save-stream family is the pump's pacing lane and must not mix with the backlog (a
    // bypassing chunk would overtake a queued Begin in the same lane); the pump calls
    // TrySendReliableToSlot itself, and this routing keeps a stray caller correct.
    if (kind == ReliableKind::SaveTransferBegin || kind == ReliableKind::SaveTransferChunk)
        return TrySendReliableToSlot(peerSlot, kind, payload, len, senderSlot);
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliableToSlot rejected (slot=%d len=%d > %d)",
                peerSlot, len, kMaxReliablePayload);
        return false;
    }
    // The pre-world gate: world-mutating kinds do not flow to a slot that has not announced
    // world-ready (a menu-mode joiner is connected 30-60 s before it has a world); the connect
    // replay rebuilds all of it. Not absorbed by the backlog: a queued gate-skip would deliver
    // stale mutations at ready time and duplicate the replay.
    if (!IsSlotWorldReady(peerSlot) && !IsPreWorldSendableKind(kind)) return false;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return false;

    uint8_t wire[sizeof(PacketHeader) + sizeof(ReliableHeader) + kMaxReliablePayload];
    const int total = BuildReliableWire_(wire, kind, payload, len,
                                         sendSeq_.fetch_add(1), ownEpoch_, senderSlot);
    // Entered the stream or the backlog: true; a dying connection: false (teardown owns the
    // cleanup). See send_backlog.h.
    return backlog_.SendOrQueue(peerSlot, static_cast<int>(LaneForKind(kind)),
                                hConn, wire, total);
}

bool Session::TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                                    int len, uint8_t senderSlot) {
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    // SaveTransferChunk is the one bulk kind: it bypasses the inbox on the receiver (the bulk
    // sink), so its bound is ReliableHeader.payloadLen's uint16; everything else keeps the
    // event-datagram cap.
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
        // Save-family sends fail routinely under send-buffer backpressure: that is the pump's
        // pacing signal, not a warning.
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
    // senderSlot 0: the admission exchange predates any slot assignment, and the receiver of these
    // kinds never reads it (the host routes by the pending tag, the client has one peer).
    BuildReliableWire_(static_cast<uint8_t*>(msg->m_pData), kind, payload, len,
                       sendSeq_.fetch_add(1), ownEpoch_, /*senderSlot*/0);
    msg->m_conn = static_cast<HSteamNetConnection>(hConn);
    msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
    // Lane 0 explicitly: the lanes are configured when the link is finished, which admission gates,
    // and GNS gives every connection lane 0 by default.
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
    linkStage_.store(static_cast<uint8_t>(LinkStage::Admitted), std::memory_order_release);
}

bool Session::SendReliable(ReliableKind kind, const void* payload, int len) {
    if (len < 0 || len > kMaxReliablePayload) {
        UE_LOGW("net: SendReliable rejected (len=%d > %d)", len, kMaxReliablePayload);
        return false;
    }
    // One wire build and one seq for the whole fan-out; the backlog copies the bytes per slot.
    uint8_t wire[sizeof(PacketHeader) + sizeof(ReliableHeader) + kMaxReliablePayload];
    const int total = BuildReliableWire_(wire, kind, payload, len,
                                         sendSeq_.fetch_add(1), ownEpoch_, /*senderSlot*/0);
    const int laneIdx = static_cast<int>(LaneForKind(kind));

    bool anySuccess = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // The pre-world gate, per slot (SendReliableToSlot's rule).
        if (!IsSlotWorldReady(i) && !IsPreWorldSendableKind(kind)) continue;
        // Delivery per slot: the stream or the backlog counts as sent.
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
    p.elementId = elementId;  // a keyless trash clump is routed by eid (key=None cannot disambiguate)
    p.ctx = ctx;              // the host's per-eid generation, so a stale throw cannot re-apply after a
                              // transition
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

void Session::NetThread() {
    const auto sendInterval = std::chrono::milliseconds(
        cfg_.sendHz > 0 ? 1000 / cfg_.sendHz : 33);
    auto nextSend = std::chrono::steady_clock::now();
    auto nextRttSample = std::chrono::steady_clock::now();
    // The host world clock streams on its own ~500 ms cadence, far slower than the pose sendHz (one
    // game minute of real time is well over 500 ms at any day length), keeping the client's frozen
    // mirror within a minute of the host. Net-thread-local.
    auto nextClockSend = std::chrono::steady_clock::now();
    auto nextDeskSimSend = std::chrono::steady_clock::now();

    auto* sockets = SteamNetworkingSockets();

    // Net diagnostics: the per-peer block below reads GNS's real-time telemetry every ~1 s, logs an
    // INFO summary and WARNs on a threshold breach, so a user's log self-flags a slow link or a
    // send-side stall. The counters accumulate between samples.
    constexpr int kHighPingMs      = 250;    // LAN ~1 ms; 250+ = a real link/relay problem
    constexpr int kHighPendingBytes = 65536; // 64 KB outbound PENDING = a real send backlog
    uint64_t sendFails  = 0;  // SendMessageToConnection rejections since the last status sample
    int      worstDrain = 0;  // worst single-pass receive drain since the last status sample
    // Pause accounting per trigger, since the last sample: a depth pause says the game thread is
    // behind, an apply-park pause says one lane asked for backpressure.
    uint64_t pauseDepthHits = 0, pauseParkHits = 0;
    size_t   pauseWorstDepth = 0;

    while (running_.load()) {
        // 0) P2P: pump the signaling transport (inbound ICE rendezvous blobs advance the handshake;
        // outbound is flushed), before RunCallbacks so a state advance a signal triggers is
        // dispatched in the same iteration. nullptr for LanDirect; set before this thread spawned
        // and reset after it joins, so the read needs no lock.
        if (signaling_) signaling_->Poll();

        // The dev wire census flushes its counters at 1 Hz from the loop, not from arrivals, so the
        // final second before a disconnect still lands in the log.
        if (dev::wire_census::Enabled()) dev::wire_census::Tick();

        // 1) Pump GNS's timers and dispatch pending status callbacks (the trampoline runs inline on
        // this thread).
        sockets->RunCallbacks();

        // 1b) Close pending sockets that never proved an identity (eight relaxed loads).
        if (cfg_.role == Role::Host) SweepPending();

        // 2) Drain inbound messages to empty every iteration: a full batch means more may be
        // queued, so the receive loops until a partial batch, and only then does the idle sleep run
        // (a 16-wide batch with an unconditional sleep capped intake below the connect snapshot's
        // ~6000 reliables/s, and the pose stream queued behind it arrived seconds stale). The
        // per-pass cap keeps the outer loop re-checking the stop flag under a sustained flood, so
        // Session::Stop's join cannot hang.
        constexpr int kMaxDrainPerPass = 4096;
        // The reliable inbox at its threshold is backpressure, not drop: pause this pass's receive
        // so GNS buffers underneath (lossless by stall: reassembly overflow does not ACK and
        // decoded-queue overflow refuses without advancing the stream, so the sender retransmits).
        // The pause is bounded by the game-thread stall that caused the pile-up, since the inbox
        // drains on the game tick. Both triggers apply to both roles: inbox depth, and a lane's
        // apply park escalating.
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
                // Attribute the pause to one trigger, depth first, so an apply park cannot read as
                // a depth pause.
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
                // Host: peerSlot is the connection user data set at accept; a client only receives
                // from the host.
                int peerSlot;
                if (cfg_.role == Role::Host) {
                    // m_nConnUserData defaults to 0 or -1 before SetConnectionUserData lands;
                    // narrowing a default 0 would corrupt slot 0, the host's own, so bounds and
                    // slot 0 are checked before narrowing.
                    const int64 ud = msgs[i]->m_nConnUserData;
                    // A pending connection is tagged outside [1, kMaxPeers) by construction, so it
                    // lands in the drop below unless forked to the admission handler first;
                    // everything that is neither a seat nor a pending tag still drops.
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
            // Stop on a partial batch or at the per-pass cap (the latter keeps Stop() live).
            if (n < static_cast<int>(std::size(msgs)) || drained >= kMaxDrainPerPass) break;
        }
        if (drained > worstDrain) worstDrain = drained;  // net-diag: receive-backlog high-water
        if (drained >= kMaxDrainPerPass)
            UE_LOGW("net-diag: receive drain hit the per-pass cap (%d) on the %s net thread -- "
                    "sustained inbound flood; the rest is queued for the next pass",
                    kMaxDrainPerPass, cfg_.role == Role::Host ? "host" : "client");

        // 3) The stream fan-out at sendHz (session_streams.cpp). `now` is computed once and shared
        // with step 4.
        const auto now = std::chrono::steady_clock::now();
        SendStreamsTick(now, sendInterval, nextSend, nextClockSend, nextDeskSimSend, sendFails);

        // 3b) Drain the reliable-send backlogs, one pass per live slot; the GNS return is the
        // headroom read, and the reserve keeps the unreliable pose and voice streams flowing during
        // a drain. A slot whose backlog trips a fatal bound is closed, never trimmed.
        for (int i = 0; i < kMaxPeers; ++i) {
            const uint32_t hConn = peerConns_[i].load();
            if (hConn == 0) continue;
            backlog_.Drain(i, hConn, sendBufBytes_);
            const char* fatalReason = nullptr;
            if (backlog_.CheckFatal(i, &fatalReason)) FatalCloseSlot(i, fatalReason);
        }

        // 4) Per-peer diagnostics every ~1 s from GetConnectionRealTimeStatus: queue time, pending
        // bytes and the allowed send rate, the send-side state that explains a laggy peer (an
        // outbound burst over the allowed rate piles up in the send queue and the pose stream
        // arrives seconds late). An INFO summary, WARNs on high ping, queue latency or pending
        // backlog; rttMsBySlot_ gets each peer's ping.
        if (state_.load() != ConnState::Connected && now >= nextRttSample) {
            // Not connected: publish zeroed rates so the net-stats panel reads offline rather than
            // the last live sample frozen.
            net_stats::PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
            nextRttSample = now + std::chrono::milliseconds(1000);
        }
        if (state_.load() == ConnState::Connected && now >= nextRttSample) {
            // Net stats: the GNS real-time view summed across live connections (wire-level bytes
            // and packets per second, acks and retransmits included) plus the worst ping.
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
                // m_usecQueueTime returns a huge sentinel whenever the estimate is undefined, which
                // is most of the time, so anything absurd clamps to 0; the pending bytes are the
                // real backlog signal.
                long long queueMs = static_cast<long long>(st.m_usecQueueTime / 1000);
                if (queueMs < 0 || queueMs > 60000) queueMs = 0;  // sentinel / no estimate -> 0
                // This slot's RTT for the nameplate and the scoreboard (event_feed fans it to the
                // puppet).
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
                // A send backlog is pending bytes over the threshold; a rate limit or a slow link
                // shows here as KB pending.
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
            // Reported every sample, quiet case included, with its input: the peak depth is what
            // the pause compares, so "no pause fired" means something only beside how close the
            // depth came.
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
