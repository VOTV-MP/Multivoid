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
constexpr size_t kMaxRefusalLines = 256;
struct TruthSlot {
    std::unordered_map<uint32_t, uint64_t> sentMs;
    uint64_t anyMs = 0;   // 0: none sent yet
    std::set<uint32_t> refusedSaid;
    bool capSaid = false;
    DevClientSlices dev;   // the drill's counts
};
coop::roster_ledger::PerSlotState<TruthSlot> g_truth;
coop::net::RefusalTally g_refusals{"container_contents"};

}  // namespace

void RefuseNonBirth(uint32_t eid, uint8_t senderSlot) {
    g_refusals.Note(senderSlot, "not a birth", coop::net::RefusalTally::Clock::now());
    TruthSlot& t = g_truth[senderSlot];
    ++t.dev.refused;
    if (t.refusedSaid.count(eid) == 0) {
        if (t.refusedSaid.size() < kMaxRefusalLines) {
            t.refusedSaid.insert(eid);
            UE_LOGI("container_contents: refused a client slice eid=%u from slot %u (not a birth)", eid,
                    static_cast<unsigned>(senderSlot));
        } else if (!t.capSaid) {
            t.capSaid = true;
            UE_LOGI("container_contents: refusal lines capped for slot %u", static_cast<unsigned>(senderSlot));
        }
    }
    SendTruthBounded(eid, senderSlot);
}

bool TruthDue(uint64_t lastSentMs, uint64_t nowMs) { return nowMs >= lastSentMs + kTruthEveryMs; }

bool SlotTruthDue(uint64_t lastAnyMs, uint64_t nowMs) {
    return lastAnyMs == 0 || nowMs >= lastAnyMs + kTruthSlotEveryMs;
}

bool SendTruthBounded(uint32_t eid, int slot) {
    if (!ccs::IsHostRole() || slot <= 0 || slot >= coop::net::kMaxPeers) return false;
    // The table is written only for a container that resolves, so a forged address grows nothing.
    void* inv = ccs::LiveWorldInventory(eid);
    if (!inv) return false;
    TruthSlot& t = g_truth[slot];
    const uint64_t now = ccs::NowMs();
    if (!SlotTruthDue(t.anyMs, now)) return false;
    const auto it = t.sentMs.find(eid);
    if (it != t.sentMs.end() && !TruthDue(it->second, now)) return false;
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
