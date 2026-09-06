// ue_wrap/core/game_thread.cpp -- the dispatcher services the ProcessEvent detour drives: the
// observer, interceptor and name-diagnostic registries (with their Bloom presence probes) and
// the posted-task pump (with the spawn-refusal drain gate). The interposition mechanism itself
// (the hook install, the detour body, the bypass, the SEH firewalls, the depth probe, the perf
// instrumentation) lives in pe_detour.cpp; the private seam is game_thread_detail.h, whose
// inline fast rejects mean only matched dispatches or a non-empty queue cross the file
// boundary.

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

// Shared hot-path state, read inline by pe_detour.cpp through the detail header.
namespace detail {
std::atomic<uint64_t> g_postBloom[kBloomWords]{};
std::atomic<uint64_t> g_preBloom[kBloomWords]{};
std::atomic<uint64_t> g_intcBloom[kBloomWords]{};
std::atomic<int> g_interceptorActive{0};
std::atomic<int> g_postObserverActive{0};
std::atomic<int> g_preObserverActive{0};
// The call-trace flag: when true, the detour logs every UFunction dispatch. A one-shot probe
// for blueprint call chains.
// The lock-free emptiness probe. The detour runs on every game-thread ProcessEvent dispatch
// (about 85k per second, measured) and once took the queue mutex just to test emptiness, a
// locked read-modify-write on the hottest path in the program, for a queue that is empty
// almost always (a post runs about a hundred times per second). The depth mirrors the queue
// size, maintained under the mutex by the writers; the detour reads it without the lock and
// drains only when it is non-zero. A just-posted task whose increment is not yet visible to
// the detour's relaxed read is drained on the next dispatch, microseconds later; tasks are not
// latency-critical at that scale. Writers stay under the mutex, so the depth and the deque
// never diverge.
std::atomic<int> g_queueDepth{0};
// The re-entrancy guard: set while inside the pump, so a task that calls a UFunction
// (re-entering ProcessEvent and the detour) skips draining and just forwards. Thread-local
// because only the game thread ever sets it, and correct even if ProcessEvent were ever called
// cross-thread.
thread_local bool t_inPump = false;
// The game thread id, recorded by the detour on its first dispatch.
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

// The multi-slot pre-dispatch interceptor table, the same atomic-slot shape as the observer
// tables; a null target is a free slot. The detour walks the table on each dispatch, and the
// first callback returning true cancels the original.
struct InterceptorSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<UFunctionInterceptor> cb{nullptr};
};
InterceptorSlot g_interceptors[kMaxInterceptors];

// The per-table count of populated slots, so the detour walk can early-terminate after finding
// every live entry instead of walking every slot per dispatch: at around a hundred thousand
// dispatches per second the 128-slot observer table cost 128 atomic loads per dispatch with a
// few dozen observers registered. The count is a release store sequenced after the target
// slot store, so the detour's acquire load of the count fences any race with a registration:
// if it sees the new count, it sees the new slot.

bool SetInterceptorSlot(void* targetFn, UFunctionInterceptor cb) {
    if (!targetFn || !cb) return false;
    // Keyed by the (target, cb) pair, the same multi-registrant invariant as the observer slots:
    // every slot matching the function is consulted and the first callback returning true stops,
    // so two interceptors on one UFunction coexist instead of the later silently overwriting the
    // earlier. The first pass is an idempotent re-register of the exact pair.
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            g_interceptors[i].cb.load(std::memory_order_relaxed) == cb) {
            return true;
        }
    }
    // The second pass takes the first empty slot. The active count is bumped after the slot
    // store, so the detour's count-bounded walk sees the new entry.
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

// The POST and PRE observer tables, fixed size, no allocation. An entry is a UFunction and a
// callback; a null function is a free slot. A registration stores the callback first, then the
// function pointer with release ordering; the detour loads the function pointer with acquire,
// then the callback relaxed (the interceptor slots follow the same pattern). The fixed size
// keeps the detour loop bounded.
struct ObserverSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<ProcessEventObserverFn> cb{nullptr};
};
ObserverSlot g_postObservers[kMaxObservers];
ObserverSlot g_preObservers[kMaxObservers];

// Add the (target, cb) pair to the table; false if the table is full. Several subsystems
// legitimately observe the same UFunction: more than one lane observes the deferred spawn, and
// the NPC and prop lifecycles both observe the actor destroy. Keyed on the target alone, the
// second registrant silently overwrote the first's callback and one of the two never fired
// (the host's NPC actor bind was clobbered and the destroy sync went dead). The firing walks
// every slot whose target matches, so each pair owns a slot and all of them run. The active
// counter is bumped only when an empty slot is populated; an exact re-register is an
// idempotent no-op.
bool SetObserverSlot(ObserverSlot table[], std::atomic<int>& activeCounter,
                     std::atomic<uint64_t>* bloom, void* targetFn, ProcessEventObserverFn cb) {
    if (!targetFn || !cb) return false;
    // The first pass: an idempotent re-register, a slot already holding this exact pair is a
    // no-op. Registration is serialised on the game thread, so the relaxed loads race only with
    // the detour reader, which the release store of the target below fences.
    for (int i = 0; i < kMaxObservers; ++i) {
        if (table[i].targetFn.load(std::memory_order_relaxed) == targetFn &&
            table[i].cb.load(std::memory_order_relaxed) == cb) {
            return true;
        }
    }
    // The second pass takes the first empty slot.
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

// Clear the slot matching the (target, cb) pair; callback-specific, so unregistering one
// subsystem's observer does not clear a co-registered observer on the same UFunction.
// Decrements the active counter once per slot actually cleared.
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

// The posted-task queue. A task is pulled out under the lock, then run unlocked, so a task
// may post without deadlocking.
std::mutex g_queueMutex;
std::deque<Task> g_queue;

// The spawn-refusal deferral episode. Tasks assume top-level game-thread context, but the
// detour fires on every ProcessEvent, including dispatches nested inside another actor's
// construction script, where a spawn silently returns null (see spawn_gate.h). During a
// save-load's mass actor construction nearly every dispatch is such a nested one, so draining
// there made every spawn a task issued fail for the whole load tail. The drain is deferred
// while the world refuses spawns; the queue keeps its order and drains on the first dispatch
// outside the window: sub-millisecond in steady state, end-of-load during a mass construction,
// exactly when spawns start succeeding. Game thread only, so plain ints.
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
    // A construction or teardown window outlasting 5 s means the world state is wedged, or a
    // teardown stalled: keep deferring (draining into the window is the bug this gate closes), but
    // say so, loudly and throttled.
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
        // The mod is compiled with asynchronous exceptions, so a structured exception (an access
        // violation, a divide by zero) raised inside a task is both caught by the SEH wrapper and
        // unwinds C++ destructors on the way out, so any lock guard the task held is released.
        // Load-bearing: posted tasks run gameplay and reflection work that can fault on a stale
        // engine pointer (a connect-edge snapshot reading a collected actor), and a fault that
        // propagated without destructors leaked the element Registry mutex locked with the in-pump
        // flag set, a permanent game-thread freeze. The SEH wrapper's filter captures the faulting
        // IP and the access address before the unwind, so an absorbed fault names its own site. The
        // pump loop continues either way, so one faulting task never stops the others or wedges the
        // tick.
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

// The detail cold halves, called by pe_detour.cpp past the inline fast rejects.

namespace detail {

// True if any interceptor for the function returns true. The acquire load on the function
// pointer pairs with the release store in the registration (no torn target-and-callback pair
// on weakly ordered ISAs; on x86-64 acquire is free, and the ordering matters for an ARM
// port). The walk is count-bounded: the active count is loaded once at the top and the loop
// ends when every live entry was found, so an N-entry table pays N loads instead of the table
// size. The empty-table and Bloom rejects already ran inline in the detour.
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

// Fire every observer in the selected table whose target matches the function, each through
// the SEH wrapper, so a fault in one callback is logged and absorbed instead of taking down
// the engine. The walk is count-bounded like the interceptors'.
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

bool DrainPostedTasksAtTopLevel() {
    if (ue_wrap::spawn_gate::WorldRefusesSpawns()) {
        // Nested inside a construction script, or the world is tearing down: a task run here gets
        // null from every spawn. Defer; the queue drains on the first dispatch outside the window.
        NoteGateDeferral();
        return false;
    }
    NoteGateEpisodeEnd();  // no-op unless a deferral episode just ended
    // RAII, so the in-pump flag is cleared on every exit from the pump, including an exception
    // path: under asynchronous exceptions the destructor runs during a structured-exception
    // unwind too, so even if a fault escaped the pump's own SEH (faulting in the queue lock
    // itself, say) the flag cannot stick and silently kill all future draining. A raw reset was
    // skipped on exactly that path, a permanent host freeze.
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

// The public API owned by this file.

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
    // An id of 0 means not known yet, so nothing can be proven and the answer is false: the guard
    // must not fire at boot before the first dispatch records the game thread id. Once known, it
    // fires only on a real mismatch.
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
    // Callback-specific: clears only this observer's slot, leaving any co-registered observer on
    // the same UFunction intact. A given callback lives in exactly one of the two tables, so
    // probing both is harmless.
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
}

int PostObserverCount() { return D::g_postObserverActive.load(std::memory_order_relaxed); }
int PreObserverCount()  { return D::g_preObserverActive.load(std::memory_order_relaxed); }
int InterceptorCount()  { return D::g_interceptorActive.load(std::memory_order_relaxed); }

}  // namespace ue_wrap::game_thread
