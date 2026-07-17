// coop/net/session_worldactor.cpp -- v80 (B3b) WorldActor pose-batch send/receive for Session.
//
// The byte-for-byte clone of session_npc.cpp for the WorldActor (non-Character event actor) pose
// stream: the unreliable HOST->client batch (MsgType::WorldActorPose). The host serializes its live
// batch ONCE per send (SerializeLocalWorldActorBatch) before the per-peer fan-out, and clients parse
// + newest-wins-store each datagram (StoreRemoteWorldActorBatch) for the game thread to drain
// (TakeRemoteWorldActorBatch). The per-peer PacketHeader stamp + SendMessageToConnection stay in
// session.cpp's send loop. Mutex discipline identical to the NPC path: local* under localMutex_,
// remote* under remoteMutex_. The batch header is the SAME EntityPoseBatchHeader (a generic count).

#include "coop/net/session.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / WorldActorPoseSnapshot / kMaxWorldActorBatchEntries

#include "ue_wrap/core/log.h"  // [WA-TRACE] wire-hop tracing (2026-07-05 0s-frozen-pyramid hunt)

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
long long TraceNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

namespace coop::net {

void Session::SetLocalWorldActorPoseBatch(const std::vector<WorldActorPoseSnapshot>& batch) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalWorldActorBatch_ = !batch.empty();  // empty -> nothing to fan out (actors gone)
    localWorldActorBatch_ = batch;              // copy reuses the vector's capacity (caller keeps its scratch)
}

bool Session::TakeRemoteWorldActorBatch(std::vector<WorldActorPoseSnapshot>& out) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteWorldActorBatch_) return false;
    out = std::move(remoteWorldActorBatch_);
    remoteWorldActorBatch_.clear();
    hasRemoteWorldActorBatch_ = false;  // consume-once
    return true;
}

int Session::SerializeLocalWorldActorBatch(uint8_t* buf) {
    // Serialize ONCE per send (same body for every peer; only the per-peer header seq differs). One
    // datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*WorldActorPoseSnapshot(28), MTU-capped
    // at kMaxWorldActorBatchEntries. The leading PacketHeader bytes are left for the caller to stamp
    // per-peer. Only the HOST ever populates localWorldActorBatch_, so on a client this returns 0.
    std::lock_guard<std::mutex> lk(localMutex_);
    if (!hasLocalWorldActorBatch_ || localWorldActorBatch_.empty()) return 0;
    size_t n = localWorldActorBatch_.size();
    if (n > static_cast<size_t>(kMaxWorldActorBatchEntries)) n = kMaxWorldActorBatchEntries;  // cap (TickPoseStream already caps)
    // [WA-TRACE host-serialize] 1 Hz: what the net thread actually fans out this second. If
    // host-read moves but this is frozen, the game->net batch handoff is the break.
    {
        static long long s_lastMs = 0;  // net-thread only (single NetThread) -- no race
        const long long nowMs = TraceNowMs();
        if (nowMs - s_lastMs >= 1000) {
            s_lastMs = nowMs;
            const auto& f = localWorldActorBatch_[0];
            UE_LOGI("[WA-TRACE host-serialize] n=%zu first: eid=%u (%.0f,%.0f,%.0f)",
                    n, f.elementId, f.x, f.y, f.z);
        }
    }
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    std::memcpy(buf + sizeof(PacketHeader) + sizeof(bh), localWorldActorBatch_.data(),
                n * sizeof(WorldActorPoseSnapshot));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) + n * sizeof(WorldActorPoseSnapshot));
}

void Session::StoreRemoteWorldActorBatch(const void* data, int len, uint32_t seq) {
    // HOST->client WorldActor pose batch. The host ORIGINATES it (never relays/receives it), so this
    // lands only on clients. Parse + store the LATEST into the WA-batch slot the game thread drains
    // (world_actor_sync::TickClientWorldActors); newest-wins via seq. Per-entry float validation
    // happens at the game-thread apply (a NaN can't reach SetActorLocation).
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxWorldActorBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(WorldActorPoseSnapshot));
    if (len < need) return;  // truncated datagram
    std::vector<WorldActorPoseSnapshot> batch(static_cast<size_t>(count));
    if (count > 0)
        std::memcpy(batch.data(),
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(WorldActorPoseSnapshot));
    // [WA-TRACE client-store] 1 Hz: what actually arrived over the wire this second (+ how many
    // datagrams the stale-seq guard dropped). If host-serialize moves but this is frozen/absent,
    // the wire hop is the break; if stale-drops dominate, the seq guard is eating the stream.
    static long long s_lastMs = 0;   // net-thread only -- no race
    static unsigned s_staleDrops = 0;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (hasRemoteWorldActorBatch_ && static_cast<int32_t>(seq - lastRemoteWorldActorSeq_) <= 0) {
        ++s_staleDrops;
        return;  // stale
    }
    const long long nowMs = TraceNowMs();
    if (nowMs - s_lastMs >= 1000 && count > 0) {
        s_lastMs = nowMs;
        const auto& f = batch[0];
        UE_LOGI("[WA-TRACE client-store] n=%d seq=%u first: eid=%u (%.0f,%.0f,%.0f) staleDrops=%u",
                count, seq, f.elementId, f.x, f.y, f.z, s_staleDrops);
        s_staleDrops = 0;
    }
    remoteWorldActorBatch_ = std::move(batch);
    lastRemoteWorldActorSeq_ = seq;
    hasRemoteWorldActorBatch_ = true;
}

}  // namespace coop::net
