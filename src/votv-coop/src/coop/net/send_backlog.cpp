// coop/net/send_backlog.cpp -- see coop/net/send_backlog.h.

#include "coop/net/send_backlog.h"

#include "session_lanes.h"  // Lane (co-located private header)

#include "coop/config/config.h"           // ResolveInt, the red arm
#include "coop/config/config_registry.h"  // rows::join_gate_red
#include "coop/net/net_stats.h"
#include "coop/net/protocol.h"            // PacketHeader / ReliableHeader, to read a held packet's kind
#include "coop/net/snapshot_send_scope.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

namespace coop::net {

namespace {

// Held messages the gate's enqueue stamps make room for at open, so a typical join does not regrow them.
constexpr size_t kJoinStampReserve = 512;

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

}  // namespace

void SendBacklog::ResetLocked_(SlotQ& s) {
    for (auto& l : s.lanes) { l.q.clear(); l.bytes = 0; }
    s.totalBytes = 0;
    s.joinGate = false;
    s.announceHolds = 0;
    s.joinQ.clear();
    s.joinStamps.clear();
    s.joinBytes = 0;
    s.joinWarned = false;
    s.hConn = 0;
    s.episodeOpen = false;
    s.episodeQueued = 0;
    s.episodePeakBytes = 0;
    s.fatal = false;
    s.fatalReason = nullptr;
    s.dyingLogged = false;
}

SendOutcome SendBacklog::SendOrQueue(int slot, int lane, uint32_t hConn,
                                     const uint8_t* wire, int len, SendAdmission& adm) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers))
        return SendOutcome::Dropped;
    if (lane < 0 || lane >= kLaneCount || !wire || len <= 0 || hConn == 0)
        return SendOutcome::Dropped;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    return SendLocked_(s, slot, lane, hConn, wire, len, adm);
}

SendOutcome SendBacklog::SendOrQueueBatch(int slot, int lane, uint32_t hConn, const uint8_t* wire,
                                          int len, int count, SendAdmission& adm, int& streamed) {
    streamed = 0;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers))
        return SendOutcome::Dropped;
    if (lane < 0 || lane >= kLaneCount || !wire || len <= 0 || count <= 0 || hConn == 0)
        return SendOutcome::Dropped;
    SlotQ& s = slots_[slot];
    // One acquisition for every packet: the join gate and the lane's queue state cannot change
    // between two packets of the batch, so all of them go straight or all of them queue behind
    // the same head. The first Dropped ends the batch, as it ends a chunk loop.
    std::lock_guard<std::mutex> lk(s.mu);
    SendOutcome worst = SendOutcome::Streamed;
    for (int i = 0; i < count; ++i) {
        const SendOutcome rc = SendLocked_(s, slot, lane, hConn, wire + static_cast<size_t>(i) * len,
                                           len, adm);
        if (rc == SendOutcome::Dropped) return rc;
        if (rc == SendOutcome::Streamed) ++streamed;
        else worst = SendOutcome::Queued;
    }
    return worst;
}

SendOutcome SendBacklog::SendLocked_(SlotQ& s, int slot, int lane, uint32_t hConn,
                                     const uint8_t* wire, int len, SendAdmission& adm) {
    // A backlog stamped for a PREVIOUS connection is dead state -- discard it
    // before this (new-connection) send, never deliver it (slot recycling).
    if ((s.totalBytes > 0 || s.joinBytes > 0) && s.hConn != hConn) {
        UE_LOGW("send_backlog: slot %d backlog (%zu B) belonged to stale conn 0x%08x -- "
                "discarded on new conn 0x%08x (teardown raced the reassign)",
                slot, s.totalBytes + s.joinBytes, s.hConn, hConn);
        ResetLocked_(s);
    }
    // The join gate: a Default-lane send to a joining slot waits in the join queue, in send order,
    // until the snapshot completes -- unless it IS the snapshot (a SnapshotSendScope for this slot).
    // Held packets are Queued: they will be delivered, and the drain counts them when it hands them over.
    if (s.joinGate && lane == static_cast<int>(Lane::Default) &&
        SnapshotSendScope::CurrentSlot() != slot) {
        if (s.totalBytes + s.joinBytes + static_cast<size_t>(len) > kMaxBytesPerSlot) {
            s.fatal = true;
            s.fatalReason = "send backlog over byte cap (link too slow for the session)";
        }
        s.hConn = hConn;
        const uint8_t kind = len >= static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))
                                 ? reinterpret_cast<const ReliableHeader*>(wire + sizeof(PacketHeader))->kind
                                 : uint8_t{0};
        s.joinQ.emplace_back(wire, wire + len);
        s.joinStamps.push_back({std::chrono::steady_clock::now(), kind});
        s.joinBytes += static_cast<size_t>(len);
        return SendOutcome::Queued;
    }
    LaneQ& l = s.lanes[lane];
    // Which of the two brims turned this packet back, for the episode line: our own reserve, or
    // the transport's buffer. They are not the same event -- the transport can pass its own limit
    // from the outside, by re-queueing a NACKed segment into pending.
    const char* why = "the lane is already queueing";
    if (l.q.empty()) {
        // Fast path: nothing queued on this lane -> attempt directly, unless the headroom rule
        // holds this packet short of the reserve, in which case it queues exactly as a refused
        // one would and departs when the buffer has room again. Holding s.mu across the attempt
        // is the FIFO-once-nonempty guarantee: a concurrent producer cannot slip a packet into
        // the stream between our refusal and our append.
        if (!adm.Admits(slot, len)) {
            adm.NoteRefused(slot);
            why = "no room above the headroom reserve";
        } else {
            const int64_t rc = AttemptSend_(hConn, lane, wire, len);
            if (rc >= 0) {
                adm.NoteHanded(slot, len);
                return SendOutcome::Streamed;
            }
            if (rc != -k_EResultLimitExceeded) {
                // Dying/dead connection (NoConnection / InvalidState) or our own
                // bug (InvalidParam). Never queued: the slot's teardown owns the
                // cleanup; state for a gone peer dies with the peer. Logged once
                // per connection (the window before the status callback tears the
                // slot down can see many sends).
                if (!s.dyingLogged) {
                    s.dyingLogged = true;
                    UE_LOGW("send_backlog: slot %d lane %d send refused fatally rc=%lld -- "
                            "connection dying (not queued; further lines folded)",
                            slot, lane, static_cast<long long>(rc));
                }
                return SendOutcome::Dropped;
            }
            // -LimitExceeded: backpressure from the transport's own buffer, which our reserve had
            // said there was room above. Fall through to queue.
            why = "the transport's send buffer is full";
        }
    }
    // Queue behind whatever is already pending on this (slot,lane).
    if (s.totalBytes + s.joinBytes + static_cast<size_t>(len) > kMaxBytesPerSlot) {
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
        UE_LOGI("send_backlog: EPISODE OPEN slot %d lane %d (%s -- queueing, delivery guaranteed)",
                slot, lane, why);
    }
    s.hConn = hConn;
    l.q.emplace_back(wire, wire + len);
    l.bytes += static_cast<size_t>(len);
    s.totalBytes += static_cast<size_t>(len);
    s.episodeQueued++;
    if (s.totalBytes > s.episodePeakBytes) s.episodePeakBytes = s.totalBytes;
    return SendOutcome::Queued;
}

int SendBacklog::Drain(int slot, uint32_t hConn, SendAdmission& adm) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return 0;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    // A gate that stays open means the slot's snapshot has not completed -- deferred, or waiting behind
    // another joiner's drain. The gate stays open on purpose (a seed sent before its props is the defect
    // it exists to stop), so the only action is this one line per open, with the queue's depth.
    if (s.joinGate && !s.joinWarned &&
        std::chrono::steady_clock::now() - s.joinOpenedAt > kJoinGateWarn) {
        s.joinWarned = true;
        UE_LOGW("[JOIN-GATE] slot %d open for over %lld s without closing -- %zu msgs, %zu B held",
                slot, static_cast<long long>(kJoinGateWarn.count()), s.joinQ.size(), s.joinBytes);
    }
    if (s.totalBytes == 0) return 0;
    if (hConn == 0 || s.hConn != hConn) {
        // The connection this backlog belonged to is gone (slot empty or
        // recycled). Person Y must never receive person X's queued state.
        UE_LOGW("send_backlog: slot %d dropping %zu queued B for stale conn 0x%08x "
                "(live=0x%08x)", slot, s.totalBytes, s.hConn, hConn);
        ResetLocked_(s);
        return 0;
    }
    // The refill stops at the headroom rule, keeping a slice free for the UnreliableNoDelay pose
    // and voice streams. The rc below remains the correctness backstop -- the rule is a fairness
    // gate, and a head it holds back stays queued rather than counting as a send refusal.
    int streamed = 0;
    bool progressed = false;
    bool blocked = false;
    int sentThisPass = 0;  // kDrainPassCap bounds the mutex hold
    for (int lane = 0; lane < kLaneCount && !blocked; ++lane) {  // High -> Default -> Transfer
        LaneQ& l = s.lanes[lane];
        while (!l.q.empty()) {
            if (sentThisPass >= kDrainPassCap) { blocked = true; break; }
            const std::vector<uint8_t>& head = l.q.front();
            const int len = static_cast<int>(head.size());
            if (!adm.Admits(slot, len)) { blocked = true; break; }
            const int64_t rc = AttemptSend_(hConn, lane, head.data(), len);
            if (rc < 0) {
                if (rc != -k_EResultLimitExceeded && !s.dyingLogged) {
                    s.dyingLogged = true;
                    UE_LOGW("send_backlog: slot %d lane %d drain hit fatal rc=%lld -- "
                            "connection dying, backlog freed at teardown (folded)",
                            slot, lane, static_cast<long long>(rc));
                }
                blocked = true;
                break;
            }
            adm.NoteHanded(slot, len);
            streamed += len;
            l.bytes -= static_cast<size_t>(len);
            s.totalBytes -= static_cast<size_t>(len);
            l.q.pop_front();
            progressed = true;
            ++sentThisPass;
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
    return streamed;
}

bool SendBacklog::CheckFatal(int slot, const char** reason) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.totalBytes == 0 && s.joinBytes == 0) return false;
    // The no-progress timer measures the lane queue only: a held join queue is not a stalled link.
    if (!s.fatal && s.totalBytes > 0 &&
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
    if (s.totalBytes > 0 || s.joinBytes > 0)
        UE_LOGI("send_backlog: slot %d teardown -- %zu queued B and %zu join-gate B discarded with the connection",
                slot, s.totalBytes, s.joinBytes);
    ResetLocked_(s);
}

namespace {
// Resolved once: the red arm of the join drills, where the gate is bypassed and the receive
// parks catch the seeds that precede their props.
bool JoinGateRed_() {
    static const bool red = coop::config::ResolveInt(coop::config_registry::rows::join_gate_red) != 0;
    return red;
}
}  // namespace

void SendBacklog::OpenJoinGate(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (JoinGateRed_()) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    ++s.announceHolds;
    OpenLocked_(s);
}

void SendBacklog::OpenJoinGateForSnapshot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (JoinGateRed_()) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    OpenLocked_(s);
}

void SendBacklog::ReleaseAnnounceHold(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.announceHolds > 0) --s.announceHolds;
}

void SendBacklog::OpenLocked_(SlotQ& s) {
    if (s.joinGate) return;  // a re-announce while open keeps the queue and the window
    s.joinGate = true;
    s.joinOpenedAt = std::chrono::steady_clock::now();
    s.joinWarned = false;
    s.joinStamps.reserve(kJoinStampReserve);
}

void SendBacklog::CloseJoinGate(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.joinGate || s.announceHolds > 0) return;
    const auto now = std::chrono::steady_clock::now();

    // The close line: the window, what the gate held, and how long each held message waited.
    uint32_t perKind[256] = {};
    std::vector<long long> delays;
    delays.reserve(s.joinStamps.size());
    for (const auto& st : s.joinStamps) {
        ++perKind[st.kind];
        delays.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(now - st.at).count());
    }
    std::vector<std::pair<uint32_t, int>> byCount;  // (count, kind id), the top eight by count
    for (int k = 0; k < 256; ++k)
        if (perKind[k] > 0) byCount.emplace_back(perKind[k], k);
    std::sort(byCount.begin(), byCount.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    char kinds[256];
    size_t at = 0;
    uint32_t other = 0;
    for (size_t i = 0; i < byCount.size(); ++i) {
        if (i < 8 && at + 16 < sizeof(kinds))
            at += static_cast<size_t>(std::snprintf(kinds + at, sizeof(kinds) - at, "%sk%d=%u",
                                                    at ? " " : "", byCount[i].second, byCount[i].first));
        else
            other += byCount[i].first;
    }
    if (at == 0) kinds[0] = '\0';
    long long median = 0, maxDelay = 0;
    if (!delays.empty()) {
        std::nth_element(delays.begin(), delays.begin() + static_cast<long>(delays.size() / 2), delays.end());
        median = delays[delays.size() / 2];
        maxDelay = *std::max_element(delays.begin(), delays.end());
    }
    UE_LOGI("[JOIN-GATE] slot %d closed: window_ms=%lld msgs=%zu bytes=%zu kinds=[%s other=%u] "
            "delay_ms median=%lld max=%lld",
            slot,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.joinOpenedAt).count()),
            s.joinQ.size(), s.joinBytes, kinds, other, median, maxDelay);

    // The splice: the held packets join the Default lane behind whatever it already holds, in send
    // order, under the lock: a swap when the lane is empty, an append otherwise. They become backlog
    // from here: the byte count moves with them, and the no-progress timer starts if the lanes were empty.
    LaneQ& dl = s.lanes[static_cast<int>(Lane::Default)];
    if (!s.joinQ.empty()) {
        if (s.totalBytes == 0) s.lastProgress = now;
        if (dl.q.empty()) {
            dl.q.swap(s.joinQ);
        } else {
            for (auto& m : s.joinQ) dl.q.push_back(std::move(m));
        }
        dl.bytes += s.joinBytes;
        s.totalBytes += s.joinBytes;
    }
    s.joinQ.clear();
    s.joinStamps.clear();
    s.joinBytes = 0;
    s.joinGate = false;
    s.joinWarned = false;
}

size_t SendBacklog::DepthBytes(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return 0;
    SlotQ& s = slots_[slot];
    std::lock_guard<std::mutex> lk(s.mu);
    return s.totalBytes;
}

}  // namespace coop::net
