// coop/garbage_sync.cpp -- Phase 5G Inc 1: stop the open-container per-tick
// AV when picking up a garbage pile on the client.
//
// Root cause (research/findings/votv-garbage-trash-interaction-RE-2026-05-27.md):
// Aprop_openContainer_C::ReceiveTick + checkPickup walk a TArray<AActor*>
// itemsInside at +0x378 every tick. On the client, after a save-load + the
// per-peer-divergent undergroundGarbageSpawner has run, the array holds
// AActor* pointers to entities the host spawned but the client never did
// (or vice versa). Walking those derefs a freed/GC'd actor -> AV reading
// at offset 0x0F (an AActor flag byte on a stale pointer). Local-only
// pickup mechanics (the GarbageContainer's mesh + collision + PHC grab)
// don't need that BP body; cancelling it via PRE-interceptor on the
// client removes the AV without affecting the held-pose stream or the
// PropPose/PropRelease shell sync that already works for these actors.
//
// The cancel is GATED on class name -- only Aprop_garbageContainer_C (and
// any future garbage-only subclass of openContainer) cancels. Storage
// suitcases / drawers / other open containers tick normally.

#include "coop/interactables/garbage_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <string>

namespace coop::garbage_sync {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace {

// Session pointer (atomic for the parallel-anim-worker ProcessEvent shape).
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// Installer state -- one-shot per process. Retries on Install() until the
// open-container UClass is loaded (BP classes load on-demand at first
// world enter).
std::atomic<bool> g_installed{false};

// Garbage-container UClass cache, resolved ONCE at Install() time and
// reused per-tick for a pointer-compare class filter. Replaces an earlier
// `R::ClassNameOf(self).find(L"garbage")` per-call test that allocated a
// fresh std::wstring on every dispatch (audit I1, 2026-05-27): heap alloc
// per frame per open-container instance, on the BP-VM dispatch hot path.
// With kMaxObservers fan-out and N open containers in the world that was
// N heap allocs/frame. Single pointer compare is O(1) zero-alloc.
//
// Today only `Aprop_garbageContainer_C` is the openContainer subclass we
// care about (Aprop_garbageBag_C derives from Aprop_C directly,
// Aprop_garbageBin_C derives from Aprop_container_C -- neither is on the
// openContainer interceptor target). If a future patch adds another
// garbage-only openContainer subclass, walk the SuperStruct chain in
// IsGarbageInstance to cover it (~3 ptr loads, still no alloc).
void* g_garbageContainerCls = nullptr;

bool IsGarbageInstance(void* self) {
    if (!g_garbageContainerCls) return false;  // not resolved yet
    return R::ClassOf(self) == g_garbageContainerCls;
}

// PRE-interceptor: client + garbage-class -> return true (cancel BP body).
// Host + any other open-container subclass -> return false (run normally).
bool OnOpenContainerReceiveTickPre(void* self, void* /*params*/) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client) return false;
    if (!IsGarbageInstance(self)) return false;
    // Throttled cancel-log so we can prove the path fires the first few
    // times but a 60-Hz tick over many garbage containers doesn't drown
    // the log. Same throttle policy as grab_observer's per-tick logs.
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
    if (!s || s->role() != coop::net::Role::Client) return false;
    if (!IsGarbageInstance(self)) return false;
    UE_LOGI("garbage_sync[checkPickup PRE]: cancelling BP body on client garbage container %p",
            self);
    return true;
}

// ---- Inc 3: host-authoritative spawner suppression ---------------------
//
// Three spawners whose output is in the trash/keyed sync universe cancel on
// the client so only the host rolls those spawns; their output rides the
// prop pipeline back to the client. Tool_garbageSpawner_C is INTENTIONALLY
// not suppressed -- toolgun fire is a per-shot local player action
// (principle 6: augment SP, route per-player inside SP systems), and its
// spawn output is captured by the Init POST observer on the firing peer +
// broadcast from there.
//
// undergroundGarbageSpawner_C deliberately UN-suppressed (Fork C
// 2026-06-10): bytecode-proven to mint ONLY dirthole_item_C buried-item
// mounds (NOT chip piles -- the old row's "output rides the broadcast"
// rationale was falsified: dirthole_item is outside the snapshot +
// broadcast universe entirely), so suppressing it just deleted the client's
// per-peer loot mounds with no host replacement. Dirtholes are per-peer
// LOCAL by doctrine (COOP_SCOPE).
//
// All 3 use RegisterInterceptor with the same shared callback that role-
// gates on client + returns true. Cancel-throttled per-class to keep the
// log readable while still proving the path fires.

// Generic role-gated PRE-cancel for a periodic / event spawner. The
// callback uses a static atomic counter per call site (via the
// MAKE_SPAWNER_CANCEL macro) so each spawner's first 3 + every 60th
// log lines are distinguishable in the log.
#define MAKE_SPAWNER_CANCEL(fn_name, log_tag)                                       \
bool fn_name(void* self, void* /*params*/) {                                        \
    auto* s = LoadSession();                                                        \
    if (!s || s->role() != coop::net::Role::Client) return false;                   \
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

// State for Inc 3 spawner installs. Independent of g_installed so an
// Inc 1 success doesn't preempt the Inc 3 retry loop (and vice versa).
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
    // The BndEvt name for Aevent_trashPiles_C is the full canonical
    // delegate signature per the CXX dump. Long names are routine in BP
    // overlap handlers; FindFunction returns the UFunction by name match.
    // baseCleaner_trashBits_C has no methods of its own; the inherited
    // ReceiveBeginPlay is on the BASE baseCleaner_C class. We register on
    // the leaf so a future override picks up; if the BP author hasn't
    // overridden, FindFunction walks up the SuperStruct chain and returns
    // the parent's UFunction -- same dispatch object.
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
    // Latch as installed only when ALL targets resolved + registered.
    // A partial install leaves some spawners ungated -> per-peer divergence
    // bug remains. Retry next Install() call until everything's loaded.
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
        // BP class not loaded yet -- retry on the next Install() call from
        // harness. No noise: the BP class loads on first world enter so
        // this is expected for the first few seconds after boot.
        return;
    }
    // Resolve + cache the garbage container UClass for the per-tick filter.
    // Gating Install() on BOTH classes being loaded avoids the case where
    // the interceptors fire while g_garbageContainerCls is null -- which
    // would let the BP body run on a garbage container (since the filter
    // false-negatives) and re-trigger the AV that this whole module exists
    // to prevent. Both BP classes load at world enter from the same .uasset
    // tree, so they typically resolve together.
    void* garbageCls = R::FindClass(L"prop_garbageContainer_C");
    if (!garbageCls) {
        // Same retry-quietly behaviour as the openContainer case above.
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
    // Try Inc 3 (spawner suppressors) in the same call; independent retry
    // path if any spawner class hasn't loaded yet.
    InstallSpawnerSuppressors();
}

}  // namespace coop::garbage_sync
