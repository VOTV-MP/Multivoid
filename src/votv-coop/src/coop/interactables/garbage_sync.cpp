// coop/interactables/garbage_sync.cpp -- the client's garbage container runs no brain of its own.
// ONE body is cancelled, prop_openContainer_C::ReceiveTick, on instances whose class is exactly
// prop_garbageContainer_C; every other open container ticks normally.
//
// Read from the cook: upright and not held, that body walks a fresh Box.GetOverlappingActors query
// every 0.25 s and APPENDS each accepted prop to itemsInside, calling setPropProps and
// K2_AttachToActor on it; propAwoken then unfreezes every prop in the array and writes the
// container's own velocities into it, most often when a player grabs one of them rather than on a
// tip-over. So the cancel stops a real second author (on a client those props are mirrors) AND
// blinds the client, which never learns its contents -- docs/piles.md has the visible half, the
// crutch register the root fix and the crash claim it still owes.
//
// checkPickup is NOT hooked and cannot be: its three callers are EX_LocalVirtualFunction inside
// the ubergraph, invisible to a ProcessEvent interceptor (docs/coop-dispatch-visibility.md). The
// canHold/canCollect flags freeze because the tick that reaches them is cancelled.

#include "coop/interactables/garbage_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <string>

namespace coop::garbage_sync {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace {

// The session pointer (atomic for the parallel-anim worker dispatch shape).
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// Installer state, one-shot per process: set once the open-container half has settled, installed or
// refused for good. The classes this file hooks are loaded at the main menu and live for the process
// (coop/dev/class_lifetime_probe measures it), so the pointers below and the registrations hold across
// a world change.
std::atomic<bool> g_installed{false};

// The garbage-container class, resolved once at install and reused per tick for a
// pointer-compare class filter. A per-call class-name search allocated a fresh string on
// every dispatch, on the blueprint dispatch hot path, one heap allocation per frame per open
// container; a pointer compare allocates nothing. Only the garbage container is the
// open-container subclass that matters (the garbage bag derives from the prop base and the
// bin from the container class, neither on the intercepted target). The compare is EXACT, so a
// future garbage-only subclass of it would not be caught and would need a descendant test here.
void* g_garbageContainerCls = nullptr;

bool IsGarbageInstance(void* self) {
    if (!g_garbageContainerCls) return false;  // not resolved yet
    return R::ClassOf(self) == g_garbageContainerCls;
}

// The interceptor: a client and the garbage class returns true (cancel the blueprint body);
// the host, or any other open-container subclass, returns false (run normally).
bool OnOpenContainerReceiveTickPre(void* self, void* /*params*/) {
    auto* s = LoadSession();
    // Gated on running, not a bare role: the role is the config's, and the stop never resets it,
    // so a bare role gate keeps cancelling in solo play after a client session ends. A
    // function-body cancel self-restores once gated on running.
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;
    if (!IsGarbageInstance(self)) return false;
    // A throttled cancel log, so the path proves it fires the first few times while a 4 Hz tick
    // (the class's own TickInterval is 0.25 s) over many garbage containers does not drown the
    // log. The same throttle policy as the grab observer's per-tick logs.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 300) == 0) {
        UE_LOGI("garbage_sync[ReceiveTick PRE]: cancelling BP body on client garbage container %p (call #%llu)",
                self, static_cast<unsigned long long>(n));
    }
    return true;
}

// Host-authoritative spawner suppression. Three spawners whose output is in the trash and
// keyed sync universe cancel on the client, so only the host rolls those spawns and their
// output rides the prop pipeline back to the client. The toolgun spawner is deliberately not
// suppressed: toolgun fire is a per-shot local player action (augment single player, route
// per player inside its systems), and its spawn output is captured by the init observer on
// the firing peer and broadcast from there. The underground garbage spawner is deliberately
// unsuppressed too: it mints only buried-item mounds, not chip piles, outside the snapshot
// and broadcast universe entirely, so suppressing it deleted the client's per-peer loot
// mounds with no host replacement; dirt holes are per-peer local by design. All three use
// one shared callback that role-gates on the client and returns true, cancel-throttled per
// class to keep the log readable while still proving the path fires.

// The generic role-gated cancel for a periodic or event spawner. The callback uses a static
// counter per call site (the macro), so each spawner's first three and every sixtieth log
// lines are distinguishable.
#define MAKE_SPAWNER_CANCEL(fn_name, log_tag)                                       \
bool fn_name(void* self, void* /*params*/) {                                        \
    auto* s = LoadSession();                                                        \
    /* running()-gated, not bare role() -- the post-session SP-bleed class (see above) */ \
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;  \
    static std::atomic<uint64_t> sCount{0};                                         \
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;          \
    if (n <= 3 || (n % 60) == 0) {                                                  \
        UE_LOGI("garbage_sync[%s PRE]: client-cancel spawner %p (call #%llu)",      \
                log_tag, self, static_cast<unsigned long long>(n));                 \
    }                                                                               \
    return true;                                                                    \
}

MAKE_SPAWNER_CANCEL(OnEventTrashPilesOverlapPre,
                    "event_trashPiles.BndEvt")
MAKE_SPAWNER_CANCEL(OnArirTrasherTrashPre,
                    "arirTrasher.trash")
#undef MAKE_SPAWNER_CANCEL

// The trash-bits cleaner's class, resolved at install for the filter below.
void* g_trashBitsCleanerCls = nullptr;

// The cleaner is NOT a spawner and cannot use the macro above. Its begin-play box-overlaps and
// DESTROYS actors, so cancelling it on a client is only right where the host's destroy reaches
// the client some other way -- which is true of the trash bits and of nothing else in the family.
// The verb is declared on baseCleaner_C, so the one UFunction also dispatches for the clump,
// grime and wall-crack cleaners; wall cracks have no sync lane anywhere in this tree, so a
// family-wide cancel would leave a client holding cracks the host cleaned, with nothing to
// reconcile them. Hence the instance filter, the same shape the container guard uses above.
bool OnBaseCleanerTrashBitsBeginPlayPre(void* self, void* /*params*/) {
    auto* s = LoadSession();
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;
    if (!g_trashBitsCleanerCls || R::ClassOf(self) != g_trashBitsCleanerCls) return false;
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 60) == 0) {
        UE_LOGI("garbage_sync[baseCleaner_trashBits.BeginPlay PRE]: client-cancel cleaner %p "
                "(call #%llu)", self, static_cast<unsigned long long>(n));
    }
    return true;
}

// State for the spawner installs, independent of the container latch so one success does
// not pre-empt the other's retry loop.
std::atomic<bool> g_spawnersInstalled{false};

bool InstallSpawnerSuppressors() {
    if (g_spawnersInstalled.load(std::memory_order_acquire)) return true;
    using ue_wrap::game_thread::UFunctionInterceptor;
    struct Target {
        const wchar_t* cls;
        const wchar_t* fn;
        UFunctionInterceptor cb;
        const char* tag;
        bool settled;     // registered, or refused for good: a settled row is not tried again
        bool registered;
    };
    // The bound-event name for the trash-piles event is the full canonical delegate signature
    // from the header dump; long names are routine in blueprint overlap handlers, and the
    // function lookup matches by name.
    //
    // Each row is asked for the DISPATCH function, not the declaration. The trash-bits cleaner
    // declares nothing at all and inherits its begin-play from the base cleaner, so FindFunction
    // -- exact-owner by contract -- returned null for it and the cancel was never installed at
    // all, leaving a client to run a body that box-overlaps and destroys props the host holds.
    // The declarer being the base means one UFunction serves all four cleaners, so the callback
    // filters on the instance; the comment above it says why the family is the wrong scope.
    static Target targets[] = {
        {L"event_trashPiles_C",
            L"BndEvt__event_funnyGascans_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
            &OnEventTrashPilesOverlapPre,
            "event_trashPiles.BndEvt", false, false},
        {L"arirTrasher_C", L"trash",
            &OnArirTrasherTrashPre,
            "arirTrasher.trash", false, false},
        {L"baseCleaner_trashBits_C", L"ReceiveBeginPlay",
            &OnBaseCleanerTrashBitsBeginPlayPre,
            "baseCleaner_trashBits.BeginPlay", false, false},
    };
    int settled = 0;
    for (auto& t : targets) {
        if (t.settled) { ++settled; continue; }
        // One index probe while the class is not loaded; the next call asks again.
        void* cls = ue_wrap::object_index::ClassByName(t.cls);
        if (!cls) continue;
        // A Blueprint class loads with its functions, so from here the row settles either way: a
        // missing name is a recooked Blueprint, and a full table is a capacity fault to fix, not a
        // slot to wait for.
        t.settled = true;
        ++settled;
        void* declarer = nullptr;
        void* fn = R::FindDispatchFunction(cls, t.fn, &declarer);
        if (!fn) {
            UE_LOGW("garbage_sync[spawner]: UFunction '%ls' not found on %ls or its supers -- that "
                    "spawner runs on a client", t.fn, t.cls);
            continue;
        }
        // The class its callback filters on, captured from the row that owns it -- not a second
        // lookup, which would be another walk of the object array.
        if (t.cb == &OnBaseCleanerTrashBitsBeginPlayPre) g_trashBitsCleanerCls = cls;
        if (declarer != cls) {
            // One UFunction for the whole family under `declarer`, so this row's callback must
            // filter on the instance -- logged, because a silent family-wide seam is how a
            // sibling class gets cancelled by accident.
            UE_LOGI("garbage_sync[spawner]: %ls::%ls is declared on %ls -- one object for that "
                    "family, so the callback filters on the instance",
                    t.cls, t.fn, R::ToString(R::NameOf(declarer)).c_str());
        }
        if (!GT::RegisterInterceptor(fn, t.cb)) {
            UE_LOGE("garbage_sync[spawner]: RegisterInterceptor failed for %ls::%ls (the interceptor "
                    "table is full) -- that spawner runs on a client", t.cls, t.fn);
            continue;
        }
        t.registered = true;
        UE_LOGI("garbage_sync[spawner]: PRE-interceptor installed -- %ls::%ls (%s)",
                t.cls, t.fn, t.tag);
    }
    // Latched once every row has settled. Install drives this half on its own until then: it used
    // to be called only from the tail of the container install, which had already latched and
    // returned early, so a class not loaded at that one instant stayed ungated for the session.
    const int total = static_cast<int>(sizeof(targets) / sizeof(targets[0]));
    if (settled == total) {
        g_spawnersInstalled.store(true, std::memory_order_release);
        int registered = 0;
        for (const auto& t : targets) registered += t.registered ? 1 : 0;
        UE_LOGI("garbage_sync[spawner]: install complete -- %d of %d spawners suppressed on client (tool_garbageSpawner_C deliberately allow-through per principle 6)",
                registered, total);
        return true;
    }
    return false;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void Install() {
    // The two halves settle independently, each on the classes the other does not resolve; a class
    // lookup is one index probe, so both run on every call until they have.
    InstallSpawnerSuppressors();
    if (g_installed.load(std::memory_order_acquire)) return;
    // The filter class is also the resolve subject: ask it which body ITS instances run, so a cook
    // that ever gives the garbage container its own ReceiveTick is hooked on that one instead of on
    // the base it would override (the R-11 rule; the declarer is logged when it is not this class).
    void* garbageCls = ue_wrap::object_index::ClassByName(L"prop_garbageContainer_C");
    if (!garbageCls) return;
    // Settled from here, as a spawner row is above.
    g_installed.store(true, std::memory_order_release);
    g_garbageContainerCls = garbageCls;
    void* declarer = nullptr;
    void* tickFn = R::FindDispatchFunction(garbageCls, L"ReceiveTick", &declarer);
    if (!tickFn) {
        UE_LOGW("garbage_sync: ReceiveTick not found on prop_garbageContainer_C or its supers -- "
                "BP class loaded but missing the expected name; the container runs on a client");
        return;
    }
    if (declarer != garbageCls) {
        UE_LOGI("garbage_sync: ReceiveTick is declared on %ls -- one object for that family, so the "
                "callback filters on the instance",
                R::ToString(R::NameOf(declarer)).c_str());
    }
    const bool okTick = GT::RegisterInterceptor(tickFn, &OnOpenContainerReceiveTickPre);
    if (!okTick) {
        UE_LOGE("garbage_sync: RegisterInterceptor(ReceiveTick) failed (the interceptor table is "
                "full) -- the container runs on a client");
        return;
    }
    UE_LOGI("garbage_sync: installed -- ReceiveTick PRE-interceptor (client-side, garbageContainer "
            "UClass=%p, declared on %ls)",
            g_garbageContainerCls, R::ToString(R::NameOf(declarer)).c_str());
}

}  // namespace coop::garbage_sync
