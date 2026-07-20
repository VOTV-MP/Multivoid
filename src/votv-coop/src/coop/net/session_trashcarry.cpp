// coop/net/session_trashcarry.cpp -- v85 (Increment 2) host-authoritative trash-clump carry/flight
// pose-batch send/receive for Session.
//
// The byte-for-byte clone of session_worldactor.cpp for the trash-clump carry stream: the unreliable
// HOST->client batch (MsgType::TrashCarryPose). A client-grabbed pile's clump is driven by the HOST (on
// the requester's puppet); the host must ORIGINATE the pose so EVERY client -- including the grabber --
// renders the clump moving (the relay never echoes a pose to its origin; a client drives only slot 0).
// The host serializes its live batch ONCE per send (SerializeLocalTrashCarryBatch) before the per-peer
// fan-out; clients parse + newest-wins-store each datagram (StoreRemoteTrashCarryBatch) for the game
// thread to drain (trash_clump_pose_stream::TickApplyAndDrive). The per-peer PacketHeader stamp +
// SendMessageToConnection stay in session.cpp's send loop. Mutex discipline identical to the WorldActor
// path: local* under localMutex_, remote* under remoteMutex_. Reuses EntityPoseBatchHeader (a count).

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / TrashClumpPoseSnapshot / kMaxTrashCarryBatchEntries

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

void Session::SetLocalTrashCarryBatch(const std::vector<TrashClumpPoseSnapshot>& batch) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalTrashCarryBatch_ = !batch.empty();  // empty -> nothing to fan out (no clump carried)
    localTrashCarryBatch_ = batch;              // copy reuses the vector's capacity (caller keeps its scratch)
}

bool Session::TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteTrashCarryBatch_) return false;
    out = std::move(remoteTrashCarryBatch_);
    remoteTrashCarryBatch_.clear();
    hasRemoteTrashCarryBatch_ = false;  // consume-once
    return true;
}

int Session::SerializeLocalTrashCarryBatch(uint8_t* buf) {
    // Serialize ONCE per send (same body for every peer; only the per-peer header seq differs). One
    // datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*TrashClumpPoseSnapshot(32), MTU-capped
    // at kMaxTrashCarryBatchEntries. The leading PacketHeader bytes are left for the caller to stamp
    // per-peer. Only the HOST ever populates localTrashCarryBatch_, so on a client this returns 0.
    std::lock_guard<std::mutex> lk(localMutex_);
    if (!hasLocalTrashCarryBatch_ || localTrashCarryBatch_.empty()) return 0;
    size_t n = localTrashCarryBatch_.size();
    if (n > static_cast<size_t>(kMaxTrashCarryBatchEntries)) n = kMaxTrashCarryBatchEntries;  // cap (publisher already caps)
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    std::memcpy(buf + sizeof(PacketHeader) + sizeof(bh), localTrashCarryBatch_.data(),
                n * sizeof(TrashClumpPoseSnapshot));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) + n * sizeof(TrashClumpPoseSnapshot));
}

void Session::StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq) {
    // HOST->client trash-clump carry batch. The host ORIGINATES it (never relays/receives it), so this
    // lands only on clients. Parse + store the LATEST into the carry-batch slot the game thread drains
    // (trash_clump_pose_stream::TickApplyAndDrive); newest-wins via seq.
    // SECURITY (W6, docs/security/TRACKER.md) -- FIXED below; kept as the record of what was wrong.
    // This comment used to claim "per-entry float validation + the ctx-freshness gate happen at the
    // game-thread apply". Only the SECOND half was ever true (trash_clump_pose_stream.cpp's
    // IsInboundStreamCtxFresh); there was NO float validation anywhere on the path, so a wire NaN
    // rode BeginLerpToPose straight into SetActorLocation. A half-true comment is worse than a false
    // one -- spot-checking the true half confirms the whole sentence.
    // Measured while fixing: neither this store NOR the game-thread apply had any role check, so a
    // client could send this host-originated kind TO the host and drive its props. Note the sibling
    // this file was cloned from (session_worldactor.cpp) has no role gate either -- the "unlike its
    // siblings" framing in the tracker was imprecise; the finite-check asymmetry is the real one
    // (session_streams.cpp :198/:222/:260/:297/:324 all validate).
    // Both gaps are closed below: a ROLE gate, then a per-entry FINITE check.
    if (role() == Role::Host) {
        UE_LOGW("trashcarry: host received a TrashCarryPose batch -- only the host originates this "
                "kind, so this is a client-authored batch. Dropping.");
        return;
    }
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxTrashCarryBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(TrashClumpPoseSnapshot));
    if (len < need) return;  // truncated datagram
    std::vector<TrashClumpPoseSnapshot> batch(static_cast<size_t>(count));
    if (count > 0)
        std::memcpy(batch.data(),
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(TrashClumpPoseSnapshot));
    // W6: per-entry finite check, matching the five scalar stream channels in session_streams.cpp
    // (:198 ValidatePose, :222, :260, :297, :324 isfinite). Without it a NaN rode straight through
    // trash_clump_pose_stream's BeginLerpToPose into SetActorLocation. Reject the WHOLE batch rather
    // than filtering entries: a batch carrying a non-finite pose is malformed, and silently applying
    // its "good" half would leave the clump set half-updated from a sender we already distrust.
    for (const TrashClumpPoseSnapshot& e : batch) {
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) ||
            !std::isfinite(e.pitch) || !std::isfinite(e.yaw) || !std::isfinite(e.roll)) {
            UE_LOGW("trashcarry: non-finite pose in a %d-entry batch -- dropping the batch", count);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (hasRemoteTrashCarryBatch_ && static_cast<int32_t>(seq - lastRemoteTrashCarrySeq_) <= 0) return;  // stale
    remoteTrashCarryBatch_ = std::move(batch);
    lastRemoteTrashCarrySeq_ = seq;
    hasRemoteTrashCarryBatch_ = true;
}

}  // namespace coop::net
