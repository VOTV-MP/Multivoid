// coop/net/eid_pose_queue.h -- the two halves of a host-originated pose lane keyed by element id,
// shared by the trash clumps' carry and roll and by the driven props.
//
// The LOCAL side is a queue merged by id: a newer pose of an element replaces its waiting one in
// place, and each send drains a datagram's worth from the front, so every element goes out in turn
// however many there are. A publisher asks for an element's turn before it reads its pose
// (PoseTurn): a waiting pose the next send takes is read again, an element with nothing waiting joins
// while the next send has room, and anything else waits rather than pay a read the next one would
// overwrite. A publisher with more elements than a datagram holds starts each tick where it left
// off, and an element published ahead -- a clump a player moved -- goes first whatever the room, so
// a carry keeps its rate whatever a broom has set rolling. The REMOTE side merges by id too, newest
// per element, so two datagrams between two game-thread drains lose none. The queue keeps its own
// mutex; the merge is one of the host's streams a client keeps, under the session's remote-state lock.

#pragma once

#include "coop/net/protocol.h"     // TrashClumpPoseSnapshot / PropPoseSnapshot
#include "coop/net/stream_slot.h"  // SeqLatch

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

// What a publisher does with an element this tick: nothing, read its waiting pose again, or read a
// pose that joins the queue.
enum class PoseTurn : uint8_t { Wait, Refresh, Join };

inline uint32_t PoseEid(const TrashClumpPoseSnapshot& p) { return p.eid; }
inline uint32_t PoseEid(const PropPoseSnapshot& p) { return p.elementId; }

template <class Pose>
class EidPoseQueue {
public:
    // Game thread: `pose` for the next send, replacing a waiting pose of the same element where it
    // stands. `ahead` places a newly waiting element before every one published without it.
    void Publish(const Pose& pose, bool ahead) {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t id = PoseEid(pose);
        for (Pose& q : local_) {
            if (PoseEid(q) == id) { q = pose; return; }
        }
        if (ahead) {
            local_.insert(local_.begin() + static_cast<std::ptrdiff_t>(ahead_), pose);
            ++ahead_;
        } else {
            local_.push_back(pose);
        }
    }

    // Game thread: element `id`'s turn, `perSend` being what one send takes and `ahead` how the element
    // is published.
    PoseTurn Turn(uint32_t id, int perSend, bool ahead) const {
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t room = static_cast<size_t>(perSend);
        for (size_t i = 0; i < local_.size(); ++i) {
            if (PoseEid(local_[i]) == id) return i < room ? PoseTurn::Refresh : PoseTurn::Wait;
        }
        return (ahead || local_.size() < room) ? PoseTurn::Join : PoseTurn::Wait;
    }

    // Net thread: move up to `max` poses from the front into `dst` (room for `max`); the count moved.
    int Drain(uint8_t* dst, int max) {
        std::lock_guard<std::mutex> lk(mutex_);
        size_t n = local_.size();
        if (max < 0) max = 0;
        if (n > static_cast<size_t>(max)) n = static_cast<size_t>(max);
        if (n == 0) return 0;
        std::memcpy(dst, local_.data(), n * sizeof(Pose));
        local_.erase(local_.begin(), local_.begin() + static_cast<std::ptrdiff_t>(n));
        ahead_ = ahead_ > n ? ahead_ - n : 0;
        return static_cast<int>(n);
    }

    // Back to empty. Any thread with the session stopped.
    void Reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        local_.clear();
        ahead_ = 0;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Pose>  local_;
    size_t             ahead_ = 0;   // local_[0, ahead_) were published ahead
};

// The receiving half, not locked: its owner's lock guards every call, and a reset is its owner's,
// by value.
template <class Pose>
class EidPoseMerge {
public:
    // Net thread: merge the already-validated poses of datagram `seq`; a datagram no newer than the
    // last one merged is dropped.
    void Merge(const Pose* poses, int count, uint32_t seq) {
        if (!latch_.Advance(seq)) return;
        for (int i = 0; i < count; ++i) {
            bool merged = false;
            for (Pose& have : remote_) {
                if (PoseEid(have) == PoseEid(poses[i])) { have = poses[i]; merged = true; break; }
            }
            if (!merged) remote_.push_back(poses[i]);
        }
    }

    // Game thread: swap out every pose merged since the last take. `out` comes in empty and keeps
    // its capacity, and the buffer it leaves behind keeps the one the net thread grew, so the
    // steady state allocates on neither thread.
    bool Take(std::vector<Pose>& out) {
        if (remote_.empty()) return false;
        out.swap(remote_);
        return true;
    }

private:
    std::vector<Pose> remote_;
    SeqLatch          latch_;
};

}  // namespace coop::net
