// coop/net/local_streams.h -- what this peer has to send on its unreliable streams: one struct the
// game thread writes and the net thread's send round copies whole, both under the session's local
// lock, and a session's start resets whole under that lock. A held stream goes out every round while
// set; a due sample goes out once. The reset kept field by field had missed the one-shot samples, and
// ran unlocked, though Start can run on the harness's timeline thread while the game thread's release
// edges write the same flags.
#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

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

struct LocalStreams {
    HeldStream<PoseSnapshot>           pose;
    std::uint32_t                      poseStateMs = 0;  // when `pose` was sampled, for its header
    HeldStream<PropPoseSnapshot>       prop;
    HeldStream<RagdollPoseSnapshot>    ragdoll;
    HeldStream<HandPoseSnapshot>       hand;
    HeldStream<DeskCursorPoseSnapshot> deskCursor;
    // The host's own: the download sim on its ~100 ms throttle, and the samples its lanes time.
    HeldStream<DeskSimSnapshot>        deskSim;
    DueSample<TimeSyncPayload>         clock;
    DueSample<DishPoseBody>            dish;
    DueSample<ReelPosePayload>         reel;
};

}  // namespace coop::net
