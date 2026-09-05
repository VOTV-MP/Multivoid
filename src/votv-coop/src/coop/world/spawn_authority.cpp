// coop/world/spawn_authority.cpp -- see the header. The cancel rows rest on these bytecode
// facts: the mushroom master arms one looping spawn timer at begin-play, and its spawn mints
// spawner children with a lifespan, so cancelled children are reaped by the engine
// independently of the cancelled event; the mushroom spawner's own looping timer
// materialises the food cap and self-destroys, and with spawn cancelled the lifespan reaps
// it; the yellow-wisp ticker spawns at a navmesh random-walk point, not around the player,
// and its product is host-mirrored, so the client must not run its own spawner; the sky-wisp
// ticker spawns the sky wisps at absolute map coordinates, so the host rolls and clients
// mirror through the source-gated catch and the variant allowlist; the roach master's
// summon and its three looping timer entries fire independently of actor tick, so the tick
// park alone cannot silence them. The park rows: the insomniac and fossilhound tickers roll
// inside their tick with no delay chains or reap duties, so parking the tick stops the roll
// and the product, and the products are host-mirrored; the roach master's tick drives roach
// movement, the food-eat mutation and crush traces, which a client running it would diverge,
// so it and its summoner are parked while the roach sync drives the client population.

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

// Suppress and park only while an active client session exists: the running flag flips true
// in the session start and false in the stop, which every disconnect path reaches; a bare
// role gate would bleed the suppression into single player after the session.
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// The cancel rows. Callbacks are atomics, a counter and a throttled log only (no engine
// calls, no post), safe for the parallel-anim worker dispatch contract.
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
    // A late-game class: it resolves only once the yellow-wisp spawner loads, and the all-done
    // latch stays open until then (an idempotent per-target retry).
    {L"ticker_yellowWispSpawner_C",  L"ReceiveTick",     &OnYellowWispTickPre,       false},
    // Sky wisps: world-anchored, so the host rolls; the source-gated catch and the variant
    // allowlist mirror the products.
    {L"ticker_wispSpawner_C",        L"ReceiveTick",     &OnSkyWispTickPre,          false},
    // Roach sim entries: the ticker's cross-object call and the three looping timer delegates
    // (timers bypass the actor-tick park). The roach sync drives the client population instead.
    {L"cockroachMaster_C",           L"summonRoach",     &OnRoachSummonPre,          false},
    {L"cockroachMaster_C",           L"addRoachTimer",   &OnRoachAddTimerPre,        false},
    {L"cockroachMaster_C",           L"spawnNestTimer",  &OnRoachNestTimerPre,       false},
    {L"cockroachMaster_C",           L"CustomEvent",     &OnRoachCustomEventPre,     false},
};

// The park rows.
constexpr const wchar_t* kParkClassNames[] = {
    L"ticker_insomniacSpawner_C",
    L"ticker_fossilhoundSpawner_C",
    // The roach master's tick runs movement, the food-eat mutation and crush traces, and the
    // ticker's tick calls the summon; both are parked on an active client session (the roach
    // sync drives the mirror).
    L"cockroachMaster_C",
    L"ticker_roachSummoner_C",
};
constexpr size_t kParkClassCount = std::size(kParkClassNames);

// The resolved class per park row. Written on the game thread (Install), read by the spawn
// pass-through from parallel-anim worker threads, so atomics.
std::atomic<void*> g_parkClasses[kParkClassCount] = {};

// The parked instances, game thread only: built, re-asserted and restored in Tick.
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

// One pass over the object array: park every live instance of a park class not yet in the
// cache. A cheap class-pointer compare per object, no name rendering; a CDO's class is the
// class itself, so the CDO is skipped by its name prefix, and only for matched objects (a
// tiny set, the instances of the park classes).
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

// Re-enable tick on every still-live parked instance and clear the cache. The loan's
// structural repayment is the mandatory menu teardown (a world reload re-runs begin-play);
// this restore covers the teardown window itself.
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
    // FindClass walks the object array, so resolve attempts are throttled to about 1 Hz of the
    // pump. The all-done latch is the only early-out and sets only at full resolution; the
    // per-target flags make partial retries safe.
    static uint32_t sResolveN = 0;
    const bool cancelsDone = g_cancelInstalled.load(std::memory_order_acquire);
    if (cancelsDone && AllParkClassesResolved()) return;
    if ((sResolveN++ % 125) != 0) return;

    if (!cancelsDone) {
        int done = 0;
        // The per-pass FindClass dedupe: the roach rows share one class four times, and each
        // FindClass is a full object-array walk, so adjacent same-class rows reuse the previous
        // resolve while the latch is open.
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
        // The session is over (any end path) or this is not a client: repay the loan even if the
        // disconnect fan-out was missed, then stay cheap.
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
        // The initial pass runs on the first gameplay tick of the client session (the join window,
        // before the world starts progressing for the player), so the spawners never get a rolling
        // window on join.
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
        // An unconditional re-park of the cached set (an idempotent setter over a handful of
        // instances, a few dispatches per second), dropping dead or recycled entries as it goes.
        size_t w = 0;
        for (size_t r = 0; r < g_parked.size(); ++r) {
            if (!R::IsLiveByIndex(g_parked[r].obj, g_parked[r].internalIdx)) continue;
            E::SetActorTickEnabled(g_parked[r].obj, false);
            g_parked[w++] = g_parked[r];
        }
        g_parked.resize(w);
    }
    // The late-instance reconcile: the join-window initial pass runs before the save-loaded
    // world has spawner instances, so the first instances are caught here. Walk at 1 Hz until
    // something is parked (bounding the client's pre-park roll window to about a second), then
    // relax to the 15 s steady cadence.
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
    // A park-class spawner is being spawned on a connected client, the structural tripwire.
    // Log only (the reconcile walk parks the new instance within 15 s); throttled and
    // thread-safe, since it fires on parallel-anim workers.
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
