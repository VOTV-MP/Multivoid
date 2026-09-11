// coop/net/session_propdrive.cpp -- the host-originated driven-prop pose batch, send and receive,
// for Session.
//
// The session_trashcarry.cpp shape with one difference on each side. The LOCAL side is a QUEUE,
// not a snapshot: the publisher sends a prop's pose only when it moved since the last one it
// counted as sent, so the newest pose of EVERY prop has to go out on the next send -- a snapshot
// would drop the other props' poses whenever one prop published later in the tick.
// PublishPropDrivePose merges by eid (a newer pose of the same prop supersedes the older, which
// is the one kind of published pose that never goes out) and SerializeLocalPropDriveBatch drains
// from the front, a datagram's worth per send. The REMOTE side merges by eid too, newest per
// prop, so two datagrams landing between one game-thread drain and the next lose no prop. Mutex
// discipline is the sibling's: local* under localMutex_, remote* under remoteMutex_.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / PropPoseSnapshot / kMaxPropDriveBatchEntries

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

void Session::PublishPropDrivePose(const PropPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    for (PropPoseSnapshot& q : localPropDriveQueue_) {
        if (q.elementId == pose.elementId) { q = pose; return; }   // newest per prop, queue position kept
    }
    localPropDriveQueue_.push_back(pose);
}

bool Session::TakeRemotePropDriveBatch(std::vector<PropPoseSnapshot>& out) {
    // A swap, not a move: the caller's buffer comes in empty and keeps its capacity across ticks,
    // and the one it leaves behind keeps the capacity the net thread grew, so once a drag has
    // sized them neither thread allocates per datagram or per tick. The three sibling batch lanes
    // still hand a fresh vector across; this lane's merge-by-eid receive has no fixed cap to size
    // an array by, so retained capacity is its shape.
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (remotePropDriveBatch_.empty()) return false;
    out.swap(remotePropDriveBatch_);
    return true;
}

int Session::SerializeLocalPropDriveBatch(uint8_t* buf) {
    // One datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*PropPoseSnapshot(64), capped at
    // kMaxPropDriveBatchEntries; the serialized entries leave the queue and the rest go out on the
    // next send. The leading PacketHeader bytes are left for the caller to stamp per-peer. Only the
    // HOST ever publishes, so on a client this returns 0.
    std::lock_guard<std::mutex> lk(localMutex_);
    if (localPropDriveQueue_.empty()) return 0;
    size_t n = localPropDriveQueue_.size();
    if (n > static_cast<size_t>(kMaxPropDriveBatchEntries)) n = kMaxPropDriveBatchEntries;
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    std::memcpy(buf + sizeof(PacketHeader) + sizeof(bh), localPropDriveQueue_.data(),
                n * sizeof(PropPoseSnapshot));
    localPropDriveQueue_.erase(localPropDriveQueue_.begin(),
                               localPropDriveQueue_.begin() + static_cast<ptrdiff_t>(n));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) + n * sizeof(PropPoseSnapshot));
}

void Session::StoreRemotePropDriveBatch(const void* data, int len, uint32_t seq) {
    // HOST->client driven-prop batch. The host originates it and never relays or receives it, so
    // this lands only on clients; the ROLE gate and the FINITE check are the trust boundary, for
    // the reasons session_trashcarry.cpp states.
    if (role() == Role::Host) {
        // Once per session: a client that keeps sending would otherwise write a line per datagram.
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("propdrive: host received a PropDrivePose batch -- only the host originates this "
                    "kind, so this is a client-authored batch. Dropping (said once).");
        }
        return;
    }
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxPropDriveBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(PropPoseSnapshot));
    if (len < need) return;  // truncated datagram
    std::vector<PropPoseSnapshot> batch(static_cast<size_t>(count));
    if (count > 0)
        std::memcpy(batch.data(),
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(PropPoseSnapshot));
    // Strict on format: a batch carrying a non-finite pose, an over-long key or an eid of zero is
    // refused whole rather than filtered.
    for (const PropPoseSnapshot& e : batch) {
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) ||
            !std::isfinite(e.pitch) || !std::isfinite(e.yaw) || !std::isfinite(e.roll) ||
            e.key.len > sizeof(e.key.data) || e.elementId == 0u) {
            static uint32_t sMalformed = 0;
            if (++sMalformed <= 3 || (sMalformed % 100) == 0)
                UE_LOGW("propdrive: malformed entry in a %d-entry batch -- dropping the batch (#%u)",
                        count, sMalformed);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (lastRemotePropDriveSeq_ != 0 && static_cast<int32_t>(seq - lastRemotePropDriveSeq_) <= 0) return;  // stale
    lastRemotePropDriveSeq_ = seq;
    for (const PropPoseSnapshot& e : batch) {
        bool merged = false;
        for (PropPoseSnapshot& have : remotePropDriveBatch_) {
            if (have.elementId == e.elementId) { have = e; merged = true; break; }
        }
        if (!merged) remotePropDriveBatch_.push_back(e);
    }
}

void Session::ResetPoseBatches() {
    {
        std::lock_guard<std::mutex> lk(localMutex_);
        localNpcBatch_.clear();        hasLocalNpcBatch_ = false;
        localWorldActorBatch_.clear(); hasLocalWorldActorBatch_ = false;
        localTrashCarryBatch_.clear(); hasLocalTrashCarryBatch_ = false;
        localPropDriveQueue_.clear();
    }
    std::lock_guard<std::mutex> lk(remoteMutex_);
    remoteNpcBatch_.clear();        hasRemoteNpcBatch_ = false;        lastRemoteNpcSeq_ = 0;
    remoteWorldActorBatch_.clear(); hasRemoteWorldActorBatch_ = false; lastRemoteWorldActorSeq_ = 0;
    remoteTrashCarryBatch_.clear(); hasRemoteTrashCarryBatch_ = false; lastRemoteTrashCarrySeq_ = 0;
    remotePropDriveBatch_.clear();  lastRemotePropDriveSeq_ = 0;
}

}  // namespace coop::net
