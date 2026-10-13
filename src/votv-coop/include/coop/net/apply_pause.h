// coop/net/apply_pause.h -- which receive lanes hold a session's receive loop paused. A lane whose apply
// park reaches its flood bound pauses the loop (pause, not drop: TRACKER W10) and lowers its pause once
// the park has drained. The set is keyed by the ReliableKind the lane parks, the net layer's own
// vocabulary, so a lane that parks names its kind and needs no entry here.
//
// Idempotent per kind: a lane that raises twice holds one pause, and a lowering of a pause never raised
// changes nothing. The Session resets this set when it starts, so a raise whose lower never came cannot
// hold a later session paused. Lock-free: the lanes set it on the game thread, the receive loop reads it
// on the net thread.
#pragma once

#include "coop/net/protocol.h"  // ReliableKind

#include <atomic>
#include <cstdint>

namespace coop::net {

static_assert(sizeof(ReliableKind) == 1, "ApplyPauseSet indexes 256 kinds by the kind's byte");

class ApplyPauseSet {
public:
    // Raises or lowers `kind`'s pause; true when this call changed it. A raise is counted by Raises().
    bool Set(ReliableKind kind, bool on) {
        const unsigned k = static_cast<std::uint8_t>(kind);
        const std::uint64_t bit = 1ull << (k & 63u);
        std::atomic<std::uint64_t>& word = words_[k >> 6];
        if (on) {
            if (word.fetch_or(bit, std::memory_order_acq_rel) & bit) return false;
            raises_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return (word.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
    }
    bool Has(ReliableKind kind) const {
        const unsigned k = static_cast<std::uint8_t>(kind);
        return (words_[k >> 6].load(std::memory_order_acquire) >> (k & 63u)) & 1u;
    }
    bool Any() const {
        for (const auto& w : words_)
            if (w.load(std::memory_order_acquire) != 0) return true;
        return false;
    }
    // Every raise this process has made, never reset: a witness read against a baseline, so a pause
    // that rose and fell between two reads is still seen.
    std::uint64_t Raises() const { return raises_.load(std::memory_order_relaxed); }
    // Lowers every pause; the raise count stays.
    void Reset() {
        for (auto& w : words_) w.store(0, std::memory_order_release);
    }

private:
    std::atomic<std::uint64_t> words_[4]{};
    std::atomic<std::uint64_t> raises_{0};
};

namespace apply_pause {
// The arithmetic selftest, run once per session start beside the other un-gated ones: a raise and a
// lower each count once however often they are repeated, a lower of a pause never raised changes
// nothing, kinds at every word's edge stay apart, Reset lowers them all and keeps the raise count.
bool RunSelftest();
}  // namespace apply_pause

}  // namespace coop::net
