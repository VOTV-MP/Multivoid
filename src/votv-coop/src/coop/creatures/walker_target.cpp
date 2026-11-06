// coop/creatures/walker_target.cpp -- see coop/creatures/walker_target.h.

#include "coop/creatures/walker_target.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace coop::walker_target {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;

// The host holds up to five walkers and the tick drains every frame, so a full queue is a defect: said once, and the walker
// is not claimed (no retry: a retry is a crutch, the sweep at the class resolve covers only what the class missed).
constexpr size_t kQueueCap = 16;

struct Entry {
    void*                    actor;
    coop::element::ElementId eid;
};

std::mutex          g_queueMutex;  // a leaf: nothing is called under it
Entry               g_queue[kQueueCap];
size_t              g_queueCount = 0;
std::atomic<size_t> g_pending{0};  // g_queueCount, readable without the lock
std::atomic<bool>   g_overflowSaid{false};

// The walker's class and the listing it was found under (ObjectIndex ClassListingId: a class unloaded and loaded again, even at
// the same address, answers a new id). While it is null the lookup is asked again only when the class set moved. The atomic is
// the pointer the subscriber compares on whichever thread MarkPropElement runs.
void*              g_class = nullptr;
uint64_t           g_listing = 0;
uint64_t           g_setAsked = ~uint64_t{0};
std::atomic<void*> g_walkerClass{nullptr};

bool g_subscribed = false;

// MarkPropElement's callback, any thread: a non-walker is rejected by the class pointer, a walker is queued.
void OnExpressed(void* actor, coop::element::ElementId eid) {
    void* const cls = g_walkerClass.load(std::memory_order_acquire);
    if (!cls || !actor || R::ClassOf(actor) != cls) return;
    std::lock_guard<std::mutex> lk(g_queueMutex);
    if (g_queueCount >= kQueueCap) {
        if (!g_overflowSaid.exchange(true))
            UE_LOGW("walker_target: the birth queue is full (%zu); a walker is not claimed", kQueueCap);
        return;
    }
    g_queue[g_queueCount++] = Entry{actor, eid};
    g_pending.store(g_queueCount, std::memory_order_release);
}

// Claim `actor` if it still holds `eid` and is alive: the registry's reverse is the test for an actor that died in the queue.
void ClaimWalker(void* actor, coop::element::ElementId eid) {
    if (coop::element::Registry::Get().EidForActor(actor) != eid || !R::IsLive(actor)) return;
    coop::prop_drive_host::Claim(actor, "walker");
}

// The class resolved: on a new class claim the walkers that already hold an eid.
void ResolveClass() {
    void* const cls = AC::WalkerClass();
    if (!cls) return;
    g_class = cls;
    g_listing = OI::ClassListingId(cls);
    g_walkerClass.store(cls, std::memory_order_release);
    std::vector<void*> alive;
    AC::ListWalkers(alive);
    for (void* w : alive) {
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(w);
        if (eid != coop::element::kInvalidId) ClaimWalker(w, eid);
    }
}

// The class again when its listing changed (an unload) and, while it is null, only when the class set moved: a class gaining
// its first instance moves the number, so the lookup is not a poll of the class table.
void RefreshClass() {
    if (g_class && OI::ClassListingId(g_class) != g_listing) {
        g_class = nullptr;
        g_walkerClass.store(nullptr, std::memory_order_release);
        g_setAsked = ~uint64_t{0};
    }
    if (g_class) return;
    const uint64_t set = OI::ClassSetVersion();
    if (set == g_setAsked) return;
    g_setAsked = set;
    ResolveClass();
}

}  // namespace

void Install(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("walker_target::Install");
    if (session.role() != coop::net::Role::Host || g_subscribed) return;
    g_subscribed = coop::prop_element_tracker::SubscribeHostPropExpressed(&OnExpressed);
    if (g_subscribed) UE_LOGI("walker_target: the host's walkers are claimed into the drive stream at their eid mint");
    else UE_LOGE("walker_target: the prop lane's subscriber table is full -- the host's walker is not claimed");
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    RefreshClass();
    if (g_pending.load(std::memory_order_acquire) == 0) return;
    Entry   taken[kQueueCap];
    size_t  n = 0;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        for (; n < g_queueCount; ++n) taken[n] = g_queue[n];
        g_queueCount = 0;
        g_pending.store(0, std::memory_order_release);
    }
    for (size_t i = 0; i < n; ++i) ClaimWalker(taken[i].actor, taken[i].eid);
}

void OnDisconnect() {
    g_walkerClass.store(nullptr, std::memory_order_release);
    g_class = nullptr;
    g_listing = 0;
    g_setAsked = ~uint64_t{0};
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queueCount = 0;
    g_pending.store(0, std::memory_order_release);
    g_overflowSaid.store(false, std::memory_order_relaxed);
}

}  // namespace coop::walker_target
