// coop/net/outbound_streams.h -- what this peer has to send on its unreliable streams, under the
// session's local lock, and reset whole at a session's start: the samples, which the net thread's
// send round copies whole, and the host's pose batches, which it serializes in place. A held stream
// goes out every round while set, the download sim on a throttle of its own; a due sample goes out
// once. The reset kept field by field had missed six of these (the hand, desk cursor and download
// sim flags, and the three one-shot host samples), and ran unlocked, though Start can run on the
// harness's timeline thread while the game thread's release edges write the same flags.
#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <vector>

namespace coop::net {

// A stream sent every round while set; a false put is the release edge and keeps the last value.
template <class T>
struct HeldStream {
    T value{};
    bool set = false;
    void Put(bool on, const T& v) {
        set = on;
        if (on) value = v;
    }
};

// One sample the next send round takes once.
template <class T>
struct DueSample {
    T value{};
    bool due = false;
    void Put(const T& v) {
        value = v;
        due = true;
    }
};

// Everything the send round copies, trivially and whole.
struct OutboundSamples {
    HeldStream<PoseSnapshot>           pose;
    std::uint32_t                      poseStateMs = 0;  // when `pose` was sampled, for its header
    HeldStream<PropPoseSnapshot>       prop;
    HeldStream<RagdollPoseSnapshot>    ragdoll;
    HeldStream<HandPoseSnapshot>       hand;
    HeldStream<DeskCursorPoseSnapshot> deskCursor;
    // The host's own: the download sim, and the samples its lanes time.
    HeldStream<DeskSimSnapshot>        deskSim;
    DueSample<TimeSyncPayload>         clock;
    DueSample<DishPoseBody>            dish;
    DueSample<ReelPosePayload>         reel;
};

struct OutboundStreams {
    OutboundSamples samples;
    // The host's NPC and WorldActor pose batches, serialized in place; empty = nothing to send.
    std::vector<EntityPoseSnapshot>     npcBatch;
    std::vector<WorldActorPoseSnapshot> worldActorBatch;
};

}  // namespace coop::net
