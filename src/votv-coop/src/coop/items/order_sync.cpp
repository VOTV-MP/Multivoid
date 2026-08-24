// coop/order_sync.cpp -- see coop/items/order_sync.h. Delivery-drone ECONOMY: client->host order
// forward, and (v136) the host-authoritative CHARGE for it.
//
// CLIENT: polls saveSlot.orders.Num (a WATERMARK -- the commit verb is BP-internal/unobservable);
// on an increment reads the new order's list_store ROW NAMES, chunks them to fit
// kMaxReliablePayload, forwards to the host, and resets the mirror drone's self-takeoff (RE Q2). It
// never mutates its own orders array (removeOrderCart needs the laptop UI and pops index 0
// unconditionally; the client is ephemeral and never saves -- save_block holds disableSave -- so the
// retained local entries are harmless and leak-free, and the panel they feed is security A46).
//
// HOST: assembles chunks per (slot, orderId), then PRICES the order from its own store table, checks
// its OWN balance, commits via the native makeAnOrder, confirms the commit by an orders.Num +1 edge,
// and only then charges. A refusal tells the ordering client, corrects its balance, and restores its
// cart. All GT-only.

#include "coop/items/order_sync.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/world/balance_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/order_economy.h"
#include "ue_wrap/world/store_catalog.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::order_sync {
namespace {

namespace OE  = ue_wrap::order_economy;
namespace SC  = ue_wrap::store_catalog;
namespace E   = ue_wrap::economy;
namespace net = coop::net;

std::atomic<net::Session*> g_session{nullptr};

// Per-item wire prefix: nameLen(1). v136: price/size/category/objLen are GONE from the wire.
constexpr int      kItemFixed         = 1;
constexpr uint64_t kAssemblyTimeoutMs = 15000;  // drop a partial order whose chunks stop arriving
constexpr uint64_t kPendingTimeoutMs  = 60000;  // drop a completed order the world never lets us commit
constexpr size_t   kMaxAssembly       = 16;     // cap concurrent partial orders, PER SLOT (see below)
constexpr size_t   kMaxPending        = 32;     // cap queued-for-commit orders, PER SLOT
constexpr int      kMaxCommitTries    = 3;      // a commit that keeps failing while committable -> drop
constexpr size_t   kMaxInFlight       = 32;     // client: remembered orders awaiting a verdict

// ---- client forward state (game thread only) ----
int32_t  g_forwardedThrough = -1;  // watermark: orders.Num value we've forwarded up to (-1 = unprimed)
uint32_t g_orderIdCounter   = 0;   // monotonic id per forwarded order (uniqueness within this sender)

// What we sent, per orderId, so a REFUSAL can put the cart back. Dropped on the first verdict and
// bounded: it is the client's only per-session accumulator on this path, and an unbounded one would
// be the same defect as the orders array it exists to avoid reading.
std::unordered_map<uint32_t, std::vector<std::wstring>> g_inFlight;

// ---- host assembly state (game thread only) ----
struct Assembly {
    uint16_t totalItems = 0;
    std::vector<std::wstring> rowNames;
    uint64_t lastMs = 0;
};
struct Pending {
    OE::OrderData od;
    uint32_t orderId = 0;
    uint64_t firstMs = 0;
    int      tries   = 0;
};

// PER SLOT, and that is load-bearing (security A49). These used to be two globals keyed by
// (senderSlot<<32)|orderId with only a session-wide reset -- but a client leaving is NOT a session
// teardown on the host, slots recycle LOWEST-FREE with no absence between occupants, and a fresh
// occupant's g_orderIdCounter restarts at 1. So a departed peer's partial assembly absorbed the
// newcomer's chunks and a departed peer's completed order committed under the newcomer's name.
// Harmless while the goods were free; a CHARGE against the shared balance attributed to someone who
// never ordered, once they are not. PerSlotState registers its own clear on the occupant-change edge
// -- the one edge no per-slot boolean can observe -- so this is coverage by construction rather than
// a check somebody has to remember. It also makes the two caps PER SLOT, so one client flooding
// partial orders can no longer deny every other peer's orders for the assembly timeout.
struct SlotOrders {
    std::unordered_map<uint32_t, Assembly> assembly;
    std::vector<Pending>                   pending;
};
coop::roster_ledger::PerSlotState<SlotOrders> g_bySlot;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host rolls the delivery ETA itself -- `[V]` the game's own Button_order does
// RandomFloatInRange(120,180), and shared-world RNG is host-authoritative
// (docs/COOP_RNG_AUTHORITY.md). The client's number is not on the wire.
float RollEta() {
    static std::mt19937 s_rng{static_cast<uint32_t>(NowMs())};
    static std::uniform_real_distribution<float> s_dist(120.f, 180.f);
    return s_dist(s_rng);
}

// Reset all per-session state. Called by BOTH Install (session start) and OnDisconnect (teardown)
// so a reconnect that re-Installs without a preceding OnDisconnect can't retain a stale client
// watermark and double-forward last session's queued orders (audit I-2). One path, RULE 2.
void ResetState() {
    g_forwardedThrough = -1;
    g_orderIdCounter   = 0;
    g_inFlight.clear();
    for (int i = 0; i < g_bySlot.size(); ++i) {
        g_bySlot[i].assembly.clear();
        g_bySlot[i].pending.clear();
    }
}

// list_store keys are ASCII identifiers -> narrow/widen losslessly (non-ASCII -> '?', which simply
// fails the host's catalog lookup and refuses the order, loudly, rather than silently mis-resolving).
std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}
std::wstring WidenAscii(const uint8_t* p, int n) {
    std::wstring w;
    w.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w.push_back(static_cast<wchar_t>(p[i]));
    return w;
}

// ---- CLIENT: serialize + chunk + forward one order ----
void ForwardOrder(net::Session* s, int32_t idx) {
    OE::OrderData od;
    if (!OE::ReadOrder(idx, od)) {
        UE_LOGW("order_sync: ReadOrder(%d) failed -- skip", idx);
        return;
    }
    const size_t total = od.rowNames.size();
    if (total > static_cast<size_t>(net::kMaxOrderItems)) {
        // Do NOT truncate. The client has already been debited locally for ALL of these, so
        // forwarding the first 64 would have the host price and deliver a DIFFERENT basket than the
        // one the player paid for, silently. `[V]` The game's own cart caps at 50 ("<n>/50" in the
        // shop UI), so a legitimate order can never reach this; refusing the whole thing is correct
        // for the only case that can, and the player is told rather than left guessing.
        UE_LOGW("order_sync: order idx=%d has %zu items > cap %d -- NOT forwarding (a partial basket "
                "would be priced and delivered differently from the one that was paid for)",
                idx, total, net::kMaxOrderItems);
        coop::peer_action_feed::AnnounceDirect(
            static_cast<uint8_t>(coop::players::Registry::Get().LocalPeerId()),
            L"could not order: too many items in one order");
        return;
    }
    const uint32_t orderId = ++g_orderIdCounter;

    std::vector<std::wstring> sent;
    sent.reserve(total);
    size_t i = 0;
    int chunks = 0;
    while (i < total) {
        uint8_t buf[net::kMaxReliablePayload];
        int pos = static_cast<int>(sizeof(net::OrderRequestHeader));
        const size_t chunkStart = i;
        uint16_t chunkCount = 0;
        while (i < total) {
            std::string name = NarrowAscii(od.rowNames[i]);
            if (name.size() > static_cast<size_t>(net::kMaxOrderRowName))
                name.resize(static_cast<size_t>(net::kMaxOrderRowName));
            const int itemSize = kItemFixed + static_cast<int>(name.size());
            if (pos + itemSize > net::kMaxReliablePayload) break;  // this chunk is full
            buf[pos++] = static_cast<uint8_t>(name.size());
            std::memcpy(buf + pos, name.data(), name.size());
            pos += static_cast<int>(name.size());
            sent.push_back(od.rowNames[i]);
            ++chunkCount;
            ++i;
        }
        if (chunkCount == 0) {
            // A single item that cannot fit even an empty chunk -- impossible given the caps
            // (header 12 + 1 + name<=96 = 109 <= 228). ABORT the whole forward rather than skip the
            // item: `totalItems` in the chunks already sent counts it, so skipping would leave the
            // host assembling an order that can never complete -- it would sit until the assembly
            // timeout and be dropped WITHOUT a refusal (only `pending` entries produce those),
            // leaving the client debited with no verdict and no cart restore.
            UE_LOGE("order_sync: item %zu cannot be chunked -- ABORTING order id=%u (already sent "
                    "%d chunk(s); the host will time the partial assembly out)", i, orderId, chunks);
            return;
        }
        net::OrderRequestHeader h{};
        h.orderId    = orderId;
        h.totalItems = static_cast<uint16_t>(total);
        h.baseIndex  = static_cast<uint16_t>(chunkStart);
        h.chunkItems = chunkCount;
        h._pad       = 0;
        std::memcpy(buf, &h, sizeof(h));
        s->SendReliable(net::ReliableKind::OrderRequest, buf, pos);
        ++chunks;
    }

    if (!sent.empty()) {
        // Evict the OLDEST (lowest orderId -- the counter is monotonic), not the whole map: wiping
        // it would strand the cart-restore data of every other order still awaiting a verdict.
        while (g_inFlight.size() >= kMaxInFlight) {
            auto oldest = g_inFlight.begin();
            for (auto it = g_inFlight.begin(); it != g_inFlight.end(); ++it)
                if (it->first < oldest->first) oldest = it;
            g_inFlight.erase(oldest);
        }
        g_inFlight[orderId] = std::move(sent);
    }
    UE_LOGI("order_sync: forwarded order idx=%d id=%u items=%zu in %d chunk(s)", idx, orderId, total,
            chunks);
}

void TickClient(net::Session* s) {
    static bool s_tickLogged = false;
    if (!s_tickLogged) { s_tickLogged = true; UE_LOGI("order_sync: client Tick active (polling saveSlot.orders)"); }
    const int32_t count = OE::OrderCount();
    if (count < 0) return;  // store not resolved yet (booting / at the menu)
    if (g_forwardedThrough < 0) {
        g_forwardedThrough = count;  // prime: pre-existing orders are old local save state, not forwarded
        UE_LOGI("order_sync: client watermark primed at orders.Num=%d (pre-existing not forwarded)", count);
        return;
    }
    if (count < g_forwardedThrough) { g_forwardedThrough = count; return; }  // queue shrank -- re-sync
    if (count == g_forwardedThrough) return;
    for (int32_t idx = g_forwardedThrough; idx < count; ++idx) ForwardOrder(s, idx);
    g_forwardedThrough = count;
    OE::QuietLocalDrone();  // reset the mirror drone's self-takeoff once after forwarding (RE Q2)
}

// ---- HOST: tell one client its order was not performed, and undo what its local run already did ----
void Refuse(net::Session* s, uint8_t slot, uint32_t orderId, net::OrderRefusedReason reason,
            const wchar_t* why) {
    UE_LOGW("order_sync: REFUSING order id=%u from slot %u -- %ls", orderId, slot, why);
    net::OrderRefusedPayload p{};
    p.orderId = orderId;
    p.reason  = static_cast<uint8_t>(reason);
    s->SendReliableToSlot(slot, net::ReliableKind::OrderRefused, &p, sizeof(p));
    // The refusal moved NOTHING on the host, so the change-polled BalanceSync broadcast can never
    // fire -- and the client already debited itself through an EX_LocalVirtualFunction we cannot
    // suppress. Its balance is corrected by saying so directly.
    coop::balance_sync::SendCurrentToSlot(static_cast<int>(slot));
}

// Price + commit + charge ONE completed order. Returns true when the entry is finished with
// (committed, or refused for good) and should be dropped.
bool ResolveOne(net::Session* s, uint8_t slot, Pending& pe) {
    if (!SC::Ready()) {
        // Fail CLOSED. The catalog is the only thing that can price this, and guessing is the one
        // outcome worse than refusing.
        Refuse(s, slot, pe.orderId, net::OrderRefusedReason::NoCatalog,
               L"the host's store catalog is unusable");
        return true;
    }

    // Resolve + sum from the HOST's own table. An intent may name WHAT, never WHAT IT COSTS.
    int64_t total = 0;
    for (const std::wstring& name : pe.od.rowNames) {
        const SC::Row* row = SC::Find(name);
        if (!row) {
            UE_LOGW("order_sync: slot %u ordered unknown store row '%ls'", slot, name.c_str());
            Refuse(s, slot, pe.orderId, net::OrderRefusedReason::UnknownItem,
                   L"an item is not in the host's store table");
            return true;
        }
        total += row->price;
    }

    // Read the balance HERE, per order -- NOT once per pass. Two orders draining in the same pass
    // would otherwise both price against the pre-debit value and overspend.
    int32_t points = 0;
    if (!E::ReadPoints(&points)) return false;  // world still resolving -- retry next tick
    if (static_cast<int64_t>(points) < total) {
        // CLIENT-SCOPED BY CONSTRUCTION: the host's own orders never reach this code at all, so this
        // is not a rule the host applies to itself (docs/security/THREAT_MODEL.md -- the host may
        // cheat and we relay it). `[V]` The game's own gate does the same test at Button_order @5990,
        // silently; what the client gets extra is a reason, because its local debit already happened.
        Refuse(s, slot, pe.orderId, net::OrderRefusedReason::Unaffordable,
               L"the shared balance is short");
        return true;
    }

    if (!OE::CanCommit()) return false;  // world still loading -- retry (kPendingTimeoutMs bounds it)

    ++pe.tries;
    const int32_t before = OE::OrderCount();
    const bool dispatched = OE::CommitOrder(pe.od, RollEta(), /*automatic*/ true);
    const int32_t after = OE::OrderCount();

    // `dispatched` only says ProcessEvent ran. The ORDER is what we charge for, so the post-condition
    // is the artifact: exactly one new row in saveSlot.orders. `[V]` That edge is exact -- nothing
    // pops synchronously inside the commit (drone::checkOrders has no removeOrderCart/Array_Remove).
    if (!dispatched || before < 0 || after != before + 1) {
        UE_LOGW("order_sync: commit did not queue an order (dispatch=%d orders %d -> %d, try %d)",
                dispatched ? 1 : 0, before, after, pe.tries);
        if (pe.tries >= kMaxCommitTries) {
            Refuse(s, slot, pe.orderId, net::OrderRefusedReason::CommitFailed,
                   L"the order could not be placed");
            return true;
        }
        return false;  // retry
    }

    // Charge AFTER the goods are confirmed queued, and synchronously. Deliberately NOT
    // balance_sync::CreditLocal: its host branch defers through GT::Post, and a deferred debit would
    // let a second order in this same drain pass price against a balance that has not moved yet.
    // The host's next balance poll broadcasts the new value to everyone, so no direct send is needed
    // on the success path.
    if (!E::AddPoints(-static_cast<int32_t>(total)))
        UE_LOGE("order_sync: order committed but AddPoints(%lld) FAILED -- the group got goods it "
                "was not charged for", static_cast<long long>(-total));
    else
        UE_LOGI("order_sync: committed slot %u order id=%u (%zu items) and charged %lld from the "
                "shared balance (%d -> %d)", slot, pe.orderId, pe.od.rowNames.size(),
                static_cast<long long>(total), points, points - static_cast<int32_t>(total));
    return true;
}

void TickHost(net::Session* s) {
    const uint64_t now = NowMs();
    for (int slot = 0; slot < g_bySlot.size(); ++slot) {
        SlotOrders& so = g_bySlot[slot];
        for (size_t i = 0; i < so.pending.size();) {
            Pending& pe = so.pending[i];
            bool done = ResolveOne(s, static_cast<uint8_t>(slot), pe);
            if (!done && now - pe.firstMs > kPendingTimeoutMs) {
                UE_LOGW("order_sync: pending order undeliverable for %llums -- dropping",
                        static_cast<unsigned long long>(now - pe.firstMs));
                Refuse(s, static_cast<uint8_t>(slot), pe.orderId,
                       net::OrderRefusedReason::CommitFailed, L"the order timed out unplaced");
                done = true;
            }
            if (done) so.pending.erase(so.pending.begin() + static_cast<long>(i));
            else ++i;
        }
        // Evict partial assemblies whose remaining chunks never arrived.
        for (auto it = so.assembly.begin(); it != so.assembly.end();) {
            if (now - it->second.lastMs > kAssemblyTimeoutMs) {
                UE_LOGW("order_sync: dropping stale partial order assembly (slot %d)", slot);
                it = so.assembly.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ---- CLIENT: a refusal came back ----
const wchar_t* ReasonText(uint8_t reason) {
    switch (static_cast<net::OrderRefusedReason>(reason)) {
        case net::OrderRefusedReason::UnknownItem:  return L"an item was not in the host's store";
        case net::OrderRefusedReason::Unaffordable: return L"there were not enough credits";
        case net::OrderRefusedReason::NoCatalog:    return L"the host could not read its store";
        case net::OrderRefusedReason::CommitFailed: return L"the delivery could not be placed";
    }
    return L"the host refused it";
}

void OnRefused(const void* payload, int len) {
    if (!payload || len < static_cast<int>(sizeof(net::OrderRefusedPayload))) {
        UE_LOGW("order_sync: OrderRefused too short (%d) -- drop", len);
        return;
    }
    net::OrderRefusedPayload p{};
    std::memcpy(&p, payload, sizeof(p));

    std::vector<std::wstring> rows;
    auto it = g_inFlight.find(p.orderId);
    if (it != g_inFlight.end()) {
        rows = std::move(it->second);
        g_inFlight.erase(it);
    }

    // Say it, then put the cart back. `[V]` The base game's own affordability gate pops BEFORE
    // Array_Clear(cart), so a refused purchase leaves the cart intact -- our refusal arrives after
    // the local run already cleared it, and not restoring would invent a punishment SP lacks.
    const std::wstring line = std::wstring(L"could not order: ") + ReasonText(p.reason);
    coop::peer_action_feed::AnnounceDirect(
        static_cast<uint8_t>(coop::players::Registry::Get().LocalPeerId()), line);
    const int32_t restored = OE::RestoreCartItems(rows);
    UE_LOGW("order_sync: order id=%u REFUSED by the host (%ls); %d of %zu item(s) restored to the "
            "cart", p.orderId, ReasonText(p.reason), restored, rows.size());
}

}  // namespace

void Install(net::Session* session) {
    // NOTE: Install is the per-net-pump-tick idempotent "ensure" path (like every sync subsystem's
    // Install), NOT a once-per-session call -- so it must NOT reset state here (that would re-prime
    // the client watermark every tick and never forward). Per-session reset lives in OnDisconnect,
    // which net_pump calls on every session-teardown edge (the invariant all 10 sibling subsystems
    // rely on). (Reverts a wrong audit-I-2 suggestion made without the per-tick call-site context.)
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == net::Role::Host) TickHost(s);
    else                              TickClient(s);
}

void OnReliable(const void* payload, int len, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != net::Role::Host) return;  // only the host ingests orders
    if (!payload || len < static_cast<int>(sizeof(net::OrderRequestHeader))) {
        UE_LOGW("order_sync: OrderRequest too short (%d) -- drop", len);
        return;
    }
    net::OrderRequestHeader h{};
    std::memcpy(&h, payload, sizeof(h));
    if (h.totalItems == 0 || h.totalItems > net::kMaxOrderItems) {
        UE_LOGW("order_sync: OrderRequest bad totalItems=%u -- drop", h.totalItems);
        return;
    }
    if (h.chunkItems == 0 ||
        static_cast<int>(h.baseIndex) + static_cast<int>(h.chunkItems) > static_cast<int>(h.totalItems)) {
        UE_LOGW("order_sync: OrderRequest bad chunk range (base=%u count=%u total=%u) -- drop",
                h.baseIndex, h.chunkItems, h.totalItems);
        return;
    }

    const uint8_t* p   = static_cast<const uint8_t*>(payload) + sizeof(h);
    const uint8_t* end = static_cast<const uint8_t*>(payload) + len;
    std::vector<std::wstring> chunkNames;
    chunkNames.reserve(h.chunkItems);
    for (uint16_t k = 0; k < h.chunkItems; ++k) {
        if (p + kItemFixed > end) { UE_LOGW("order_sync: OrderRequest truncated item -- drop"); return; }
        const uint8_t nameLen = *p++;
        if (nameLen == 0 || nameLen > net::kMaxOrderRowName || p + nameLen > end) {
            UE_LOGW("order_sync: OrderRequest bad nameLen=%u -- drop", nameLen);
            return;
        }
        chunkNames.push_back(WidenAscii(p, nameLen));
        p += nameLen;
    }

    SlotOrders& so = g_bySlot[static_cast<int>(senderSlot)];
    auto itA = so.assembly.find(h.orderId);
    if (itA == so.assembly.end()) {
        if (h.baseIndex != 0) {
            UE_LOGW("order_sync: first chunk baseIndex=%u != 0 (lost head) -- drop", h.baseIndex);
            return;
        }
        if (so.assembly.size() >= kMaxAssembly) {
            UE_LOGW("order_sync: slot %u assembly table full (%zu) -- drop new order", senderSlot,
                    so.assembly.size());
            return;
        }
        Assembly a;
        a.totalItems = h.totalItems;
        a.lastMs     = NowMs();
        itA = so.assembly.emplace(h.orderId, std::move(a)).first;
    }
    Assembly& a = itA->second;
    if (h.baseIndex != static_cast<uint16_t>(a.rowNames.size()) || h.totalItems != a.totalItems) {
        UE_LOGW("order_sync: chunk out-of-order/mismatch (base=%u have=%zu total=%u/%u) -- drop assembly",
                h.baseIndex, a.rowNames.size(), h.totalItems, a.totalItems);
        so.assembly.erase(itA);
        return;
    }
    for (auto& n : chunkNames) a.rowNames.push_back(std::move(n));
    a.lastMs = NowMs();

    if (a.rowNames.size() >= a.totalItems) {
        if (so.pending.size() >= kMaxPending) {
            UE_LOGW("order_sync: slot %u pending-commit queue full (%zu) -- drop completed order",
                    senderSlot, so.pending.size());
            so.assembly.erase(itA);
            return;
        }
        Pending pe;
        pe.od.rowNames = std::move(a.rowNames);
        pe.orderId     = h.orderId;
        pe.firstMs     = NowMs();
        pe.tries       = 0;
        const size_t nItems = pe.od.rowNames.size();
        so.pending.push_back(std::move(pe));
        so.assembly.erase(itA);
        UE_LOGI("order_sync: order assembled (slot=%u id=%u items=%zu) -- queued for pricing",
                senderSlot, h.orderId, nItems);
    }
}

void OnReliableRefused(const void* payload, int len) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == net::Role::Host) return;  // host->client only
    OnRefused(payload, len);
}

void OnDisconnect() {
    ResetState();
}

}  // namespace coop::order_sync
