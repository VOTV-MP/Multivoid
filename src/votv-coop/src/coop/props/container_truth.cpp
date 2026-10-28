// coop/props/container_truth.cpp -- see coop/props/container_truth.h.

#include "coop/props/container_truth.h"
#include "coop/props/container_contents_internal.h"

#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/core/log.h"

#include <set>
#include <unordered_map>

namespace coop::props::container_truth {
namespace {

namespace ccs = coop::props::container_contents_sync::internal;

// Host, per client slot (cleared with the occupant and at the session's end): when the host's truth last went to the
// slot, per container and across them, and which containers' refused client slices were already said.
struct TruthSlot {
    std::unordered_map<uint32_t, uint64_t> sentMs;
    uint64_t anyMs = 0;   // 0: none sent yet
    RefusalOnce refused;
    DevClientSlices dev;   // the drill's counts
};
coop::roster_ledger::PerSlotState<TruthSlot> g_truth;
coop::net::RefusalTally g_refusals{"container_contents"};

}  // namespace

RefusalOnce::Say RefusalOnce::Note(uint32_t eid) {
    if (said_.count(eid) != 0) return Say::Quiet;
    if (said_.size() < kMaxRefusalLines) {
        said_.insert(eid);
        return Say::Refusal;
    }
    if (capSaid_) return Say::Quiet;
    capSaid_ = true;
    return Say::Capped;
}

// The author of a refused slice is sent the host's truth: MTA's sync-back of a cancelled client change
// (CGame.cpp:2777-2795).
void RefuseNonBirth(uint32_t eid, uint8_t senderSlot) {
    g_refusals.Note(senderSlot, "not a birth", coop::net::RefusalTally::Clock::now());
    TruthSlot& t = g_truth[senderSlot];
    ++t.dev.refused;
    switch (t.refused.Note(eid)) {
        case RefusalOnce::Say::Refusal:
            UE_LOGI("container_contents: refused a client slice eid=%u from slot %u (not a birth)", eid,
                    static_cast<unsigned>(senderSlot));
            break;
        case RefusalOnce::Say::Capped:
            UE_LOGI("container_contents: refusal lines capped for slot %u", static_cast<unsigned>(senderSlot));
            break;
        case RefusalOnce::Say::Quiet: break;
    }
    SendTruthBounded(eid, senderSlot);
}

bool TruthDue(uint64_t lastSentMs, uint64_t nowMs) { return nowMs >= lastSentMs + kTruthEveryMs; }

bool SlotTruthDue(uint64_t lastAnyMs, uint64_t nowMs) {
    return lastAnyMs == 0 || nowMs >= lastAnyMs + kTruthSlotEveryMs;
}

// Divergence from MTA's unthrottled sync-back (CGame.cpp:2777-2795): its unit is one value, ours a whole container
// blob, so the truth is bounded. A container that does not resolve gets no answer where MTA sends
// REMOVE_ELEMENT_DATA (:2790-2794); the entity lane replicates its destroy.
bool SendTruthBounded(uint32_t eid, int slot) {
    if (!ccs::IsHostRole() || slot <= 0 || slot >= coop::net::kMaxPeers) return false;
    // The bounds are read first, so a flood the slot's floor already refuses pays no container resolve. The table is
    // written only for a container that resolves, so a forged address grows nothing.
    TruthSlot& t = g_truth[slot];
    const uint64_t now = ccs::NowMs();
    if (!SlotTruthDue(t.anyMs, now)) return false;
    const auto it = t.sentMs.find(eid);
    if (it != t.sentMs.end() && !TruthDue(it->second, now)) return false;
    void* inv = ccs::LiveWorldInventory(eid);
    if (!inv) return false;
    t.sentMs[eid] = now;
    t.anyMs = now;
    ++t.dev.truths;
    return ccs::SendContainerTo(eid, inv, slot);
}

DevClientSlices DevClientSlicesOf(int slot) {
    return slot > 0 && slot < coop::net::kMaxPeers ? g_truth[slot].dev : DevClientSlices{};
}

void NoteSliceReceived(uint8_t senderSlot) { ++g_truth[senderSlot].dev.received; }

void Flush() { g_refusals.Flush(coop::net::RefusalTally::Clock::now()); }

void Reset() {
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) g_truth[slot] = TruthSlot{};
    g_refusals.Clear();
}

}  // namespace coop::props::container_truth
