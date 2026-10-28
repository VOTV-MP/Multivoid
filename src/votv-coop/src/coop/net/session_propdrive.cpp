// coop/net/session_propdrive.cpp -- the driven-prop pose batch (the host's, and a syncing client's to the
// host), send and receive, for Session.
//
// The session_trashcarry.cpp shape. The LOCAL side is a QUEUE, not a snapshot: the publisher sends
// a prop's pose only when it moved since the last one it counted as sent, so the newest pose of
// EVERY prop has to go out on the next send -- a snapshot would drop the other props' poses
// whenever one prop published later in the tick. The queues and their rules are
// coop/net/eid_pose_lane.h; this file is the datagram's framing and the trust boundary.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / PropPoseSnapshot / kMaxPropDriveBatchEntries
#include "coop/props/prop_drive_stream.h"  // dev::NoteMerged: the rows that reached the store, for the syncer drill

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

PoseTurn Session::PropDrivePoseTurn(uint32_t eid) const {
    return propDrivePoses_.Turn(eid, kMaxPropDriveBatchEntries, /*ahead=*/false);
}

void Session::PublishPropDrivePose(const PropPoseSnapshot& pose) {
    propDrivePoses_.Publish(pose, /*ahead=*/false);
}

bool Session::TakeRemotePropDriveBatch(std::vector<PropPoseSnapshot>& out) {
    return TakeHost(&HostStreams::propDrive, out);
}

uint32_t Session::TakeRemotePropDriveBatches(std::vector<PropPoseSnapshot> (&out)[kMaxPeers]) {
    return TakeOrigins(&OriginStreams::propDrive, out);
}

int Session::SerializeLocalPropDriveBatch(uint8_t* buf) {
    // One datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*PropPoseSnapshot(64), capped at
    // kMaxPropDriveBatchEntries; the rest go out on the next send. The leading PacketHeader bytes
    // are left for the caller to stamp per-peer. The host publishes its driven rows and a client the
    // rows of a prop it syncs; the net thread sends a client's to the host connection alone.
    const int n = propDrivePoses_.Drain(buf + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                                        kMaxPropDriveBatchEntries);
    if (n == 0) return 0;
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) +
                            static_cast<size_t>(n) * sizeof(PropPoseSnapshot));
}

void Session::StoreRemotePropDriveBatch(int originSlot, const void* data, int len, uint32_t seq) {
    // A driven-prop batch: the host's to a client, or a syncing client's to the host. The host never
    // relays one; a client's rows are merged per origin slot and judged on the game thread
    // (prop_syncer_host), so no authority test runs here and the FINITE check is the trust boundary,
    // for the reasons session_trashcarry.cpp states.
    const bool onHost = role() == Role::Host;
    if (onHost && (originSlot < 1 || originSlot >= kMaxPeers)) return;
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxPropDriveBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(PropPoseSnapshot));
    if (len < need) return;  // truncated datagram
    // A fixed array, since the count is capped by the format: no allocation per datagram.
    PropPoseSnapshot batch[kMaxPropDriveBatchEntries];
    if (count > 0)
        std::memcpy(batch,
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(PropPoseSnapshot));
    // Strict on format: a batch carrying a non-finite pose, an over-long key or an eid of zero is
    // refused whole rather than filtered; on the host a client's rows also meet the bounds a client's
    // held-prop pose meets (PropPoseInBounds), since a client's rows reach the host's copy of the prop.
    for (int i = 0; i < count; ++i) {
        const PropPoseSnapshot& e = batch[i];
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) ||
            !std::isfinite(e.pitch) || !std::isfinite(e.yaw) || !std::isfinite(e.roll) ||
            e.key.len > sizeof(e.key.data) || e.elementId == 0u || (onHost && !PropPoseInBounds(e))) {
            static uint32_t sMalformed = 0;
            if (++sMalformed <= 3 || (sMalformed % 100) == 0)
                UE_LOGW("propdrive: malformed entry in a %d-entry batch -- dropping the batch (#%u)",
                        count, sMalformed);
            return;
        }
    }
    for (int i = 0; i < count; ++i) coop::prop_drive_stream::dev::NoteMerged(batch[i].elementId);   // [dev] counted for the drill
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (onHost) origin_[originSlot].propDrive.Merge(batch, count, seq);
    else        host_.propDrive.Merge(batch, count, seq);
}

}  // namespace coop::net
