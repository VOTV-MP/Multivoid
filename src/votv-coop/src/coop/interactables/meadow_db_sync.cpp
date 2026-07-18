// coop/meadow_db_sync.cpp -- see coop/interactables/meadow_db_sync.h.
//
// v120 L9 (votv-meadow-db-L9-impl-DESIGN-2026-07-19.md, 15-round /qf).
// Content-hash MULTISET shadow over saveSlot.savedSignals_0; id-preserving
// reflected ui_laptop.addSignal / removeSignal applies; tombstone counts;
// symmetric per-slot join seed via
//   seedDelta(h) = curCount(h) - snapCount(h) - unmaskedPendingNet(h)
// with GT-op-counter masks. Apply+shadow is GT-atomic per line. The client
// lane sends nothing until its own ClientWorldReady announce.

#include "coop/interactables/meadow_db_sync.h"

#include "coop/config/config.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_wire.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/vm_dispatch.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace coop::meadow_db_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace MS = ue_wrap::meadow_store;
namespace SD = ue_wrap::signal_dynamic;
namespace vm = ue_wrap::vm_dispatch;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);
constexpr auto kTombstoneTTL = std::chrono::seconds(20);
constexpr int  kVerbMark = 1;  // one id: both verbs only accelerate the poll

// ---- shadow: the broadcast-acknowledged multiset ----
std::map<uint64_t, int32_t> g_shadow;   // ContentHash -> count
bool g_primed = false;
Clock::time_point g_nextPoll{};
uint64_t g_opCounter = 0;               // GT-monotonic line-author counter

// ---- 0x45 dirty mark (scoped: ui_laptop ctx only; relaxed, drained at poll) ----
std::atomic<bool> g_dirty{false};
bool g_verbsRegistered = false;

// ---- order-as-state (v120 per-rule-1 user decision: the sort order IS synced) ----
// Baseline = the last broadcast/applied hash SEQUENCE (the store's array order).
// Appends/deletes (organic + wire) update it in place; only a reorder of the
// COMMON elements (= a sortSignal move, or drift) authors a MeadowOrder line.
// Convergence = HOST-CANONICAL (the RackState shape): client lines are
// host-terminal; the host applies last-writer-wins and broadcasts ITS
// canonical; clients apply host-authored lines only.
std::vector<uint64_t> g_orderBase;
coop::blob_chunks::Assembler g_orderAsm;

// ---- pending (authored lines whose send failed / pre-ready client organics) ----
struct Pending {
    uint64_t hash = 0;
    bool     isDelete = false;
    uint64_t bornOp = 0;                 // vs SlotSnap.opAt (mask criterion)
    uint32_t excludeMask = 0;            // slots the seed already covered
    uint32_t sentMask = 0;               // slots already delivered (masked retry)
    std::vector<uint8_t> blob;           // append: serialized row (sans image)
};
std::vector<Pending> g_pending;

// ---- tombstones: outstanding unresolved deletes (one entry = one count) ----
struct Tomb { uint64_t hash; Clock::time_point until; };
std::vector<Tomb> g_tombs;

// ---- per-slot join-seed snapshots (the g_blobKeys lifetime) ----
struct SlotSnap {
    bool valid = false;
    uint64_t opAt = 0;
    std::map<uint64_t, int32_t> counts;
};
SlotSnap g_snap[coop::net::kMaxPeers];
bool g_seededOnce[coop::net::kMaxPeers] = {};  // audit fix 2: ConnectReplayForSlot
                                               // re-fires on every world-change
                                               // re-announce -- only the FIRST
                                               // missing snapshot is a warning
bool g_orderPending = false;  // an order change detected but not yet sent/broadcast

coop::blob_chunks::Assembler g_assembler;
uint32_t g_nextSeq = 1;

// ---- [dev] meadow_selftest (HOST): inject -> digest 0->1 -> remove -> 0 ----
int  g_selftestStage = 0;               // 0=idle/armed 10=waiting 1=injected 2=done
Clock::time_point g_selftestAt{};
Clock::time_point g_selftestDeadline{}; // audit fix: the remove RETRIES until this
uint64_t g_selftestHash = 0;

// ---- counters (60 s line) ----
std::atomic<uint64_t> g_cMarks{0};
uint64_t g_cAppendsSent = 0, g_cDeletesSent = 0, g_cAppendsApplied = 0,
         g_cDeletesApplied = 0, g_cTombConsumed = 0, g_cSeedLines = 0,
         g_cOrderSent = 0, g_cOrderApplied = 0;
Clock::time_point g_nextStats{};

// --------------------------------------------------------------------------
// helpers

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// The client lane is mute until its own world-ready announce (design R6: a
// pre-ready client line reaching the host before the flip rides the seed back
// as a dup). The host is always ready.
bool CanSend() {
    return IsHost() || coop::net_pump::HasAnnouncedWorldReady();
}

uint64_t HashRow(const SD::Row& r, std::vector<uint8_t>& scratch) {
    scratch = coop::signal_wire::Serialize(r, /*adopt=*/false);
    return coop::signal_wire::ContentHash(scratch);
}

int32_t SumShadow() {
    int32_t n = 0;
    for (const auto& [h, c] : g_shadow) n += c;
    return n;
}

// Net pending effect on the local store vs the shadow (append +1, delete -1).
int32_t PendingNetAll() {
    int32_t n = 0;
    for (const auto& p : g_pending) n += p.isDelete ? -1 : 1;
    return n;
}

// Re-hash the live store into a fresh multiset (+ optionally the ordered hash
// SEQUENCE for the order mirror). False if any row is unreadable (world
// transition mid-walk -- retry next poll). Cold/edge path only (the pre-gate
// keeps it off the steady state); the per-row Serialize allocations are
// accepted there (perf audit W-1 -- the row object is hoisted).
bool HashStore(std::map<uint64_t, int32_t>& out, std::vector<uint64_t>* seq) {
    out.clear();
    if (seq) seq->clear();
    const int32_t n = MS::Count();
    if (n < 0) return false;
    std::vector<uint8_t> scratch;
    SD::Row r;
    if (seq) seq->reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        if (!MS::ReadRow(i, r)) return false;
        const uint64_t h = HashRow(r, scratch);
        ++out[h];
        if (seq) seq->push_back(h);
    }
    return true;
}

// Find the store index of a row whose content hash matches (apply-time
// resolve; deletes are rare -- O(N) serialize is fine).
int32_t ResolveIndexByHash(uint64_t hash) {
    const int32_t n = MS::Count();
    if (n < 0) return -1;
    std::vector<uint8_t> scratch;
    SD::Row r;
    for (int32_t i = 0; i < n; ++i) {
        if (!MS::ReadRow(i, r)) continue;
        if (HashRow(r, scratch) == hash) return i;
    }
    return -1;
}

// Does the order of the COMMON elements differ between the baseline and the
// live sequence? Filter each to the multiset intersection, preserving order;
// pure appends (tail) and deletes (remove-one) never change this -- only a
// MOVE (or drift) does.
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

std::vector<uint8_t> OrderBlob(const std::vector<uint64_t>& seq) {
    const uint16_t n = static_cast<uint16_t>(seq.size() > 0xFFFF ? 0xFFFF : seq.size());
    std::vector<uint8_t> b(2 + static_cast<size_t>(n) * 8);
    std::memcpy(b.data(), &n, 2);
    for (uint16_t i = 0; i < n; ++i)
        std::memcpy(b.data() + 2 + static_cast<size_t>(i) * 8, &seq[i], 8);
    return b;
}

bool ParseOrderBlob(const std::vector<uint8_t>& b, std::vector<uint64_t>& out) {
    if (b.size() < 2) return false;
    uint16_t n = 0;
    std::memcpy(&n, b.data(), 2);
    if (b.size() < 2 + static_cast<size_t>(n) * 8) return false;
    out.resize(n);
    for (uint16_t i = 0; i < n; ++i)
        std::memcpy(&out[i], b.data() + 2 + static_cast<size_t>(i) * 8, 8);
    return true;
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

// --------------------------------------------------------------------------
// the 0x45 mark (capture-only; ctx class-checked -- boot-window same-FName
// dispatches from other classes must not degenerate the pre-gate)

void OnVerbEntry(const vm::Bracket& b) {
    void* cls = MS::LaptopWidgetClass();
    if (!cls || !b.ctx) return;
    if (R::ClassOf(b.ctx) != cls) return;
    g_dirty.store(true, std::memory_order_relaxed);
    g_cMarks.fetch_add(1, std::memory_order_relaxed);
}

// Called only AFTER MS::EnsureResolved() succeeds (perf audit C-1: an
// unthrottled FindClass retry here was a 60 Hz pre-world array-walk bomb; the
// class now comes from the store's own 2 s-throttled resolver).
void EnsureVerbsRegistered() {
    if (g_verbsRegistered) return;
    if (!MS::LaptopWidgetClass()) return;
    const bool ok =
        vm::RegisterVirtualVerb(L"addSignal",    kVerbMark, &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"removeSignal", kVerbMark, &OnVerbEntry) &&
        vm::RegisterVirtualVerb(L"sortSignal",   kVerbMark, &OnVerbEntry);
    if (ok) {
        g_verbsRegistered = true;
        UE_LOGI("meadow_db: 3 verb matchers registered (0x45 poll accelerators)");
    }
}

// --------------------------------------------------------------------------
// send paths (shadow advances ONLY on successful delivery)

bool SendAppendBroadcast(coop::net::Session* s, const std::vector<uint8_t>& blob) {
    return coop::blob_chunks::SendBlob(
        s, coop::net::ReliableKind::MeadowAppend, g_nextSeq++, blob);
}

bool SendDeleteBroadcast(coop::net::Session* s, uint64_t hash) {
    coop::net::ContentHashPayload p{hash};
    return s->SendReliable(coop::net::ReliableKind::MeadowDelete, &p, sizeof(p));
}

// toSlot < 0 = broadcast (host canonical); else point (client -> host op, or
// the join seed's canonical to one joiner).
bool SendOrder(coop::net::Session* s, const std::vector<uint64_t>& seq, int toSlot) {
    const std::vector<uint8_t> blob = OrderBlob(seq);
    if (toSlot < 0)
        return coop::blob_chunks::SendBlob(
            s, coop::net::ReliableKind::MeadowOrder, g_nextSeq++, blob);
    return coop::blob_chunks::SendBlobToSlot(
        s, toSlot, coop::net::ReliableKind::MeadowOrder, g_nextSeq++, blob);
}

// Author one line: try to send now; on failure (or a muted pre-ready client)
// queue it as pending. The shadow advances only on success.
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

// Retry queued lines. mask==0 entries retry as plain broadcasts (the v65
// channel-refused shape); masked entries deliver per-slot to every ready slot
// not yet covered (the seed already served the masked ones).
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

// --------------------------------------------------------------------------
// apply paths (wire -> store; shadow updated in the SAME GT callback)

void ApplyAppendBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = coop::signal_wire::ContentHash(blob);
    // Tombstone consume: an outstanding delete beats the append (race cover).
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
    } else {
        UE_LOGW("meadow_db: addSignal apply FAILED for slot %u ('%ls') -- row lost",
                static_cast<unsigned>(senderSlot), row.name.c_str());
    }
}

// True when the delete resolved and applied (shadow decremented).
bool ApplyDeleteByHash(uint64_t hash) {
    if (!g_primed) return false;
    if (!MS::EnsureResolved() || !MS::Widget()) return false;
    const int32_t idx = ResolveIndexByHash(hash);
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
    return true;
}

// Apply an order-as-state line: byte-permute the rows to the target sequence +
// the game's own genSignalList widget rebuild. Clients accept HOST lines only;
// the host applies any peer's line last-writer-wins and broadcasts ITS
// canonical (echo-proof: the baseline updates in the same GT callback).
void ApplyOrderBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    std::vector<uint64_t> tgt;
    if (!ParseOrderBlob(blob, tgt)) {
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
    if (!HashStore(cur, &seq)) return;
    const int32_t n = static_cast<int32_t>(seq.size());
    // Permutation: listed hashes in the target order (first unused instance
    // wins -- duplicates are byte-identical); unlisted (in-flight appends)
    // keep their relative order at the tail; missing hashes skip.
    std::vector<int32_t> perm;
    perm.reserve(static_cast<size_t>(n));
    std::vector<bool> used(static_cast<size_t>(n), false);
    for (uint64_t h : tgt) {
        for (int32_t i = 0; i < n; ++i) {
            if (!used[static_cast<size_t>(i)] && seq[static_cast<size_t>(i)] == h) {
                used[static_cast<size_t>(i)] = true;
                perm.push_back(i);
                break;
            }
        }
    }
    for (int32_t i = 0; i < n; ++i)
        if (!used[static_cast<size_t>(i)]) perm.push_back(i);
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
    }
    if (host && senderSlot != 0) {
        // Canonical back to everyone (the author sees its state confirmed).
        // Same FIFO guard as the poll (audit HIGH-1): never broadcast an order
        // that references a still-pending append/delete.
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

// --------------------------------------------------------------------------
// [dev] self-test (HOST): a synthetic 0->1->0 so the smoke's digest assert
// discriminates its axis on an empty store (design R14).

bool SelftestEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("meadow_selftest");
    return s;
}

void SelftestTick(coop::net::Session* s) {
    if (!SelftestEnabled() || !IsHost() || !g_primed) return;
    if (!s || !s->connected()) return;
    const auto now = Clock::now();
    switch (g_selftestStage) {
        case 0:
            g_selftestAt = now + std::chrono::seconds(8);
            g_selftestStage = 10;
            break;
        case 10: {
            if (now < g_selftestAt) break;
            SD::Row row;
            row.name = L"MEADOW-SELFTEST";
            row.id = L"selftest-0";
            row.object.clear();   // empty = NAME_None (the literal "None" string
            row.signal.clear();   // trips WriteFNameField's failed-intern check)
            row.level = 1;
            row.size = 1.0f;
            row.decoded = 1.0f;
            row.hasData = true;
            if (MS::ApplyAddSignal(row)) {
                std::vector<uint8_t> scratch;
                g_selftestHash = HashRow(row, scratch);
                g_selftestStage = 1;
                g_selftestAt = now + std::chrono::seconds(8);
                g_selftestDeadline = now + std::chrono::seconds(40);
                UE_LOGI("meadow_db: SELFTEST injected (hash %016llx)",
                        static_cast<unsigned long long>(g_selftestHash));
            } else {
                // Discriminate the failing step (dead-guard lesson) + RETRY:
                // the widget gate may open late (the device back-pointer).
                UE_LOGW("meadow_db: SELFTEST inject failed -- %s; retrying",
                        MS::Widget() ? "addSignal call refused" : "widget gate closed");
                g_selftestAt = now + std::chrono::seconds(5);
                if (g_selftestDeadline == Clock::time_point{})
                    g_selftestDeadline = now + std::chrono::seconds(60);
                if (now >= g_selftestDeadline) g_selftestStage = 2;
            }
            break;
        }
        case 1: {
            // Audit fix 1: the remove RETRIES each poll until the deadline --
            // a fire-and-forget failure would leave the synthetic row to be
            // written into the real save.
            if (now < g_selftestAt) break;
            const int32_t idx = ResolveIndexByHash(g_selftestHash);
            if (idx >= 0 && MS::ApplyRemoveSignal(idx)) {
                UE_LOGI("meadow_db: SELFTEST removed");
                g_selftestStage = 2;
            } else if (now >= g_selftestDeadline) {
                UE_LOGW("meadow_db: SELFTEST remove FAILED past deadline (idx=%d) -- "
                        "synthetic row may persist in the save", idx);
                g_selftestStage = 2;
            }
            break;
        }
        default: break;
    }
}

}  // namespace

// --------------------------------------------------------------------------

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!MS::EnsureResolved()) return;  // 2 s-throttled; gates the registration too (perf C-1)
    EnsureVerbsRegistered();
    vm::TickResolvePending();

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
        // World down: fresh allocations at world-up -- never diff across it
        // (the v65 shape; unsent lines drop with the world, heal at join).
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
        // Adopt-without-broadcast (v65 g_primed idiom). Absorbs the save-loaded
        // store AND any pre-prime wire applies (measured: prime precedes the
        // ready flip by ~8 s in a real join; correct at gap->0 too).
        if (!HashStore(g_shadow, &g_orderBase)) return;
        g_primed = true;
        UE_LOGI("meadow_db: shadow primed at %d row(s)", n);
        LogDigest("prime");
    } else {
        // Pre-gate: one count read + the scoped mark. add/remove change the
        // count; a sortSignal MOVE fires the mark (3rd matcher, v120 order
        // sync); no in-place edit verb exists (censused). g_orderPending keeps
        // an unsent order change retrying across polls.
        const bool marked = g_dirty.exchange(false, std::memory_order_relaxed);
        const int32_t expected = SumShadow() + PendingNetAll();
        if (marked || n != expected || g_orderPending) {
            std::map<uint64_t, int32_t> cur;
            std::vector<uint64_t> seq;
            if (!HashStore(cur, &seq)) return;
            // target-vs-current: what the store holds vs shadow (+) pending.
            std::map<uint64_t, int32_t> target = g_shadow;
            for (const auto& p : g_pending) {
                target[p.hash] += p.isDelete ? -1 : 1;
            }
            // union walk
            std::vector<uint8_t> scratch;
            for (const auto& [h, c] : cur) {
                int32_t want = c - (target.count(h) ? target[h] : 0);
                while (want-- > 0) {
                    // find a serialized row with this hash for the blob
                    const int32_t idx = ResolveIndexByHash(h);
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
            // Persistent mismatch after reconcile = a real bug (dead-guard
            // lesson: every exit instrumented).
            const int32_t post = SumShadow() + PendingNetAll();
            if (post != n)
                UE_LOGW("meadow_db: shadow/store mismatch persists after reconcile "
                        "(store %d vs shadow+pending %d)", n, post);
            else
                LogDigest("poll-reconcile");

            // ORDER (v120, per-rule-1): a reorder of the COMMON elements = a
            // sortSignal move (or drift). Host broadcasts its canonical; a
            // client sends its order to the HOST only (host-canonical lane).
            // Send failure leaves the baseline old -> g_orderPending retries.
            // FIFO guard (order audit HIGH-1): an order line referencing a
            // hash whose append/delete is still PENDING would overtake it on
            // the wire (the lane pin orders only what was actually handed to
            // GNS) -> the receiver skips the unknown hash and the late append
            // lands at the tail = permanent per-peer order divergence. Defer
            // every order send until the pending queue is empty.
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

    // tombstone retry (a matching row may have appeared)
    for (auto it = g_tombs.begin(); it != g_tombs.end();) {
        if (ApplyDeleteByHash(it->hash)) it = g_tombs.erase(it);
        else ++it;
    }

    RetryPending(s);
    SelftestTick(s);

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
    if (!ApplyDeleteByHash(p.contentHash))
        g_tombs.push_back({p.contentHash, Clock::now() + kTombstoneTTL});
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
    if (!HashStore(snap.counts, nullptr)) {
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
        // No snapshot = no save baseline knowledge; seeding the full DB would
        // duplicate the joiner's save copy. Loud ONLY at the slot's first
        // replay -- ConnectReplayForSlot re-fires on every mid-session
        // world-change re-announce, where a consumed snapshot is normal
        // (audit fix 2: a Warn per cave travel would bury real join failures).
        if (!g_seededOnce[peerSlot])
            UE_LOGW("meadow_db: no join snapshot for slot %d -- seed skipped", peerSlot);
        return;
    }
    g_seededOnce[peerSlot] = true;
    if (!MS::EnsureResolved()) { snap.valid = false; return; }

    std::map<uint64_t, int32_t> cur;
    std::vector<uint64_t> seq;
    if (!HashStore(cur, &seq)) { snap.valid = false; return; }

    // Mask criterion (design R12): a pending born BEFORE the snapshot has its
    // effect inside the save the joiner loaded -- the retry must skip this
    // slot; younger pendings deliver via the retry and stay OUT of the seed
    // (unmaskedPendingNet below).
    const uint32_t bit = 1u << peerSlot;
    std::map<uint64_t, int32_t> unmaskedNet;
    for (auto& p : g_pending) {
        if (p.bornOp <= snap.opAt) p.excludeMask |= bit;
        else unmaskedNet[p.hash] += p.isDelete ? -1 : 1;
    }

    // seedDelta(h) = cur - snap - unmaskedPendingNet, per hash over the union.
    std::map<uint64_t, int32_t> delta = cur;
    for (const auto& [h, c] : snap.counts) delta[h] -= c;
    for (const auto& [h, c] : unmaskedNet) delta[h] -= c;

    int sentA = 0, sentD = 0;
    for (const auto& [h, d] : delta) {
        if (d > 0) {
            const int32_t idx = ResolveIndexByHash(h);
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
    // The canonical ORDER always rides after the deltas (same FIFO lane): the
    // joiner's save order may predate in-window moves, and order is now synced
    // state (per-rule-1 user decision). FIFO guard (audit HIGH-1): with lines
    // still pending, the order would reference undelivered hashes -- defer to
    // the poll's canonical broadcast (it reaches this slot too, post-flush).
    int sentO = 0;
    if (!seq.empty() && g_pending.empty() && SendOrder(s, seq, peerSlot)) sentO = 1;
    else if (!g_pending.empty()) g_orderPending = true;
    g_cSeedLines += static_cast<uint64_t>(sentA + sentD + sentO);
    if (sentA || sentD || sentO)
        UE_LOGI("meadow_db: seed slot=%d +%d/-%d rows%s", peerSlot, sentA, sentD,
                sentO ? " +order" : "");
    snap.valid = false;
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
    g_selftestStage = 0;
    g_selftestHash = 0;
    g_cAppendsSent = g_cDeletesSent = g_cAppendsApplied = g_cDeletesApplied = 0;
    g_cTombConsumed = g_cSeedLines = 0;
    g_cOrderSent = g_cOrderApplied = 0;
}

}  // namespace coop::meadow_db_sync
