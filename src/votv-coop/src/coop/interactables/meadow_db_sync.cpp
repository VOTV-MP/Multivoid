// coop/interactables/meadow_db_sync.cpp -- see coop/interactables/meadow_db_sync.h. A
// content-hash multiset shadow over the saved signals; id-preserving reflected addSignal and
// removeSignal applies; tombstone counts; a symmetric per-slot join seed, where the seed delta
// per hash is the current count minus the snapshot count minus the unmasked pending net, with
// op-counter masks. An apply and its shadow update are game-thread-atomic per line. The
// client lane sends nothing until its own world-ready announce.

#include "coop/interactables/meadow_db_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/interactables/meadow_db_hash.h"
#include "coop/interactables/signal_wire.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <atomic>
#include <chrono>
#include <map>
#include <vector>

namespace coop::meadow_db_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace MH = coop::meadow_db_hash;
namespace MS = ue_wrap::meadow_store;
namespace SD = ue_wrap::signal_dynamic;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);
constexpr auto kTombstoneTTL = std::chrono::seconds(20);
// The absolute size bound on the tombstone vector (see OnDelete for why the unmatched case is
// the inserting case). Legitimate tombstones are bounded by the deletes racing an append that
// has not landed yet, a handful within one TTL; 256 is far above that and caps the vector at
// about 4 KB.
constexpr size_t kTombstoneCap = 256;
constexpr int  kVerbMark = 1;  // one id: both verbs only accelerate the poll

// The shadow: the broadcast-acknowledged multiset.
std::map<uint64_t, int32_t> g_shadow;   // ContentHash -> count
bool g_primed = false;
Clock::time_point g_nextPoll{};
uint64_t g_opCounter = 0;               // GT-monotonic line-author counter

// The dirty mark from the virtual-verb bracket, scoped to the laptop widget's context;
// relaxed, drained at the poll.
std::atomic<bool> g_dirty{false};
bool g_verbsRegistered = false;

// Order as state: the sort order is synced. The baseline is the last broadcast or applied hash
// sequence, the store's array order. Appends and deletes (organic and wire) update it in
// place; only a reorder of the common elements (a sort move, or drift) authors an order line.
// Convergence is host-canonical: client lines are host-terminal, the host applies
// last-writer-wins and broadcasts its canonical, and clients apply host-authored lines only.
std::vector<uint64_t> g_orderBase;
coop::blob_chunks::Assembler g_orderAsm;

// Pending: authored lines whose send failed, and a pre-ready client's organic lines.
struct Pending {
    uint64_t hash = 0;
    bool     isDelete = false;
    uint64_t bornOp = 0;                 // vs SlotSnap.opAt (mask criterion)
    uint32_t excludeMask = 0;            // slots the seed already covered
    uint32_t sentMask = 0;               // slots already delivered (masked retry)
    std::vector<uint8_t> blob;           // append: serialized row (sans image)
};
std::vector<Pending> g_pending;

// Tombstones: outstanding unresolved deletes, one entry per count.
struct Tomb { uint64_t hash; Clock::time_point until; };
std::vector<Tomb> g_tombs;

// Per-slot join-seed snapshots.
struct SlotSnap {
    bool valid = false;
    uint64_t opAt = 0;
    std::map<uint64_t, int32_t> counts;
};
SlotSnap g_snap[coop::net::kMaxPeers];
bool g_seededOnce[coop::net::kMaxPeers] = {};  // the connect replay re-fires on every world-change re-announce; only the first missing snapshot warns
bool g_orderPending = false;  // an order change detected but not yet sent/broadcast

coop::blob_chunks::Assembler g_assembler;
uint32_t g_nextSeq = 1;

// Counters for the 60 s line.
std::atomic<uint64_t> g_cMarks{0};
uint64_t g_cAppendsSent = 0, g_cDeletesSent = 0, g_cAppendsApplied = 0,
         g_cDeletesApplied = 0, g_cTombConsumed = 0, g_cSeedLines = 0,
         g_cOrderSent = 0, g_cOrderApplied = 0;
Clock::time_point g_nextStats{};

coop::meadow_db_sync::ApplyObserver g_applyObserver = nullptr;  // [dev] the selftest's
void NotifyApplied() {
    if (g_applyObserver) g_applyObserver();
}

// Helpers.

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// The client lane is mute until its own world-ready announce: a pre-ready client line reaching
// the host before the flip rides the seed back as a duplicate. The host is always ready.
bool CanSend() {
    return IsHost() || coop::net_pump::HasAnnouncedWorldReady();
}

int32_t SumShadow() {
    int32_t n = 0;
    for (const auto& [h, c] : g_shadow) n += c;
    return n;
}

// The net pending effect on the local store against the shadow: an append +1, a delete -1.
int32_t PendingNetAll() {
    int32_t n = 0;
    for (const auto& p : g_pending) n += p.isDelete ? -1 : 1;
    return n;
}

// Does the order of the common elements differ between the baseline and the live sequence?
// Filter each to the multiset intersection, preserving order; pure appends (the tail) and
// deletes (remove one) never change this, only a move or drift does.
bool CommonOrderChanged(const std::vector<uint64_t>& base,
                        const std::vector<uint64_t>& live) {
    std::map<uint64_t, int32_t> inBase, inLive;
    for (uint64_t h : base) ++inBase[h];
    for (uint64_t h : live) ++inLive[h];
    std::map<uint64_t, int32_t> quota;
    for (const auto& [h, c] : inBase) {
        auto it = inLive.find(h);
        if (it != inLive.end()) quota[h] = c < it->second ? c : it->second;
    }
    std::vector<uint64_t> fb, fl;
    std::map<uint64_t, int32_t> q1 = quota, q2 = quota;
    for (uint64_t h : base) { auto it = q1.find(h); if (it != q1.end() && it->second > 0) { --it->second; fb.push_back(h); } }
    for (uint64_t h : live) { auto it = q2.find(h); if (it != q2.end() && it->second > 0) { --it->second; fl.push_back(h); } }
    return fb != fl;
}

void LogDigest(const char* why) {
    uint64_t sum = 0;
    int32_t n = 0;
    for (const auto& [h, c] : g_shadow) {
        n += c;
        sum += h * static_cast<uint64_t>(c);  // wrapping, order-independent
    }
    UE_LOGI("meadow_db: digest n=%d sum=%016llx (%s)",
            n, static_cast<unsigned long long>(sum), why);
}

// The virtual-verb mark, capture only; the context is class-checked, since boot-window
// dispatches of the same name from other classes must not degenerate the pre-gate.

sg::Verdict OnVerbEntry(const sg::Call& b) {
    if (b.fromOurCode) return sg::Verdict::Run;   // our own apply calls these verbs by reflection
    void* cls = MS::LaptopWidgetClass();
    if (!cls || !b.object) return sg::Verdict::Run;
    if (R::ClassOf(b.object) != cls) return sg::Verdict::Run;
    g_dirty.store(true, std::memory_order_relaxed);
    g_cMarks.fetch_add(1, std::memory_order_relaxed);
    return sg::Verdict::Run;
}

// Called only after the store resolved: an unthrottled class lookup retry here was a per-frame
// pre-world array walk; the class now comes from the store's own throttled resolver.
void EnsureVerbsRegistered() {
    if (g_verbsRegistered) return;
    if (!MS::LaptopWidgetClass()) return;
    const bool ok =
        sg::WatchName(L"addSignal",    kVerbMark, &OnVerbEntry, nullptr) &&
        sg::WatchName(L"removeSignal", kVerbMark, &OnVerbEntry, nullptr) &&
        sg::WatchName(L"sortSignal",   kVerbMark, &OnVerbEntry, nullptr);
    if (ok) {
        g_verbsRegistered = true;
        UE_LOGI("meadow_db: 3 verb watches live (poll accelerators at the script-body gate)");
    }
}

// Send paths; the shadow advances only on successful delivery.

bool SendAppendBroadcast(coop::net::Session* s, const std::vector<uint8_t>& blob) {
    return coop::blob_chunks::SendBlob(
        s, coop::net::ReliableKind::MeadowAppend, g_nextSeq++, blob);
}

bool SendDeleteBroadcast(coop::net::Session* s, uint64_t hash) {
    coop::net::ContentHashPayload p{hash};
    return s->SendReliable(coop::net::ReliableKind::MeadowDelete, &p, sizeof(p));
}

// A negative slot broadcasts (the host canonical); otherwise point-to-point (a client's op to
// the host, or the join seed's canonical to one joiner).
bool SendOrder(coop::net::Session* s, const std::vector<uint64_t>& seq, int toSlot) {
    const std::vector<uint8_t> blob = MH::OrderBlob(seq);
    if (toSlot < 0)
        return coop::blob_chunks::SendBlob(
            s, coop::net::ReliableKind::MeadowOrder, g_nextSeq++, blob);
    return coop::blob_chunks::SendBlobToSlot(
        s, toSlot, coop::net::ReliableKind::MeadowOrder, g_nextSeq++, blob);
}

// Author one line: try to send now; on failure (or a muted pre-ready client) queue it as
// pending. The shadow advances only on success.
void AuthorLine(coop::net::Session* s, uint64_t hash, bool isDelete,
                const std::vector<uint8_t>& blob) {
    ++g_opCounter;
    const bool sendable = s && s->connected() && CanSend();
    bool sent = false;
    if (sendable) {
        sent = isDelete ? SendDeleteBroadcast(s, hash)
                        : SendAppendBroadcast(s, blob);
    }
    if (sent) {
        if (isDelete) {
            auto it = g_shadow.find(hash);
            if (it != g_shadow.end() && --it->second <= 0) g_shadow.erase(it);
            ++g_cDeletesSent;
        } else {
            ++g_shadow[hash];
            ++g_cAppendsSent;
        }
    } else {
        Pending p;
        p.hash = hash;
        p.isDelete = isDelete;
        p.bornOp = g_opCounter;
        if (!isDelete) p.blob = blob;
        g_pending.push_back(std::move(p));
    }
}

// Retry the queued lines. Unmasked entries retry as plain broadcasts (the channel-refused
// shape); masked entries deliver per slot to every ready slot not yet covered, since the seed
// already served the masked ones.
void RetryPending(coop::net::Session* s) {
    if (!s || !s->connected() || !CanSend()) return;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        Pending& p = *it;
        bool done = false;
        if (p.excludeMask == 0) {
            done = p.isDelete ? SendDeleteBroadcast(s, p.hash)
                              : SendAppendBroadcast(s, p.blob);
        } else {
            bool allCovered = true;
            for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
                const uint32_t bit = 1u << slot;
                if ((p.excludeMask | p.sentMask) & bit) continue;
                if (!s->IsSlotWorldReady(slot)) continue;
                bool ok;
                if (p.isDelete) {
                    coop::net::ContentHashPayload cp{p.hash};
                    ok = s->SendReliableToSlot(slot, coop::net::ReliableKind::MeadowDelete,
                                               &cp, sizeof(cp));
                } else {
                    ok = coop::blob_chunks::SendBlobToSlot(
                        s, slot, coop::net::ReliableKind::MeadowAppend, g_nextSeq++, p.blob);
                }
                if (ok) p.sentMask |= bit;
                else allCovered = false;
            }
            done = allCovered;
        }
        if (done) {
            if (p.isDelete) {
                auto sh = g_shadow.find(p.hash);
                if (sh != g_shadow.end() && --sh->second <= 0) g_shadow.erase(sh);
                ++g_cDeletesSent;
            } else {
                ++g_shadow[p.hash];
                ++g_cAppendsSent;
            }
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
}

// Apply paths, wire to store; the shadow is updated in the same game-thread callback.

void ApplyAppendBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = coop::signal_wire::ContentHash(blob);
    // Tombstone consume: an outstanding delete beats the append, the race cover.
    for (auto it = g_tombs.begin(); it != g_tombs.end(); ++it) {
        if (it->hash == hash) {
            g_tombs.erase(it);
            ++g_cTombConsumed;
            UE_LOGI("meadow_db: append from slot %u consumed by an outstanding delete "
                    "(hash %016llx)", static_cast<unsigned>(senderSlot),
                    static_cast<unsigned long long>(hash));
            return;
        }
    }
    SD::Row row;
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(blob, row, adopt)) {
        UE_LOGW("meadow_db: malformed row blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!MS::EnsureResolved() || !MS::Widget()) {
        UE_LOGW("meadow_db: append from slot %u dropped -- store/widget unresolved "
                "(world transition?)", static_cast<unsigned>(senderSlot));
        return;
    }
    if (MS::ApplyAddSignal(row)) {
        ++g_shadow[hash];
        g_orderBase.push_back(hash);  // addSignal appends at the tail on every peer
        ++g_cAppendsApplied;
        UE_LOGI("meadow_db: applied append from slot %u ('%ls' lvl %d)",
                static_cast<unsigned>(senderSlot), row.name.c_str(), row.level);
        LogDigest("apply-append");
        NotifyApplied();
    } else {
        UE_LOGW("meadow_db: addSignal apply FAILED for slot %u ('%ls') -- row lost",
                static_cast<unsigned>(senderSlot), row.name.c_str());
    }
}

// True when the delete resolved and applied, the shadow decremented.
bool ApplyDeleteByHash(uint64_t hash) {
    if (!g_primed) return false;
    if (!MS::EnsureResolved() || !MS::Widget()) return false;
    const int32_t idx = MH::IndexOf(hash);
    if (idx < 0) return false;
    if (!MS::ApplyRemoveSignal(idx)) return false;
    auto it = g_shadow.find(hash);
    if (it != g_shadow.end() && --it->second <= 0) g_shadow.erase(it);
    for (auto ob = g_orderBase.begin(); ob != g_orderBase.end(); ++ob) {
        if (*ob == hash) { g_orderBase.erase(ob); break; }  // one instance; drift self-heals at the next poll's order check
    }
    ++g_cDeletesApplied;
    UE_LOGI("meadow_db: applied delete (row %d, hash %016llx)",
            idx, static_cast<unsigned long long>(hash));
    LogDigest("apply-delete");
    NotifyApplied();
    return true;
}

// Apply an order line: permute the rows to the target sequence and run the game's own list
// rebuild. Clients accept host lines only; the host applies any peer's line last-writer-wins
// and broadcasts its canonical (echo-proof: the baseline updates in the same callback).
void ApplyOrderBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    std::vector<uint64_t> tgt;
    if (!MH::ParseOrderBlob(blob, tgt)) {
        UE_LOGW("meadow_db: malformed order blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    const bool host = IsHost();
    if (!host && senderSlot != 0) {
        UE_LOGW("meadow_db: non-host order line from slot %u dropped (host-canonical lane)",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!MS::EnsureResolved() || !MS::Widget()) {
        UE_LOGW("meadow_db: order from slot %u dropped -- store/widget unresolved",
                static_cast<unsigned>(senderSlot));
        return;
    }
    std::map<uint64_t, int32_t> cur;
    std::vector<uint64_t> seq;
    if (!MH::HashStore(cur, &seq)) return;
    const int32_t n = static_cast<int32_t>(seq.size());
    // In-flight appends the target does not list yet keep their relative order at the tail.
    const std::vector<int32_t> perm = MH::PermutationTo(seq, tgt);
    bool identity = true;
    for (int32_t i = 0; i < n; ++i)
        if (perm[static_cast<size_t>(i)] != i) { identity = false; break; }
    if (!identity) {
        if (!MS::ReorderRows(perm.data(), n)) {
            UE_LOGW("meadow_db: order permute failed (n=%d) -- dropped", n);
            return;
        }
        MS::ApplyGenSignalList();
        std::vector<uint64_t> ns;
        ns.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
            ns.push_back(seq[static_cast<size_t>(perm[static_cast<size_t>(i)])]);
        seq.swap(ns);
        ++g_cOrderApplied;
        UE_LOGI("meadow_db: applied order from slot %u (n=%d)",
                static_cast<unsigned>(senderSlot), n);
        NotifyApplied();
    }
    if (host && senderSlot != 0) {
        // The canonical back to everyone, so the author sees its state confirmed. The same FIFO
        // guard as the poll: never broadcast an order that references a still-pending append or
        // delete.
        auto* s = g_session.load(std::memory_order_acquire);
        const bool ok = (s && s->connected())
                            ? (g_pending.empty() && SendOrder(s, seq, -1))
                            : true;
        if (ok) {
            g_orderBase = seq;
            g_orderPending = false;
            ++g_cOrderSent;
        } else {
            g_orderPending = true;  // baseline stays old -> the poll re-detects + rebroadcasts
        }
    } else {
        g_orderBase = seq;
        g_orderPending = false;
    }
}

}  // namespace

// Entry points.

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!MS::EnsureResolved()) return;  // 2 s throttled; gates the registration too
    EnsureVerbsRegistered();
    sg::ResolvePendingNames();

    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    g_assembler.Sweep(now, kAssemblyTTL);
    for (auto it = g_tombs.begin(); it != g_tombs.end();) {
        if (now >= it->until) {
            UE_LOGW("meadow_db: delete for hash %016llx expired unmatched",
                    static_cast<unsigned long long>(it->hash));
            it = g_tombs.erase(it);
        } else ++it;
    }

    const int32_t n = MS::Count();
    if (n < 0) {
        // World down: fresh allocations at world-up, never a diff across it; unsent lines drop with
        // the world and heal at the join.
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_pending.clear();
            g_tombs.clear();
            g_orderBase.clear();
            g_orderPending = false;
        }
        return;
    }

    if (!g_primed) {
        // Adopt without broadcast: absorbs the save-loaded store and any pre-prime wire applies
        // (the prime precedes the ready flip by seconds in a real join; correct at a zero gap too).
        if (!MH::HashStore(g_shadow, &g_orderBase)) return;
        g_primed = true;
        UE_LOGI("meadow_db: shadow primed at %d row(s)", n);
        LogDigest("prime");
    } else {
        // The pre-gate: one count read and the scoped mark. Adds and removes change the count; a
        // sort move fires the mark (the third matcher); no in-place edit verb exists. An unsent
        // order change keeps retrying across polls.
        const bool marked = g_dirty.exchange(false, std::memory_order_relaxed);
        const int32_t expected = SumShadow() + PendingNetAll();
        if (marked || n != expected || g_orderPending) {
            std::map<uint64_t, int32_t> cur;
            std::vector<uint64_t> seq;
            if (!MH::HashStore(cur, &seq)) return;
            // Target against current: what the store holds against the shadow plus pending.
            std::map<uint64_t, int32_t> target = g_shadow;
            for (const auto& p : g_pending) {
                target[p.hash] += p.isDelete ? -1 : 1;
            }
            // The union walk.
            std::vector<uint8_t> scratch;
            for (const auto& [h, c] : cur) {
                int32_t want = c - (target.count(h) ? target[h] : 0);
                while (want-- > 0) {
                    // Find a serialised row with this hash for the blob.
                    const int32_t idx = MH::IndexOf(h);
                    std::vector<uint8_t> blob;
                    if (idx >= 0) {
                        SD::Row r;
                        if (MS::ReadRow(idx, r))
                            blob = coop::signal_wire::Serialize(r, false);
                    }
                    if (blob.empty()) break;  // unreadable mid-walk: next poll
                    AuthorLine(s, h, /*isDelete=*/false, blob);
                }
            }
            for (const auto& [h, c] : target) {
                const int32_t have = cur.count(h) ? cur[h] : 0;
                int32_t drop = c - have;
                static const std::vector<uint8_t> kNoBlob;
                while (drop-- > 0) AuthorLine(s, h, /*isDelete=*/true, kNoBlob);
            }
            // A persistent mismatch after the reconcile is a real bug; every exit is instrumented.
            const int32_t post = SumShadow() + PendingNetAll();
            if (post != n)
                UE_LOGW("meadow_db: shadow/store mismatch persists after reconcile "
                        "(store %d vs shadow+pending %d)", n, post);
            else
                LogDigest("poll-reconcile");

            // Order: a reorder of the common elements is a sort move, or drift. The host broadcasts
            // its canonical; a client sends its order to the host only. A send failure leaves the
            // baseline old, so the pending flag retries. The FIFO guard: an order line referencing
            // a hash whose append or delete is still pending would overtake it on the wire (the
            // lane pin orders only what was handed to the transport), so the receiver would skip
            // the unknown hash and the late append would land at the tail, a permanent per-peer
            // order divergence. Every order send is deferred until the pending queue is empty.
            if (CommonOrderChanged(g_orderBase, seq)) {
                if (!s->connected()) {
                    g_orderBase = seq;  // solo: nobody to tell; joiners get save+seed
                    g_orderPending = false;
                } else if (!g_pending.empty()) {
                    g_orderPending = true;  // retry after RetryPending flushes
                } else if (CanSend() && SendOrder(s, seq, IsHost() ? -1 : 0)) {
                    g_orderBase = seq;
                    g_orderPending = false;
                    ++g_cOrderSent;
                    UE_LOGI("meadow_db: order %s (n=%zu)",
                            IsHost() ? "canonical broadcast" : "sent to host", seq.size());
                } else {
                    g_orderPending = true;
                }
            } else {
                g_orderBase = seq;  // tail/delete adjustments -- silent
                g_orderPending = false;
            }
        }
    }

    // The tombstone retry; a matching row may have appeared.
    for (auto it = g_tombs.begin(); it != g_tombs.end();) {
        if (ApplyDeleteByHash(it->hash)) it = g_tombs.erase(it);
        else ++it;
    }

    RetryPending(s);

    if (now >= g_nextStats) {
        g_nextStats = now + std::chrono::seconds(60);
        const uint64_t marks = g_cMarks.exchange(0, std::memory_order_relaxed);
        if (marks || g_cAppendsSent || g_cDeletesSent || g_cAppendsApplied ||
            g_cDeletesApplied || g_cTombConsumed || g_cSeedLines ||
            g_cOrderSent || g_cOrderApplied || !g_pending.empty())
            UE_LOGI("meadow_db: 60s marks=%llu sent=%llu/%llu applied=%llu/%llu "
                    "order=%llu/%llu tombConsumed=%llu seed=%llu pending=%zu tombs=%zu",
                    static_cast<unsigned long long>(marks),
                    static_cast<unsigned long long>(g_cAppendsSent),
                    static_cast<unsigned long long>(g_cDeletesSent),
                    static_cast<unsigned long long>(g_cAppendsApplied),
                    static_cast<unsigned long long>(g_cDeletesApplied),
                    static_cast<unsigned long long>(g_cOrderSent),
                    static_cast<unsigned long long>(g_cOrderApplied),
                    static_cast<unsigned long long>(g_cTombConsumed),
                    static_cast<unsigned long long>(g_cSeedLines),
                    g_pending.size(), g_tombs.size());
    }
}

void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (g_assembler.OnChunk(p, senderSlot, blob))
        ApplyAppendBlob(blob, senderSlot);
}

void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    if (p.contentHash == 0) return;
    if (!ApplyDeleteByHash(p.contentHash)) {
        // A tombstone is recorded exactly when the delete did not match: the unmatched case is the
        // inserting case, so a stream of attacker-chosen hashes grows this vector at line rate, and
        // the TTL bounds it in time but not in rate. Refuse past the bound rather than evict:
        // eviction would let a flood push out the legitimate not-yet-arrived-row tombstone this
        // exists to hold.
        if (g_tombs.size() >= kTombstoneCap) {
            UE_LOGW("meadow_db: tombstone REFUSED (hash=%016llx, from slot %u) -- at the "
                    "%zu-entry bound",
                    static_cast<unsigned long long>(p.contentHash),
                    static_cast<unsigned>(senderSlot), kTombstoneCap);
            return;
        }
        g_tombs.push_back({p.contentHash, Clock::now() + kTombstoneTTL});
    }
}

void OnOrderChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (g_orderAsm.OnChunk(p, senderSlot, blob))
        ApplyOrderBlob(blob, senderSlot);
}

void CaptureJoinSnapshot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    if (!IsHost()) return;
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
    snap.opAt = g_opCounter;
    snap.valid = true;
    UE_LOGI("meadow_db: join snapshot for slot %d (%zu distinct hashes, op=%llu)",
            peerSlot, snap.counts.size(),
            static_cast<unsigned long long>(snap.opAt));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    if (!IsHost()) return;
    auto* s = g_session.load(std::memory_order_acquire);
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
    std::map<uint64_t, int32_t> unmaskedNet;
    for (auto& p : g_pending) {
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
                        g_nextSeq++, blob))
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
    // The canonical order always rides after the deltas on the same FIFO lane: the joiner's save
    // order may predate in-window moves, and order is synced state. The FIFO guard: with lines
    // still pending the order would reference undelivered hashes, so defer to the poll's canonical
    // broadcast, which reaches this slot too after the flush.
    int sentO = 0;
    if (!seq.empty() && g_pending.empty() && SendOrder(s, seq, peerSlot)) sentO = 1;
    else if (!g_pending.empty()) g_orderPending = true;
    g_cSeedLines += static_cast<uint64_t>(sentA + sentD + sentO);
    if (sentA || sentD || sentO)
        UE_LOGI("meadow_db: seed slot=%d +%d/-%d rows%s", peerSlot, sentA, sentD,
                sentO ? " +order" : "");
    snap.valid = false;
}

bool IsPrimed() {
    return g_primed;
}

SentCounts SentLines() {
    return {g_cAppendsSent, g_cDeletesSent, g_cOrderSent};
}

void SetApplyObserver(ApplyObserver fn) {
    g_applyObserver = fn;
}

void CancelJoinSnapshot(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= coop::net::kMaxPeers) return;
    g_snap[peerSlot] = SlotSnap{};
    g_seededOnce[peerSlot] = false;
    const uint32_t bit = 1u << peerSlot;
    for (auto& p : g_pending) {
        p.excludeMask &= ~bit;   // slot reuse must not inherit stale excludes
        p.sentMask &= ~bit;
    }
}

void OnDisconnect() {
    g_assembler.Clear();
    g_orderAsm.Clear();
    g_shadow.clear();
    g_pending.clear();
    g_tombs.clear();
    g_orderBase.clear();
    g_orderPending = false;
    for (auto& sn : g_snap) sn = SlotSnap{};
    for (auto& so : g_seededOnce) so = false;
    g_primed = false;
    g_dirty.store(false, std::memory_order_relaxed);
    g_nextSeq = 1;
    g_nextPoll = {};
    g_opCounter = 0;
    g_cAppendsSent = g_cDeletesSent = g_cAppendsApplied = g_cDeletesApplied = 0;
    g_cTombConsumed = g_cSeedLines = 0;
    g_cOrderSent = g_cOrderApplied = 0;
}

}  // namespace coop::meadow_db_sync
