// coop/world/spawn_authority.cpp -- see header + docs/COOP_RNG_AUTHORITY.md
// "T1 STRUCTURAL DESIGN". Absorbs coop/session/ambient_spawner_suppress
// (RULE-2 dissolve 2026-07-10) -- its four PRE-cancels are the t3 rows here,
// callbacks byte-identical to the proven originals.
//
// t3 bytecode facts (research/bp_reflection, 2026-06-10):
// - mushroomMaster_C: ONE looping K2_SetTimerDelegate('spawn', Time=15s CDO)
//   armed in ReceiveBeginPlay; spawn() mints mushroomSpawner_C children with
//   SetLifeSpan(1800) -- cancelled children are reaped by the ENGINE lifespan
//   (native Destroy), independent of the cancelled BP event.
// - mushroomSpawner_C: ONE looping K2_SetTimerDelegate('spawn', timer=2s CDO);
//   spawn() materializes the prop_food_C cap when not recently rendered and
//   self-destroys; with spawn cancelled, the lifespan-1800 fallback reaps it.
// - pineconeSpawner_C: NOT a row (2026-07-10 reversal). Measured anchor =
//   GetPlayerCameraManager->GetActorLocation (research/bp_reflection dump):
//   forest drops land around the LOCAL player -> OWNER-EFFECT tier
//   ([[feedback-owner-effect-rule]]). Each peer rolls its own; the ambient
//   owner-mirror in coop/props/host_spawn_watcher makes them cross-visible.
// - ticker_yellowWispSpawner_C: ReceiveTick; anchor is a navmesh RANDOM-WALK
//   point (p = ProjectPointToNavigation(p + rotated random offset), measured
//   2026-07-10 -- NOT player-anchored); killerwisp_C is host-mirrored
//   (kNpcAllowlist), so the client must not run its own spawner.
// - ticker_wispSpawner_C: ReceiveTick (1200-3600 s interval); spawns the sky
//   wisps (wisp_C + 8 color variants) at ABSOLUTE map coords (+-60-70k X/Y,
//   Z=80k -- measured 2026-07-10, REVERSING the earlier OWNER-EFFECT call).
//   World-anchored -> host rolls, clients mirror via the EX-catch source row
//   (npc_world_enum) + the variant allowlist rows.
// - cockroachMaster_C (the roach infestation sim): summonRoach (called by
//   ticker_roachSummoner via the gamemode ref) + the three timer entries
//   (addRoachTimer / spawnNestTimer / CustomEvent re-armed by looping
//   K2_SetTimerDelegate -- timers fire independently of actor tick, so the
//   t1 park alone cannot silence them). Roaches are components on the ONE
//   world-anchored master (nests near food, growth by eating) = shared world
//   state; coop/creatures/roach_sync mirrors the host population.
//
// t1 park facts (gate read 2026-07-10, research/bp_reflection):
// - ticker_insomniacSpawner_C / ticker_fossilhoundSpawner_C: rolls live INSIDE
//   ReceiveTick (RandomBoolWithWeight per interval; SetActorTickInterval
//   self-re-arm; NO Delay chains, NO destroy/reap duties -> pure spawn ->
//   parking the tick stops roll AND product). Products insomniac_C /
//   fossilhound_C are host-mirrored (kNpcAllowlist), so the park is a pure
//   improvement: no content change, the divergent client roll stops.
// - cockroachMaster_C / ticker_roachSummoner_C (roach lane, 2026-07-10): the
//   master's ReceiveTick drives calc() = per-roach movement + the food-eat
//   mutation (drains prop_food_C foodData, destroys depleted food) + crush
//   traces -- a client running it DIVERGES the shared food/roach state.
//   Parked; roach_sync drives the client population from host RoachState.
//   Interaction EVENTS (steppedOn/actionOptionIndex/impactSquishCPP) are NOT
//   tick-driven and stay live on the parked master -- the local eat/stomp
//   runs natively and roach_sync forwards the consumption intent.

#include "coop/world/spawn_authority.h"

#include "coop/net/session.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>    // wcscmp (resolve-pass FindClass dedupe)
#include <iterator>
#include <string>
#include <vector>

namespace coop::spawn_authority {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// Suppress/park only while an ACTIVE client session exists. running_ flips
// true in Session::Start and false in Stop, which every disconnect path
// reaches; a bare role() gate is the post-session SP-bleed defect class
// ([[lesson-suppression-needs-paired-restore-or-running-gate]]).
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// ---- t3 CANCEL rows (migrated verbatim from ambient_spawner_suppress) ----
// Callbacks are atomics + counter + throttled log ONLY (no engine calls, no
// Post) -- safe for the parallel-anim-worker dispatch contract.
#define MAKE_SPAWN_CANCEL(fn_name, log_tag)                                      \
bool fn_name(void* self, void* /*params*/) {                                     \
    if (!IsActiveClientSession()) return false;                                   \
    static std::atomic<uint64_t> sCount{0};                                       \
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;        \
    if (n <= 3 || (n % 300) == 0) {                                               \
        UE_LOGI("spawn_authority[%s t3-cancel]: client-cancel %p (call #%llu)",  \
                log_tag, self, static_cast<unsigned long long>(n));               \
    }                                                                             \
    return true;                                                                  \
}

MAKE_SPAWN_CANCEL(OnMushroomMasterSpawnPre,  "mushroomMaster.Spawn")
MAKE_SPAWN_CANCEL(OnMushroomSpawnerSpawnPre, "mushroomSpawner.Spawn")
MAKE_SPAWN_CANCEL(OnYellowWispTickPre,       "yellowWispSpawner.ReceiveTick")
MAKE_SPAWN_CANCEL(OnSkyWispTickPre,          "wispSpawner.ReceiveTick")
MAKE_SPAWN_CANCEL(OnRoachSummonPre,          "cockroachMaster.summonRoach")
MAKE_SPAWN_CANCEL(OnRoachAddTimerPre,        "cockroachMaster.addRoachTimer")
MAKE_SPAWN_CANCEL(OnRoachNestTimerPre,       "cockroachMaster.spawnNestTimer")
MAKE_SPAWN_CANCEL(OnRoachCustomEventPre,     "cockroachMaster.CustomEvent")

#undef MAKE_SPAWN_CANCEL

struct CancelTarget {
    const wchar_t* cls;
    const wchar_t* fn;   // exact-case from the LIVE CXX header dump (FindFunction is case-SENSITIVE)
    GT::UFunctionInterceptor cb;
    bool registered;
};
CancelTarget g_cancelTargets[] = {
    {L"mushroomMaster_C",            L"Spawn",           &OnMushroomMasterSpawnPre,  false},
    {L"mushroomSpawner_C",           L"Spawn",           &OnMushroomSpawnerSpawnPre, false},
    // Late-game class: resolves only once the yellow-wisp spawner loads; the
    // all-done latch stays open until then (idempotent per-target retry).
    {L"ticker_yellowWispSpawner_C",  L"ReceiveTick",     &OnYellowWispTickPre,       false},
    // Sky wisps: world-anchored (absolute map coords) -- host rolls, EX-catch
    // source row + variant allowlist mirror the products (2026-07-10).
    {L"ticker_wispSpawner_C",        L"ReceiveTick",     &OnSkyWispTickPre,          false},
    // Roach sim entries: the ticker's cross-object call + the three looping
    // timer delegates (timers bypass the t1 actor-tick park). The client
    // population is driven by coop/creatures/roach_sync instead.
    {L"cockroachMaster_C",           L"summonRoach",     &OnRoachSummonPre,          false},
    {L"cockroachMaster_C",           L"addRoachTimer",   &OnRoachAddTimerPre,        false},
    {L"cockroachMaster_C",           L"spawnNestTimer",  &OnRoachNestTimerPre,       false},
    {L"cockroachMaster_C",           L"CustomEvent",     &OnRoachCustomEventPre,     false},
};

// ---- t1 PARK rows ----
constexpr const wchar_t* kParkClassNames[] = {
    L"ticker_insomniacSpawner_C",
    L"ticker_fossilhoundSpawner_C",
    // Roach lane (2026-07-10): the master's tick runs calc() = movement +
    // food-eat mutation + crush traces; the ticker's tick calls summonRoach.
    // Both parked on an active client session (roach_sync drives the mirror).
    L"cockroachMaster_C",
    L"ticker_roachSummoner_C",
};
constexpr size_t kParkClassCount = std::size(kParkClassNames);

// Resolved UClass* per park row. Written on the game thread (Install), read
// by NoteClientSpawnPassThrough from parallel-anim worker threads -> atomics.
std::atomic<void*> g_parkClasses[kParkClassCount] = {};

// Parked instances (game-thread only: built/re-asserted/restored in Tick).
struct ParkedInstance {
    void* obj;
    int32_t internalIdx;  // recycle-proof liveness via IsLiveByIndex
};
std::vector<ParkedInstance> g_parked;

bool g_sessionLatch = false;   // an active client session is being suppressed
bool g_initialPassDone = false;
long long g_lastReparkMs = 0;
long long g_lastReconcileMs = 0;

constexpr long long kReparkPeriodMs = 1000;      // cached-set re-park (BP re-enables)
constexpr long long kReconcilePeriodMs = 15000;  // late-instance walk

long long NowMs() { return static_cast<long long>(::GetTickCount64()); }

bool IsParkClassPtr(void* cls) {
    if (!cls) return false;
    for (size_t i = 0; i < kParkClassCount; ++i)
        if (g_parkClasses[i].load(std::memory_order_acquire) == cls) return true;
    return false;
}

bool AlreadyParked(void* obj) {
    for (const auto& p : g_parked)
        if (p.obj == obj) return true;
    return false;
}

// One pass over GUObjectArray: park every live instance of a park class not
// yet in the cache. Cheap class-POINTER compare per object (no NameOf --
// [[lesson-full-array-walk-cheap-filter-before-nameof]]); CDOs are skipped by
// ClassOf(cdo)!=cls never holding for CDOs?  No: a CDO's class IS the class,
// so skip via the object's own name prefix ONLY for matched objects (matched
// set is tiny: instance count of 2 spawner classes).
int ParkWalk(const char* why) {
    int newlyParked = 0;
    const int n = R::NumObjects();
    for (int i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !R::IsLive(obj)) continue;
        void* cls = R::ClassOf(obj);
        if (!IsParkClassPtr(cls)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // the CDO, not an instance
        if (AlreadyParked(obj)) continue;
        if (E::SetActorTickEnabled(obj, false)) {
            g_parked.push_back({obj, R::InternalIndexOf(obj)});
            ++newlyParked;
            UE_LOGI("spawn_authority[t1-park]: parked '%ls' %p (%s)", nm.c_str(), obj, why);
        } else {
            UE_LOGW("spawn_authority[t1-park]: SetActorTickEnabled(false) FAILED on '%ls' %p (%s)",
                    nm.c_str(), obj, why);
        }
    }
    return newlyParked;
}

// Re-enable tick on every still-live parked instance + clear the cache. The
// loan's structural repayment is the mandatory menu teardown (world reload
// re-runs BeginPlay); this restore is belt for the teardown window itself.
void RestoreAll(const char* why) {
    int restored = 0;
    for (const auto& p : g_parked) {
        if (!R::IsLiveByIndex(p.obj, p.internalIdx)) continue;  // recycled/dead slot
        if (E::SetActorTickEnabled(p.obj, true)) ++restored;
    }
    if (!g_parked.empty() || restored > 0) {
        UE_LOGI("spawn_authority[t1-park]: restored %d/%zu parked spawner tick(s) (%s)",
                restored, g_parked.size(), why);
    }
    g_parked.clear();
    g_initialPassDone = false;
}

bool AllParkClassesResolved() {
    for (size_t i = 0; i < kParkClassCount; ++i)
        if (!g_parkClasses[i].load(std::memory_order_acquire)) return false;
    return true;
}

std::atomic<bool> g_cancelInstalled{false};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // FindClass walks GUObjectArray -- throttle resolve attempts to ~1 Hz of
    // the 125 Hz pump. Shape rule (garbage_sync defect): the all-done latch is
    // the ONLY early-out and sets only at full resolution; per-target flags
    // make partial retries safe.
    static uint32_t sResolveN = 0;
    const bool cancelsDone = g_cancelInstalled.load(std::memory_order_acquire);
    if (cancelsDone && AllParkClassesResolved()) return;
    if ((sResolveN++ % 125) != 0) return;

    if (!cancelsDone) {
        int done = 0;
        // Per-pass FindClass dedupe (audit 2026-07-10): the roach rows share one
        // class 4x; each FindClass is a full GUObjectArray walk, so adjacent
        // same-class rows reuse the previous resolve while the latch is open.
        const wchar_t* lastClsName = nullptr;
        void* lastCls = nullptr;
        for (auto& t : g_cancelTargets) {
            if (t.registered) { ++done; continue; }
            void* cls = (lastClsName && wcscmp(lastClsName, t.cls) == 0)
                            ? lastCls : R::FindClass(t.cls);
            lastClsName = t.cls;
            lastCls = cls;
            if (!cls) continue;  // BP class not loaded yet; retry next ensure
            void* fn = R::FindFunction(cls, t.fn);
            if (!fn) {
                UE_LOGW("spawn_authority: '%ls' not found on %ls -- skipping", t.fn, t.cls);
                continue;
            }
            if (!GT::RegisterInterceptor(fn, t.cb)) {
                UE_LOGE("spawn_authority: RegisterInterceptor failed for %ls::%ls (table full?)",
                        t.cls, t.fn);
                continue;
            }
            t.registered = true;
            ++done;
            UE_LOGI("spawn_authority: t3 PRE-cancel installed -- %ls::%ls", t.cls, t.fn);
        }
        if (done == static_cast<int>(std::size(g_cancelTargets))) {
            g_cancelInstalled.store(true, std::memory_order_release);
            UE_LOGI("spawn_authority: %zu/%zu t3 cancels registered (mushroom x2, "
                    "yellowWisp+skyWisp ReceiveTick, cockroachMaster summonRoach+3 timer "
                    "entries); active only on a running client session",
                    std::size(g_cancelTargets), std::size(g_cancelTargets));
        }
    }

    for (size_t i = 0; i < kParkClassCount; ++i) {
        if (g_parkClasses[i].load(std::memory_order_acquire)) continue;
        if (void* cls = R::FindClass(kParkClassNames[i])) {
            g_parkClasses[i].store(cls, std::memory_order_release);
            UE_LOGI("spawn_authority: t1 park class resolved -- %ls", kParkClassNames[i]);
        }
    }
}

void Tick() {
    if (!IsActiveClientSession()) {
        // Session over (any end path) or not a client: repay the loan even if
        // the DisconnectAll fanout was missed, then stay cheap.
        if (g_sessionLatch) {
            RestoreAll("session ended (tick gate)");
            g_sessionLatch = false;
        }
        return;
    }
    if (!AllParkClassesResolved()) return;  // Install still retrying (pre-world)
    g_sessionLatch = true;

    const long long now = NowMs();
    if (!g_initialPassDone) {
        // The initial pass runs on the first gameplay tick of the client
        // session (the join window -- before the world starts progressing for
        // the player), so the spawners never get a rolling window on join.
        const int parked = ParkWalk("initial join-window pass");
        g_initialPassDone = true;
        g_lastReparkMs = now;
        g_lastReconcileMs = now;
        UE_LOGI("spawn_authority[t1-park]: initial pass parked %d spawner instance(s) "
                "(%zu cached)", parked, g_parked.size());
        return;
    }
    if (now - g_lastReparkMs >= kReparkPeriodMs) {
        g_lastReparkMs = now;
        // Unconditional re-park of the cached set (idempotent setter; N is the
        // instance count of 2 classes -> a handful of dispatches per second).
        // Drop dead/recycled entries while at it.
        size_t w = 0;
        for (size_t r = 0; r < g_parked.size(); ++r) {
            if (!R::IsLiveByIndex(g_parked[r].obj, g_parked[r].internalIdx)) continue;
            E::SetActorTickEnabled(g_parked[r].obj, false);
            g_parked[w++] = g_parked[r];
        }
        g_parked.resize(w);
    }
    // Late-instance reconcile. Measured (2026-07-10 smoke): the join-window
    // "initial" pass runs before the save-loaded world has spawner instances
    // (parked 0), so the FIRST instances are caught here -- walk at 1 Hz until
    // something is parked (bounds the client's pre-park roll window to ~1 s),
    // then relax to the 15 s steady-state cadence.
    const long long reconcilePeriod = g_parked.empty() ? kReparkPeriodMs : kReconcilePeriodMs;
    if (now - g_lastReconcileMs >= reconcilePeriod) {
        g_lastReconcileMs = now;
        ParkWalk(g_parked.empty() ? "1s first-instance hunt" : "15s reconcile");
    }
}

void OnDisconnect() {
    RestoreAll("DisconnectAll fanout");
    g_sessionLatch = false;
}

bool NoteClientSpawnPassThrough(void* actorClass) {
    if (!IsParkClassPtr(actorClass)) return false;
    // A park-class spawner is being SPAWNED on a connected client -- the
    // structural tripwire. Log-only (the reconcile walk parks the new instance
    // within ~15 s); throttled, thread-safe (fires on parallel-anim workers).
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 50) == 0) {
        UE_LOGW("spawn_authority[TRIPWIRE]: park-class spawner spawning on a connected "
                "client (class=%p, occurrence #%llu) -- reconcile walk will park it",
                actorClass, static_cast<unsigned long long>(n));
    }
    return true;
}

}  // namespace coop::spawn_authority
