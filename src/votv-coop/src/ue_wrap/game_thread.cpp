#include "ue_wrap/game_thread.h"

#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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

// Transparent-bypass deadline (steady_clock ms; 0 = off). While NowMs() < this,
// ProcessEventDetour forwards STRAIGHT to the original ProcessEvent -- skipping
// interceptors, observers, the posted-task pump, diagnostics, AND the outer SEH
// wrapper -- making our DLL fully transparent. Armed during the local-death flee
// to the menu: VOTV's transition("/Game/menu") tears down the 50k-object
// untitled_1 world, firing ReceiveEndPlay/EndPlay through our detour per dying
// actor; our observers + the outer SEH (which catches and does NOT forward,
// mangling half-run EndPlays) deadlock the swap (proven to hang the teardown).
// Arming the bypass for the teardown window lets VOTV travel natively, then it
// auto-expires so the fresh menu world runs with our layer fully normal again.
std::atomic<long long> g_bypassUntilMs{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Perf instrumentation (MEASURE-first 15-FPS audit; see coop/dev/perf_probe) --
// g_peCountOn gates the per-dispatch counter: OFF (default/shipping) the detour
// pays a single relaxed bool load; ON it adds one relaxed XADD per dispatch.
// g_peSelfOn additionally arms the sampled self-timer (1 dispatch in
// kSelfSampleMask+1) that brackets the detour body EXCLUDING g_originalPE -- i.e.
// OUR per-dispatch overhead only. All totals are monotonic; perf_probe diffs them
// per second. Defined here (before SafeCall*/the detour) so all users see it.
std::atomic<bool> g_peCountOn{false};
std::atomic<bool> g_peSelfOn{false};
std::atomic<unsigned long long> g_peDispatchCount{0};    // all threads
std::atomic<unsigned long long> g_peDispatchCountGT{0};  // game-thread subset (the per-dispatch substrate cost only applies here)
std::atomic<unsigned long long> g_peSelfNs{0};
std::atomic<unsigned long long> g_peSelfSamples{0};
constexpr unsigned long long kSelfSampleMask = 0xFF;  // sample 1 dispatch in 256

// Observer/interceptor CALLBACK-BODY timing. The audit's rank-2 suspect for the
// 50 ms is not the table WALK but a callback BODY that secretly calls an uncached
// reflection Find*/CountObjectsByClass (a ~1M-entry GUObjectArray walk + a wstring
// alloc per entry) on a hot/common UFunction. SafeCallObserver/SafeCallInterceptor
// bracket each cb with QPC when counting is armed and record the running total +
// the single worst call (with its UFunction*, resolved to a name by perf_probe).
std::atomic<unsigned long long> g_obsBodyNs{0};       // summed cb-body time across all fired observers+interceptors
std::atomic<unsigned long long> g_obsWorstNs{0};      // worst single cb-body call seen (ns)
std::atomic<void*>              g_obsWorstFn{nullptr}; // the UFunction* of that worst call

// QPC ticks/sec, cached on first use (QueryPerformanceFrequency is constant for
// the process lifetime). 0 until resolved.
long long QpcFreq() {
    static long long s_freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    return s_freq;
}
inline unsigned long long QpcDeltaToNs(long long ticks) {
    const long long f = QpcFreq();
    return f > 0 ? static_cast<unsigned long long>((ticks * 1000000000LL) / f) : 0ull;
}
// Record one observer/interceptor cb-body duration (ns). Updates total + worst.
// Only called on the (rare) path where a dispatched UFunction matched a registrant.
inline void RecordCbBodyNs(void* function, unsigned long long ns) {
    g_obsBodyNs.fetch_add(ns, std::memory_order_relaxed);
    if (ns > g_obsWorstNs.load(std::memory_order_relaxed)) {
        g_obsWorstNs.store(ns, std::memory_order_relaxed);
        g_obsWorstFn.store(function, std::memory_order_relaxed);
    }
}

// ---- O(1) observer/interceptor presence probe (perf, 2026-06-04) --------------
// The detour walked up to ~75 POST-observer slots PER game-thread dispatch
// (measured: post=75) even though the dispatched UFunction matches a registrant on
// far under 1% of dispatches. A per-table Bloom bitmask gives an O(1) reject: the
// dispatched function's bit is tested first; if clear, NO registrant targets it and
// the slot walk is skipped entirely (one word load + one bit test). Bits are only
// ever SET (on register), never cleared on a single Unregister -- so a LIVE
// registrant's bit is ALWAYS set => no false negatives (a missed observer fire is
// impossible); an unregistered target may leave a stale bit (a harmless false
// positive that just falls through to the now-no-match slot walk). ClearAll* zeroes
// the mask (no readers during teardown). Lock-free: the detour fires on worker
// threads too, and a Bloom load racing a register's fetch_or is benign (the acquire
// load pairs with the release fetch_or; worst case is one dispatch's reject lag,
// the same window the slot-walk's count-bounded read already tolerates).
constexpr int kBloomWords = 64;                         // 4096 bits; ~75 entries => ~2% false positive
constexpr unsigned kBloomBits = kBloomWords * 64u;
std::atomic<uint64_t> g_postBloom[kBloomWords]{};
std::atomic<uint64_t> g_preBloom[kBloomWords]{};
std::atomic<uint64_t> g_intcBloom[kBloomWords]{};
inline unsigned BloomBit(void* fn) {
    // UFunction pointers are >= 16-aligned; drop the dead low bits before masking.
    return static_cast<unsigned>((reinterpret_cast<uintptr_t>(fn) >> 4) & (kBloomBits - 1));
}
inline void BloomAdd(std::atomic<uint64_t>* bloom, void* fn) {
    if (!fn) return;
    const unsigned b = BloomBit(fn);
    bloom[b >> 6].fetch_or(1ull << (b & 63), std::memory_order_release);
}
inline bool BloomMaybe(const std::atomic<uint64_t>* bloom, void* fn) {
    const unsigned b = BloomBit(fn);
    return (bloom[b >> 6].load(std::memory_order_acquire) & (1ull << (b & 63))) != 0;
}
inline void BloomClear(std::atomic<uint64_t>* bloom) {
    for (int i = 0; i < kBloomWords; ++i) bloom[i].store(0, std::memory_order_release);
}

// Multi-slot UFunction-pre-dispatch interceptor table. Same atomic-slot shape
// as the observer tables; null targetFn = free slot. The detour walks the
// table on each dispatch; first cb returning true cancels the original.
struct InterceptorSlot {
    std::atomic<void*> targetFn{nullptr};
    std::atomic<UFunctionInterceptor> cb{nullptr};
};
InterceptorSlot g_interceptors[kMaxInterceptors];

// D4-2 (2026-05-29 foundation audit fix): per-table count of populated
// slots so the detour walk can early-terminate after finding all live
// entries instead of always walking all kMaxInterceptors / kMaxObservers
// slots per ProcessEvent dispatch. UE4 dispatches ~100k PE/sec; with the
// 128-slot post-observer table that was 128 atomic acquire-loads per
// dispatch on x86 even when only ~30 observers were registered. The
// count is a release-store sequenced AFTER the targetFn slot store so
// the detour's acquire-load of the count fences any race-with-register
// (if the detour sees the new count, it also sees the new slot).
std::atomic<int> g_interceptorActive{0};

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
            g_interceptorActive.fetch_add(1, std::memory_order_release);
            BloomAdd(g_intcBloom, targetFn);  // O(1) presence probe
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
            g_interceptorActive.fetch_sub(1, std::memory_order_release);
        }
    }
}

void ClearAllInterceptors() {
    for (int i = 0; i < kMaxInterceptors; ++i) {
        g_interceptors[i].targetFn.store(nullptr, std::memory_order_release);
        g_interceptors[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    g_interceptorActive.store(0, std::memory_order_release);
    BloomClear(g_intcBloom);
}

// SEH-wrapped single-callback dispatch. MSVC disallows mixing C++ unwind
// (std::wstring destructor) with __try/__except in the same function, so
// the __try wrapper does ONLY the raw call + an int-returning "did it
// crash?" sentinel; the C++ logging path lives in a separate function.
//
// 2026-05-27 (post-anim-ship crash diagnostic): introduced because the user
// reported "client crashed picking up a pile of garbage" with the AV deep in
// our DLL but no symbol-mapped frames. Routing each observer dispatch through
// here surfaces the function name in the log next time, so we know exactly
// which callback to inspect. KEEP this wrapper -- it doubles as a crash
// firewall against future observer regressions.
void LogObserverAv(void* function, void* self, const char* phase) {
    const auto fname = reflection::NameOf(function);
    const std::wstring nameStr = reflection::ToString(fname);
    UE_LOGE("game_thread: PE %s-callback AV caught -- function='%ls' (%p) self=%p; absorbing, process continues",
            phase, nameStr.c_str(), function, self);
}

// Returns 0 on clean completion, 1 if SEH caught an exception. cb returns
// its own bool via *outIntercept (only meaningful if return value is 0).
// __try / __except is the ONLY thing in this function -- no C++ destructors.
int RunInterceptorSEH(UFunctionInterceptor cb, void* self, void* params, bool* outIntercept) {
    __try {
        *outIntercept = cb(self, params);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int RunObserverSEH(ProcessEventObserverFn cb, void* self, void* function, void* params) {
    __try {
        cb(self, function, params);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// ---- Absorbed-fault localization (firewall diagnosability) -----------------
// The Pump() crash firewall absorbs a faulting task so the host survives, but
// historically it logged only a GENERIC "absorbed exception" line -- it did not
// say WHERE the fault was. That blind spot cost real RE time twice (the bug1
// per-tick AV balloon needed convergent-agent RE; bug2 -- the intermittent
// unpossessed-first-client AV flood -- still can't be pinned without it). These
// pieces capture the faulting instruction pointer + the access address + the
// containing module/RVA, so the next absorbed fault names its own site. A
// votv-coop.dll RVA maps to a function via votv-coop.map (/MAP) +
// tools/maprva.py; a game-exe hit means the fault is inside a ProcessEvent-
// dispatched UFunction on a bad object.
struct TaskFaultInfo {
    void*         faultingIP = nullptr;  // EXCEPTION_RECORD::ExceptionAddress
    void*         accessAddr = nullptr;  // ExceptionInformation[1] on an AV
    unsigned long code       = 0;        // 0xC0000005 == EXCEPTION_ACCESS_VIOLATION
};
thread_local TaskFaultInfo t_lastTaskFault{};

// SEH filter -- runs in the faulting context (registers still valid) BEFORE the
// unwind, so it must only stash, never allocate. Returns EXCEPTION_EXECUTE_HANDLER.
int TaskFaultFilter(EXCEPTION_POINTERS* ep) {
    t_lastTaskFault.code       = ep->ExceptionRecord->ExceptionCode;
    t_lastTaskFault.faultingIP = ep->ExceptionRecord->ExceptionAddress;
    t_lastTaskFault.accessAddr =
        (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
         ep->ExceptionRecord->NumberParameters >= 2)
            ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
            : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

// SEH-only (no C++ destructors in this frame -- MSVC constraint, same contract
// as RunObserverSEH; `task` is a reference so it has no destructor here).
// Under /EHa the __except unwind STILL runs the task frame's C++ destructors
// (that is precisely why this image is built /EHa), so the load-bearing
// lock-release property of the Pump catch it replaces is fully preserved.
// Catches both structured exceptions (AV/div0) AND C++ throws (the latter as
// code 0xE06D7363). Returns 0 clean, 1 if an exception was caught.
int RunTaskSEH(const Task& task) {
    __try {
        task();
        return 0;
    } __except (TaskFaultFilter(GetExceptionInformation())) {
        return 1;
    }
}

// Resolve a faulting IP to "module+0xRVA" for the log. C++ (uses Win32 + a
// thread-local buffer); called only from Pump's C++ body, never from the
// SEH-only RunTaskSEH. The logged RVA is ASLR-independent (ip - runtime base),
// so it maps directly against votv-coop.map's preferred-base RVAs.
const char* FormatModuleRva(void* ip) {
    static thread_local char buf[320];
    HMODULE hmod = nullptr;
    if (ip && ::GetModuleHandleExW(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                  reinterpret_cast<LPCWSTR>(ip), &hmod) &&
        hmod) {
        char path[MAX_PATH] = {0};
        ::GetModuleFileNameA(hmod, path, MAX_PATH);
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        const unsigned long long rva =
            reinterpret_cast<uintptr_t>(ip) - reinterpret_cast<uintptr_t>(hmod);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s+0x%llX modbase=%p",
                    base, rva, static_cast<void*>(hmod));
    } else {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "<unknown-module ip=%p>", ip);
    }
    return buf;
}
// ---- end absorbed-fault localization ---------------------------------------

bool SafeCallInterceptor(UFunctionInterceptor cb, void* self, void* params, void* function) {
    bool intercept = false;
    const bool timed = g_peCountOn.load(std::memory_order_relaxed);
    LARGE_INTEGER a{};
    if (timed) ::QueryPerformanceCounter(&a);
    const int rc = RunInterceptorSEH(cb, self, params, &intercept);
    if (timed) { LARGE_INTEGER b{}; ::QueryPerformanceCounter(&b); RecordCbBodyNs(function, QpcDeltaToNs(b.QuadPart - a.QuadPart)); }
    if (rc != 0) {
        LogObserverAv(function, self, "interceptor");
        return false;  // treat as "no interception" so original PE still runs
    }
    return intercept;
}

void SafeCallObserver(ProcessEventObserverFn cb, void* self, void* function, void* params,
                      const char* phase /* "PRE" or "POST" */) {
    const bool timed = g_peCountOn.load(std::memory_order_relaxed);
    LARGE_INTEGER a{};
    if (timed) ::QueryPerformanceCounter(&a);
    const int rc = RunObserverSEH(cb, self, function, params);
    if (timed) { LARGE_INTEGER b{}; ::QueryPerformanceCounter(&b); RecordCbBodyNs(function, QpcDeltaToNs(b.QuadPart - a.QuadPart)); }
    if (rc != 0) {
        LogObserverAv(function, self, phase);
    }
}

// Returns true if any interceptor for `function` returns true. Acquire load
// on the function pointer pairs with the release store in SetInterceptorSlot
// (no torn target+cb pair on weakly-ordered ISAs; on x86-64 TSO acquire is
// free, ordering matters for a future ARM port).
//
// D4-2 (2026-05-29): count-bounded walk. Loads g_interceptorActive ONCE at
// the top; loop terminates when `found == active` so an N-entry table
// pays N acquire-loads instead of kMaxInterceptors. Common case (no
// interceptors registered, active == 0) exits immediately. Re-register
// race during the walk is safe: the slot is still visible after a single
// dispatch's count window; the acquired count fences any concurrent
// register's release-store of targetFn.
inline bool FireInterceptors(void* self, void* function, void* params) {
    const int active = g_interceptorActive.load(std::memory_order_acquire);
    if (active <= 0) return false;
    if (!BloomMaybe(g_intcBloom, function)) return false;  // O(1) reject: nothing intercepts `function`
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

// D4-2: per-table active-slot counts paired with the tables above. Each
// fetch_add is sequenced AFTER the targetFn release-store, so the detour's
// acquire-load of the count also acquires the new slot. Matches the
// g_interceptorActive shape; see its comment block for the full rationale.
std::atomic<int> g_postObserverActive{0};
std::atomic<int> g_preObserverActive{0};

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

// Fire all observers in `table` whose target matches `function`. Each call
// runs through SafeCallObserver (SEH wrapper) so an AV in any single
// callback is logged + absorbed instead of taking down the engine.
//
// D4-2 count-bounded walk: the hot path here used to be 128 atomic acquire-
// loads per dispatch (the worst-case kMaxObservers walk) on top of every
// other ProcessEvent observer pass. With the per-table active count loaded
// ONCE up front and `found == active` early-termination, an N-registered
// table pays N + holes loads instead of kMaxObservers. Common steady-state
// case ~30 observers -> 30 loads instead of 128 (or 0 loads when the table
// is empty, e.g. for the pre-observer table during early boot).
inline void FireObservers(const ObserverSlot table[], const std::atomic<int>& activeCounter,
                          const std::atomic<uint64_t>* bloom,
                          void* self, void* function, void* params,
                          const char* phase) {
    const int active = activeCounter.load(std::memory_order_acquire);
    if (active <= 0) return;
    if (!BloomMaybe(bloom, function)) return;  // O(1) reject: no observer targets `function`
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

// Lock-free emptiness probe (perf, 2026-06-04). The detour ran on EVERY game-
// thread ProcessEvent dispatch (~85k/sec, measured) and took g_queueMutex JUST to
// test g_queue.empty() -- a barriered LOCK-prefixed RMW on the hottest path in the
// program, for a queue that is empty ~99.9% of the time (Post runs ~60-125x/sec).
// g_queueDepth mirrors g_queue.size(), maintained under g_queueMutex by Post()/
// Pump(); the detour reads it WITHOUT the lock and only locks+drains when it is
// non-zero. A just-Posted task whose increment isn't yet visible to the detour's
// relaxed-acquire read is drained on the next dispatch microseconds later (tasks
// are not sub-millisecond-latency-critical). Writers stay under the mutex so depth
// and the deque never diverge.
std::atomic<int> g_queueDepth{0};

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
            g_queueDepth.store(static_cast<int>(g_queue.size()), std::memory_order_release);
        }
        // The mod is compiled /EHa (CMakeLists.txt) -- so a STRUCTURED
        // exception (AV, div-by-zero) raised inside task() is both caught by
        // catch(...) below AND unwinds C++ destructors on the way out, so any
        // std::lock_guard / RAII the task held is released. This is
        // load-bearing: posted tasks run gameplay/reflection work that can AV
        // on a stale engine pointer (e.g. the connect-edge snapshot
        // enumeration reading a GC'd actor). Before /EHa that AV propagated
        // to the outer SEH __except WITHOUT running destructors, leaking the
        // element Registry mutex locked + the t_inPump flag set -> permanent
        // game-thread freeze (diagnosed 2026-05-30, Tier 8 4-peer smoke).
        // catch(const std::exception&) still names genuine C++ throws via
        // what(); catch(...) covers the rest (other C++ throws + absorbed
        // structured exceptions). The pump LOOP CONTINUES either way, so one
        // faulting task never stops the others or wedges the tick.
        // RunTaskSEH wraps task() in an SEH frame whose filter captures the
        // faulting IP + AV access address BEFORE the unwind, so an absorbed fault
        // names its own site instead of logging the old generic message. Under
        // /EHa the __except unwind still runs the task's C++ destructors, so the
        // load-bearing lock-release property is unchanged. The pump LOOP CONTINUES
        // either way, so one faulting task never stops the others or wedges the tick.
        t_lastTaskFault = {};
        if (RunTaskSEH(task) != 0) {
            constexpr unsigned long kCppExceptionCode = 0xE06D7363u;  // MSVC C++ throw
            if (t_lastTaskFault.code == kCppExceptionCode) {
                UE_LOGE("game_thread: posted task threw a C++ exception at ip=%p [%s]; "
                        "skipped, pump continues",
                        t_lastTaskFault.faultingIP,
                        FormatModuleRva(t_lastTaskFault.faultingIP));
            } else {
                UE_LOGE("game_thread: posted task FAULT code=0x%08lX ip=%p [%s] access=%p; "
                        "skipped, pump continues",
                        t_lastTaskFault.code, t_lastTaskFault.faultingIP,
                        FormatModuleRva(t_lastTaskFault.faultingIP), t_lastTaskFault.accessAddr);
            }
        }
        g_tasksRun.fetch_add(1, std::memory_order_relaxed);
    }
}

// Inner detour body. Contains all the C++ destructor unwinds (lock_guard,
// std::wstring, etc.) -- MSVC disallows mixing SEH __try/__except with C++
// unwind in the same function. Called via SEH-only outer ProcessEventDetour
// below so any AV anywhere in the detour body (observer callbacks, Pump'd
// tasks, FireNameDiagnostics, ToString allocations, etc.) is caught + logged
// instead of crashing the engine.
void __fastcall ProcessEventDetourImpl(void* self, void* function, void* params) {
    // Record the game thread id the first time we run here. CAS so that if two
    // threads race the very first dispatch, exactly one wins (a plain load+store
    // could let a worker thread overwrite the real game thread id).
    if (g_gameThreadId.load(std::memory_order_relaxed) == 0) {
        unsigned long expected = 0;
        g_gameThreadId.compare_exchange_strong(expected, ::GetCurrentThreadId(),
                                               std::memory_order_relaxed, std::memory_order_relaxed);
    }

    // Perf probe (MEASURE-first; off in shipping -> one relaxed bool load here).
    // ord drives the 1/256 self-time sampling. t0 is captured BEFORE the queue-
    // empty mutex check so the per-dispatch mutex cost is INCLUDED in the sample;
    // a dispatch that actually drains the pump drops its sample (net_pump::Tick
    // runs inside Pump() and would dwarf the ~150 ns we are trying to measure).
    const bool countOn = g_peCountOn.load(std::memory_order_relaxed);
    unsigned long long ord = 0;
    if (countOn) {
        ord = g_peDispatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (::GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed))
            g_peDispatchCountGT.fetch_add(1, std::memory_order_relaxed);
    }
    bool sampleSelf = countOn && g_peSelfOn.load(std::memory_order_relaxed) &&
                      ((ord & kSelfSampleMask) == 0);
    LARGE_INTEGER t0{}, t1{}, t2{}, t3{};
    if (sampleSelf) ::QueryPerformanceCounter(&t0);

    // ProcessEvent is also called from task-graph WORKER threads (parallel anim,
    // etc.), not just the game thread. Posted tasks call engine UFunctions, which
    // are game-thread-only -- running them on a worker thread corrupts engine state
    // and crashes (seen as an AV on TaskGraphThreadHP). So drain the queue ONLY on
    // the recorded game thread (the first ProcessEvent caller, validated by the
    // self-test). Other threads just forward.
    // Lock-free emptiness probe FIRST (perf): the depth load + the t_inPump check
    // reject the empty common case without the per-dispatch mutex OR the TEB read.
    // Only when there is queued work do we confirm the game thread and drain.
    if (!t_inPump && g_queueDepth.load(std::memory_order_acquire) != 0 &&
        ::GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed)) {
        // RAII so t_inPump is cleared on EVERY exit from Pump(), incl. an
        // exception path. Under /EHa the dtor runs during a structured-
        // exception unwind too -- so even if an AV ever escaped Pump's
        // own catch (e.g. faulting in the queue lock itself), t_inPump
        // cannot get stuck `true` and silently kill all future draining.
        // The raw `t_inPump = false` it replaces was skipped on exactly
        // that path -> the 2026-05-30 permanent host freeze.
        struct InPumpGuard { ~InPumpGuard() { t_inPump = false; } } pumpGuard;
        t_inPump = true;
        Pump();
        sampleSelf = false;  // pump drain time is not per-dispatch detour overhead
    }

    // UFunction interceptors: pre-dispatch hooks on a multi-slot table. If
    // any interceptor for `function` returns true, the original ProcessEvent
    // is SKIPPED -- the UFunction's body is replaced for this call. Cost is
    // an O(1) Bloom rejection for non-intercepted functions; on a Bloom hit
    // the walk is count-bounded by g_interceptorActive (D4-2), with the cb
    // load only happening on a target match.
    if (FireInterceptors(self, function, params)) return;  // intercepted -> drops the sample (rare)

    // PRE-observers: fire BEFORE the original. Used to snapshot state the BP
    // is about to clear (e.g. PHC.ReleaseComponent PRE reads handle+176
    // GrabbedComponent before PhysX clears it).
    // D4-2: count-bounded walk -- pays N atomic loads where N is the active
    // observer count (typically <= 30) instead of kMaxObservers=128 per
    // dispatch. Empty-table case exits with a single acquire load.
    FireObservers(g_preObservers, g_preObserverActive, g_preBloom, self, function, params, "PRE");

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

    if (sampleSelf) ::QueryPerformanceCounter(&t1);
    g_originalPE(self, function, params);
    if (sampleSelf) ::QueryPerformanceCounter(&t2);

    // POST-observers: fire AFTER the original. Used to read state the BP just
    // wrote (e.g. PHC.GrabComponentAtLocation POST reads handle+176 to see
    // what was just grabbed; PHC.SetTargetLocation POST sees the per-tick
    // drive target). Count-bounded same as PRE.
    FireObservers(g_postObservers, g_postObserverActive, g_postBloom, self, function, params, "POST");

    if (sampleSelf) {
        ::QueryPerformanceCounter(&t3);
        // OUR overhead = pre-original segment (incl. the empty-check mutex + the
        // interceptor/PRE walks) + post-original segment (the POST walk). The
        // engine's own ProcessEvent (t1..t2) is EXCLUDED.
        const long long ours = (t1.QuadPart - t0.QuadPart) + (t3.QuadPart - t2.QuadPart);
        if (ours > 0) {
            g_peSelfNs.fetch_add(QpcDeltaToNs(ours), std::memory_order_relaxed);
            g_peSelfSamples.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// SEH-only outer detour. No C++ destructors here so __try/__except is
// legal. Catches ANY AV / illegal instruction / int divide / etc. that
// propagates out of ProcessEventDetourImpl -- including AVs in posted
// task lambdas drained by Pump(), in FireNameDiagnostics's ToString
// allocation, in the call-trace log path, in observer callbacks (the
// inner SafeCallObserver/SafeCallInterceptor wrappers already catch
// these, but if a future code path bypasses them this outer catch is
// the backstop), or in the original ProcessEvent's BP-VM dispatch when
// BP code derefs a stale UObject*.
//
// On catch we log "PE detour AV caught" and return normally; the engine
// continues. The function name + dispatched self are logged so the next
// run pinpoints which UFunction's call chain crashed. KEEP this outer
// SEH frame -- it is the load-bearing crash firewall for all of
// ProcessEventDetour's downstream paths.
int RunDetourSEH(void* self, void* function, void* params) {
    __try {
        ProcessEventDetourImpl(self, function, params);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

void __fastcall ProcessEventDetour(void* self, void* function, void* params) {
    // Transparent bypass (local-death flee to menu): forward straight to the engine
    // and skip ALL our logic (observers, interceptors, pump, diagnostics, the SEH
    // wrapper) so VOTV's world teardown + menu travel runs exactly as it would with
    // no DLL present. Auto-expires when the deadline passes -> normal detour resumes.
    const long long until = g_bypassUntilMs.load(std::memory_order_relaxed);
    if (until != 0) {
        if (NowMs() < until) {
            if (g_originalPE) g_originalPE(self, function, params);
            return;
        }
        g_bypassUntilMs.store(0, std::memory_order_relaxed);  // expired -> resume normal detour
    }
    if (RunDetourSEH(self, function, params) != 0) {
        // The Impl crashed somewhere -- recover by logging + returning
        // without forwarding to the original PE (the engine's caller frame
        // expects PE to return; we honor that contract). LogObserverAv
        // already resolves the function name + logs at ERROR level.
        LogObserverAv(function, self, "detour-outer");
    }
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

void SetTransparentBypass(int ms) {
    g_bypassUntilMs.store(ms > 0 ? NowMs() + ms : 0, std::memory_order_relaxed);
    UE_LOGW("game_thread: transparent bypass %s (ms=%d) -- detour forwards straight to "
            "the engine (world-teardown flee)", ms > 0 ? "ARMED" : "cleared", ms);
}

void Post(Task task) {
    if (!task) return;
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queue.push_back(std::move(task));
    g_queueDepth.store(static_cast<int>(g_queue.size()), std::memory_order_release);
}

bool IsGameThread() {
    const unsigned long gt = g_gameThreadId.load(std::memory_order_relaxed);
    return gt != 0 && gt == ::GetCurrentThreadId();
}

bool IsDefinitelyOffGameThread() {
    // gt==0 means "not known yet" -- we cannot prove anything, so report false
    // (the guard must not fire at boot before the first ProcessEvent dispatch
    // records the game thread id). Once known, fire only on a real mismatch.
    const unsigned long gt = g_gameThreadId.load(std::memory_order_relaxed);
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
    return SetObserverSlot(g_postObservers, g_postObserverActive, g_postBloom, targetUFunction, cb);
}

bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb) {
    return SetObserverSlot(g_preObservers, g_preObserverActive, g_preBloom, targetUFunction, cb);
}

void UnregisterObservers(void* targetUFunction, ProcessEventObserverFn cb) {
    if (!targetUFunction || !cb) return;
    // cb-specific: clears only THIS observer's slot, leaving any co-registered
    // observer on the same UFunction intact. A given cb lives in exactly one of
    // the two tables, so probing both is harmless.
    ClearObserverSlot(g_postObservers, g_postObserverActive, targetUFunction, cb);
    ClearObserverSlot(g_preObservers, g_preObserverActive, targetUFunction, cb);
}

void ClearAllObservers() {
    for (int i = 0; i < kMaxObservers; ++i) {
        g_postObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_postObservers[i].cb.store(nullptr, std::memory_order_relaxed);
        g_preObservers[i].targetFn.store(nullptr, std::memory_order_release);
        g_preObservers[i].cb.store(nullptr, std::memory_order_relaxed);
    }
    g_postObserverActive.store(0, std::memory_order_release);
    g_preObserverActive.store(0, std::memory_order_release);
    BloomClear(g_postBloom);
    BloomClear(g_preBloom);
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

void SetPerfCounting(bool countDispatches, bool sampleSelfTime) {
    // Arm self-timing first so that the first counted dispatch can already sample.
    g_peSelfOn.store(countDispatches && sampleSelfTime, std::memory_order_relaxed);
    g_peCountOn.store(countDispatches, std::memory_order_relaxed);
}

unsigned long long PeDispatchCountTotal()   { return g_peDispatchCount.load(std::memory_order_relaxed); }
unsigned long long PeDispatchCountGTTotal() { return g_peDispatchCountGT.load(std::memory_order_relaxed); }
unsigned long long PeSelfNsTotal()          { return g_peSelfNs.load(std::memory_order_relaxed); }
unsigned long long PeSelfSampleTotal()      { return g_peSelfSamples.load(std::memory_order_relaxed); }
unsigned long long PeObserverBodyNsTotal()  { return g_obsBodyNs.load(std::memory_order_relaxed); }
unsigned long long PeObserverWorstNs()      { return g_obsWorstNs.load(std::memory_order_relaxed); }
void*              PeObserverWorstFn()      { return g_obsWorstFn.load(std::memory_order_relaxed); }
int PostObserverCount() { return g_postObserverActive.load(std::memory_order_relaxed); }
int PreObserverCount()  { return g_preObserverActive.load(std::memory_order_relaxed); }
int InterceptorCount()  { return g_interceptorActive.load(std::memory_order_relaxed); }

}  // namespace ue_wrap::game_thread
