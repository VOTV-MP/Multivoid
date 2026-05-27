#include "ue_wrap/game_thread.h"

#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace ue_wrap::game_thread {
namespace {

// ProcessEvent's signature (x64 ABI). Matches reflection's ProcessEventFn.
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

ProcessEventFn g_originalPE = nullptr;  // trampoline to the real ProcessEvent
void* g_hookTarget = nullptr;
bool g_installed = false;

std::atomic<unsigned long> g_gameThreadId{0};
std::atomic<unsigned long long> g_tasksRun{0};

// Multi-slot UFunction-pre-dispatch interceptor table. Same atomic-slot shape
// as the observer tables; null targetFn = free slot. The detour walks the
// table on each dispatch; first cb returning true cancels the original.
struct InterceptorSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<UFunctionInterceptor> cb{nullptr};
};
InterceptorSlot g_interceptors[kMaxInterceptors];

bool SetInterceptorSlot(void* targetFn, UFunctionInterceptor cb) {
    if (!targetFn || !cb) return false;
    // First pass: replace existing entry for this target (idempotent registration).
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == targetFn) {
            g_interceptors[i].cb.store(cb, std::memory_order_relaxed);
            g_interceptors[i].targetFn.store(targetFn, std::memory_order_release);
            return true;
        }
    }
    // Second pass: take the first empty slot.
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == nullptr) {
            g_interceptors[i].cb.store(cb, std::memory_order_relaxed);
            g_interceptors[i].targetFn.store(targetFn, std::memory_order_release);
            return true;
        }
    }
    return false;  // table full
}

void ClearInterceptorSlot(void* targetFn) {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        if (g_interceptors[i].targetFn.load(std::memory_order_relaxed) == targetFn) {
            g_interceptors[i].targetFn.store(nullptr, std::memory_order_release);
            g_interceptors[i].cb.store(nullptr, std::memory_order_relaxed);
        }
    }
}

void ClearAllInterceptors() {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        g_interceptors[i].targetFn.store(nullptr, std::memory_order_release);
        g_interceptors[i].cb.store(nullptr, std::memory_order_relaxed);
    }
}

// Returns true if any interceptor for `function` returns true. Acquire load
// on the function pointer pairs with the release store in SetInterceptorSlot
// (no torn target+cb pair on weakly-ordered ISAs; on x86-64 TSO acquire is
// free, ordering matters for a future ARM port).
inline bool FireInterceptors(void* self, void* function, void* params) {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        void* tgt = g_interceptors[i].targetFn.load(std::memory_order_acquire);
        if (!tgt || tgt != function) continue;
        UFunctionInterceptor cb = g_interceptors[i].cb.load(std::memory_order_relaxed);
        if (cb && cb(self, params)) return true;
    }
    return false;
}

// Multi-target POST and PRE observers (fixed-size tables, no allocation).
// Each entry is {UFunction*, ProcessEventObserverFn}. A null UFunction*
// entry is a free slot. Writes via SetObserverSlot store the cb FIRST then
// the function pointer with release ordering; reads in the detour load the
// function pointer with acquire ordering then the cb with relaxed (same
// pattern as g_interceptUFunc/g_interceptCb). Fixed size kMaxObservers (see
// game_thread.h; currently 16) keeps the detour loop bounded.
struct ObserverSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<ProcessEventObserverFn> cb{nullptr};
};
ObserverSlot g_postObservers[kMaxObservers];
ObserverSlot g_preObservers[kMaxObservers];

// Set a free slot in `table` to (targetFn, cb). Returns false if no free slot
// or duplicate (we permit overwriting an existing entry for the same target).
bool SetObserverSlot(ObserverSlot table[], void* targetFn, ProcessEventObserverFn cb) {
    if (!targetFn || !cb) return false;
    // First pass: replace existing entry for this target.
    for (int i = 0; i < kMaxObservers; ++i) {
        void* cur = table[i].targetFn.load(std::memory_order_relaxed);
        if (cur == targetFn) {
            table[i].cb.store(cb, std::memory_order_relaxed);
            table[i].targetFn.store(targetFn, std::memory_order_release);
            return true;
        }
    }
    // Second pass: take the first empty slot.
    for (int i = 0; i < kMaxObservers; ++i) {
        void* cur = table[i].targetFn.load(std::memory_order_relaxed);
        if (cur == nullptr) {
            table[i].cb.store(cb, std::memory_order_relaxed);
            table[i].targetFn.store(targetFn, std::memory_order_release);
            return true;
        }
    }
    return false;  // table full
}

// Clear any slot in `table` matching `targetFn`.
void ClearObserverSlot(ObserverSlot table[], void* targetFn) {
    for (int i = 0; i < kMaxObservers; ++i) {
        if (table[i].targetFn.load(std::memory_order_relaxed) == targetFn) {
            table[i].targetFn.store(nullptr, std::memory_order_release);
            table[i].cb.store(nullptr, std::memory_order_relaxed);
        }
    }
}

// Fire all observers in `table` whose target matches `function`.
inline void FireObservers(const ObserverSlot table[], void* self, void* function, void* params) {
    for (int i = 0; i < kMaxObservers; ++i) {
        void* tgt = table[i].targetFn.load(std::memory_order_acquire);
        if (tgt && tgt == function) {
            ProcessEventObserverFn cb = table[i].cb.load(std::memory_order_relaxed);
            if (cb) cb(self, function, params);
        }
    }
}

// Name-prefix diagnostic table. Each slot is (prefix wchar buffer, cb).
// `anySet` is the fast-path bypass: when no slots are populated, the detour
// skips the whole name-resolve+compare path entirely (zero cost in the
// common case). Single producer / multiple readers, atomic publish.
struct NameDiagSlot {
    wchar_t prefix[kMaxNameDiagnosticPrefixLen]{};
    std::atomic<ProcessEventNameDiagnosticFn> cb{nullptr};
    std::atomic<int> prefixLen{0};  // 0 = empty slot
};
NameDiagSlot g_nameDiagSlots[kMaxNameDiagnostics];
std::atomic<int> g_nameDiagAnySet{0};

// 2026-05-26 deep-RE call trace flag. When true, ProcessEventDetour
// logs every UFunction dispatch. Used as a one-shot probe to capture
// BP call chains when reflection-invoked BPs appear to no-op.
std::atomic<bool> g_callTrace{false};

inline void FireNameDiagnostics(void* self, void* function, void* params) {
    if (g_nameDiagAnySet.load(std::memory_order_acquire) == 0) return;
    if (!function) return;
    // Resolve the function name. This calls into reflection -- cheap-ish but
    // not free; gated by the anySet fast path above.
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

// The posted-task queue. Pulled out under the lock, then run unlocked so a task
// may Post() without deadlocking.
std::mutex g_queueMutex;
std::deque<Task> g_queue;

// Re-entrancy guard: set while we are inside the pump, so a task that calls a
// UFunction (re-entering ProcessEvent -> this detour) skips draining and just
// forwards. Thread-local because only the game thread ever sets it, but a guard
// keeps it correct even if ProcessEvent were ever called cross-thread.
thread_local bool t_inPump = false;

void Pump() {
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_queue.empty()) return;
            task = std::move(g_queue.front());
            g_queue.pop_front();
        }
        // A task running engine code can throw nothing we control; if it does,
        // letting it propagate would corrupt the engine's call. Swallow + log.
        try {
            task();
        } catch (...) {
            UE_LOGE("game_thread: a posted task threw; swallowed");
        }
        g_tasksRun.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall ProcessEventDetour(void* self, void* function, void* params) {
    // Record the game thread id the first time we run here. CAS so that if two
    // threads race the very first dispatch, exactly one wins (a plain load+store
    // could let a worker thread overwrite the real game thread id).
    if (g_gameThreadId.load(std::memory_order_relaxed) == 0) {
        unsigned long expected = 0;
        g_gameThreadId.compare_exchange_strong(expected, ::GetCurrentThreadId(),
                                               std::memory_order_relaxed, std::memory_order_relaxed);
    }

    // ProcessEvent is also called from task-graph WORKER threads (parallel anim,
    // etc.), not just the game thread. Posted tasks call engine UFunctions, which
    // are game-thread-only -- running them on a worker thread corrupts engine state
    // and crashes (seen as an AV on TaskGraphThreadHP). So drain the queue ONLY on
    // the recorded game thread (the first ProcessEvent caller, validated by the
    // self-test). Other threads just forward.
    if (!t_inPump && ::GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed)) {
        bool hasWork;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            hasWork = !g_queue.empty();
        }
        if (hasWork) {
            t_inPump = true;
            Pump();
            t_inPump = false;
        }
    }

    // UFunction interceptors: pre-dispatch hooks on a multi-slot table. If
    // any interceptor for `function` returns true, the original ProcessEvent
    // is SKIPPED -- the UFunction's body is replaced for this call. The walk
    // is kMaxInterceptors (16) acquire-loads of a function-pointer, with
    // the cb load only happening on a target match -- cheap empty-table case.
    if (FireInterceptors(self, function, params)) return;

    // PRE-observers: fire BEFORE the original. Used to snapshot state the BP
    // is about to clear (e.g. PHC.ReleaseComponent PRE reads handle+176
    // GrabbedComponent before PhysX clears it).
    // The walk is kMaxObservers (16) pointer compares -- same cost class as the
    // single interceptor compare above. NO observers registered = NO cost
    // beyond the 16 null loads.
    FireObservers(g_preObservers, self, function, params);

    // Diagnostic name-prefix sniffer (zero cost when no slot is set).
    FireNameDiagnostics(self, function, params);

    // 2026-05-26 deep-RE call trace (one-shot diagnostic). When the
    // trace flag is on, log every ProcessEvent dispatch. Used to
    // capture BP call chains when reflection-invoked BPs don't appear
    // to do anything. The atomic load is relaxed (we don't care about
    // strict ordering -- the trace is best-effort observability).
    if (g_callTrace.load(std::memory_order_relaxed) && function) {
        auto fname = reflection::NameOf(function);
        std::wstring nameStr = reflection::ToString(fname);
        UE_LOGI("trace: PE self=%p func=%ls", self, nameStr.c_str());
    }

    g_originalPE(self, function, params);

    // POST-observers: fire AFTER the original. Used to read state the BP just
    // wrote (e.g. PHC.GrabComponentAtLocation POST reads handle+176 to see
    // what was just grabbed; PHC.SetTargetLocation POST sees the per-tick
    // drive target).
    FireObservers(g_postObservers, self, function, params);
}

}  // namespace

bool Install() {
    if (g_installed) return true;

    void* pe = reinterpret_cast<void*>(reflection::ProcessEventAddr());
    if (!pe) {
        UE_LOGE("game_thread: ProcessEvent unresolved; resolve reflection first");
        return false;
    }
    if (!hook::Init()) return false;
    if (!hook::Install(pe, reinterpret_cast<void*>(&ProcessEventDetour),
                       reinterpret_cast<void**>(&g_originalPE))) {
        return false;
    }
    g_hookTarget = pe;
    g_installed = true;
    UE_LOGI("game_thread: ProcessEvent hooked; game-thread dispatcher live");
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    ClearAllObservers();
    ClearAllInterceptors();
    hook::Uninstall(g_hookTarget);
    g_installed = false;
    g_hookTarget = nullptr;
    // Audit C3 (2026-05-27): DO NOT null g_originalPE here. A worker thread
    // already inside ProcessEventDetour when MinHook ran Uninstall above is
    // racing with us -- it loaded g_originalPE before the unhook, but if the
    // store below lands BEFORE its load completes (out-of-order via cache),
    // it dereferences null. After hook::Uninstall the original UE4 PE has
    // been restored at the call site, so ProcessEventDetour is no longer
    // being entered for new dispatches; the field is no longer read once
    // in-flight workers drain. Leaving the pointer non-null is harmless
    // (UAF is not possible because g_originalPE points at the engine's PE,
    // a process-lifetime entry point that is never unloaded). Add a tiny
    // drain Sleep so any in-flight detour body finishes before we tear
    // down g_hookTarget. SC_CLOSE's 100ms worker drain reduces the window
    // but doesn't close it; this is belt-and-braces.
    ::Sleep(50);
}

bool IsInstalled() { return g_installed; }

void Post(Task task) {
    if (!task) return;
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queue.push_back(std::move(task));
}

bool IsGameThread() {
    const unsigned long gt = g_gameThreadId.load(std::memory_order_relaxed);
    return gt != 0 && gt == ::GetCurrentThreadId();
}

unsigned long long TasksRun() { return g_tasksRun.load(std::memory_order_relaxed); }

bool RegisterInterceptor(void* targetUFunction, UFunctionInterceptor cb) {
    return SetInterceptorSlot(targetUFunction, cb);
}

void UnregisterInterceptor(void* targetUFunction) {
    ClearInterceptorSlot(targetUFunction);
}

bool RegisterPostObserver(void* targetUFunction, ProcessEventObserverFn cb) {
    return SetObserverSlot(g_postObservers, targetUFunction, cb);
}

bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb) {
    return SetObserverSlot(g_preObservers, targetUFunction, cb);
}

void UnregisterObservers(void* targetUFunction) {
    if (!targetUFunction) return;
    ClearObserverSlot(g_postObservers, targetUFunction);
    ClearObserverSlot(g_preObservers, targetUFunction);
}

void ClearAllObservers() {
    for (int i = 0; i < kMaxObservers; ++i) {
        g_postObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_postObservers[i].cb.store(nullptr, std::memory_order_relaxed);
        g_preObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_preObservers[i].cb.store(nullptr, std::memory_order_relaxed);
    }
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
        g_nameDiagAnySet.store(anySet, std::memory_order_release);
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
    g_nameDiagAnySet.store(1, std::memory_order_release);
    return true;
}

void ClearAllNameDiagnostics() {
    for (int i = 0; i < kMaxNameDiagnostics; ++i) {
        g_nameDiagSlots[i].prefixLen.store(0, std::memory_order_release);
        g_nameDiagSlots[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    g_nameDiagAnySet.store(0, std::memory_order_release);
}

void SetCallTrace(bool enabled) {
    g_callTrace.store(enabled, std::memory_order_release);
}

bool GetCallTrace() {
    return g_callTrace.load(std::memory_order_acquire);
}

}  // namespace ue_wrap::game_thread
