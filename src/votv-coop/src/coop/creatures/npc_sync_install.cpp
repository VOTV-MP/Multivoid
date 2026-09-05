// coop/creatures/npc_sync_install.cpp -- the staged reflection resolve and the observer and
// interceptor registration of the NPC spawn seam, plus its readiness and refs surface. Owns
// the install-side state: the installed latch, the resolve caches, the observer latches. The
// shared globals the seam's hot paths read (the three param offsets, the allowlist and the
// disabled flag) are defined here and declared in npc_sync_internal.h; the three ProcessEvent
// callbacks this file registers are defined in npc_sync.cpp. Game thread only: Install runs
// from the pump tick at world-up.

#include "coop/creatures/npc_sync.h"
#include "npc_sync_internal.h"  // the 5 shared globals (defined below) + the 3 callback decls

#include "coop/creatures/npc_mirror.h"      // ClientRefs / SetClientRefs (receiver-side cache push)
#include "coop/creatures/npc_world_enum.h"  // InstallExSpawnCatch (the EX_CallMath spawn catch)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>

namespace coop::npc_sync {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Install-owned state; the globals npc_sync_internal.h declares are the shared subset, defined
// here and read by the seam.

// Idempotency: once installed, or permanently failed, Install short-circuits. Atomic: Install
// runs on the game thread, and the interceptor and observer paths read it indirectly through
// latches that depend on Install having completed.
std::atomic<bool> g_installed{false};

// The allowlisted NPC classes, resolved at install from the allowlist names in sdk_profile.h.
// Sized by the allowlist constant, so an addition to the list binds the array length too.
void* g_npcAllowlist[ue_wrap::profile::name::kNpcAllowlistSize] = {};

void* g_npcSpawnFn = nullptr;
int32_t g_npcSpawnActorClassParamOff = -1;
int32_t g_npcSpawnReturnParamOff = -1;
int32_t g_npcSpawnXformParamOff = -1;  // SpawnTransform; cached at Install time

// Install bookkeeping for the receiver-side cache: the client-side refs live in npc_mirror.
// These two are kept here only because the resolution happens in two stages (the
// GameplayStatics primitives first, then the destroy UFunction once the actor class binds),
// and the full set is pushed together when the destroy function lands. The receivers do not
// read these.
void* g_installCacheFinishSpawnFn = nullptr;
void* g_installCacheGsCdo = nullptr;

// The observer handle bookkeeping, so a second Install does not double-register. Atomic, so
// the host PRE interceptor can read them from a parallel-animation worker while the game
// thread writes them.
std::atomic<bool> g_destroyObserverInstalled{false};
std::atomic<bool> g_spawnPostObserverInstalled{false};
// True after Install permanently gave up registering one of the lifecycle observers (the table
// was full). The PRE path gates host-side EntitySpawn broadcasts on it, so a partial-lifecycle
// install never leaks Npc Elements, allocated by PRE and never bound or destroyed.
std::atomic<bool> g_npcSyncDisabledThisProcess{false};

bool IsInstalled() {
    // True once Install's attempt completed, so it stops retrying. On the happy path the latch is
    // set only after every allowlisted class resolves (a partial resolve early-returns without
    // latching), so a latched install resolved all of it and the allowlist test is fully usable.
    // On a broken build (the deferred-spawn UFunction or its params unresolvable) it latches with
    // a null allowlist to stop retrying, and the reconcile sweep safely no-ops. Deliberately
    // ignores the disabled flag (the interceptor disabled but the allowlist resolved): callers
    // must not block unrelated prop or door replay on the NPC interceptor being live.
    return g_installed.load(std::memory_order_acquire);
}

void Install(coop::net::Session* session) {
    SetSession(session);  // cache (caller guarantees outlives us)
    if (g_installed.load(std::memory_order_acquire)) return;
    // The throttle guard: every Find call below walks the object array with a string allocation
    // per entry, so retries are bounded to about one per half second during the unresolved window.
    static int s_installRetryCountdown = 0;
    if (s_installRetryCountdown > 0) {
        --s_installRetryCountdown;
        return;
    }
    // Cache the intermediate resolutions: once the class, the function and the offsets are
    // resolved, later retries skip them; a partial NPC-class resolution would otherwise re-walk
    // all five every retry until the classes finish loading.
    if (!g_npcSpawnFn) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        if (!gsCls) {
            s_installRetryCountdown = 60;
            return;
        }
        void* fn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
        if (!fn) {
            UE_LOGW("npc-suppress: %ls.%ls UFunction not found -- disabled permanently",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t classOff = R::FindParamOffset(fn, L"ActorClass");
        if (classOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'ActorClass' param not found (BP recook?) -- disabled",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t retOff = R::FindParamOffset(fn, L"ReturnValue");
        if (retOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'ReturnValue' param not found -- disabled",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t xformOff = R::FindParamOffset(fn, L"SpawnTransform");
        if (xformOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'SpawnTransform' param not found -- "
                    "EntitySpawn broadcasts will lack position/rotation",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            // Do not bail: position-less spawns still work.
        }
        // The receiver side, in npc_mirror: resolve the finish-spawn UFunction and the
        // GameplayStatics CDO, so the spawn receiver can materialise mirrors without re-walking the
        // object array on every host broadcast. Non-fatal if missing; the receiver null-checks each
        // field and logs.
        void* finishFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        if (!finishFn) {
            UE_LOGW("npc-sync[receiver]: %ls.%ls UFunction not found -- client mirror "
                    "materialization will be disabled (host EntitySpawn packets will be "
                    "logged + dropped)",
                    P::name::GameplayStaticsClass, P::name::FinishSpawningActorFn);
        }
        void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
        if (!gsCdo) {
            UE_LOGW("npc-sync[receiver]: %ls CDO not found -- client mirror materialization "
                    "will be disabled",
                    P::name::GameplayStaticsClass);
        }
        // Commit the cache; later retries skip the five Find calls above and only retry the class
        // loop.
        g_npcSpawnFn = fn;
        g_npcSpawnActorClassParamOff = classOff;
        g_npcSpawnReturnParamOff = retOff;
        g_npcSpawnXformParamOff = xformOff;
        // Stash for the second push, where the destroy function lands together; the partial set is
        // also pushed now, so the receiver degrades gracefully (a drop with a warning) if the
        // install retries before the destroy function resolves.
        g_installCacheFinishSpawnFn = finishFn;
        g_installCacheGsCdo         = gsCdo;
        coop::npc_mirror::ClientRefs refs{};
        refs.spawnFn             = fn;
        refs.finishSpawnFn       = finishFn;
        refs.gsCdo               = gsCdo;
        refs.spawnReturnParamOff = retOff;
        refs.k2DestroyFn         = nullptr;  // resolved + re-pushed below
        coop::npc_mirror::SetClientRefs(refs);
    }
    void* const fn = g_npcSpawnFn;
    const int32_t classOff = g_npcSpawnActorClassParamOff;
    const int32_t retOff = g_npcSpawnReturnParamOff;
    const int32_t xformOff = g_npcSpawnXformParamOff;

    // Resolve the NPC classes. A partial resolution is fine here, since a missing class is simply
    // not suppressed yet; most of the game's NPC classes are loaded on gameplay-level entry.
    // Already-resolved entries skip the lookup, and the unresolved walks are bounded by the
    // throttle gate at the top.
    size_t resolved = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (!g_npcAllowlist[i]) {
            g_npcAllowlist[i] = R::FindClass(P::name::kNpcAllowlist[i]);
        }
        if (g_npcAllowlist[i]) ++resolved;
    }
    if (resolved < P::name::kNpcAllowlistSize) {
        // Not installed yet: every class must be cached before going live, or some NPCs would be
        // suppressed and others not, depending on resolve timing. Throttle the next attempt.
        s_installRetryCountdown = 60;
        UE_LOGI("npc-suppress: NPC class load partial (%zu/%zu) -- throttled retry in ~0.5s",
                resolved, P::name::kNpcAllowlistSize);
        return;
    }

    // Every class resolved. Cache the function pointer and the offsets. The lifecycle observers go
    // in first: if either registration fails (the observer table full), the disabled flag is set
    // and the interceptor registration below is skipped, so a permanent interceptor slot is not
    // burnt for a system that cannot function.
    g_npcSpawnFn = fn;
    g_npcSpawnActorClassParamOff = classOff;
    g_npcSpawnReturnParamOff = retOff;
    g_npcSpawnXformParamOff = xformOff;  // may be -1 if param missing; interceptor null-checks

    // Atomic two-observer registration: if either lifecycle observer fails, the other is rolled
    // back, so a disabled system burns no permanent slot. The destroy function's reflection
    // dependencies are resolved first; if they fail, POST is skipped entirely and nothing needs
    // rolling back.
    if (!g_spawnPostObserverInstalled.load(std::memory_order_acquire) ||
        !g_destroyObserverInstalled.load(std::memory_order_acquire)) {
        void* actorCls = R::FindClass(P::name::ActorClassName);
        void* destroyFn = actorCls ? R::FindFunction(actorCls, P::name::DestroyActorFn) : nullptr;
        if (!actorCls || !destroyFn) {
            g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
            UE_LOGE("npc-sync: cannot resolve %ls.%ls (actorCls=%p destroyFn=%p) -- NPC sync "
                    "DISABLED for process lifetime (Element lifecycle cannot close)",
                    P::name::ActorClassName, P::name::DestroyActorFn, actorCls, destroyFn);
        } else {
            // Promote the destroy function into the receiver-side cache owned by npc_mirror (the
            // destroy receiver reuses it on every host teardown, skipping an object-array walk per
            // packet). The full refs are re-pushed, so the receiver sees the GameplayStatics set
            // cached above together with the newly resolved destroy function.
            {
                coop::npc_mirror::ClientRefs refs{};
                refs.spawnFn             = g_npcSpawnFn;
                refs.finishSpawnFn       = g_installCacheFinishSpawnFn;
                refs.gsCdo               = g_installCacheGsCdo;
                refs.spawnReturnParamOff = g_npcSpawnReturnParamOff;
                refs.k2DestroyFn         = destroyFn;
                coop::npc_mirror::SetClientRefs(refs);
            }
            // The POST observer first; if it succeeds, register the destroy PRE observer. If that
            // fails, roll back the POST, so a half-installed state does not burn an observer-table
            // slot.
            const bool postOk = ue_wrap::game_thread::RegisterPostObserver(fn, &NpcSpawn_POST);
            if (!postOk) {
                g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
                UE_LOGE("npc-sync: RegisterPostObserver FAILED (observer table full) -- "
                        "NPC sync DISABLED for process lifetime");
            } else if (!ue_wrap::game_thread::RegisterPreObserver(destroyFn, &NpcDestroy_PRE)) {
                // The destroy observer failed after POST succeeded: roll back POST, so it does not
                // fire forever for a disabled system.
                ue_wrap::game_thread::UnregisterObservers(fn, &NpcSpawn_POST);
                g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
                UE_LOGE("npc-sync: RegisterPreObserver FAILED for K2_DestroyActor "
                        "(observer table full) -- NPC sync DISABLED + rolled back POST "
                        "registration to free its slot");
            } else {
                g_spawnPostObserverInstalled.store(true, std::memory_order_release);
                g_destroyObserverInstalled.store(true, std::memory_order_release);
                UE_LOGI("npc-sync: registered POST observer for %ls.%ls (binds AActor* into Npc Element)",
                        P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
                UE_LOGI("npc-sync: registered K2_DestroyActor PRE observer (Npc Element lifecycle close)");
            }
        }
    }

    // Register the PRE interceptor last, and only if both lifecycle observers succeeded: it
    // consumes a slot in the interceptor table, and if NPC sync is disabled for the session the
    // slot stays free for other subsystems. The client-side suppression the interceptor implements
    // is also useless without the host-side broadcast pipeline.
    g_installed.store(true, std::memory_order_release);
    if (g_npcSyncDisabledThisProcess.load(std::memory_order_acquire)) {
        UE_LOGW("npc-suppress: lifecycle observer install FAILED -- skipping interceptor "
                "registration entirely (NPC sync disabled for process lifetime; see prior "
                "[Error] lines)");
        return;
    }
    ue_wrap::game_thread::RegisterInterceptor(fn, &NpcSuppress_Interceptor);
    UE_LOGI("npc-suppress: installed interceptor on %ls.%ls @ %p (ActorClass@%d, ReturnValue@%d, SpawnTransform@%d, %zu/%zu NPC classes resolved + lifecycle observers live)",
            P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn,
            fn, classOff, retOff, xformOff,
            P::name::kNpcAllowlistSize, P::name::kNpcAllowlistSize);
    // The EX_CallMath spawn catch (the Func thunk, source-gated), installed under the same
    // lifecycle gate as the interceptor: an Element it enrols gets the identical destroy close.
    // Idempotent across retries.
    coop::npc_world_enum::InstallExSpawnCatch(fn);
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        UE_LOGI("npc-suppress: allowlist[%zu] '%ls' = %p",
                i, P::name::kNpcAllowlist[i], g_npcAllowlist[i]);
    }
}

bool GetDevSpawnRefs(DevSpawnRefs& out) {
    // Valid only once Install resolved the GameplayStatics UFunctions and CDO; the deferred-spawn
    // function is the one the interceptor hooks.
    if (!g_installed.load(std::memory_order_acquire) || !g_npcSpawnFn ||
        !g_installCacheFinishSpawnFn || !g_installCacheGsCdo) {
        return false;
    }
    out.beginDeferredFn = g_npcSpawnFn;
    out.finishSpawnFn   = g_installCacheFinishSpawnFn;
    out.gsCdo           = g_installCacheGsCdo;
    return true;
}

bool IsHostNpcSyncDisabled() {
    // True if Install permanently disabled the host NPC lifecycle this process (the observer table
    // full, so no guaranteed POST or destroy observer). The world enumeration gates on this and
    // IsInstalled before allocating any Npc Element, the same gate the interceptor uses, so it
    // never leaks an Element with no destroy observer to close it.
    return g_npcSyncDisabledThisProcess.load(std::memory_order_acquire);
}

}  // namespace coop::npc_sync
