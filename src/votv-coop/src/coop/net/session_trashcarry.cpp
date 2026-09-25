// coop/net/session_trashcarry.cpp -- host-authoritative trash-clump carry/flight pose-batch send
// and receive for Session.
//
// The unreliable HOST->client batch (MsgType::TrashCarryPose). A trash clump the host drives or
// simulates -- carried on a client's puppet, thrown, or rolling free -- has its pose ORIGINATED by
// the host, so every client, the one who set it moving included, renders it moving (the relay never
// echoes a pose to its origin). The queues and their rules are coop/net/eid_pose_queue.h; this file
// is the datagram's framing and the trust boundary. Reuses EntityPoseBatchHeader (a count).

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / TrashClumpPoseSnapshot / kMaxTrashCarryBatchEntries

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

PoseTurn Session::TrashCarryPoseTurn(uint32_t eid, bool ahead) const {
    return trashCarryPoses_.Turn(eid, kMaxTrashCarryBatchEntries, ahead);
}

void Session::PublishTrashCarryPose(const TrashClumpPoseSnapshot& pose, bool ahead) {
    trashCarryPoses_.Publish(pose, ahead);
}

bool Session::TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out) {
    return TakeHost(&HostStreams::trashCarry, out);
}

int Session::SerializeLocalTrashCarryBatch(uint8_t* buf) {
    // One datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*TrashClumpPoseSnapshot(32),
    // capped at kMaxTrashCarryBatchEntries; the rest go out on the next send. The leading
    // PacketHeader bytes are left for the caller to stamp per-peer. Only the HOST ever publishes, so
    // on a client this returns 0.
    const int n = trashCarryPoses_.Drain(buf + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                                         kMaxTrashCarryBatchEntries);
    if (n == 0) return 0;
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) +
                            static_cast<size_t>(n) * sizeof(TrashClumpPoseSnapshot));
}

void Session::StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq) {
    // HOST->client trash-clump carry batch. The host ORIGINATES it (never relays or receives it),
    // so this lands only on clients, merged for trash_clump_pose_stream::TickApplyAndDrive.
    //
    // The two gates below are the trust boundary. The ROLE gate: nothing on the wire stops a client
    // from sending this host-originated kind TO the host, and without it neither this store nor the
    // game-thread apply asks who sent it. The FINITE check: the apply's freshness gate
    // (trash_clump_pose_stream.cpp's IsInboundStreamCtxFresh) judges staleness, not values, so
    // without it a wire NaN reaches BeginLerpToPose and then SetActorLocation.
    if (role() == Role::Host) {
        // Once per session: a client that keeps sending would otherwise write a line per datagram.
        if (!saidClientTrashCarry_.exchange(true, std::memory_order_relaxed)) {
            UE_LOGW("trashcarry: host received a TrashCarryPose batch -- only the host originates this "
                    "kind, so this is a client-authored batch. Dropping (said once).");
        }
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
    // A fixed array, since the count is capped by the format: no allocation per datagram.
    TrashClumpPoseSnapshot batch[kMaxTrashCarryBatchEntries];
    if (count > 0)
        std::memcpy(batch,
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(TrashClumpPoseSnapshot));
    // Per-entry finite check, matching the five scalar stream channels in session_streams.cpp.
    // Reject the WHOLE batch rather than filtering entries: a batch carrying a non-finite pose is
    // malformed, and silently applying its "good" half would leave the clump set half-updated from
    // a sender we already distrust.
    for (int i = 0; i < count; ++i) {
        const TrashClumpPoseSnapshot& e = batch[i];
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) ||
            !std::isfinite(e.pitch) || !std::isfinite(e.yaw) || !std::isfinite(e.roll)) {
            UE_LOGW("trashcarry: non-finite pose in a %d-entry batch -- dropping the batch", count);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(remoteMutex_);
    host_.trashCarry.Merge(batch, count, seq);
}

}  // namespace coop::net
