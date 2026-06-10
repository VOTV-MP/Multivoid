// coop/ambient_spawner_suppress.cpp -- see coop/ambient_spawner_suppress.h.
//
// Bytecode facts grounding this module (research/bp_reflection, 2026-06-10):
// - mushroomMaster_C: ONE looping K2_SetTimerDelegate('spawn', Time=15s CDO)
//   armed in ReceiveBeginPlay; spawn() mints mushroomSpawner_C children with
//   SetLifeSpan(1800) -- cancelled children are reaped by the ENGINE lifespan
//   (native Destroy), independent of the cancelled BP event.
// - mushroomSpawner_C: ONE looping K2_SetTimerDelegate('spawn', timer=2s CDO);
//   spawn() materializes the prop_food_C cap when not recently rendered and
//   self-destroys; with spawn cancelled, the lifespan-1800 fallback reaps it.
// - pineconeSpawner_C: ReceiveTick only (SetActorTickInterval(random) INSIDE
//   the body + 5 spawn branches, each SetLifeSpan(600)); cancelling freezes
//   the tick interval at its last roll -- it re-rolls on the first
//   un-cancelled run after disconnect. Output classes (prop_stick_C /
//   prop_food_pinecone_C / prop_crystal_C...) self-expire via lifespan.
// All three dispatch through ProcessEvent (engine timer delegates / actor
// tick), the precedent-proven interceptable shapes (changeWindOrigin v50,
// prop_openContainer ReceiveTick) -- NOT the v44 EX_LocalVirtualFunction trap.

#include "coop/ambient_spawner_suppress.h"

#include "coop/net/session.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::ambient_spawner_suppress {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};

// Suppress only while an ACTIVE client session exists. running_ flips true in
// Session::Start -- covering the HANDSHAKE + the connect-snapshot drain --
// and false in Stop, which every disconnect path reaches (FleeToMainMenu ->
// Stop). role() reads cfg_.role, which Stop never resets -- only trustworthy
// behind running(); a bare role() gate is the post-session SP-bleed defect
// class this module deliberately avoids (a finished client session must not
// keep suppressing solo play in the same process).
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// Callbacks are atomics + counter + throttled log ONLY (no engine calls, no
// Post) -- safe for the parallel-anim-worker dispatch contract.
#define MAKE_AMBIENT_CANCEL(fn_name, log_tag)                                   \
bool fn_name(void* self, void* /*params*/) {                                    \
    if (!IsActiveClientSession()) return false;                                  \
    static std::atomic<uint64_t> sCount{0};                                      \
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;       \
    if (n <= 3 || (n % 300) == 0) {                                              \
        UE_LOGI("ambient_suppress[%s PRE]: client-cancel %p (call #%llu)",       \
                log_tag, self, static_cast<unsigned long long>(n));              \
    }                                                                            \
    return true;                                                                 \
}

MAKE_AMBIENT_CANCEL(OnMushroomMasterSpawnPre,  "mushroomMaster.spawn")
MAKE_AMBIENT_CANCEL(OnMushroomSpawnerSpawnPre, "mushroomSpawner.spawn")
MAKE_AMBIENT_CANCEL(OnPineconeTickPre,         "pineconeSpawner.ReceiveTick")

#undef MAKE_AMBIENT_CANCEL

struct Target {
    const wchar_t* cls;
    const wchar_t* fn;
    GT::UFunctionInterceptor cb;
    bool registered;
};
// Exact-case names from the LIVE CXX header dump (CXXHeaderDump/*.hpp:
// `void Spawn();` on both mushroom classes) -- the bp_reflection asset dump
// lowercases them, and reflection::FindFunction compares names
// case-SENSITIVELY (the 2026-06-10 smoke caught 'spawn' never resolving).
Target g_targets[] = {
    {L"mushroomMaster_C",  L"Spawn",       &OnMushroomMasterSpawnPre,  false},
    {L"mushroomSpawner_C", L"Spawn",       &OnMushroomSpawnerSpawnPre, false},
    {L"pineconeSpawner_C", L"ReceiveTick", &OnPineconeTickPre,         false},
};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return;
    // FindClass walks GUObjectArray -- throttle resolve attempts to ~1 Hz of
    // the 125 Hz pump. Shape rule (from the verified garbage_sync defect
    // where an outer latch starved its own retry): the all-3 latch is the
    // ONLY early-out and sets only at 3/3; per-target `registered` flags +
    // RegisterInterceptor's (target,cb) idempotence make partial retries safe.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;
    int done = 0;
    for (auto& t : g_targets) {
        if (t.registered) { ++done; continue; }
        void* cls = R::FindClass(t.cls);
        if (!cls) continue;  // BP class not loaded yet; retry next ensure
        void* fn = R::FindFunction(cls, t.fn);
        if (!fn) {
            UE_LOGW("ambient_suppress: '%ls' not found on %ls -- skipping", t.fn, t.cls);
            continue;
        }
        if (!GT::RegisterInterceptor(fn, t.cb)) {
            UE_LOGE("ambient_suppress: RegisterInterceptor failed for %ls::%ls (table full?)",
                    t.cls, t.fn);
            continue;
        }
        t.registered = true;
        ++done;
        UE_LOGI("ambient_suppress: PRE-interceptor installed -- %ls::%ls", t.cls, t.fn);
    }
    if (done == static_cast<int>(std::size(g_targets))) {
        g_installed.store(true, std::memory_order_release);
        UE_LOGI("ambient_spawner_suppress: 3/3 ambient spawner suppressors registered "
                "(mushroomMaster.spawn, mushroomSpawner.spawn, pineconeSpawner.ReceiveTick); "
                "cancel active only while a client session is running");
    }
}

}  // namespace coop::ambient_spawner_suppress
