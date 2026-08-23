// coop/net/send_backlog.cpp -- see coop/net/send_backlog.h.

#include "coop/net/send_backlog.h"

#include "coop/net/net_stats.h"
#include "ue_wrap/core/log.h"

#include <cstring>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

namespace coop::net {

namespace {

// One GNS send attempt of a prebuilt wire packet. Returns the raw rc
// (message number >= 0 on success, -EResult on refusal). The refused
// message is deleted by GNS (bDeleteFailedMessages) -- the caller's copy
// of the bytes is the retry source, never the GNS message object.
int64_t AttemptSend_(uint32_t hConn, int lane, const uint8_t* wire, int len) {
    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();
    if (!sockets || !utils) return -k_EResultInvalidState;
    SteamNetworkingMessage_t* msg = utils->AllocateMessage(len);
    if (!msg) return -k_EResultInvalidState;
    std::memcpy(msg->m_pData, wire, static_cast<size_t>(len));
    msg->m_conn = hConn;
    msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
    msg->m_idxLane = static_cast<uint16>(lane);
    int64 outMsgNum = 0;
    sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
    if (outMsgNum >= 0) net_stats::AddSent(static_cast<uint32_t>(len));
    return outMsgNum;
}

// The connection's TOTAL pending bytes (reliable + unreliable) -- the exact
// sum GNS's enqueue check compares against SendBufferSize
// (steamnetworkingsockets_snp.cpp:320, PendingBytesTotal()). Fresh read.
int PendingBytesTotal_(uint32_t hConn) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return 0;
    SteamNetConnectionRealTimeStatus_t st{};
    if (sockets->GetConnectionRealTimeStatus(hConn, &st, 0, nullptr) != k_EResultOK) return 0;
    return st.m_cbPendingReliable + st.m_cbPendingUnreliable;
}

}  // namespace

void SendBacklog::ResetLocked_(SlotQ& s) {
    for (auto& l : s.lanes) { l.q.clear(); l.bytes = 0; }
    s.totalBytes = 0;
    s.hConn = 0;
    s.episodeOpen = false;
    s.episodeQueued = 0;
    s.episodePeakBytes = 0;
    s.fatal = false;
    s.fatalReason = nullptr;
}

bool SendBacklog::SendOrQueue(int slot, int lane, uint32_t hConn, const uint8_t* wire, int len) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    if (lane < 0 || lane >= kLaneCount || !wire || len <= 0 || hConn == 0) return false;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    // A backlog stamped for a PREVIOUS connection is dead state -- discard it
    // before this (new-connection) send, never deliver it (slot recycling).
    if (s.totalBytes > 0 && s.hConn != hConn) {
        UE_LOGW("send_backlog: slot %d backlog (%zu B) belonged to stale conn 0x%08x -- "
                "discarded on new conn 0x%08x (teardown raced the reassign)",
                slot, s.totalBytes, s.hConn, hConn);
        ResetLocked_(s);
    }
    LaneQ& l = s.lanes[lane];
    if (l.q.empty()) {
        // Fast path: nothing queued on this lane -> attempt directly. Holding
        // s.mu across the attempt is the FIFO-once-nonempty guarantee: a
        // concurrent producer cannot slip a packet into the stream between our
        // refusal and our append.
        const int64_t rc = AttemptSend_(hConn, lane, wire, len);
        if (rc >= 0) return true;
        if (rc != -k_EResultLimitExceeded) {
            // Dying/dead connection (NoConnection / InvalidState) or our own
            // bug (InvalidParam). Never queued: the slot's teardown owns the
            // cleanup; state for a gone peer dies with the peer.
            UE_LOGW("send_backlog: slot %d lane %d send refused fatally rc=%lld -- "
                    "connection dying (not queued)",
                    slot, lane, static_cast<long long>(rc));
            return false;
        }
        // -LimitExceeded: backpressure. Fall through to queue.
    }
    // Queue behind whatever is already pending on this (slot,lane).
    if (s.totalBytes + static_cast<size_t>(len) > kMaxBytesPerSlot) {
        // The fatal byte cap. Do NOT drop this message silently -- queue it and
        // latch fatal; the net thread kicks the slot (the whole backlog dies
        // with the connection, which is the guarantee's honest boundary).
        s.fatal = true;
        s.fatalReason = "send backlog over byte cap (link too slow for the session)";
    }
    if (!s.episodeOpen) {
        s.episodeOpen = true;
        s.episodeQueued = 0;
        s.episodePeakBytes = 0;
        s.lastProgress = std::chrono::steady_clock::now();
        UE_LOGI("send_backlog: EPISODE OPEN slot %d lane %d (send buffer full -- queueing, "
                "delivery guaranteed)", slot, lane);
    }
    s.hConn = hConn;
    l.q.emplace_back(wire, wire + len);
    l.bytes += static_cast<size_t>(len);
    s.totalBytes += static_cast<size_t>(len);
    s.episodeQueued++;
    if (s.totalBytes > s.episodePeakBytes) s.episodePeakBytes = s.totalBytes;
    return true;
}

void SendBacklog::Drain(int slot, uint32_t hConn, int sendBufBytes) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.totalBytes == 0) return;
    if (hConn == 0 || s.hConn != hConn) {
        // The connection this backlog belonged to is gone (slot empty or
        // recycled). Person Y must never receive person X's queued state.
        UE_LOGW("send_backlog: slot %d dropping %zu queued B for stale conn 0x%08x "
                "(live=0x%08x)", slot, s.totalBytes, s.hConn, hConn);
        ResetLocked_(s);
        return;
    }
    // D8 reserve: stop refilling once pending crosses sendBufBytes - kReserve,
    // keeping headroom for the UnreliableNoDelay pose/voice streams. The rc
    // below remains the correctness backstop -- this read is a fairness gate.
    int pending = PendingBytesTotal_(hConn);
    bool progressed = false;
    bool blocked = false;
    for (int lane = 0; lane < kLaneCount && !blocked; ++lane) {  // High -> Normal -> Bulk
        LaneQ& l = s.lanes[lane];
        while (!l.q.empty()) {
            const std::vector<uint8_t>& head = l.q.front();
            const int len = static_cast<int>(head.size());
            if (pending + len > sendBufBytes - kReserve) { blocked = true; break; }
            const int64_t rc = AttemptSend_(hConn, lane, head.data(), len);
            if (rc < 0) {
                if (rc != -k_EResultLimitExceeded) {
                    UE_LOGW("send_backlog: slot %d lane %d drain hit fatal rc=%lld -- "
                            "connection dying, backlog freed at teardown",
                            slot, lane, static_cast<long long>(rc));
                }
                blocked = true;
                break;
            }
            pending += len;
            l.bytes -= static_cast<size_t>(len);
            s.totalBytes -= static_cast<size_t>(len);
            l.q.pop_front();
            progressed = true;
        }
    }
    if (progressed) s.lastProgress = std::chrono::steady_clock::now();
    if (s.totalBytes == 0 && s.episodeOpen) {
        UE_LOGI("send_backlog: EPISODE CLOSE slot %d -- %llu msgs absorbed, peak %zu B, "
                "all delivered", slot,
                static_cast<unsigned long long>(s.episodeQueued), s.episodePeakBytes);
        s.episodeOpen = false;
        s.episodeQueued = 0;
        s.episodePeakBytes = 0;
    }
}

bool SendBacklog::CheckFatal(int slot, const char** reason) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.totalBytes == 0) return false;
    if (!s.fatal &&
        std::chrono::steady_clock::now() - s.lastProgress > kNoProgress) {
        s.fatal = true;
        s.fatalReason = "send backlog made no progress (link stalled)";
    }
    if (s.fatal && reason) *reason = s.fatalReason ? s.fatalReason : "send backlog fatal";
    return s.fatal;
}

void SendBacklog::FreeSlot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.totalBytes > 0)
        UE_LOGI("send_backlog: slot %d teardown -- %zu queued B discarded with the connection",
                slot, s.totalBytes);
    ResetLocked_(s);
}

size_t SendBacklog::DepthBytes(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return 0;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    return s.totalBytes;
}

}  // namespace coop::net
