// coop/props/container_truth.h -- the host's truth of a container sent back to a client slot: what a refused client
// slice is answered with, and the bound on it. The send is container_contents_sync's own blob; this module owns the
// per-slot state that says when one may go and which refusals were already said.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>

namespace coop::props::container_truth {

// One slot's once-set of refused containers: each is said the first time; once kMaxRefusalLines are in, the next new
// one is answered Capped (said once) and the rest are Quiet. Pure; the selftest drives it.
inline constexpr size_t kMaxRefusalLines = 256;
class RefusalOnce {
public:
    enum class Say : uint8_t { Quiet, Refusal, Capped };
    Say Note(uint32_t eid);

private:
    std::set<uint32_t> said_;
    bool capSaid_ = false;
};

// Host: a client slice that is not a birth. Counted, said once per (slot, container); the author gets the host's
// truth, bounded. Game thread.
void RefuseNonBirth(uint32_t eid, uint8_t senderSlot);

// Host: send a container's live contents to one slot, forced past the unchanged-hash guard, at most one per
// kTruthEveryMs per (slot, container) and one per kTruthSlotEveryMs per slot across containers, so a forged address buys
// no more than one container blob per interval and a flood of distinct addresses no more than the slot's floor. The
// table is written only for a container that resolves. False when nothing was sent. For a refusal the sender can
// repeat; a send owed once (a put commit's) uses container_contents_sync::SendTruthToSlot. Game thread.
bool SendTruthBounded(uint32_t eid, int slot);
inline constexpr uint64_t kTruthEveryMs = 2000;
inline constexpr uint64_t kTruthSlotEveryMs = 500;
bool TruthDue(uint64_t lastSentMs, uint64_t nowMs);
bool SlotTruthDue(uint64_t lastAnyMs, uint64_t nowMs);   // lastAnyMs 0: none sent yet

// Dev (container_intent_drill): what the host took in from one client slot since the session began (or the slot's
// occupant changed): the slices that arrived, those refused as not a birth, and the truths it sent back through
// SendTruthBounded. Host; zeros for a slot that is not a client's. Game thread.
struct DevClientSlices { uint32_t received = 0, refused = 0, truths = 0; };
DevClientSlices DevClientSlicesOf(int slot);
void NoteSliceReceived(uint8_t senderSlot);

// The refusal log's window, closed on the pump tick; cheap with none open.
void Flush();

// The session's end: every slot's state and the refusal tally back to empty.
void Reset();

}  // namespace coop::props::container_truth
