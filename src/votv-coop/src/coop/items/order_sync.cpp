// coop/items/order_sync.cpp -- see coop/items/order_sync.h. Delivery-drone ECONOMY: a client
// sends its order to the host, and the host performs and charges for it.
//
// CLIENT: the gate, at the laptop's makeAnOrder, reads the order the player placed from the verb's
// own parameter and sends its list_store row names to the host in chunks that fit
// kMaxReliablePayload. The order does not enter the client's own queue (the host's queue, mirrored
// by order_queue_sync, is the only one) and the client's drone is not sent (the host's flies).
//
// HOST: assembles the chunks per (slot, orderId), prices the order from its own store table,
// checks its OWN balance, commits through the native makeAnOrder, confirms by an orders.Num +1
// edge, and only then charges. A refusal tells the ordering client, corrects its balance and
// restores its cart. All game-thread only.

#include "coop/items/order_sync.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/items/order_queue_sync.h"
#include "coop/items/order_rows.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/world/balance_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
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
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
namespace net = coop::net;

std::atomic<net::Session*> g_session{nullptr};

// An item on the wire is its row name (coop/items/order_rows.h). Price, size, category and class are
// not on the wire; the host resolves all of them from the row name in its own table.
constexpr uint64_t kAssemblyTimeoutMs = 15000;  // drop a partial order whose chunks stop arriving
constexpr uint64_t kPendingTimeoutMs  = 60000;  // drop a completed order the world never lets us commit
constexpr size_t   kMaxAssembly       = 16;     // cap concurrent partial orders, PER SLOT (see below)
constexpr size_t   kMaxPending        = 32;     // cap queued-for-commit orders, PER SLOT
constexpr int      kMaxCommitTries    = 3;      // a commit that keeps failing while committable -> drop
constexpr size_t   kMaxInFlight       = 32;     // client: remembered orders awaiting a verdict

// ---- client forward state (game thread only) ----
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

// PER SLOT, and that is load-bearing. Slots recycle lowest-free with no absence between
// occupants and a fresh occupant's orderId counter restarts at 1, so state keyed by
// (slot, orderId) alone would let a departed peer's partial assembly absorb the newcomer's
// chunks and its completed order commit -- and be CHARGED to the shared balance -- under the
// newcomer's name. PerSlotState clears on the occupant-change edge, the one edge no per-slot
// boolean can observe. It also makes the two caps per slot, so one client flooding partial
// orders cannot deny every other peer's orders for the assembly timeout.
struct SlotOrders {
    std::unordered_map<uint32_t, Assembly> assembly;
    std::vector<Pending>                   pending;
};
coop::roster_ledger::PerSlotState<SlotOrders> g_bySlot;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host rolls the delivery ETA itself: the game's own Button_order draws
// RandomFloatInRange(120, 180) and shared-world randomness is host-authoritative. The client's
// number is not on the wire.
float RollEta() {
    static std::mt19937 s_rng{static_cast<uint32_t>(NowMs())};
    static std::uniform_real_distribution<float> s_dist(120.f, 180.f);
    return s_dist(s_rng);
}

// Reset all per-session state at teardown (OnDisconnect).
void ResetState() {
    g_orderIdCounter   = 0;
    g_inFlight.clear();
    for (int i = 0; i < g_bySlot.size(); ++i) {
        g_bySlot[i].assembly.clear();
        g_bySlot[i].pending.clear();
    }
}

// ---- CLIENT: serialize + chunk + forward one order ----
void ForwardOrder(net::Session* s, const OE::OrderData& od) {
    const size_t total = od.rowNames.size();
    if (total > static_cast<size_t>(net::kMaxOrderItems)) {
        // Do NOT truncate. The client has already been debited locally for ALL of these, so
        // forwarding the first 64 would have the host price and deliver a DIFFERENT basket than the
        // one the player paid for, silently. The game's own cart caps at 50, so a legitimate order
        // can never reach this; refusing the whole thing is right for the only case that can, and
        // the player is told.
        UE_LOGW("order_sync: an order of %zu items > cap %d -- NOT forwarding (a partial basket "
                "would be priced and delivered differently from the one that was paid for)",
                total, net::kMaxOrderItems);
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
        const uint16_t chunkCount = static_cast<uint16_t>(
            coop::order_rows::Pack(od.rowNames, i, buf, pos, net::kMaxReliablePayload));
        for (uint16_t k = 0; k < chunkCount; ++k) sent.push_back(od.rowNames[i + k]);
        i += chunkCount;
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
    UE_LOGI("order_sync: forwarded order id=%u items=%zu in %d chunk(s)", orderId, total, chunks);
}

// ---- CLIENT: the gate at the order verbs ----
// A client's laptop runs makeAnOrder as single player does -- from its order button, or with
// automatic set from a world event (daynightCycle's daily order, trigger_eventer's gifts) -- and the
// body appends to the local queue (addOrderCart) and sends the drone (sendShop) with orders[0]. On a
// client the queue and the drone are the host's, so at the gate:
//   makeAnOrder entry: a player's order goes to the host as a request; a world event's is left to
//     the host's own copy of the event, which makes it there;
//   addOrderCart entry, from makeAnOrder: refused, so the local queue holds only what the host's
//     queue sends (order_queue_sync);
//   the drone's sendShop entry: refused, whoever calls it.
// makeAnOrder's own body runs on: it clears the cart and its slot widgets, as the button expects.
constexpr const wchar_t* kLaptopClass = L"ui_laptop_C";
constexpr const wchar_t* kDroneClass  = L"drone_C";
constexpr const wchar_t* kMakeVerb    = L"makeAnOrder";
constexpr const wchar_t* kAddVerb     = L"addOrderCart";
constexpr const wchar_t* kSendVerb    = L"sendShop";
constexpr int kTagMake = 0x4F534D4B;  // 'OSMK'
constexpr int kTagAdd  = 0x4F534144;  // 'OSAD'
constexpr int kTagSend = 0x4F535344;  // 'OSSD'
bool g_gateWatched = false;

net::Session* ClientSession() {
    net::Session* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() != net::Role::Host) ? s : nullptr;
}

sg::Verdict OnMakeAnOrderPre(const sg::Call& c) {
    net::Session* s = ClientSession();
    if (!s) return sg::Verdict::Run;
    const int32_t autoOff = R::FindParamOffset(c.function, L"automatic");
    if (autoOff >= 0 && c.locals && c.locals[autoOff]) {
        UE_LOGI("order_sync: a world event's order on this client is not sent -- the host's copy of the "
                "event makes it");
        return sg::Verdict::Run;
    }
    const int32_t itemOff = R::FindParamOffset(c.function, L"NewItem");
    const uint8_t* order = itemOff >= 0 ? sg::OutParamPtr(c, itemOff) : nullptr;  // passed by reference
    if (!order && itemOff >= 0 && c.locals) order = c.locals + itemOff;
    OE::OrderData od;
    if (!order || !OE::ReadOrderAt(order, od)) {
        UE_LOGW("order_sync: this client's order could not be read -- not sent");
        return sg::Verdict::Run;
    }
    ForwardOrder(s, od);
    return sg::Verdict::Run;
}

sg::Verdict OnAddOrderCartPre(const sg::Call& c) {
    // The mirror's own append comes through ProcessEvent, with no calling Blueprint frame.
    if (!ClientSession() || !c.callerFunction) return sg::Verdict::Run;
    return R::NameEquals(R::NameOf(c.callerFunction), kMakeVerb) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnSendShopPre(const sg::Call&) {
    return ClientSession() ? sg::Verdict::Cancel : sg::Verdict::Run;
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
        // CLIENT-SCOPED BY CONSTRUCTION: the host's own orders never reach this code, so this is
        // not a rule the host applies to itself. The game's own gate does the same test at
        // Button_order and says nothing; what the client gets extra is a reason, because its local
        // debit already happened.
        Refuse(s, slot, pe.orderId, net::OrderRefusedReason::Unaffordable,
               L"the shared balance is short");
        return true;
    }

    if (!OE::CanCommit()) return false;  // world still loading -- retry (kPendingTimeoutMs bounds it)

    ++pe.tries;
    const int32_t before = OE::OrderCount();
    const bool dispatched = OE::CommitOrder(pe.od, RollEta(), /*automatic*/ true);
    const int32_t after = OE::OrderCount();

    // `dispatched` only says ProcessEvent ran. The ORDER is what we charge for, so the
    // post-condition is the artifact: exactly one new row in saveSlot.orders. That edge is exact:
    // drone::checkOrders contains no removeOrderCart and no array removal, so nothing pops
    // synchronously inside the commit.
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

    // Say it, then put the cart back. The base game's own affordability gate pops BEFORE it clears
    // the cart, so a refused purchase leaves the cart intact; our refusal arrives after the local
    // run already cleared it, and not restoring would invent a punishment single-player lacks.
    const std::wstring line = std::wstring(L"could not order: ") + ReasonText(p.reason);
    coop::peer_action_feed::AnnounceDirect(
        static_cast<uint8_t>(coop::players::Registry::Get().LocalPeerId()), line);
    const int32_t restored = OE::RestoreCartItems(rows);
    UE_LOGW("order_sync: order id=%u REFUSED by the host (%ls); %d of %zu item(s) restored to the "
            "cart", p.orderId, ReasonText(p.reason), restored, rows.size());
}

}  // namespace

void Install(net::Session* session) {
    // Install is the per-net-pump-tick idempotent "ensure" path, as in every sync subsystem, NOT a
    // once-per-session call, so it must not reset state here. Per-session reset lives in
    // OnDisconnect, which net_pump calls on every session-teardown edge.
    g_session.store(session, std::memory_order_release);
    if (!g_gateWatched)
        g_gateWatched = sg::WatchClassName(kLaptopClass, kMakeVerb, kTagMake, &OnMakeAnOrderPre, nullptr) &&
                        sg::WatchClassName(kLaptopClass, kAddVerb, kTagAdd, &OnAddOrderCartPre, nullptr) &&
                        sg::WatchClassName(kDroneClass, kSendVerb, kTagSend, &OnSendShopPre, nullptr);
    coop::order_queue_sync::Install(session);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == net::Role::Host) TickHost(s);
    coop::order_queue_sync::Tick();
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
    if (!coop::order_rows::Unpack(p, end, h.chunkItems, chunkNames)) {
        UE_LOGW("order_sync: OrderRequest carries a truncated or oversized item -- drop");
        return;
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
    coop::order_queue_sync::OnDisconnect();
}

}  // namespace coop::order_sync
