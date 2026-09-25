// coop/net/remote_streams.h -- what a session keeps of the streams it receives, grouped by the owner
// whose edge resets them. A group is reset whole, by value, so a stream added to it is reset with it:
// the reset kept member by member missed two (the host's single streams across a restart, the desk
// cursor). Every group lives under the session's remote-state lock.
#pragma once

#include "coop/net/eid_pose_lane.h"   // EidPoseMerge
#include "coop/net/protocol.h"
#include "coop/net/stream_slot.h"

#include <vector>

namespace coop::net {

// One origin's streams, the five the host relays, kept per origin slot. Reset when the slot's
// connection closes, at a session's stop, and on a client at its roster's edges for a relayed slot:
// the occupancy that left, and another occupancy named (coop/net/origin_context.h). A client's
// relayed slots have no connection of their own: they reset, every slot, when its host link closes.
struct OriginStreams {
    StreamSlot<PoseSnapshot>           pose;
    StreamSlot<PropPoseSnapshot>       prop;
    StreamSlot<RagdollPoseSnapshot>    ragdoll;
    StreamSlot<HandPoseSnapshot>       hand;
    StreamSlot<DeskCursorPoseSnapshot> deskCursor;

    void Reset() { *this = OriginStreams{}; }
};

// The host's own streams, on a client, reset with the host link and at a session's stop: their
// sequence is the host process's counter, so a restarted host's would be judged against the last
// one's. The batches are taken, consumed once; the rest are read.
struct HostStreams {
    StreamSlot<TimeSyncPayload>                     clock;
    StreamSlot<DeskSimSnapshot>                     deskSim;
    StreamSlot<DishPoseBody>                        dish;
    StreamSlot<ReelPosePayload>                     reel;
    StreamSlot<std::vector<EntityPoseSnapshot>>     npcBatch;
    StreamSlot<std::vector<WorldActorPoseSnapshot>> worldActorBatch;
    EidPoseMerge<TrashClumpPoseSnapshot>            trashCarry;
    EidPoseMerge<PropPoseSnapshot>                  propDrive;

    void Reset() { *this = HostStreams{}; }
};

}  // namespace coop::net
