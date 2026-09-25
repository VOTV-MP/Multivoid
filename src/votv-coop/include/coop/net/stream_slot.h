// coop/net/stream_slot.h -- one received stream's newest value. GNS delivers unreliable datagrams in
// any order, so the receiver keeps each stream's sequence itself and refuses a datagram at or behind
// the last one stored. MTA sends its streams UNRELIABLE_SEQUENCED and leaves that drop to RakNet, per
// connection (reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:338); GNS has no sequenced
// unreliable send, so the latch lives here, and whoever owns a group of these resets it at the owner's
// edges (coop/net/remote_streams.h). Not locked: the owner's lock guards every call.
#pragma once

#include <cstdint>
#include <utility>

namespace coop::net {

// The newest-wins test on a sender's datagram sequence, wrap-safe: an arrival counts only when it is
// ahead of the last one counted, or is the first.
class SeqLatch {
public:
    // Counts `seq` when it is ahead; false, and nothing counted, when it is not.
    bool Advance(std::uint32_t seq) {
        if (latched_ && static_cast<std::int32_t>(seq - last_) <= 0) return false;
        latched_ = true;
        last_ = seq;
        return true;
    }
    bool Latched() const { return latched_; }
    std::uint32_t Last() const { return last_; }

private:
    std::uint32_t last_ = 0;
    bool latched_ = false;
};

// One stream's newest value, and whether its reader has seen it. A stream is either read, the value
// kept for the next read, or taken, consumed once. The latch outlives a Take, so a batch arriving
// reordered behind one already taken is refused: a consumed flag that doubled as the latch let it in.
template <class T>
class StreamSlot {
public:
    // Stores the value of datagram `seq` when it is the newest; false when refused.
    bool Offer(std::uint32_t seq, const T& v) {
        T* slot = Claim(seq);
        if (slot) *slot = v;
        return slot != nullptr;
    }
    // A value built in place, a batch: the value to overwrite, or null when `seq` is refused.
    T* Claim(std::uint32_t seq) {
        if (!latch_.Advance(seq)) return nullptr;
        ++stored_;
        return &value_;
    }
    // The newest value, and whether it arrived since the last Read; false before the first.
    bool Read(T& out, bool* isNew) {
        if (!latch_.Latched()) return false;
        out = value_;
        if (isNew) *isNew = stored_ != seen_;
        seen_ = stored_;
        return true;
    }
    // Swaps the newest value out when it arrived since the last Take. `out` comes in empty, and the
    // buffer it leaves behind is the one the next Claim overwrites, so a batch whose reader keeps its
    // vector allocates on neither thread.
    bool Take(T& out) {
        if (stored_ == seen_) return false;
        using std::swap;
        swap(out, value_);
        seen_ = stored_;
        return true;
    }
    std::uint32_t LastSeq() const { return latch_.Last(); }
    bool Latched() const { return latch_.Latched(); }

private:
    T value_{};
    SeqLatch latch_;
    std::uint64_t stored_ = 0;  // values stored
    std::uint64_t seen_ = 0;    // stored_ at the last Read or Take
};

namespace stream_slot {
// The arithmetic selftest, run once per session start beside the other un-gated ones: the newest
// wins across the sequence's wrap, a read tells a fresh value from a re-read, a batch reordered behind
// one already taken is refused, a take hands its buffer back, and a reset slot takes a new sender.
bool RunSelftest();
}  // namespace stream_slot

}  // namespace coop::net
