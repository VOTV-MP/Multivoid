// coop/interactables/meadow_db_sync.cpp -- see coop/interactables/meadow_db_sync.h. A
// content-hash multiset shadow over the saved signals, taken the first time a world needs it and
// reconciled at the exit of every body that writes the database; id-preserving reflected addSignal and
// removeSignal applies; tombstone counts; a symmetric per-slot join seed, where the seed delta per hash
// is the current count minus the snapshot count minus the unmasked pending net, with op-counter masks.
// An apply and its shadow update are game-thread-atomic per line. The client lane sends nothing until
// its own world-ready announce.

#include "coop/interactables/meadow_db_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/interactables/meadow_db_hash.h"
#include "coop/interactables/signal_wire.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <chrono>
#include <iterator>
#include <map>
#include <vector>

namespace coop::meadow_db_sync {
namespace {

namespace MH = coop::meadow_db_hash;
namespace MS = ue_wrap::meadow_store;
namespace SD = ue_wrap::signal_dynamic;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kRetryInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL   = std::chrono::seconds(20);
constexpr auto kTombstoneTTL  = std::chrono::seconds(20);
// The absolute size bound on the tombstone vector (see OnDelete for why the unmatched case is
// the inserting case). Legitimate tombstones are bounded by the deletes racing an append that
// has not landed yet, a handful within one TTL; 256 is far above that and caps the vector at
// about 4 KB.
constexpr size_t kTombstoneCap = 256;

// The database's writers, from the bytecode of every asset that names it: ui_laptop_C's addSignal (an
// Add), removeSignal (a Remove) and sortSignal (a move, a Remove then an Insert), and the rename window,
// whose ubergraph writes a row's name in place when its button is clicked. saveSlot_C::reset_days clears
// the store too, but only on a save the reset menu loads from disk, never on the live one. Each body's
// entry takes the shadow if this world has none yet, and its exit sends what it changed.
struct Writer { const wchar_t* cls; const wchar_t* fn; };
constexpr Writer kWriters[] = {
    {L"ui_laptop_C", L"addSignal"},
    {L"ui_laptop_C", L"removeSignal"},
    {L"ui_laptop_C", L"sortSignal"},
    {L"ui_signalName_C", L"ExecuteUbergraph_ui_signalName"},
};
constexpr int kTagWriter = 0x4D445742;  // 'MDWB'
bool g_writersWatched = false;  // registered, once a process
bool g_writersLive = false;     // their names resolved at the gate

// The lane's own verb calls in progress (game thread). Its applies run the same verbs a player does, and
// keep the shadow themselves in the same callback, so a writer's exit inside one sends nothing. A scope
// in our own frame, which every path out of the call unwinds.
int g_applyDepth = 0;
struct ApplyScope {
    ApplyScope() { ++g_applyDepth; }
    ~ApplyScope() { --g_applyDepth; }
    ApplyScope(const ApplyScope&) = delete;
    ApplyScope& operator=(const ApplyScope&) = delete;
};

// The shadow: the broadcast-acknowledged multiset, of the world whose gamemode `g_primedIn` holds.
std::map<uint64_t, int32_t> g_shadow;   // ContentHash -> count
bool g_primed = false;
bool g_primeMissed = false;             // a writer entered while the database could not be read
ue_wrap::CachedObjRef g_primedIn;
uint64_t g_opCounter = 0;               // GT-monotonic line-author counter

// Order as state: the sort order is synced. The baseline is the sequence every peer will hold once
// the lines authored so far reach it -- each receiver takes a delete's first row of that content out and
// puts an append at the tail -- so an order line goes exactly when this peer's own order differs from it.
// Convergence is host-canonical: client lines are host-terminal, the host applies last-writer-wins and
// broadcasts its canonical, and clients apply host-authored lines only.
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
Clock::time_point g_nextRetry{};

// Session totals, logged once a minute while any is not zero.
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

void EraseFirst(std::vector<uint64_t>& seq, uint64_t hash) {
    for (auto it = seq.begin(); it != seq.end(); ++it) {
        if (*it == hash) { seq.erase(it); return; }
    }
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

// The shadow is taken of one world's database, the first time the lane needs it there: a writer's
// entry, a line to apply, a retry. A new world -- a travel, a rehost's load -- takes it again, and the
// lines still waiting in the old one go with it; a joiner's seed carries what they would have.
bool EnsurePrimed() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (g_primed && gm && g_primedIn.Is(gm)) return true;
    if (g_primed) {
        UE_LOGI("meadow_db: a new world -- the shadow and %zu waiting line(s) dropped",
                g_pending.size() + g_tombs.size());
        g_primed = false;
        g_shadow.clear();
        g_pending.clear();
        g_tombs.clear();
        g_orderBase.clear();
        g_orderPending = false;
    }
    if (!gm || !MS::EnsureResolved()) return false;
    if (!MH::HashStore(g_shadow, &g_orderBase)) return false;
    g_primedIn.Set(gm);
    g_primed = true;
    UE_LOGI("meadow_db: shadow primed at %zu row(s)", g_orderBase.size());
    LogDigest("prime");
    return true;
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
// pending. The shadow advances only on success; the order baseline at once, since every
// peer will apply the line in its turn.
void AuthorLine(coop::net::Session* s, uint64_t hash, bool isDelete,
                const std::vector<uint8_t>& blob) {
    ++g_opCounter;
    if (isDelete) EraseFirst(g_orderBase, hash);
    else g_orderBase.push_back(hash);
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

// The order line, when the order every peer will hold is not this peer's: the host's goes to every
// peer, a client's to the host, which answers with its own. The FIFO guard: an order line that
// references a hash whose append or delete is still pending would overtake it on the wire (the lane
// pin orders only what was handed to the transport), the receiver would skip the unknown hash and the
// late append would land at the tail, a permanent per-peer divergence -- so it waits for the queue to
// empty. A send failure leaves the baseline old, so the retry sends it.
void SendOrderIfDiffers(coop::net::Session* s, const std::vector<uint64_t>& seq) {
    if (seq == g_orderBase) {
        g_orderPending = false;
    } else if (!s || !s->connected()) {
        g_orderBase = seq;  // nobody to tell; a joiner gets the save and the seed
        g_orderPending = false;
    } else if (!g_pending.empty() || !CanSend() || !SendOrder(s, seq, IsHost() ? -1 : 0)) {
        g_orderPending = true;
    } else {
        g_orderBase = seq;
        g_orderPending = false;
        ++g_cOrderSent;
        UE_LOGI("meadow_db: order %s (n=%zu)", IsHost() ? "sent to every peer" : "sent to the host", seq.size());
    }
}

// What the writer bodies changed since the shadow, sent: each row that went a delete, each that came an
// append in the database's own order, then the order line when the order every peer will hold after
// those lines is not this peer's. Game thread.
void Reconcile(coop::net::Session* s) {
    if (!EnsurePrimed()) return;
    std::map<uint64_t, int32_t> cur;
    std::vector<uint64_t> seq;
    if (!MH::HashStore(cur, &seq)) {
        UE_LOGW("meadow_db: the database could not be read after a writer -- its change goes with the next one");
        return;
    }
    // What every peer will hold: the shadow with the lines still waiting to go.
    std::map<uint64_t, int32_t> target = g_shadow;
    for (const auto& p : g_pending) target[p.hash] += p.isDelete ? -1 : 1;
    static const std::vector<uint8_t> kNoBlob;
    for (const auto& [h, c] : target) {
        const auto it = cur.find(h);
        for (int32_t drop = c - (it == cur.end() ? 0 : it->second); drop > 0; --drop)
            AuthorLine(s, h, /*isDelete=*/true, kNoBlob);
    }
    std::map<uint64_t, int32_t> want;
    for (const auto& [h, c] : cur) {
        const auto it = target.find(h);
        const int32_t w = c - (it == target.end() ? 0 : it->second);
        if (w > 0) want[h] = w;
    }
    for (size_t i = 0; i < seq.size() && !want.empty(); ++i) {
        const auto it = want.find(seq[i]);
        if (it == want.end()) continue;
        SD::Row r;
        if (!MS::ReadRow(static_cast<int32_t>(i), r)) {
            UE_LOGW("meadow_db: row %zu unreadable after a writer -- its append goes with the next one", i);
            continue;
        }
        AuthorLine(s, seq[i], /*isDelete=*/false, coop::signal_wire::Serialize(r, /*adopt=*/false));
        if (--it->second == 0) want.erase(it);
    }
    // A persistent mismatch after the reconcile is a real bug; every exit is instrumented.
    const int32_t post = SumShadow() + PendingNetAll();
    if (post != static_cast<int32_t>(seq.size()))
        UE_LOGW("meadow_db: shadow/store mismatch persists after reconcile "
                "(store %zu vs shadow+pending %d)", seq.size(), post);
    else
        LogDigest("writer");
    SendOrderIfDiffers(s, seq);
}

// Outside a session the gate runs only for a dev probe that holds it; the lane takes no part there.
coop::net::Session* RunningSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running()) ? s : nullptr;
}

sg::Verdict OnWriterPre(const sg::Call&) {
    if (g_applyDepth == 0 && RunningSession()) g_primeMissed = !EnsurePrimed();
    return sg::Verdict::Run;
}

void OnWriterPost(const sg::Call&) {
    if (g_applyDepth > 0) return;  // the lane's own apply keeps its shadow in the same callback
    coop::net::Session* s = RunningSession();
    if (!s) return;
    if (g_primeMissed) {
        // Whatever it changed is in the lane's first picture of the database, and no peer is told.
        g_primeMissed = false;
        UE_LOGW("meadow_db: a writer ran while the database could not be read -- its change is not sent");
        return;
    }
    Reconcile(s);
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
    SD::Row row;
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(blob, row, adopt)) {
        UE_LOGW("meadow_db: malformed row blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!EnsurePrimed() || !MS::Widget()) {
        UE_LOGW("meadow_db: append from slot %u dropped -- store/widget unresolved "
                "(world transition?)", static_cast<unsigned>(senderSlot));
        return;
    }
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
    bool added;
    {
        ApplyScope scope;
        added = MS::ApplyAddSignal(row);
    }
    if (added) {
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
    if (!EnsurePrimed() || !MS::Widget()) return false;
    const int32_t idx = MH::IndexOf(hash);
    if (idx < 0) return false;
    bool removed;
    {
        ApplyScope scope;
        removed = MS::ApplyRemoveSignal(idx);
    }
    if (!removed) return false;
    auto it = g_shadow.find(hash);
    if (it != g_shadow.end() && --it->second <= 0) g_shadow.erase(it);
    EraseFirst(g_orderBase, hash);  // the first row of that content, as every receiver takes it
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
    if (!EnsurePrimed() || !MS::Widget()) {
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
        {
            ApplyScope scope;
            if (!MS::ReorderRows(perm.data(), n)) {
                UE_LOGW("meadow_db: order permute failed (n=%d) -- dropped", n);
                return;
            }
            MS::ApplyGenSignalList();
        }
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
        // guard as a writer's order: never broadcast an order that references a still-pending
        // append or delete.
        auto* s = g_session.load(std::memory_order_acquire);
        const bool ok = (s && s->connected())
                            ? (g_pending.empty() && SendOrder(s, seq, -1))
                            : true;
        if (ok) {
            g_orderBase = seq;
            g_orderPending = false;
            ++g_cOrderSent;
        } else {
            g_orderPending = true;  // baseline stays old -> the retry sends it
        }
    } else {
        g_orderBase = seq;
        g_orderPending = false;
    }
}

void WatchUntilLive() {
    sg::ResolvePendingNames();
    for (const Writer& w : kWriters)
        if (!sg::ClassNameWatchLive(w.cls, w.fn, kTagWriter)) return;
    g_writersLive = true;
    UE_LOGI("meadow_db: the database's %zu writers are watched at the script-body gate", std::size(kWriters));
}

void LogTotals(Clock::time_point now) {
    if (now < g_nextStats) return;
    g_nextStats = now + std::chrono::seconds(60);
    if (!(g_cAppendsSent || g_cDeletesSent || g_cAppendsApplied || g_cDeletesApplied || g_cTombConsumed ||
          g_cSeedLines || g_cOrderSent || g_cOrderApplied || !g_pending.empty()))
        return;
    UE_LOGI("meadow_db: session totals sent=%llu/%llu applied=%llu/%llu "
            "order=%llu/%llu tombConsumed=%llu seed=%llu pending=%zu tombs=%zu",
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

}  // namespace

// Entry points.

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_writersWatched) return;
    bool ok = true;
    for (const Writer& w : kWriters)
        ok = sg::WatchClassName(w.cls, w.fn, kTagWriter, &OnWriterPre, &OnWriterPost) && ok;
    g_writersWatched = ok;
    if (!ok) UE_LOGW("meadow_db: a writer's watch was refused -- a change it makes is not sent");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_writersLive) WatchUntilLive();
    const auto now = Clock::now();
    LogTotals(now);
    // The retries -- a line whose send failed or waits for this client's world-ready, a delete that
    // came before its row, an order held behind them, a row's chunks in flight -- once a second, and
    // only while one of them waits.
    if (g_pending.empty() && g_tombs.empty() && !g_orderPending && g_assembler.Idle() && g_orderAsm.Idle())
        return;
    if (now < g_nextRetry) return;
    g_nextRetry = now + kRetryInterval;
    g_assembler.Sweep(now, kAssemblyTTL);
    g_orderAsm.Sweep(now, kAssemblyTTL);
    for (auto it = g_tombs.begin(); it != g_tombs.end();) {
        if (now >= it->until) {
            UE_LOGW("meadow_db: delete for hash %016llx expired unmatched",
                    static_cast<unsigned long long>(it->hash));
            it = g_tombs.erase(it);
        } else ++it;
    }
    if (!EnsurePrimed()) return;
    for (auto it = g_tombs.begin(); it != g_tombs.end();) {
        if (ApplyDeleteByHash(it->hash)) it = g_tombs.erase(it);
        else ++it;
    }
    RetryPending(s);
    if (g_orderPending && g_pending.empty()) {
        std::map<uint64_t, int32_t> cur;
        std::vector<uint64_t> seq;
        if (MH::HashStore(cur, &seq)) SendOrderIfDiffers(s, seq);
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
    // still pending the order would reference undelivered hashes, so defer to the retry's canonical
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
    // A recycled slot must not finish the departed peer's half-sent row or order.
    g_assembler.ClearSlot(static_cast<uint8_t>(peerSlot));
    g_orderAsm.ClearSlot(static_cast<uint8_t>(peerSlot));
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
    g_primeMissed = false;
    g_primedIn.Reset();
    g_nextSeq = 1;
    g_nextRetry = {};
    g_opCounter = 0;
    g_cAppendsSent = g_cDeletesSent = g_cAppendsApplied = g_cDeletesApplied = 0;
    g_cTombConsumed = g_cSeedLines = 0;
    g_cOrderSent = g_cOrderApplied = 0;
}

}  // namespace coop::meadow_db_sync
