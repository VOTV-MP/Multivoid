// ue_wrap/game_thread.cpp -- the DISPATCHER SERVICES the ProcessEvent detour
// drives: the observer / interceptor / name-diagnostic registries (+ their
// Bloom presence probes) and the posted-task pump (+ the spawn-refusal drain
// gate). The interposition mechanism itself -- hook install, detour body,
// bypass, SEH firewalls, depth probe, perf instrumentation -- lives in
// pe_detour.cpp; the private seam is game_thread_detail.h (2026-07-04
// modularity extraction; the hot-path fast rejects are inline in that header,
// so only matched dispatches / a non-empty queue cross the TU boundary).

#include "ue_wrap/core/game_thread.h"

#include "game_thread_detail.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/spawn_gate.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace ue_wrap::game_thread {

// ---- shared hot-path state (read inline by pe_detour.cpp via the detail header) --
namespace detail {
std::atomic<uint64_t> g_postBloom[kBloomWords]{};
std::atomic<uint64_t> g_preBloom[kBloomWords]{};
std::atomic<uint64_t> g_intcBloom[kBloomWords]{};
std::atomic<int> g_interceptorActive{0};
std::atomic<int> g_postObserverActive{0};
std::atomic<int> g_preObserverActive{0};
std::atomic<int> g_nameDiagAnySet{0};
// 2026-05-26 deep-RE call trace flag. When true, the detour logs every
// UFunction dispatch. One-shot probe for BP call chains.
std::atomic<bool> g_callTrace{false};
// Lock-free emptiness probe (perf, 2026-06-04). The detour runs on EVERY game-
// thread ProcessEvent dispatch (~85k/sec, measured) and used to take the queue
// mutex JUST to test empty() -- a barriered LOCK-prefixed RMW on the hottest
// path in the program, for a queue that is empty ~99.9% of the time (Post runs
// ~60-125x/sec). g_queueDepth mirrors g_queue.size(), maintained under
// g_queueMutex by Post()/Pump(); the detour reads it WITHOUT the lock and only
// drains when it is non-zero. A just-Posted task whose increment isn't yet
// visible to the detour's relaxed-acquire read is drained on the next dispatch
// microseconds later (tasks are not sub-millisecond-latency-critical). Writers
// stay under the mutex so depth and the deque never diverge.
std::atomic<int> g_queueDepth{0};
// Re-entrancy guard: set while we are inside the pump, so a task that calls a
// UFunction (re-entering ProcessEvent -> the detour) skips draining and just
// forwards. Thread-local because only the game thread ever sets it, but a guard
// keeps it correct even if ProcessEvent were ever called cross-thread.
thread_local bool t_inPump = false;
// The game thread id, CAS-recorded by the detour on its first dispatch.
std::atomic<unsigned long> g_gameThreadId{0};
}  // namespace detail

namespace {

namespace D = detail;

std::atomic<unsigned long long> g_tasksRun{0};

inline void BloomAdd(std::atomic<uint64_t>* bloom, void* fn) {
    if (!fn) return;
    const unsigned b = D::BloomBit(fn);
    bloom[b >> 6].fetch_or(1ull << (b & 63), std::memory_order_release);
}
inline void BloomClear(std::atomic<uint64_t>* bloom) {
    for (int i = 0; i < D::kBloomWords; ++i) bloom[i].store(0, std::memory_order_release);
}

// Multi-slot UFunction-pre-dispatch interceptor table. Same atomic-slot shape
// as the observer tables; null targetFn = free slot. The detour walks the
// table on each dispatch; first cb returning true cancels the original.
struct InterceptorSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<UFunctionInterceptor> cb{nullptr};
};
InterceptorSlot g_interceptors[kMaxInterceptors];

// D4-2 (2026-05-29 foundation audit fix): per-table count of populated slots
// (detail::g_interceptorActive etc.) so the detour walk can early-terminate
// after finding all live entries instead of always walking all
// kMaxInterceptors / kMaxObservers slots per ProcessEvent dispatch. UE4
// dispatches ~100k PE/sec; with the 128-slot post-observer table that was 128
// atomic acquire-loads per dispatch on x86 even when only ~30 observers were
// registered. The count is a release-store sequenced AFTER the targetFn slot
// store so the detour's acquire-load of the count fences any race-with-register
// (if the detour sees the new count, it also sees the new slot).

bool SetInterceptorSlot(void* targetFn, UFunctionInterceptor cb) {
    if (!targetFn || !cb) return false;
    // (targetFn, cb)-keyed, same multi-registrant invariant as SetObserverSlot
    // (2026-05-30): FireInterceptors consults EVERY slot matching `function` and
    // stops at the first cb returning true, so two distinct interceptors on one
    // UFunction now coexist instead of the later silently overwriting the
    // earlier. No active collision exists today (npc_sync is the sole
    // BeginDeferred interceptor), but this kills the same latent clobber class.
    // First pass: idempotent re-register of the exact (target, cb) pair.
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            g_interceptors[i].cb.load(std::memory_order_relaxed) == cb) {
            return true;
        }
    }
    // Second pass: take the first empty slot. Bump active count AFTER the
    // slot store so the detour's count-bounded walk sees the new entry.
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == nullptr) {
            g_interceptors[i].cb.store(cb, std::memory_order_relaxed);
            g_interceptors[i].targetFn.store(targetFn, std::memory_order_release);
            D::g_interceptorActive.fetch_add(1, std::memory_order_release);
            BloomAdd(D::g_intcBloom, targetFn);  // O(1) presence probe
            return true;
        }
    }
    return false;  // table full
}

void ClearInterceptorSlot(void* targetFn, UFunctionInterceptor cb) {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            g_interceptors[i].cb.load(std::memory_order_relaxed) == cb) {
            g_interceptors[i].targetFn.store(nullptr, std::memory_order_release);
            g_interceptors[i].cb.store(nullptr, std::memory_order_relaxed);
            D::g_interceptorActive.fetch_sub(1, std::memory_order_release);
        }
    }
}

// Multi-target POST and PRE observers (fixed-size tables, no allocation).
// Each entry is {UFunction*, ProcessEventObserverFn}. A null UFunction*
// entry is a free slot. Writes via SetObserverSlot store the cb FIRST then
// the function pointer with release ordering; reads in the detour load the
// function pointer with acquire ordering then the cb with relaxed (same
// pattern as the interceptor slots). Fixed size kMaxObservers (see
// game_thread.h) keeps the detour loop bounded.
struct ObserverSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<ProcessEventObserverFn> cb{nullptr};
};
ObserverSlot g_postObservers[kMaxObservers];
ObserverSlot g_preObservers[kMaxObservers];

// Add (targetFn, cb) to `table`. Returns false if the table is full.
//
// MULTIPLE observers per UFunction (2026-05-30 root-cause fix): the dedup key
// is the (targetFn, cb) PAIR, not targetFn alone. Several subsystems
// legitimately observe the SAME UFunction -- e.g. BeginDeferredActorSpawnFromClass
// is a POST-observer target for BOTH npc_sync (binds the spawned NPC actor into
// its Element) AND weather_lightning (detects a lightningStrike spawn); and
// Actor.K2_DestroyActor is a PRE-observer target for BOTH npc_sync and
// prop_lifecycle. The old key-on-targetFn-only logic made the SECOND registrant
// silently OVERWRITE the first's cb in the shared slot, so one of the two
// observers never fired (host NPC actor-bind was clobbered by lightning ->
// POST-bound=0, destroy-sync dead). FireObservers fires EVERY slot whose target
// matches `function`, so giving each (target, cb) its own slot runs all of them.
// `activeCounter` is bumped only when an empty slot is populated; an exact
// (target, cb) re-register is an idempotent no-op (does not change the count).
bool SetObserverSlot(ObserverSlot table[], std::atomic<int>& activeCounter,
                     std::atomic<uint64_t>* bloom, void* targetFn, ProcessEventObserverFn cb) {
    if (!targetFn || !cb) return false;
    // First pass: idempotent re-register -- a slot already holding THIS EXACT
    // (target, cb) pair is a no-op. (Registration is serialized on the game
    // thread, so the relaxed loads race only with the detour reader, which the
    // release-store of targetFn below fences.)
    for (int i = 0; i < kMaxObservers; ++i) {
        if (table[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            table[i].cb.load(std::memory_order_relaxed) == cb) {
            return true;
        }
    }
    // Second pass: take the first empty slot.
    for (int i = 0; i < kMaxObservers; ++i) {
        if (table[i].targetFn.load(std::memory_order_relaxed) == nullptr) {
            table[i].cb.store(cb, std::memory_order_relaxed);
            table[i].targetFn.store(targetFn, std::memory_order_release);
            activeCounter.fetch_add(1, std::memory_order_release);
            BloomAdd(bloom, targetFn);  // O(1) presence probe
            return true;
        }
    }
    return false;  // table full
}

// Clear the slot in `table` matching the (targetFn, cb) PAIR. cb-specific so
// unregistering one subsystem's observer does NOT clear a CO-REGISTERED
// observer on the same UFunction (the multi-observer-per-target invariant
// above). Decrements activeCounter once per slot actually cleared.
void ClearObserverSlot(ObserverSlot table[], std::atomic<int>& activeCounter,
                       void* targetFn, ProcessEventObserverFn cb) {
    for (int i = 0; i < kMaxObservers; ++i) {
        if (table[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            table[i].cb.load(std::memory_order_relaxed) == cb) {
            table[i].targetFn.store(nullptr, std::memory_order_release);
            table[i].cb.store(nullptr, std::memory_order_relaxed);
            activeCounter.fetch_sub(1, std::memory_order_release);
        }
    }
}

// Name-prefix diagnostic table. Each slot is (prefix wchar buffer, cb).
// detail::g_nameDiagAnySet is the fast-path bypass: when no slots are
// populated, the detour skips the whole name-resolve+compare path entirely
// (zero cost in the common case). Single producer / multiple readers, atomic
// publish.
struct NameDiagSlot {
    wchar_t prefix[kMaxNameDiagnosticPrefixLen]{};
    std::atomic<ProcessEventNameDiagnosticFn> cb{nullptr};
    std::atomic<int> prefixLen{0};  // 0 = empty slot
};
NameDiagSlot g_nameDiagSlots[kMaxNameDiagnostics];

// The posted-task queue. Pulled out under the lock, then run unlocked so a task
// may Post() without deadlocking.
std::mutex g_queueMutex;
std::deque<Task> g_queue;

// Spawn-refusal deferral episode (2026-07-04 join-window BeginDeferred-null
// root fix). Tasks assume TOP-LEVEL game-thread context, but the detour fires
// on EVERY ProcessEvent -- including dispatches nested inside another actor's
// construction script, where UWorld::SpawnActor silently returns null (see
// spawn_gate.h). During a save-load's mass actor construction nearly every
// dispatch is such a nested one, so draining there made every spawn a task
// issued fail for the whole load tail (871 trash-proxy + 92 keyed-prop nulls
// on the 2026-07-04 joining client). DrainPostedTasksAtTopLevel DEFERS the
// drain while spawn_gate::WorldRefusesSpawns() holds; the queue keeps order
// and drains on the first dispatch outside the window (sub-ms in steady state,
// end-of-load during a mass construction -- exactly when spawns start
// succeeding). GT-only (the drain block is game-thread gated), so plain ints.
int g_gateDeferrals = 0;
std::chrono::steady_clock::time_point g_gateEpisodeStart{};
std::chrono::steady_clock::time_point g_gateNextHoldWarn{};

void NoteGateDeferral() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (g_gateDeferrals++ == 0) {
        g_gateEpisodeStart = now;
        g_gateNextHoldWarn = now + seconds(5);
        return;
    }
    // A construction/teardown window outlasting 5 s means the world state is
    // wedged (or a teardown is stalled) -- keep deferring (draining INTO the
    // window is the bug this gate closes) but say so, loudly and throttled.
    if (now >= g_gateNextHoldWarn) {
        g_gateNextHoldWarn = now + seconds(5);
        UE_LOGW("game_thread: pump deferred for %lld ms and counting (%d dispatches) -- "
                "world still refuses spawns (construction-script/teardown window held open)",
                static_cast<long long>(duration_cast<milliseconds>(now - g_gateEpisodeStart).count()),
                g_gateDeferrals);
    }
}

void NoteGateEpisodeEnd() {
    using namespace std::chrono;
    if (g_gateDeferrals == 0) return;
    UE_LOGI("game_thread: pump drain deferred %d dispatch(es) over %lld ms "
            "(world spawn-refusal window) -- draining now at top-level context",
            g_gateDeferrals,
            static_cast<long long>(duration_cast<milliseconds>(
                steady_clock::now() - g_gateEpisodeStart).count()));
    g_gateDeferrals = 0;
}

void Pump() {
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_queue.empty()) return;
            task = std::move(g_queue.front());
            g_queue.pop_front();
            D::g_queueDepth.store(static_cast<int>(g_queue.size()), std::memory_order_release);
        }
        // The mod is compiled /EHa (CMakeLists.txt) -- so a STRUCTURED
        // exception (AV, div-by-zero) raised inside task() is both caught by
        // the SEH wrapper below AND unwinds C++ destructors on the way out, so
        // any std::lock_guard / RAII the task held is released. This is
        // load-bearing: posted tasks run gameplay/reflection work that can AV
        // on a stale engine pointer (e.g. the connect-edge snapshot
        // enumeration reading a GC'd actor). Before /EHa that AV propagated
        // WITHOUT running destructors, leaking the element Registry mutex
        // locked + the t_inPump flag set -> permanent game-thread freeze
        // (diagnosed 2026-05-30, Tier 8 4-peer smoke).
        // detail::RunTaskSEH wraps task() in an SEH frame whose filter captures
        // the faulting IP + AV access address BEFORE the unwind, so an absorbed
        // fault names its own site instead of logging a generic message. The
        // pump LOOP CONTINUES either way, so one faulting task never stops the
        // others or wedges the tick.
        D::LastTaskFault() = {};
        if (D::RunTaskSEH(task) != 0) {
            constexpr unsigned long kCppExceptionCode = 0xE06D7363u;  // MSVC C++ throw
            const D::TaskFaultInfo& f = D::LastTaskFault();
            if (f.code == kCppExceptionCode) {
                UE_LOGE("game_thread: posted task threw a C++ exception at ip=%p [%s]; "
                        "skipped, pump continues",
                        f.faultingIP, D::FormatModuleRva(f.faultingIP));
            } else {
                UE_LOGE("game_thread: posted task FAULT code=0x%08lX ip=%p [%s] access=%p; "
                        "skipped, pump continues",
                        f.code, f.faultingIP, D::FormatModuleRva(f.faultingIP), f.accessAddr);
            }
        }
        g_tasksRun.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

// ---- detail cold halves (called by pe_detour.cpp past the inline fast rejects) --

namespace detail {

// Returns true if any interceptor for `function` returns true. Acquire load
// on the function pointer pairs with the release store in SetInterceptorSlot
// (no torn target+cb pair on weakly-ordered ISAs; on x86-64 TSO acquire is
// free, ordering matters for a future ARM port).
//
// D4-2 (2026-05-29): count-bounded walk. Loads g_interceptorActive ONCE at
// the top; loop terminates when `found == active` so an N-entry table
// pays N acquire-loads instead of kMaxInterceptors. (The empty-table and
// Bloom rejects already ran inline in the detour.)
bool FireInterceptorsMatched(void* self, void* function, void* params) {
    const int active = g_interceptorActive.load(std::memory_order_acquire);
    int found = 0;
    for (int i = 0; i < kMaxInterceptors && found < active; ++i) {
        void* tgt = g_interceptors[i].targetFn.load(std::memory_order_acquire);
        if (!tgt) continue;
        ++found;
        if (tgt != function) continue;
        UFunctionInterceptor cb = g_interceptors[i].cb.load(std::memory_order_relaxed);
        if (cb && SafeCallInterceptor(cb, self, params, function)) return true;
    }
    return false;
}

// Fire all observers in the selected table whose target matches `function`.
// Each call runs through SafeCallObserver (SEH wrapper) so an AV in any single
// callback is logged + absorbed instead of taking down the engine.
//
// D4-2 count-bounded walk: an N-registered table pays N + holes loads instead
// of kMaxObservers. (Empty-table / Bloom rejects already ran inline.)
void FireObserversMatched(bool post, void* self, void* function, void* params) {
    const ObserverSlot* table = post ? g_postObservers : g_preObservers;
    const std::atomic<int>& activeCounter = post ? g_postObserverActive : g_preObserverActive;
    const char* phase = post ? "POST" : "PRE";
    const int active = activeCounter.load(std::memory_order_acquire);
    int found = 0;
    for (int i = 0; i < kMaxObservers && found < active; ++i) {
        void* tgt = table[i].targetFn.load(std::memory_order_acquire);
        if (!tgt) continue;
        ++found;
        if (tgt != function) continue;
        ProcessEventObserverFn cb = table[i].cb.load(std::memory_order_relaxed);
        if (cb) SafeCallObserver(cb, self, function, params, phase);
    }
}

void FireNameDiagnosticsMatched(void* self, void* function, void* params) {
    // Resolve the function name. This calls into reflection -- cheap-ish but
    // not free; gated by the anySet fast path inline in the detour.
    auto fname = reflection::NameOf(function);
    std::wstring nameStr = reflection::ToString(fname);
    const wchar_t* funcName = nameStr.c_str();
    const size_t funcLen = nameStr.size();
    for (int i = 0; i < kMaxNameDiagnostics; ++i) {
        const int prefLen = g_nameDiagSlots[i].prefixLen.load(std::memory_order_acquire);
        if (prefLen <= 0) continue;
        if (static_cast<int>(funcLen) < prefLen) continue;
        // Case-sensitive prefix compare. We could lowercase both sides for
        // case-insensitive matching; UE4 FNames are case-preserving but
        // case-insensitive-equal -- for prefix debugging we just match the
        // case the BP author wrote.
        if (std::wmemcmp(funcName, g_nameDiagSlots[i].prefix, prefLen) == 0) {
            ProcessEventNameDiagnosticFn cb = g_nameDiagSlots[i].cb.load(std::memory_order_relaxed);
            if (cb) cb(self, funcName, params);
        }
    }
}

bool DrainPostedTasksAtTopLevel() {
    if (ue_wrap::spawn_gate::WorldRefusesSpawns()) {
        // Nested inside a construction script (or the world is tearing
        // down): a task run HERE gets null from every SpawnActor. Defer;
        // the queue drains on the first dispatch outside the window.
        NoteGateDeferral();
        return false;
    }
    NoteGateEpisodeEnd();  // no-op unless a deferral episode just ended
    // RAII so t_inPump is cleared on EVERY exit from Pump(), incl. an
    // exception path. Under /EHa the dtor runs during a structured-
    // exception unwind too -- so even if an AV ever escaped Pump's
    // own SEH (e.g. faulting in the queue lock itself), t_inPump
    // cannot get stuck `true` and silently kill all future draining.
    // The raw `t_inPump = false` it replaces was skipped on exactly
    // that path -> the 2026-05-30 permanent host freeze.
    struct InPumpGuard { ~InPumpGuard() { t_inPump = false; } } pumpGuard;
    t_inPump = true;
    Pump();
    return true;
}

void ClearAllInterceptors() {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        g_interceptors[i].targetFn.store(nullptr, std::memory_order_release);
        g_interceptors[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    g_interceptorActive.store(0, std::memory_order_release);
    BloomClear(g_intcBloom);
}

}  // namespace detail

// ---- public API owned by this TU ----------------------------------------------

void Post(Task task) {
    if (!task) return;
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queue.push_back(std::move(task));
    D::g_queueDepth.store(static_cast<int>(g_queue.size()), std::memory_order_release);
}

bool IsGameThread() {
    const unsigned long gt = D::g_gameThreadId.load(std::memory_order_relaxed);
    return gt != 0 && gt == ::GetCurrentThreadId();
}

bool IsDefinitelyOffGameThread() {
    // gt==0 means "not known yet" -- we cannot prove anything, so report false
    // (the guard must not fire at boot before the first ProcessEvent dispatch
    // records the game thread id). Once known, fire only on a real mismatch.
    const unsigned long gt = D::g_gameThreadId.load(std::memory_order_relaxed);
    return gt != 0 && gt != ::GetCurrentThreadId();
}

unsigned long long TasksRun() { return g_tasksRun.load(std::memory_order_relaxed); }

bool RegisterInterceptor(void* targetUFunction, UFunctionInterceptor cb) {
    return SetInterceptorSlot(targetUFunction, cb);
}

void UnregisterInterceptor(void* targetUFunction, UFunctionInterceptor cb) {
    ClearInterceptorSlot(targetUFunction, cb);
}

bool RegisterPostObserver(void* targetUFunction, ProcessEventObserverFn cb) {
    return SetObserverSlot(g_postObservers, D::g_postObserverActive, D::g_postBloom, targetUFunction, cb);
}

bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb) {
    return SetObserverSlot(g_preObservers, D::g_preObserverActive, D::g_preBloom, targetUFunction, cb);
}

void UnregisterObservers(void* targetUFunction, ProcessEventObserverFn cb) {
    if (!targetUFunction || !cb) return;
    // cb-specific: clears only THIS observer's slot, leaving any co-registered
    // observer on the same UFunction intact. A given cb lives in exactly one of
    // the two tables, so probing both is harmless.
    ClearObserverSlot(g_postObservers, D::g_postObserverActive, targetUFunction, cb);
    ClearObserverSlot(g_preObservers, D::g_preObserverActive, targetUFunction, cb);
}

void ClearAllObservers() {
    for (int i = 0; i < kMaxObservers; ++i) {
        g_postObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_postObservers[i].cb.store(nullptr, std::memory_order_relaxed);
        g_preObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_preObservers[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    D::g_postObserverActive.store(0, std::memory_order_release);
    D::g_preObserverActive.store(0, std::memory_order_release);
    BloomClear(D::g_postBloom);
    BloomClear(D::g_preBloom);
    ClearAllNameDiagnostics();
}

bool SetNameDiagnostic(int slot, const wchar_t* prefix, ProcessEventNameDiagnosticFn cb) {
    if (slot < 0 || slot >= kMaxNameDiagnostics) return false;
    if (!prefix || !cb || !*prefix) {
        // Clear this slot.
        g_nameDiagSlots[slot].prefixLen.store(0, std::memory_order_release);
        g_nameDiagSlots[slot].cb.store(nullptr, std::memory_order_relaxed);
        // Recompute anySet.
        int anySet = 0;
        for (int i = 0; i < kMaxNameDiagnostics; ++i) {
            if (g_nameDiagSlots[i].prefixLen.load(std::memory_order_relaxed) > 0) { anySet = 1; break; }
        }
        D::g_nameDiagAnySet.store(anySet, std::memory_order_release);
        return true;
    }
    // Copy prefix (bounded). Compute length up to kMaxNameDiagnosticPrefixLen-1.
    int len = 0;
    while (len < kMaxNameDiagnosticPrefixLen - 1 && prefix[len] != L'\0') {
        g_nameDiagSlots[slot].prefix[len] = prefix[len];
        ++len;
    }
    g_nameDiagSlots[slot].prefix[len] = L'\0';
    g_nameDiagSlots[slot].cb.store(cb, std::memory_order_relaxed);
    g_nameDiagSlots[slot].prefixLen.store(len, std::memory_order_release);
    D::g_nameDiagAnySet.store(1, std::memory_order_release);
    return true;
}

void ClearAllNameDiagnostics() {
    for (int i = 0; i < kMaxNameDiagnostics; ++i) {
        g_nameDiagSlots[i].prefixLen.store(0, std::memory_order_release);
        g_nameDiagSlots[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    D::g_nameDiagAnySet.store(0, std::memory_order_release);
}

void SetCallTrace(bool enabled) {
    D::g_callTrace.store(enabled, std::memory_order_release);
}

bool GetCallTrace() {
    return D::g_callTrace.load(std::memory_order_acquire);
}

int PostObserverCount() { return D::g_postObserverActive.load(std::memory_order_relaxed); }
int PreObserverCount()  { return D::g_preObserverActive.load(std::memory_order_relaxed); }
int InterceptorCount()  { return D::g_interceptorActive.load(std::memory_order_relaxed); }

}  // namespace ue_wrap::game_thread
