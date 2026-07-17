// coop/order_sync.cpp -- see coop/order_sync.h. Delivery-drone ECONOMY: client->host order forward.
//
// CLIENT: polls saveSlot.orders.Num (a WATERMARK -- the commit verb is BP-internal/unobservable);
// on an increment serializes the new order(s), chunks them to fit kMaxReliablePayload, forwards to
// the host, and resets the mirror drone's self-takeoff (RE Q2). It never mutates its own orders array
// (removeOrderCart needs the laptop UI + is host-only; the client is ephemeral + never saves, so the
// retained local entries are harmless and leak-free).
// HOST: assembles the chunks per (senderSlot, orderId), then commits a completed order via the native
// makeAnOrder (ue_wrap::order_economy) -- which queues + flies + natively drains. All GT-only.

#include "coop/items/order_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/order_economy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::order_sync {
namespace {

namespace OE  = ue_wrap::order_economy;
namespace net = coop::net;

std::atomic<net::Session*> g_session{nullptr};

// Per-item fixed prefix on the wire: price(4) + size(4) + category(1) + objLen(1).
constexpr int      kItemFixed        = 10;
constexpr uint64_t kAssemblyTimeoutMs = 15000;  // drop a partial order whose chunks stop arriving
constexpr uint64_t kPendingTimeoutMs  = 60000;  // drop a completed order the world never lets us commit
constexpr size_t   kMaxAssembly       = 16;     // cap concurrent partial orders (anti-flood)
constexpr size_t   kMaxPending        = 32;     // cap queued-for-commit orders (anti-flood)
constexpr int      kMaxCommitTries    = 3;      // a commit that keeps failing while committable -> drop

// ---- client forward state (game thread only) ----
int32_t  g_forwardedThrough = -1;  // watermark: orders.Num value we've forwarded up to (-1 = unprimed)
uint32_t g_orderIdCounter   = 0;   // monotonic id per forwarded order (uniqueness within this sender)

// ---- host assembly state (game thread only) ----
struct Assembly {
    uint16_t totalItems = 0;
    float    time       = 0.f;
    std::vector<OE::OrderItem> items;
    uint64_t lastMs     = 0;
};
std::unordered_map<uint64_t, Assembly> g_assembly;  // key = (senderSlot<<32)|orderId

struct Pending {
    OE::OrderData od;
    uint64_t      firstMs = 0;
    int           tries   = 0;
};
std::vector<Pending> g_pending;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t AsmKey(uint8_t slot, uint32_t orderId) {
    return (static_cast<uint64_t>(slot) << 32) | orderId;
}

// Reset all per-session state. Called by BOTH Install (session start) and OnDisconnect (teardown)
// so a reconnect that re-Installs without a preceding OnDisconnect can't retain a stale client
// watermark and double-forward last session's queued orders (audit I-2). One path, RULE 2.
void ResetState() {
    g_forwardedThrough = -1;
    g_orderIdCounter   = 0;
    g_assembly.clear();
    g_pending.clear();
}

// UE class/FName leaf names are ASCII -> narrow/widen losslessly (non-ASCII -> '?', which simply
// fails FindClass host-side and the item is skipped, logged).
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
    size_t total = od.items.size();
    if (total > static_cast<size_t>(net::kMaxOrderItems)) {
        UE_LOGW("order_sync: order idx=%d has %zu items > cap %d -- forwarding first %d (rest dropped)",
                idx, total, net::kMaxOrderItems, net::kMaxOrderItems);
        total = net::kMaxOrderItems;
    }
    const uint32_t orderId = ++g_orderIdCounter;
    size_t i = 0;
    int chunks = 0;
    while (i < total) {
        uint8_t buf[net::kMaxReliablePayload];
        int pos = static_cast<int>(sizeof(net::OrderRequestHeader));
        const size_t chunkStart = i;
        uint16_t chunkCount = 0;
        while (i < total) {
            std::string name = NarrowAscii(od.items[i].objectClass);
            if (name.size() > static_cast<size_t>(net::kMaxOrderClassName))
                name.resize(net::kMaxOrderClassName);
            const int itemSize = kItemFixed + static_cast<int>(name.size());
            if (pos + itemSize > net::kMaxReliablePayload) break;  // this chunk is full
            std::memcpy(buf + pos, &od.items[i].price, 4); pos += 4;
            std::memcpy(buf + pos, &od.items[i].size, 4);  pos += 4;
            buf[pos++] = od.items[i].category;
            buf[pos++] = static_cast<uint8_t>(name.size());
            std::memcpy(buf + pos, name.data(), name.size()); pos += static_cast<int>(name.size());
            ++chunkCount;
            ++i;
        }
        if (chunkCount == 0) {
            // A single item that can't fit even an empty chunk -- impossible given the caps
            // (header 16 + fixed 10 + name<=96 = 122 <= 228), but never spin forever.
            UE_LOGW("order_sync: item %zu too large to chunk -- skipping", i);
            ++i;
            continue;
        }
        net::OrderRequestHeader h{};
        h.orderId    = orderId;
        h.totalItems = static_cast<uint16_t>(total);
        h.baseIndex  = static_cast<uint16_t>(chunkStart);
        h.chunkItems = chunkCount;
        h._pad       = 0;
        h.time       = od.time;
        std::memcpy(buf, &h, sizeof(h));
        s->SendReliable(net::ReliableKind::OrderRequest, buf, pos);
        ++chunks;
    }
    UE_LOGI("order_sync: forwarded order idx=%d id=%u items=%zu in %d chunk(s)", idx, orderId, total, chunks);
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

void TickHost() {
    const uint64_t now = NowMs();
    // Commit completed orders (busy/broken drone is fine -- the native queues; CanCommit only guards
    // against a null actor while the world is still loading).
    for (size_t i = 0; i < g_pending.size();) {
        Pending& pe = g_pending[i];
        bool done = false;
        if (OE::CanCommit()) {
            ++pe.tries;
            if (OE::CommitOrder(pe.od, /*automatic*/ true)) {
                done = true;
            } else if (pe.tries >= kMaxCommitTries) {
                UE_LOGW("order_sync: commit failed %d times -- dropping order", pe.tries);
                done = true;
            }
        }
        if (!done && now - pe.firstMs > kPendingTimeoutMs) {
            UE_LOGW("order_sync: pending order undeliverable for %llums -- dropping",
                    static_cast<unsigned long long>(now - pe.firstMs));
            done = true;
        }
        if (done) g_pending.erase(g_pending.begin() + static_cast<long>(i));
        else ++i;
    }
    // Evict partial assemblies whose remaining chunks never arrived.
    for (auto it = g_assembly.begin(); it != g_assembly.end();) {
        if (now - it->second.lastMs > kAssemblyTimeoutMs) {
            UE_LOGW("order_sync: dropping stale partial order assembly");
            it = g_assembly.erase(it);
        } else {
            ++it;
        }
    }
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
    if (s->role() == net::Role::Host) TickHost();
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
    std::vector<OE::OrderItem> chunkItems;
    chunkItems.reserve(h.chunkItems);
    for (uint16_t k = 0; k < h.chunkItems; ++k) {
        if (p + kItemFixed > end) { UE_LOGW("order_sync: OrderRequest truncated item -- drop"); return; }
        OE::OrderItem it;
        std::memcpy(&it.price, p, 4); p += 4;
        std::memcpy(&it.size,  p, 4); p += 4;
        it.category = *p++;
        const uint8_t objLen = *p++;
        if (objLen > net::kMaxOrderClassName || p + objLen > end) {
            UE_LOGW("order_sync: OrderRequest bad objLen=%u -- drop", objLen);
            return;
        }
        it.objectClass = WidenAscii(p, objLen);
        p += objLen;
        chunkItems.push_back(std::move(it));
    }

    const uint64_t key = AsmKey(senderSlot, h.orderId);
    auto itA = g_assembly.find(key);
    if (itA == g_assembly.end()) {
        if (h.baseIndex != 0) {
            UE_LOGW("order_sync: first chunk baseIndex=%u != 0 (lost head) -- drop", h.baseIndex);
            return;
        }
        if (g_assembly.size() >= kMaxAssembly) {
            UE_LOGW("order_sync: assembly table full (%zu) -- drop new order", g_assembly.size());
            return;
        }
        Assembly a;
        a.totalItems = h.totalItems;
        a.time       = h.time;
        a.lastMs     = NowMs();
        itA = g_assembly.emplace(key, std::move(a)).first;
    }
    Assembly& a = itA->second;
    if (h.baseIndex != static_cast<uint16_t>(a.items.size()) || h.totalItems != a.totalItems) {
        UE_LOGW("order_sync: chunk out-of-order/mismatch (base=%u have=%zu total=%u/%u) -- drop assembly",
                h.baseIndex, a.items.size(), h.totalItems, a.totalItems);
        g_assembly.erase(itA);
        return;
    }
    for (auto& it : chunkItems) a.items.push_back(std::move(it));
    a.lastMs = NowMs();

    if (a.items.size() >= a.totalItems) {
        if (g_pending.size() >= kMaxPending) {
            UE_LOGW("order_sync: pending-commit queue full (%zu) -- drop completed order", g_pending.size());
            g_assembly.erase(itA);
            return;
        }
        Pending pe;
        pe.od.items = std::move(a.items);
        pe.od.time  = a.time;
        pe.firstMs  = NowMs();
        pe.tries    = 0;
        const size_t nItems = pe.od.items.size();
        g_pending.push_back(std::move(pe));
        g_assembly.erase(itA);
        UE_LOGI("order_sync: order assembled (slot=%u id=%u items=%zu) -- queued for commit",
                senderSlot, h.orderId, nItems);
    }
}

void OnDisconnect() {
    ResetState();
}

}  // namespace coop::order_sync
