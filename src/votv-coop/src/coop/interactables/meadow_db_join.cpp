// coop/interactables/meadow_db_join.cpp -- the meadow lane's join seed (see
// coop/interactables/meadow_db_sync.h): at a joiner's save request, a snapshot of the database's
// multiset; at its world-ready, the lines that bring the copy in its save to the host's database, then
// the order. The waiting lines it masks and the lane's sends come through meadow_db_internal.h.

#include "coop/interactables/meadow_db_sync.h"

#include "coop/interactables/meadow_db_hash.h"
#include "coop/interactables/meadow_db_internal.h"
#include "coop/interactables/meadow_db_park.h"
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/slot_mask.h"

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
// Joiners whose world-ready came while this host's database was away, and the pen's length then: their seed
// runs once the database is back, before the pen drains, with only the lines that came before they were
// ready -- the session relayed every later one to them as it came.
coop::players::SlotMask g_seedOwed;
size_t   g_owedUpTo[coop::net::kMaxPeers] = {};

// The pen's lines a joiner's seed forwards: those the session relayed to the peers ready when they came,
// before this joiner was one of them. A client's own lines never go back to it, and the session never
// relays an order line.
struct Forward { coop::net::Session* s; int slot; int appends; int deletes; };
void ForwardParked(void* ctx, meadow_db_park::Kind kind, const std::vector<uint8_t>& blob, uint64_t hash,
                   uint8_t senderSlot) {
    auto* f = static_cast<Forward*>(ctx);
    if (senderSlot == f->slot || senderSlot == 0) return;
    if (kind == meadow_db_park::Kind::Append) {
        if (coop::blob_chunks::SendBlobToSlot(f->s, f->slot, coop::net::ReliableKind::MeadowAppend, I::NextSeq(),
                                              blob))
            ++f->appends;
    } else if (kind == meadow_db_park::Kind::Delete) {
        coop::net::ContentHashPayload cp{hash};
        if (f->s->SendReliableToSlot(f->slot, coop::net::ReliableKind::MeadowDelete, &cp, sizeof(cp))) ++f->deletes;
    }
}

void SeedSlot(int peerSlot, size_t forwardUpTo);
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
    if (I::DatabaseAway()) {
        g_seedOwed.Set(peerSlot);
        g_owedUpTo[peerSlot] = meadow_db_park::Parked();
        UE_LOGI("meadow_db: the seed for slot %d waits for this host's database, away (%zu line(s) parked)",
                peerSlot, g_owedUpTo[peerSlot]);
        return;
    }
    SeedSlot(peerSlot, meadow_db_park::Parked());
}

namespace {

void SeedSlot(int peerSlot, size_t forwardUpTo) {
    auto* s = I::SessionPtr();
    SlotSnap& snap = g_snap[peerSlot];
    if (!s || !s->connected() || !snap.valid) return;
    if (!MS::EnsureResolved()) { snap.valid = false; return; }

    std::map<uint64_t, int32_t> cur;
    std::vector<uint64_t> seq;
    if (!MH::HashStore(cur, &seq)) { snap.valid = false; return; }

    // The mask criterion: a pending born before the snapshot has its effect inside the save the
    // joiner loaded, so the retry must skip this slot; younger pendings deliver through the retry
    // and stay out of the seed.
    std::vector<I::Pending>& waiting = I::Waiting();
    std::map<uint64_t, int32_t> unmaskedNet;
    for (auto& p : waiting) {
        if (p.bornOp <= snap.opAt) p.excludeMask.Set(peerSlot);
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
            const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r);
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
    // The pen's lines the session skipped for this joiner follow the delta: without them the joiner would
    // never hold the rows the host has yet to drain into its own database.
    Forward fwd{s, peerSlot, 0, 0};
    meadow_db_park::ForEachParked(forwardUpTo, &ForwardParked, &fwd);
    sentA += fwd.appends;
    sentD += fwd.deletes;

    // The canonical order always rides after the deltas on the same FIFO lane: the joiner's save order
    // may predate in-window moves, the seed's appends went in hash order, and order is synced state. The
    // FIFO guard: with lines still waiting, or parked in the pen behind the host's own database, the order
    // would name a sequence the joiner does not hold, so it is owed to this slot and the retry sends it once
    // none waits -- as it is when this send is refused.
    int sentO = 0;
    if (!seq.empty()) {
        if (waiting.empty() && meadow_db_park::Parked() == 0 && I::SendOrder(s, seq, peerSlot)) sentO = 1;
        else I::OweOrderTo(peerSlot);
    }
    I::CountSeedLines(static_cast<uint64_t>(sentA + sentD + sentO));
    if (sentA || sentD || sentO)
        UE_LOGI("meadow_db: seed slot=%d +%d/-%d rows%s (%d/%d of them from the pen)", peerSlot, sentA, sentD,
                sentO ? " +order" : "", fwd.appends, fwd.deletes);
    snap.valid = false;
}

}  // namespace

void CancelJoinSnapshot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    g_snap[peerSlot] = SlotSnap{};
    g_seededOnce[peerSlot] = false;
    g_seedOwed.Clear(peerSlot);
    g_owedUpTo[peerSlot] = 0;
    I::ForgetSlot(static_cast<uint8_t>(peerSlot));
    for (auto& p : I::Waiting()) {
        p.excludeMask.Clear(peerSlot);   // slot reuse must not inherit stale excludes
        p.sentMask.Clear(peerSlot);
    }
}

namespace internal {

void ResetJoinSeeds() {
    for (auto& sn : g_snap) sn = SlotSnap{};
    for (auto& so : g_seededOnce) so = false;
    g_seedOwed.ClearAll();
}

bool HasOwedSeeds() {
    return g_seedOwed.Any();
}

void RunOwedSeeds() {
    const coop::players::SlotMask owed = g_seedOwed;
    g_seedOwed.ClearAll();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (owed.Has(slot)) SeedSlot(slot, g_owedUpTo[slot]);
}

void DropOwedSeeds() {
    if (g_seedOwed.Any()) UE_LOGW("meadow_db: a new database -- the seeds owed to joiners are dropped with it");
    g_seedOwed.ClearAll();
}

}  // namespace internal

}  // namespace coop::meadow_db_sync
