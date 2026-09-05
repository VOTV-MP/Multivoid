// coop/interactables/garbage_sync.cpp -- stop the open-container per-tick crash when picking
// up a garbage pile on the client. The open container's tick and pickup check walk an array
// of contained actors every tick; on the client, after a save load and a per-peer divergent
// underground spawner, the array holds pointers to entities the host spawned but the client
// never did (or the reverse), and walking them dereferences a freed actor. Local-only pickup
// mechanics (the container's mesh, collision and physics-handle grab) do not need that
// blueprint body; cancelling it on the client removes the crash without touching the
// held-pose stream or the prop pose and release sync that already works for these actors.
// The cancel is gated on class: only the garbage container (and any future garbage-only
// subclass of the open container) cancels; storage suitcases, drawers and other open
// containers tick normally.

#include "coop/interactables/garbage_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
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

// Installer state, one-shot per process; Install retries until the open-container class is
// loaded (blueprint classes load on demand at the first world enter).
std::atomic<bool> g_installed{false};

// The garbage-container class, resolved once at install and reused per tick for a
// pointer-compare class filter. A per-call class-name search allocated a fresh string on
// every dispatch, on the blueprint dispatch hot path, one heap allocation per frame per open
// container; a pointer compare allocates nothing. Only the garbage container is the
// open-container subclass that matters (the garbage bag derives from the prop base and the
// bin from the container class, neither on the intercepted target); a future garbage-only
// subclass would need the superclass walk here.
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
    // A throttled cancel log, so the path proves it fires the first few times while a 60 Hz
    // tick over many garbage containers does not drown the log. The same throttle policy as the
    // grab observer's per-tick logs.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 300) == 0) {
        UE_LOGI("garbage_sync[ReceiveTick PRE]: cancelling BP body on client garbage container %p (call #%llu)",
                self, static_cast<unsigned long long>(n));
    }
    return true;
}

bool OnOpenContainerCheckPickupPre(void* self, void* /*params*/) {
    auto* s = LoadSession();
    // Gated on running, not a bare role: the role is the config's, and the stop never resets it,
    // so a bare role gate keeps cancelling in solo play after a client session ends. A
    // function-body cancel self-restores once gated on running.
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;
    if (!IsGarbageInstance(self)) return false;
    UE_LOGI("garbage_sync[checkPickup PRE]: cancelling BP body on client garbage container %p",
            self);
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
MAKE_SPAWNER_CANCEL(OnBaseCleanerTrashBitsBeginPlayPre,
                    "baseCleaner_trashBits.BeginPlay")

#undef MAKE_SPAWNER_CANCEL

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
    };
    // The bound-event name for the trash-piles event is the full canonical delegate signature
    // from the header dump; long names are routine in blueprint overlap handlers, and the
    // function lookup matches by name. The trash-bits cleaner has no methods of its own and
    // inherits its begin-play from the base cleaner class; registering on the leaf picks up a
    // future override, and otherwise the lookup walks up the superclass chain and returns the
    // parent's UFunction, the same dispatch object.
    const Target targets[] = {
        {L"event_trashPiles_C",
            L"BndEvt__event_funnyGascans_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
            &OnEventTrashPilesOverlapPre,
            "event_trashPiles.BndEvt"},
        {L"arirTrasher_C", L"trash",
            &OnArirTrasherTrashPre,
            "arirTrasher.trash"},
        {L"baseCleaner_trashBits_C", L"ReceiveBeginPlay",
            &OnBaseCleanerTrashBitsBeginPlayPre,
            "baseCleaner_trashBits.BeginPlay"},
    };
    int registered = 0;
    for (const auto& t : targets) {
        void* cls = R::FindClass(t.cls);
        if (!cls) continue;  // BP class not loaded yet; retry next Install()
        void* fn = R::FindFunction(cls, t.fn);
        if (!fn) {
            UE_LOGW("garbage_sync[spawner]: UFunction '%ls' not found on %ls -- skipping",
                    t.fn, t.cls);
            continue;
        }
        if (!GT::RegisterInterceptor(fn, t.cb)) {
            UE_LOGE("garbage_sync[spawner]: RegisterInterceptor failed for %ls::%ls (table full?)",
                    t.cls, t.fn);
            continue;
        }
        ++registered;
        UE_LOGI("garbage_sync[spawner]: PRE-interceptor installed -- %ls::%ls (%s)",
                t.cls, t.fn, t.tag);
    }
    // Latch as installed only when all targets resolved and registered: a partial install
    // leaves some spawners ungated and the per-peer divergence remains. Retry on the next
    // Install call until everything is loaded.
    const int total = static_cast<int>(sizeof(targets) / sizeof(targets[0]));
    if (registered == total) {
        g_spawnersInstalled.store(true, std::memory_order_release);
        UE_LOGI("garbage_sync[spawner]: Inc 3 install complete -- %d spawners suppressed on client (tool_garbageSpawner_C deliberately allow-through per principle 6)",
                registered);
        return true;
    }
    return false;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void Install() {
    if (g_installed.load(std::memory_order_acquire)) return;
    void* cls = R::FindClass(L"prop_openContainer_C");
    if (!cls) {
        // The class is not loaded yet; retry on the next Install call. No noise: the class loads on
        // the first world enter, so this is expected for the first seconds after boot.
        return;
    }
    // Resolve and cache the garbage-container class for the per-tick filter. Gating the install
    // on both classes being loaded avoids the interceptors firing while the class is null, which
    // would let the body run on a garbage container (the filter false-negative) and re-trigger
    // the crash this module exists to prevent. Both classes load at world enter from the same
    // asset tree, so they typically resolve together.
    void* garbageCls = R::FindClass(L"prop_garbageContainer_C");
    if (!garbageCls) {
        // The same quiet retry as the open-container case above.
        return;
    }
    g_garbageContainerCls = garbageCls;
    void* tickFn      = R::FindFunction(cls, L"ReceiveTick");
    void* checkPickup = R::FindFunction(cls, L"checkPickup");
    if (!tickFn || !checkPickup) {
        UE_LOGW("garbage_sync: UFunction(s) not found on prop_openContainer_C (tick=%p checkPickup=%p) -- BP class loaded but missing the expected names; CXX dump may be stale",
                tickFn, checkPickup);
        return;
    }
    const bool okTick  = GT::RegisterInterceptor(tickFn,      &OnOpenContainerReceiveTickPre);
    const bool okPick  = GT::RegisterInterceptor(checkPickup, &OnOpenContainerCheckPickupPre);
    if (!okTick || !okPick) {
        UE_LOGE("garbage_sync: RegisterInterceptor failed (tick=%d checkPickup=%d) -- interceptor table full?",
                okTick ? 1 : 0, okPick ? 1 : 0);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("garbage_sync: Inc 1 installed -- prop_openContainer_C::ReceiveTick + checkPickup PRE-interceptors (client-side, garbageContainer UClass=%p)",
            g_garbageContainerCls);
    // Try the spawner suppressors in the same call; an independent retry path if any spawner
    // class has not loaded yet.
    InstallSpawnerSuppressors();
}

}  // namespace coop::garbage_sync
