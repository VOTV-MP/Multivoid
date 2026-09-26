// coop/interactables/meadow_db_join.cpp -- the meadow lane's join seed (see
// coop/interactables/meadow_db_sync.h): at a joiner's save request, a snapshot of the database's
// multiset; at its world-ready, the lines that bring the copy in its save to the host's database, then
// the order. The waiting lines it masks and the lane's sends come through meadow_db_internal.h.

#include "coop/interactables/meadow_db_sync.h"

#include "coop/interactables/meadow_db_hash.h"
#include "coop/interactables/meadow_db_internal.h"
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <map>
#include <vector>

namespace coop::meadow_db_sync {
namespace {

namespace I  = internal;
namespace MH = coop::meadow_db_hash;
namespace MS = ue_wrap::meadow_store;
namespace SD = ue_wrap::signal_dynamic;

// Per-slot join-seed snapshots.
struct SlotSnap {
    bool valid = false;
    uint64_t opAt = 0;
    std::map<uint64_t, int32_t> counts;
};
SlotSnap g_snap[coop::net::kMaxPeers];
bool g_seededOnce[coop::net::kMaxPeers] = {};  // the connect replay re-fires on every world-change re-announce; only the first missing snapshot warns

}  // namespace

void CaptureJoinSnapshot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    if (!I::IsHost()) return;
    SlotSnap& snap = g_snap[peerSlot];
    snap = SlotSnap{};
    if (!MS::EnsureResolved()) {
        UE_LOGW("meadow_db: join snapshot for slot %d skipped (store unresolved)", peerSlot);
        return;
    }
    if (!MH::HashStore(snap.counts, nullptr)) {
        UE_LOGW("meadow_db: join snapshot for slot %d unreadable -- no seed", peerSlot);
        return;
    }
    snap.opAt = I::OpCounter();
    snap.valid = true;
    UE_LOGI("meadow_db: join snapshot for slot %d (%zu distinct hashes, op=%llu)",
            peerSlot, snap.counts.size(),
            static_cast<unsigned long long>(snap.opAt));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    if (!I::IsHost()) return;
    auto* s = I::SessionPtr();
    if (!s || !s->connected()) return;
    SlotSnap& snap = g_snap[peerSlot];
    if (!snap.valid) {
        // No snapshot means no knowledge of the save baseline; seeding the full store would
        // duplicate the joiner's save copy. Loud only at the slot's first replay: the connect
        // replay re-fires on every mid-session world-change re-announce, where a consumed snapshot
        // is normal, and a warning per cave travel would bury real join failures.
        if (!g_seededOnce[peerSlot])
            UE_LOGW("meadow_db: no join snapshot for slot %d -- seed skipped", peerSlot);
        return;
    }
    g_seededOnce[peerSlot] = true;
    if (!MS::EnsureResolved()) { snap.valid = false; return; }

    std::map<uint64_t, int32_t> cur;
    std::vector<uint64_t> seq;
    if (!MH::HashStore(cur, &seq)) { snap.valid = false; return; }

    // The mask criterion: a pending born before the snapshot has its effect inside the save the
    // joiner loaded, so the retry must skip this slot; younger pendings deliver through the retry
    // and stay out of the seed.
    const uint32_t bit = 1u << peerSlot;
    std::vector<I::Pending>& waiting = I::Waiting();
    std::map<uint64_t, int32_t> unmaskedNet;
    for (auto& p : waiting) {
        if (p.bornOp <= snap.opAt) p.excludeMask |= bit;
        else unmaskedNet[p.hash] += p.isDelete ? -1 : 1;
    }

    // The seed delta per hash over the union: current minus snapshot minus the unmasked pending
    // net.
    std::map<uint64_t, int32_t> delta = cur;
    for (const auto& [h, c] : snap.counts) delta[h] -= c;
    for (const auto& [h, c] : unmaskedNet) delta[h] -= c;

    int sentA = 0, sentD = 0;
    for (const auto& [h, d] : delta) {
        if (d > 0) {
            const int32_t idx = MH::IndexOf(h);
            if (idx < 0) continue;  // raced away; the store moved -- fine
            SD::Row r;
            if (!MS::ReadRow(idx, r)) continue;
            const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r, false);
            for (int32_t k = 0; k < d; ++k) {
                if (coop::blob_chunks::SendBlobToSlot(
                        s, peerSlot, coop::net::ReliableKind::MeadowAppend,
                        I::NextSeq(), blob))
                    ++sentA;
            }
        } else if (d < 0) {
            coop::net::ContentHashPayload cp{h};
            for (int32_t k = 0; k < -d; ++k) {
                if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::MeadowDelete,
                                          &cp, sizeof(cp)))
                    ++sentD;
            }
        }
    }
    // The canonical order always rides after the deltas on the same FIFO lane: the joiner's save order
    // may predate in-window moves, the seed's appends went in hash order, and order is synced state. The
    // FIFO guard: with lines still waiting the order would name hashes not yet delivered, so it is owed to
    // this slot and the retry sends it once none waits -- as it is when this send is refused.
    int sentO = 0;
    if (!seq.empty()) {
        if (waiting.empty() && I::SendOrder(s, seq, peerSlot)) sentO = 1;
        else I::OweOrderTo(peerSlot);
    }
    I::CountSeedLines(static_cast<uint64_t>(sentA + sentD + sentO));
    if (sentA || sentD || sentO)
        UE_LOGI("meadow_db: seed slot=%d +%d/-%d rows%s", peerSlot, sentA, sentD,
                sentO ? " +order" : "");
    snap.valid = false;
}

void CancelJoinSnapshot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    g_snap[peerSlot] = SlotSnap{};
    g_seededOnce[peerSlot] = false;
    I::ForgetSlot(static_cast<uint8_t>(peerSlot));
    const uint32_t bit = 1u << peerSlot;
    for (auto& p : I::Waiting()) {
        p.excludeMask &= ~bit;   // slot reuse must not inherit stale excludes
        p.sentMask &= ~bit;
    }
}

namespace internal {

void ResetJoinSeeds() {
    for (auto& sn : g_snap) sn = SlotSnap{};
    for (auto& so : g_seededOnce) so = false;
}

}  // namespace internal

}  // namespace coop::meadow_db_sync
