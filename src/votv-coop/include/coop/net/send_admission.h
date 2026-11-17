// coop/net/send_admission.h -- who may put reliable bytes into a connection's send buffer.
//
// The transport admits a message only while a connection's pending bytes plus that message fit the
// send buffer, and that sum is connection-wide: a bulk stream at the brim refuses the unreliable
// pose and voice datagrams beside it, and the probe that times the path they ride. This rule bounds
// OURS -- every reliable send but the probe pair and the join beacon stops short of a reserve --
// and a SECOND bound, in TIME, holds the unqueued path to a budget of this link's own measured
// delivery, because bytes inside the transport can be neither retracted nor reordered. Neither
// guarantees room: a NACKed segment is re-queued into pending with no admission check, so on a
// lossy link it passes the buffer size from outside while we add nothing. A send this rule refuses
// is never lost -- it goes to the backlog, or it is the save pump's pacing signal.
//
// The occupancy compared is an estimate, re-anchored on the transport's own pending total at each
// link sample: high when a drain went unseen, which is the safe direction, and low by a bounded
// amount, both corrected at the next anchor. Both bounds' measurements are in docs/send-path.md.

#pragma once

#include "coop/player/slot_count.h"  // kMaxPeers

#include <atomic>
#include <cstdint>

namespace coop::net {

// Threading: five atomics per slot, written at the send choke points from whichever thread reached
// them and re-anchored on the net thread; a test costs two acquire loads for the occupancy and, for
// the paced bound, two relaxed ones beside them. No lock on any path.
class SendAdmission {
public:
    // Headroom kept free for the unreliable streams and the probe pair. The worst realistic
    // concurrent unreliable load is ~0.4 KB a peer per 16 ms tick (voice frames plus poses), under
    // 6 KB with fifteen, so 64 KB is about ten times that. A buffer too small to give that away yields a quarter
    // of itself instead, which keeps the rule from starving a drill-pinned buffer outright.
    static constexpr int kReserve = 64 * 1024;
    static constexpr int kMaxReserveFraction = 4;

    // How much of a queue the unqueued path may stand behind, in milliseconds of this link's own
    // MEASURED DELIVERY (`SendRateControl::ServedBps`, not the rate the transport is paced at --
    // see there for what that cost). Two seconds is about four times the worst frame the host's
    // game thread takes between pump passes, so the transport never runs dry waiting for the next
    // one, and it is 63x under what the byte rule alone allowed on a contended uplink. It bounds
    // what is COMMITTED against the delivery measured at that moment and cannot retract, so a rate
    // that collapses inside a second re-prices bytes already handed over and a contended link still
    // shows a tail. `[dev] bulk_queue_cap_ms` overrides it; 0 disables the bound, which is the
    // before-picture and the only way to stage one on a single binary.
    //
    // MTA's shape, `CLatentSendQueue::DoPulse`
    // (reference/mtasa-blue/Shared/mods/deathmatch/logic/CLatentSendQueue.cpp:46-69), meters a bulk
    // transfer by rate*time with a carried remainder. DELIBERATE DIVERGENCE: it has to integrate
    // because RakNet publishes no queue depth, while the transport here does, so we bound the DEPTH
    // directly -- the same discipline with nothing to integrate and so nothing to drift.
    static constexpr int64_t kQueueCapMs = 2000;

    // Session::Start: a new session opens every slot empty.
    void Reset();

    // Slot teardown. Any thread: the pair is re-anchored from the transport's own total at the
    // next sample, so a reader that catches the reset half-done costs one wrong verdict on a slot
    // that is going away -- and the transport's own admission check stands behind every send in
    // any case.
    void FreeSlot(int slot);

    // The per-connection send buffer this session configures (the drill knob, or the default).
    // Written when a peer's lanes are configured, read by every test.
    void SetSendBufBytes(int bytes);

    // The time bound's budget, resolved once at Session::Start from `[dev] bulk_queue_cap_ms`.
    // 0 leaves the byte rule alone.
    void SetQueueCapMs(int64_t ms);

    // What this slot's link DELIVERS per second (`SendRateControl::ServedBps`), published from the
    // same link sample that re-anchors the occupancy. Measured delivery, not the rate the transport
    // is paced at: with the law on it hands back our own last written rate through its equal-bounds
    // branch, which on a policed link stood 1.29x above what that link carried -- so a budget built
    // on it admitted 2.57 s while its name said 2 s, and the metric checking it divided by the same
    // inflated number. Net thread. A non-positive reading is not stored, so the last measured rate
    // stands until the next sample; a zero would switch the bound OFF for this slot, not tighten it.
    void SetDrainRate(int slot, int64_t bytesPerSec);

    // Re-anchor a slot on the transport's pending total (reliable plus unreliable, the sum the
    // transport itself compares against the buffer size). `readPending` returns that total, or a
    // negative number when the status could not be read, in which case the previous anchor stands.
    // The call order inside is the point of the callback: our own handed count is latched BEFORE
    // the status read, so a send that slips between the two is counted twice -- reading high, the
    // safe direction -- instead of vanishing from the estimate. Net thread.
    template <class ReadPendingFn>
    void Anchor(int slot, ReadPendingFn&& readPending) {
        if (slot < 0 || slot >= kSlots) return;
        const int64_t mark = slots_[slot].handed.load(std::memory_order_acquire);
        const int pending = readPending();
        if (pending < 0) return;
        slots_[slot].floor.store(static_cast<int64_t>(pending) - mark, std::memory_order_release);
    }

    // Bytes the connection accepted, reliable or unreliable. Any thread, at the send choke points.
    void NoteHanded(int slot, int bytes);

    // May `len` more non-urgent bytes enter this slot's buffer? Any thread. A pure predicate: the
    // backlog drain asks it once per queued head and its held-back state is already reported by
    // the episode lines, so only the send paths count a refusal.
    bool Admits(int slot, int len) const;

    // Both bounds, for the send path that has no queue behind it. Tests the queue as it STANDS
    // rather than the sum with `len`: a slot whose queue has drained under the budget always takes
    // one more message, whatever its size, so no message can be too large to ever depart and the
    // depth settles at the budget plus one message rather than deadlocking below it. Any thread.
    bool AdmitsPacedStream(int slot, int len) const;

    // One reliable send this rule held back, counted at the send paths for the diagnostics.
    void NoteRefused(int slot);
    // The count since the last call, for the per-second line.
    uint32_t TakeRefusals(int slot);

    // One send the TIME bound held back, counted apart from the byte rule's refusals. Folding the
    // two would rewrite the meaning of a number the headroom rule's own measurement rests on --
    // and they say different things: a refusal is a buffer with no room, a hold is a producer
    // being paced by a buffer that has plenty.
    void NotePacedHold(int slot);
    uint32_t TakePacedHolds(int slot);

private:
    static constexpr int kSlots = static_cast<int>(coop::players::kMaxPeers);

    struct Slot {
        // The estimate is floor + handed. `floor` is the transport's pending total at the last
        // anchor minus the handed count latched just before it, so adding today's handed count
        // back gives that total plus everything handed since.
        std::atomic<int64_t>  floor{0};
        std::atomic<int64_t>  handed{0};
        // The delivery measured on this connection, and the holds the time bound built from it.
        // Opens at zero, which reads as "not measured yet" and leaves the time bound out of the
        // way until the first link sample publishes one. Kept beside the two int64s above, ahead
        // of the two counters, so the struct packs to 32 bytes with no padding hole.
        std::atomic<int64_t>  drainBps{0};
        std::atomic<uint32_t> refusals{0};
        std::atomic<uint32_t> pacedHolds{0};
    };

    // Does `len` fit the buffer above the reserve, given an occupancy already read?
    bool FitsBuffer_(int64_t estimate, int len) const;

    // The occupancy estimate, read once. Both bounds go through this, so a paced test cannot pair
    // one bound's reading of the pair with the other's.
    int64_t Estimate_(const Slot& s) const {
        // Acquire on both, so a fresh floor cannot be paired with a handed count from before the
        // anchor that produced it -- the one pairing that would read low by more than the header
        // term.
        return s.floor.load(std::memory_order_acquire) + s.handed.load(std::memory_order_acquire);
    }

    Slot slots_[kSlots];
    // One buffer size for every connection: the session configures them all alike.
    std::atomic<int> sendBufBytes_{512 * 1024};
    // The time budget, one for the session for the same reason.
    std::atomic<int64_t> queueCapMs_{kQueueCapMs};
};

}  // namespace coop::net
