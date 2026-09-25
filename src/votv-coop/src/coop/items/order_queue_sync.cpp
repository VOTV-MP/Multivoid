// coop/items/order_queue_sync.cpp -- see coop/items/order_queue_sync.h.

#include "coop/items/order_queue_sync.h"

#include "coop/items/order_rows.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"  // NowMs

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

void SendChunk(net::Session* s, const uint8_t* buf, int len, int slot) {
    if (slot >= 0)
        s->SendReliableToSlot(slot, net::ReliableKind::OrderQueue, buf, len);
    else
        s->SendReliable(net::ReliableKind::OrderQueue, buf, len);
}

// The queue's order at `index`, appended, in consecutive messages when it needs more than one. Every
// append the queue made is sent, so a client's queue keeps the host's count: an order this host cannot
// read goes as an order of no items, which still takes its place until the host's pop removes it.
void SendAppend(net::Session* s, int32_t index, int slot) {
    OE::QueuedOrder od;
    if (!OE::ReadQueuedOrder(index, od)) {
        UE_LOGW("order_queue: the host's order %d could not be read -- sent as an order of no items, to keep "
                "its place", index);
        od = OE::QueuedOrder{};
    }
    if (od.items.size() > static_cast<size_t>(net::kMaxOrderItems)) od.items.resize(net::kMaxOrderItems);
    const size_t total = od.items.size();
    uint8_t buf[net::kMaxReliablePayload];
    net::OrderQueueHeader h{};
    h.op = kOpAppend;
    h.eta = od.eta;
    h.totalItems = static_cast<uint16_t>(total);
    if (total == 0) {
        std::memcpy(buf, &h, sizeof(h));
        SendChunk(s, buf, static_cast<int>(sizeof(h)), slot);
        return;
    }
    size_t i = 0;
    while (i < total) {
        int pos = static_cast<int>(sizeof(net::OrderQueueHeader));
        const size_t from = i;
        const int packed = coop::order_rows::PackQueued(od.items, i, buf, pos, net::kMaxReliablePayload);
        if (packed <= 0) return;  // an item the caps make impossible; the client drops the partial append
        i += static_cast<size_t>(packed);
        h.baseIndex = static_cast<uint16_t>(from);
        h.chunkItems = static_cast<uint16_t>(packed);
        std::memcpy(buf, &h, sizeof(h));
        SendChunk(s, buf, pos, slot);
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
    OE::QueuedOrder order;  // an append's
};

// An append arriving in consecutive messages.
struct Assembly {
    bool active = false;
    float eta = 0.f;
    uint16_t total = 0;
    std::vector<OE::QueuedItem> items;
};
Assembly g_asm;
bool g_synced = false;         // a reset arrived this session
std::deque<Change> g_waiting;  // changes the laptop could not take yet, oldest first
constexpr size_t   kMaxWaiting  = 64;
constexpr uint64_t kRetryMs     = 1000;  // a waiting change is tried again once a second, not per tick
uint64_t g_nextRetryMs = 0;
bool     g_waitSaid    = false;          // the head's wait, said once a streak
Counts   g_counts;

OE::Applied Apply(const Change& c) {
    switch (c.op) {
    case kOpReset: {
        // Bounded by the count it starts from: a pop that moves nothing ends the pass as Later.
        const int32_t n = OE::OrderCount();
        if (n < 0) return OE::Applied::Later;
        for (int32_t k = 0; k < n; ++k)
            if (OE::PopOrder() != OE::Applied::Done) return OE::Applied::Later;
        return OE::Applied::Done;
    }
    case kOpAppend: {
        int left = 0;
        const OE::Applied r = OE::AppendOrder(c.order, &left);
        if (r == OE::Applied::Done) {
            ++g_counts.orders;
            for (const auto& it : c.order.items) g_counts.byClass += it.row.empty() ? 1 : 0;
            g_counts.leftOut += static_cast<uint64_t>(left);
        }
        return r;
    }
    case kOpPop: {
        const int32_t n = OE::OrderCount();
        if (n < 0) return OE::Applied::Later;  // unresolved, not empty
        if (n == 0) {
            UE_LOGW("order_queue: the host popped an order this client's queue does not hold -- nothing to pop");
            return OE::Applied::Done;
        }
        return OE::PopOrder();
    }
    }
    return OE::Applied::Done;
}

void Take(Change c) {
    if (c.op == kOpReset) g_waiting.clear();  // a reset states everything before it
    if (g_waiting.empty() && Apply(c) == OE::Applied::Done) return;
    if (g_waiting.size() >= kMaxWaiting) {
        // Dropping one change would leave this queue a different length from the host's for good, so
        // the mirror stops instead and says so; the next reset, at this client's next join, resumes it.
        UE_LOGW("order_queue: %zu changes wait for the laptop -- this client's queue stops following the "
                "host's until its next join", g_waiting.size());
        g_waiting.clear();
        g_synced = false;
        return;
    }
    if (g_waiting.empty()) g_nextRetryMs = coop::active_drive::NowMs() + kRetryMs;
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
    if (g_waiting.empty()) return;
    const uint64_t now = coop::active_drive::NowMs();
    if (now < g_nextRetryMs) return;
    while (!g_waiting.empty() && Apply(g_waiting.front()) == OE::Applied::Done) {
        g_waiting.pop_front();
        if (g_waitSaid) {
            g_waitSaid = false;
            UE_LOGI("order_queue: the laptop took the waiting change -- %zu still wait", g_waiting.size());
        }
    }
    if (g_waiting.empty()) return;
    g_nextRetryMs = now + kRetryMs;
    if (!g_waitSaid) {
        g_waitSaid = true;
        UE_LOGW("order_queue: %zu change(s) wait for the laptop, the queue or the catalog -- tried again each "
                "second", g_waiting.size());
    }
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
    if (h.totalItems == 0 && h.baseIndex == 0 && h.chunkItems == 0) {
        // The host could not read the order: it still takes its place, empty.
        g_asm = Assembly{};
        Change c{kOpAppend, {}};
        c.order.eta = h.eta;
        UE_LOGI("order_queue: the host's queue gained an order of no items -- appending it to keep the count");
        Take(std::move(c));
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
    } else if (!g_asm.active || h.baseIndex != g_asm.items.size() || h.totalItems != g_asm.total) {
        UE_LOGW("order_queue: append message out of order (base=%u have=%zu) -- dropping the order", h.baseIndex,
                g_asm.items.size());
        g_asm = Assembly{};
        return;
    }
    const uint8_t* p = static_cast<const uint8_t*>(payload) + sizeof(h);
    const uint8_t* end = static_cast<const uint8_t*>(payload) + len;
    if (!coop::order_rows::UnpackQueued(p, end, h.chunkItems, g_asm.items)) {
        UE_LOGW("order_queue: append carries a truncated or oversized item -- dropping the order");
        g_asm = Assembly{};
        return;
    }
    if (g_asm.items.size() < g_asm.total) return;
    Change c{kOpAppend, {}};
    c.order.items = std::move(g_asm.items);
    c.order.eta = g_asm.eta;
    g_asm = Assembly{};
    UE_LOGI("order_queue: the host's queue gained an order of %zu item(s) -- appending", c.order.items.size());
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

Counts ClientCounts() { return g_counts; }

void OnDisconnect() {
    g_counts = Counts{};
    g_synced = false;
    g_asm = Assembly{};
    g_waiting.clear();
    g_countAtEntry = -1;
    g_nextRetryMs = 0;
    g_waitSaid = false;
}

}  // namespace coop::order_queue_sync
