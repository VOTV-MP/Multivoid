// coop/net/session_npc.cpp -- NPC pose-batch send/receive for Session.
//
// Owns the unreliable HOST->client NPC pose batch (MsgType::EntityPose): the host serializes
// its live batch ONCE per send (SerializeLocalNpcBatch) before the per-peer fan-out, and
// clients parse and newest-wins-store each datagram (StoreRemoteNpcBatch) for the game thread
// to drain (TakeRemoteNpcBatch). The per-peer PacketHeader stamp and SendMessageToConnection
// stay in session.cpp's send loop, which owns the per-peer conn handle and a fresh seq. Mutex
// discipline: local* under localMutex_, remote* under remoteMutex_.

#include "coop/net/session.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPose{Snapshot,BatchHeader} / kMaxNpcBatchEntries

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

void Session::SetLocalNpcPoseBatch(const std::vector<EntityPoseSnapshot>& batch) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalNpcBatch_ = !batch.empty();  // empty -> nothing to fan out (NPCs gone)
    localNpcBatch_ = batch;              // copy reuses localNpcBatch_'s capacity (caller keeps its scratch)
}

bool Session::TakeRemoteNpcBatch(std::vector<EntityPoseSnapshot>& out) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteNpcBatch_) return false;
    out = std::move(remoteNpcBatch_);
    remoteNpcBatch_.clear();
    hasRemoteNpcBatch_ = false;  // consume-once
    return true;
}

int Session::SerializeLocalNpcBatch(uint8_t* buf) {
    // Serialize ONCE per send (same body for every peer; only the per-peer header seq differs). One
    // datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*EntityPoseSnapshot(44 B each),
    // MTU-capped at kMaxNpcBatchEntries. The leading PacketHeader bytes are left for the caller to
    // stamp per-peer.
    std::lock_guard<std::mutex> lk(localMutex_);
    if (!hasLocalNpcBatch_ || localNpcBatch_.empty()) return 0;
    size_t n = localNpcBatch_.size();
    if (n > static_cast<size_t>(kMaxNpcBatchEntries)) n = kMaxNpcBatchEntries;  // cap (TickPoseStream already caps)
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    std::memcpy(buf + sizeof(PacketHeader) + sizeof(bh), localNpcBatch_.data(),
                n * sizeof(EntityPoseSnapshot));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) + n * sizeof(EntityPoseSnapshot));
}

void Session::StoreRemoteNpcBatch(const void* data, int len, uint32_t seq) {
    // HOST->client NPC pose batch. The host ORIGINATES it (never relays or receives it), so this
    // lands only on clients. Parse and store the LATEST into the npc-batch slot the game thread
    // drains (npc_mirror::TickClientNpcs); newest-wins via seq. Per-entry float validation happens
    // at the game-thread apply, so a NaN cannot reach SetActorLocation.
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxNpcBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(EntityPoseSnapshot));
    if (len < need) return;  // truncated datagram
    std::vector<EntityPoseSnapshot> batch(static_cast<size_t>(count));
    if (count > 0)
        std::memcpy(batch.data(),
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(EntityPoseSnapshot));
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (hasRemoteNpcBatch_ && static_cast<int32_t>(seq - lastRemoteNpcSeq_) <= 0) return;  // stale
    remoteNpcBatch_ = std::move(batch);
    lastRemoteNpcSeq_ = seq;
    hasRemoteNpcBatch_ = true;
}

}  // namespace coop::net
