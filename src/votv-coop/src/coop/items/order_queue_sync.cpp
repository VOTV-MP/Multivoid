// coop/items/order_queue_sync.cpp -- see coop/items/order_queue_sync.h.

#include "coop/items/order_queue_sync.h"

#include "coop/items/order_rows.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/order_economy.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace coop::order_queue_sync {
namespace {

namespace OE  = ue_wrap::order_economy;
namespace sg  = ue_wrap::script_gate;
namespace net = coop::net;

std::atomic<net::Session*> g_session{nullptr};

constexpr uint8_t kOpReset  = 0;
constexpr uint8_t kOpAppend = 1;
constexpr uint8_t kOpPop    = 2;

constexpr const wchar_t* kLaptopClass = L"ui_laptop_C";
constexpr const wchar_t* kAddVerb     = L"addOrderCart";
constexpr const wchar_t* kRemoveVerb  = L"removeOrderCart";
constexpr int kTagAdded   = 0x4F514144;  // 'OQAD'
constexpr int kTagRemoved = 0x4F515245;  // 'OQRE'
bool g_watched = false;

net::Session* HostSession() {
    net::Session* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() == net::Role::Host) ? s : nullptr;
}

// ---- HOST: the queue's changes, sent as its verbs ran ----

// A reset or a pop, to `slot`, or to every peer when `slot` is negative.
void SendOp(net::Session* s, uint8_t op, int slot) {
    net::OrderQueueHeader h{};
    h.op = op;
    if (slot >= 0)
        s->SendReliableToSlot(slot, net::ReliableKind::OrderQueue, &h, sizeof(h));
    else
        s->SendReliable(net::ReliableKind::OrderQueue, &h, sizeof(h));
}

// The queue's order at `index`, appended, in consecutive messages when it needs more than one.
void SendAppend(net::Session* s, int32_t index, int slot) {
    OE::OrderData od;
    if (!OE::ReadOrder(index, od)) {
        UE_LOGW("order_queue: the host's order %d could not be read -- not sent, so a client's queue misses it",
                index);
        return;
    }
    if (od.rowNames.size() > static_cast<size_t>(net::kMaxOrderItems)) od.rowNames.resize(net::kMaxOrderItems);
    const size_t total = od.rowNames.size();
    size_t i = 0;
    while (i < total) {
        uint8_t buf[net::kMaxReliablePayload];
        int pos = static_cast<int>(sizeof(net::OrderQueueHeader));
        const size_t from = i;
        const int packed = coop::order_rows::Pack(od.rowNames, i, buf, pos, net::kMaxReliablePayload);
        if (packed <= 0) return;  // a row the caps make impossible; the client drops the partial append
        i += static_cast<size_t>(packed);
        net::OrderQueueHeader h{};
        h.op = kOpAppend;
        h.eta = od.eta;
        h.totalItems = static_cast<uint16_t>(total);
        h.baseIndex = static_cast<uint16_t>(from);
        h.chunkItems = static_cast<uint16_t>(packed);
        std::memcpy(buf, &h, sizeof(h));
        if (slot >= 0)
            s->SendReliableToSlot(slot, net::ReliableKind::OrderQueue, buf, pos);
        else
            s->SendReliable(net::ReliableKind::OrderQueue, buf, pos);
    }
}

// The queue's count at the verb's entry, read again at its exit: a change is sent only when the count
// moved (removeOrderCart pops with no bound check, so on an empty queue it changes nothing).
int32_t g_countAtEntry = -1;

sg::Verdict OnQueueVerbPre(const sg::Call&) {
    g_countAtEntry = HostSession() ? OE::OrderCount() : -1;
    return sg::Verdict::Run;
}

void OnAddedPost(const sg::Call&) {
    net::Session* s = HostSession();
    const int32_t n = OE::OrderCount();
    if (!s || g_countAtEntry < 0 || n != g_countAtEntry + 1) return;
    SendAppend(s, n - 1, -1);
    UE_LOGI("order_queue: the host's queue gained order %d -- sent to every peer", n - 1);
}

void OnRemovedPost(const sg::Call&) {
    net::Session* s = HostSession();
    if (!s || g_countAtEntry <= 0 || OE::OrderCount() != g_countAtEntry - 1) return;
    SendOp(s, kOpPop, -1);
    UE_LOGI("order_queue: the host's queue popped its first order -- sent to every peer");
}

// ---- CLIENT: the host's changes, applied in order ----

struct Change {
    uint8_t op = kOpReset;
    OE::OrderData order;  // an append's
};

// An append arriving in consecutive messages.
struct Assembly {
    bool active = false;
    float eta = 0.f;
    uint16_t total = 0;
    std::vector<std::wstring> rows;
};
Assembly g_asm;
bool g_synced = false;         // a reset arrived this session
std::deque<Change> g_waiting;  // changes the laptop could not take yet, oldest first
constexpr size_t kMaxWaiting = 64;

bool Apply(const Change& c) {
    switch (c.op) {
    case kOpReset:
        while (OE::OrderCount() > 0)
            if (!OE::PopOrder()) return false;
        return OE::OrderCount() == 0;
    case kOpAppend:
        return OE::AppendOrder(c.order);
    case kOpPop:
        if (OE::OrderCount() <= 0) {
            UE_LOGW("order_queue: the host popped an order this client's queue does not hold -- nothing to pop");
            return true;
        }
        return OE::PopOrder();
    }
    return true;
}

void Take(Change c) {
    if (c.op == kOpReset) g_waiting.clear();  // a reset states everything before it
    if (g_waiting.empty() && Apply(c)) return;
    if (g_waiting.size() >= kMaxWaiting) {
        UE_LOGW("order_queue: %zu changes wait for the laptop -- this one is dropped; the next join snapshot "
                "restates the queue", g_waiting.size());
        return;
    }
    g_waiting.push_back(std::move(c));
}

}  // namespace

void Install(net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watched)
        g_watched = sg::WatchClassName(kLaptopClass, kAddVerb, kTagAdded, &OnQueueVerbPre, &OnAddedPost) &&
                    sg::WatchClassName(kLaptopClass, kRemoveVerb, kTagRemoved, &OnQueueVerbPre, &OnRemovedPost);
}

void Tick() {
    while (!g_waiting.empty() && Apply(g_waiting.front())) g_waiting.pop_front();
}

void OnReliable(const void* payload, int len, uint8_t senderSlot) {
    net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == net::Role::Host) return;  // host to clients only
    if (senderSlot != 0) {
        UE_LOGW("order_queue: OrderQueue from slot %u, not the host -- dropping", senderSlot);
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(net::OrderQueueHeader))) {
        UE_LOGW("order_queue: OrderQueue too short (%d) -- dropping", len);
        return;
    }
    net::OrderQueueHeader h{};
    std::memcpy(&h, payload, sizeof(h));
    if (h.op == kOpReset) {
        g_synced = true;
        g_asm = Assembly{};
        Take(Change{kOpReset, {}});
        UE_LOGI("order_queue: reset from the host -- this client's queue follows the host's from here");
        return;
    }
    if (!g_synced) return;  // the snapshot at this peer's world-ready states it
    if (h.op == kOpPop) {
        Take(Change{kOpPop, {}});
        return;
    }
    if (h.op != kOpAppend) {
        UE_LOGW("order_queue: unknown op %u -- dropping", h.op);
        return;
    }
    if (h.totalItems == 0 || h.totalItems > net::kMaxOrderItems || h.chunkItems == 0 ||
        static_cast<int>(h.baseIndex) + static_cast<int>(h.chunkItems) > static_cast<int>(h.totalItems)) {
        UE_LOGW("order_queue: append with a bad range (base=%u count=%u total=%u) -- dropping", h.baseIndex,
                h.chunkItems, h.totalItems);
        g_asm = Assembly{};
        return;
    }
    if (h.baseIndex == 0) {
        g_asm = Assembly{};
        g_asm.active = true;
        g_asm.eta = h.eta;
        g_asm.total = h.totalItems;
    } else if (!g_asm.active || h.baseIndex != g_asm.rows.size() || h.totalItems != g_asm.total) {
        UE_LOGW("order_queue: append message out of order (base=%u have=%zu) -- dropping the order", h.baseIndex,
                g_asm.rows.size());
        g_asm = Assembly{};
        return;
    }
    const uint8_t* p = static_cast<const uint8_t*>(payload) + sizeof(h);
    const uint8_t* end = static_cast<const uint8_t*>(payload) + len;
    if (!coop::order_rows::Unpack(p, end, h.chunkItems, g_asm.rows)) {
        UE_LOGW("order_queue: append carries a truncated or oversized item -- dropping the order");
        g_asm = Assembly{};
        return;
    }
    if (g_asm.rows.size() < g_asm.total) return;
    Change c{kOpAppend, {}};
    c.order.rowNames = std::move(g_asm.rows);
    c.order.eta = g_asm.eta;
    g_asm = Assembly{};
    UE_LOGI("order_queue: the host's queue gained an order of %zu item(s) -- appending", c.order.rowNames.size());
    Take(std::move(c));
}

void QueueConnectBroadcastForSlot(int slot) {
    net::Session* s = HostSession();
    if (!s || slot <= 0) return;
    const int32_t n = OE::OrderCount();
    if (n < 0) return;  // the host's queue is unresolved: the joiner keeps its save's
    SendOp(s, kOpReset, slot);
    for (int32_t i = 0; i < n; ++i) SendAppend(s, i, slot);
    UE_LOGI("order_queue: connect snapshot -- a reset and %d queued order(s) to slot %d", n, slot);
}

void OnDisconnect() {
    g_synced = false;
    g_asm = Assembly{};
    g_waiting.clear();
    g_countAtEntry = -1;
}

}  // namespace coop::order_queue_sync
