// ue_wrap/core/object_index.cpp -- per-class lists of live slots over the object array, fed by
// the engine's create and delete notifications.
#include "ue_wrap/core/object_index.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/uobject_listeners.h"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace ue_wrap::object_index {
namespace {

namespace R = reflection;
using Event = uobject_listeners::Event;

// One entry per object slot; `obj` null = not linked. The class's instances form a doubly linked
// list through the slots, so a birth and a death are both O(1).
struct Slot {
    void*   obj  = nullptr;
    void*   cls  = nullptr;
    int32_t next = -1;
    int32_t prev = -1;
};
struct ClassEntry {
    int32_t head  = -1;
    int32_t count = 0;
};

// Game thread only, all of it.
std::vector<Slot>                        g_slots;
std::unordered_map<void*, ClassEntry>    g_classes;
std::vector<Event>                       g_pending;  // drained events not yet applied
size_t                                   g_pendingHead = 0;
bool                                     g_installed = false;
bool                                     g_seeded = false;
Observer                                 g_observer{};
size_t                                   g_linked = 0;
// The 60 s summary's counters.
uint64_t g_applied = 0, g_births = 0, g_deaths = 0, g_stale = 0, g_relinked = 0, g_seedOverlap = 0;
std::chrono::steady_clock::time_point g_summarySince{};

// Events applied per Drain, so a level load's backlog (a few hundred thousand) spreads over a few
// ticks instead of landing whole on one frame.
constexpr size_t kMaxAppliedPerDrain = 16384;

void EnsureSlot(int32_t index) {
    if (index < static_cast<int32_t>(g_slots.size())) return;
    const int32_t n = R::NumObjects();
    g_slots.resize(static_cast<size_t>(index < n ? n : index + 1));
}

void Link(int32_t index, void* obj, void* cls) {
    EnsureSlot(index);
    Slot& s = g_slots[static_cast<size_t>(index)];
    ClassEntry& e = g_classes[cls];
    s.obj = obj;
    s.cls = cls;
    s.prev = -1;
    s.next = e.head;
    if (e.head >= 0) g_slots[static_cast<size_t>(e.head)].prev = index;
    e.head = index;
    ++e.count;
    ++g_linked;
}

// Unlink; true when the class lost its last instance (the entry is erased).
bool Unlink(int32_t index) {
    Slot& s = g_slots[static_cast<size_t>(index)];
    auto it = g_classes.find(s.cls);
    if (s.prev >= 0) g_slots[static_cast<size_t>(s.prev)].next = s.next;
    else if (it != g_classes.end()) it->second.head = s.next;
    if (s.next >= 0) g_slots[static_cast<size_t>(s.next)].prev = s.prev;
    s = Slot{};
    --g_linked;
    if (it != g_classes.end() && --it->second.count == 0) {
        g_classes.erase(it);
        return true;
    }
    return false;
}

void ApplyCreated(const Event& ev) {
    // The slot must still hold the object: a birth whose object died and was freed before this
    // drain, or whose slot was recycled since, is skipped; the successor's own event follows.
    if (R::ObjectAt(ev.index) != ev.obj) { ++g_stale; return; }
    EnsureSlot(ev.index);
    Slot& s = g_slots[static_cast<size_t>(ev.index)];
    if (s.obj == ev.obj) { ++g_seedOverlap; return; }   // the seed walk already linked it
    if (s.obj) {
        // A tenant this queue never saw die: unlink it before the slot's new owner goes in. The
        // class is read first, since Unlink clears the slot `s` refers to.
        ++g_relinked;
        void* const gone = s.cls;
        if (Unlink(ev.index) && g_observer.OnClassGone) g_observer.OnClassGone(g_observer.ctx, gone);
    }
    Link(ev.index, ev.obj, ev.cls);
    ++g_births;
    if (g_classes[ev.cls].count == 1 && g_observer.OnClassAppeared)
        g_observer.OnClassAppeared(g_observer.ctx, ev.cls, ev.obj);
    if (g_observer.OnObjectCreated)
        g_observer.OnObjectCreated(g_observer.ctx, ev.obj, ev.cls, ev.index);
}

void ApplyDeleted(const Event& ev) {
    if (ev.index < 0 || ev.index >= static_cast<int32_t>(g_slots.size())) { ++g_stale; return; }
    Slot& s = g_slots[static_cast<size_t>(ev.index)];
    if (s.obj != ev.obj) { ++g_stale; return; }   // never linked, or already replaced by the seed
    void* cls = s.cls;
    ++g_deaths;
    if (Unlink(ev.index) && g_observer.OnClassGone) g_observer.OnClassGone(g_observer.ctx, cls);
}

// The first instance of a class whose memory may be read: its slot still holds it (the destructor
// nulls the slot before the memory is freed) and it is not unreachable (only unreachable objects
// are freed, and only the game thread marks them, so a clear bit read here holds until the next
// collection). An entry between its FinishDestroy and the drain of its delete event fails one of
// the two, and is skipped. Null when the class has no readable instance right now.
void* ReadableInstance(const ClassEntry& e) {
    for (int32_t i = e.head; i >= 0; i = g_slots[static_cast<size_t>(i)].next) {
        void* obj = g_slots[static_cast<size_t>(i)].obj;
        if (R::ObjectAt(i) != obj) continue;
        if (R::SlotFlags(i) & R::slot_flags::Unreachable) continue;
        return obj;
    }
    return nullptr;
}

// The one walk this module ever makes: the array as it stands when the first drain runs. Objects
// whose birth is already queued are linked here and their event then reads as overlap.
void Seed() {
    const auto t0 = std::chrono::steady_clock::now();
    const int32_t n = R::NumObjects();
    g_slots.resize(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || g_slots[static_cast<size_t>(i)].obj) continue;
        // An unreachable object is on its way out and its memory may be freed by the purge thread
        // at any moment; its delete event, if it ever drains, then reads as stale.
        if (R::SlotFlags(i) & R::slot_flags::Unreachable) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        Link(i, obj, cls);
        if (g_classes[cls].count == 1 && g_observer.OnClassAppeared)
            g_observer.OnClassAppeared(g_observer.ctx, cls, obj);
    }
    g_seeded = true;
    g_summarySince = std::chrono::steady_clock::now();
    UE_LOGI("object_index: seeded %zu objects in %zu classes from %d slots in %lld us",
            g_linked, g_classes.size(), n,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count()));
}

void SummaryIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_summarySince < std::chrono::seconds(60)) return;
    g_summarySince = now;
    const auto st = uobject_listeners::GetStats();
    UE_LOGI("object_index: steady summary (60s): objects=%zu classes=%zu applied=%llu births=%llu "
            "deaths=%llu stale=%llu relinked=%llu backlog=%zu queue-high-water=%zu",
            g_linked, g_classes.size(), static_cast<unsigned long long>(g_applied),
            static_cast<unsigned long long>(g_births), static_cast<unsigned long long>(g_deaths),
            static_cast<unsigned long long>(g_stale), static_cast<unsigned long long>(g_relinked),
            g_pending.size() - g_pendingHead, st.highWater);
    g_applied = g_births = g_deaths = g_stale = g_relinked = 0;
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!uobject_listeners::Install()) return false;
    g_installed = true;
    return true;
}

size_t Drain() {
    UE_ASSERT_GAME_THREAD("object_index::Drain");
    if (!g_installed) return 0;
    if (!g_seeded) {
        if (!R::ObjectArrayAddress()) return 0;
        Seed();
    }
    uobject_listeners::Drain(g_pending);
    size_t applied = 0;
    while (g_pendingHead < g_pending.size() && applied < kMaxAppliedPerDrain) {
        const Event& ev = g_pending[g_pendingHead++];
        if (ev.created) ApplyCreated(ev);
        else            ApplyDeleted(ev);
        ++applied;
    }
    if (g_pendingHead == g_pending.size()) {
        g_pending.clear();
        g_pendingHead = 0;
    } else if (g_pendingHead > (g_pending.size() / 2)) {
        g_pending.erase(g_pending.begin(), g_pending.begin() + static_cast<ptrdiff_t>(g_pendingHead));
        g_pendingHead = 0;
    }
    g_applied += applied;
    SummaryIfDue();
    return applied;
}

bool IsSeeded() { return g_seeded; }

size_t ForEachInstance(void* cls, InstanceFn fn, void* ctx) {
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) return 0;
    size_t n = 0;
    for (int32_t i = it->second.head; i >= 0;) {
        const Slot& s = g_slots[static_cast<size_t>(i)];
        const int32_t next = s.next;   // read before the callback, which may not unlink but may look
        fn(ctx, s.obj, i);
        ++n;
        i = next;
    }
    return n;
}

size_t ForEachClass(ClassFn fn, void* ctx) {
    size_t n = 0;
    for (const auto& kv : g_classes) {
        void* inst = ReadableInstance(kv.second);
        if (!inst) continue;
        fn(ctx, kv.first, inst);
        ++n;
    }
    return n;
}

void SetObserver(const Observer& o) { g_observer = o; }

Parity DebugCompareWithWalk() {
    UE_ASSERT_GAME_THREAD("object_index::DebugCompareWithWalk");
    Parity p{};
    p.objects = g_linked;
    p.classes = g_classes.size();
    std::vector<int32_t> missing;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || (R::SlotFlags(i) & R::slot_flags::Unreachable)) continue;
        if (i >= static_cast<int32_t>(g_slots.size()) || g_slots[static_cast<size_t>(i)].obj != obj)
            missing.push_back(i);
    }
    for (size_t i = 0; i < g_slots.size(); ++i) {
        const Slot& s = g_slots[i];
        if (s.obj && R::ObjectAt(static_cast<int32_t>(i)) != s.obj) ++p.indexedNotLive;
    }
    // A birth in flight during the walk above has its event queued by now: apply, then re-judge.
    Drain();
    for (int32_t i : missing) {
        void* obj = R::ObjectAt(i);
        if (!obj || (R::SlotFlags(i) & R::slot_flags::Unreachable)) continue;
        if (i >= static_cast<int32_t>(g_slots.size()) || g_slots[static_cast<size_t>(i)].obj != obj)
            ++p.liveNotIndexed;
    }
    return p;
}

}  // namespace ue_wrap::object_index
