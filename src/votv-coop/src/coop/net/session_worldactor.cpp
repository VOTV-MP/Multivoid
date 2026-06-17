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

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

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
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (hasRemoteWorldActorBatch_ && static_cast<int32_t>(seq - lastRemoteWorldActorSeq_) <= 0) return;  // stale
    remoteWorldActorBatch_ = std::move(batch);
    lastRemoteWorldActorSeq_ = seq;
    hasRemoteWorldActorBatch_ = true;
}

}  // namespace coop::net
