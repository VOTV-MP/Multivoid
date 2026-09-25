// ue_wrap/core/object_index.cpp -- per-class lists of live slots over the object array, fed by
// the engine's create and delete notifications.
#include "ue_wrap/core/object_index.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/uobject_listeners.h"

#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>
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
bool                                     g_draining = false;  // a drain's callbacks run inside it
Observer                                 g_observer{};
size_t                                   g_linked = 0;
// The 60 s summary's counters. `recycled`: births whose slot and pointer a new object of another
// class had taken by the drain; `refused`: entries ForEachInstance skipped, gone or recycled.
uint64_t g_applied = 0, g_births = 0, g_deaths = 0, g_stale = 0, g_relinked = 0, g_seedOverlap = 0;
uint64_t g_recycled = 0, g_refusedGone = 0, g_refusedRecycled = 0;
std::chrono::steady_clock::time_point g_summarySince{};

// The loaded classes by name. A class object is an instance of a meta-class (Class,
// BlueprintGeneratedClass, WidgetBlueprintGeneratedClass and the like), and a meta-class is whatever
// the class of an indexed object's class is, so the metas are learned from the objects themselves,
// never by name. Each class object is listed under its short name, hashed and compared without case
// (as the engine compares names), when it links and dropped when it unlinks: a class loaded with no instance is
// found as surely as one with many, and a class never loaded answers null in one lookup, where
// reflection::FindClass walks the whole array on every miss.
struct NameHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view s) const noexcept {
        size_t h = 1469598103934665603ull;
        for (wchar_t c : s) { h ^= static_cast<size_t>(::towlower(c)); h *= 1099511628211ull; }
        return h;
    }
};
struct NameEq {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (::towlower(a[i]) != ::towlower(b[i])) return false;
        return true;
    }
};
std::vector<void*>                                                    g_metas;             // few; game thread
std::unordered_map<std::wstring, std::vector<int32_t>, NameHash, NameEq> g_classSlotsByName; // name -> class-object slots
std::unordered_map<int32_t, std::wstring>                             g_classNameBySlot;   // the key each listed slot is under

// Events applied per Drain, so a level load's backlog (a few hundred thousand) spreads over several
// task batches instead of landing whole on one.
constexpr size_t kMaxAppliedPerDrain = 16384;

void EnsureSlot(int32_t index) {
    if (index < static_cast<int32_t>(g_slots.size())) return;
    const int32_t n = R::NumObjects();
    g_slots.resize(static_cast<size_t>(index < n ? n : index + 1));
}

bool IsMeta(void* cls) {
    for (void* m : g_metas)
        if (m == cls) return true;
    return false;
}

void ListClassName(int32_t index, void* classObj) {
    if (g_classNameBySlot.count(index)) return;   // listed already
    std::wstring key = R::ToString(R::NameOf(classObj));
    if (key.empty()) return;
    g_classSlotsByName[key].push_back(index);
    g_classNameBySlot.emplace(index, std::move(key));
}

void UnlistClassName(int32_t index) {
    const auto k = g_classNameBySlot.find(index);
    if (k == g_classNameBySlot.end()) return;
    const auto it = g_classSlotsByName.find(k->second);
    if (it != g_classSlotsByName.end()) {
        auto& v = it->second;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == index) { v[i] = v.back(); v.pop_back(); break; }
        if (v.empty()) g_classSlotsByName.erase(it);
    }
    g_classNameBySlot.erase(k);
}

// The meta-class of `cls`, the class of an object just linked. A meta first seen here lists the
// class objects linked under it before it was known.
void LearnMetaOf(void* cls) {
    void* meta = R::ClassOf(cls);
    if (!meta || IsMeta(meta)) return;
    g_metas.push_back(meta);
    const auto it = g_classes.find(meta);
    if (it == g_classes.end()) return;
    for (int32_t i = it->second.head; i >= 0; i = g_slots[static_cast<size_t>(i)].next)
        ListClassName(i, g_slots[static_cast<size_t>(i)].obj);
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
    if (IsMeta(cls)) ListClassName(index, obj);
    LearnMetaOf(cls);
}

// Unlink; true when the class lost its last instance (the entry is erased).
bool Unlink(int32_t index) {
    Slot& s = g_slots[static_cast<size_t>(index)];
    if (IsMeta(s.cls)) UnlistClassName(index);
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

// Whether a slot's tenant is the object an event or an entry names: the slot still holds the
// pointer, the object is not unreachable (the purge thread may free it from that bit on), and its
// class is the named class, which tells a new object of another class, put by the allocator in the
// same slot at the same address, from the one named. A successor of the same class passes and is
// what a class's list promises, a live instance of it. RE-UE4SS applies a death inside its
// listener (reference/RE-UE4SS/UE4SS/src/GUI/LiveView.cpp:294), so its lists never hold a
// recycled slot; this index drains a queue a tick or more later, so it checks.
enum class Tenant { Named, Gone, Recycled };
Tenant TenantOf(int32_t index, void* obj, void* cls) {
    if (R::ObjectAt(index) != obj) return Tenant::Gone;
    if (R::SlotFlags(index) & R::slot_flags::Unreachable) return Tenant::Gone;
    return R::ClassOf(obj) == cls ? Tenant::Named : Tenant::Recycled;
}

void ApplyCreated(const Event& ev) {
    // A birth whose object died before this drain is skipped, and so is one whose slot and address
    // a new object of another class has taken since; the successor's own event follows.
    const Tenant t = TenantOf(ev.index, ev.obj, ev.cls);
    if (t == Tenant::Gone) { ++g_stale; return; }
    if (t == Tenant::Recycled) { ++g_recycled; return; }
    EnsureSlot(ev.index);
    Slot& s = g_slots[static_cast<size_t>(ev.index)];
    if (s.obj == ev.obj && s.cls == ev.cls) { ++g_seedOverlap; return; }   // the seed walk linked it
    if (s.obj) {
        // A tenant this queue never saw die, or this pointer listed under a class it no longer has:
        // unlink it before the slot's new owner goes in. The class is read first, since Unlink
        // clears the slot `s` refers to.
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

// The first instance of `cls` whose memory may be read and which is still of `cls` (TenantOf). The
// destructor nulls the slot before the memory is freed, and only the game thread marks an object
// unreachable, so a clear bit read here holds until the next collection. An entry between its
// FinishDestroy and the drain of its delete event, or whose slot an object of another class took,
// is skipped: the hub classifies the whole class by the object this returns. Null when there is
// none now.
void* ReadableInstance(void* cls, const ClassEntry& e) {
    for (int32_t i = e.head; i >= 0; i = g_slots[static_cast<size_t>(i)].next) {
        void* obj = g_slots[static_cast<size_t>(i)].obj;
        if (TenantOf(i, obj, cls) == Tenant::Named) return obj;
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
    UE_LOGI("object_index: seeded %zu objects in %zu classes from %d slots in %lld us; %zu loaded classes "
            "under %zu names by %zu meta-classes",
            g_linked, g_classes.size(), n,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count()),
            g_classNameBySlot.size(), g_classSlotsByName.size(), g_metas.size());
}

void SummaryIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_summarySince < std::chrono::seconds(60)) return;
    g_summarySince = now;
    const auto st = uobject_listeners::GetStats();
    UE_LOGI("object_index: steady summary (60s): objects=%zu classes=%zu applied=%llu births=%llu "
            "deaths=%llu stale=%llu relinked=%llu backlog=%zu queue-high-water=%zu recycled=%llu "
            "refused=%llu gone/%llu recycled",
            g_linked, g_classes.size(), static_cast<unsigned long long>(g_applied),
            static_cast<unsigned long long>(g_births), static_cast<unsigned long long>(g_deaths),
            static_cast<unsigned long long>(g_stale), static_cast<unsigned long long>(g_relinked),
            g_pending.size() - g_pendingHead, st.highWater, static_cast<unsigned long long>(g_recycled),
            static_cast<unsigned long long>(g_refusedGone),
            static_cast<unsigned long long>(g_refusedRecycled));
    g_applied = g_births = g_deaths = g_stale = g_relinked = 0;
    g_recycled = g_refusedGone = g_refusedRecycled = 0;
}

}  // namespace

namespace {
void DrainBeforeTasks() { Drain(); }
}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!uobject_listeners::Install()) return false;
    g_installed = true;
    // The index's one driver: the dispatcher drains it before every batch of posted tasks, the boot's
    // included. A drain only the play loop made left every boot task an unseeded index, and the boot's
    // save load, which asks it for the game instance, retried for two minutes and gave up.
    game_thread::SetPumpPrologue(&DrainBeforeTasks);
    return true;
}

size_t Drain() {
    UE_ASSERT_GAME_THREAD("object_index::Drain");
    if (!g_installed || g_draining) return 0;
    struct Draining {
        Draining() { g_draining = true; }
        ~Draining() { g_draining = false; }
    } draining;
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

size_t Backlog() { return g_pending.size() - g_pendingHead; }

size_t ForEachInstance(void* cls, InstanceFn fn, void* ctx) {
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) return 0;
    size_t n = 0;
    for (int32_t i = it->second.head; i >= 0;) {
        const Slot& s = g_slots[static_cast<size_t>(i)];
        const int32_t next = s.next;   // read before the callback, which may not unlink but may look
        const Tenant t = TenantOf(i, s.obj, cls);
        if (t == Tenant::Named) {
            fn(ctx, s.obj, i);
            ++n;
        } else if (t == Tenant::Gone) {
            ++g_refusedGone;
        } else {
            ++g_refusedRecycled;
        }
        i = next;
    }
    return n;
}

size_t ForEachClass(ClassFn fn, void* ctx) {
    size_t n = 0;
    for (const auto& kv : g_classes) {
        void* inst = ReadableInstance(kv.first, kv.second);
        if (!inst) continue;
        fn(ctx, kv.first, inst);
        ++n;
    }
    return n;
}

void SetObserver(const Observer& o) { g_observer = o; }

void* ClassByName(const wchar_t* name) {
    UE_ASSERT_GAME_THREAD("object_index::ClassByName");
    if (!name || !*name) return nullptr;
    const auto it = g_classSlotsByName.find(std::wstring_view(name));
    if (it == g_classSlotsByName.end()) return nullptr;
    for (int32_t i : it->second) {
        const Slot& s = g_slots[static_cast<size_t>(i)];
        if (TenantOf(i, s.obj, s.cls) == Tenant::Named) return s.obj;
    }
    return nullptr;
}

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
        if (!s.obj) continue;
        const int32_t idx = static_cast<int32_t>(i);
        if (R::ObjectAt(idx) != s.obj) { ++p.indexedNotLive; continue; }
        if (TenantOf(idx, s.obj, s.cls) == Tenant::Recycled) ++p.misclassed;
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

void DebugUnlinkSlot(int32_t index) {
    UE_ASSERT_GAME_THREAD("object_index::DebugUnlinkSlot");
    if (index < 0 || index >= static_cast<int32_t>(g_slots.size())) return;
    const Slot& s = g_slots[static_cast<size_t>(index)];
    if (!s.obj) return;
    void* const gone = s.cls;
    if (Unlink(index) && g_observer.OnClassGone) g_observer.OnClassGone(g_observer.ctx, gone);
}

void DebugApplyCreate(void* obj, void* cls, int32_t index) {
    UE_ASSERT_GAME_THREAD("object_index::DebugApplyCreate");
    ApplyCreated(Event{obj, cls, index, true});
}

bool DebugSlotListing(int32_t index, void** obj, void** cls) {
    UE_ASSERT_GAME_THREAD("object_index::DebugSlotListing");
    if (index < 0 || index >= static_cast<int32_t>(g_slots.size())) return false;
    const Slot& s = g_slots[static_cast<size_t>(index)];
    if (!s.obj) return false;
    *obj = s.obj;
    *cls = s.cls;
    return true;
}

void DebugListUnchecked(void* obj, void* cls, int32_t index) {
    UE_ASSERT_GAME_THREAD("object_index::DebugListUnchecked");
    DebugUnlinkSlot(index);
    Link(index, obj, cls);
}

}  // namespace ue_wrap::object_index
