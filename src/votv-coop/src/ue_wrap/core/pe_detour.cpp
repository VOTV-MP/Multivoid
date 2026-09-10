// ue_wrap/core/pe_detour.cpp -- how the mod sits on ProcessEvent: the MinHook install and disable,
// the detour body, the transparent bypass, the SEH crash firewalls with fault localisation,
// the re-entrancy depth probe and the perf self-timing. What runs on a dispatch (the observer,
// interceptor and name-diagnostic registries, the posted-task pump) is game_thread.cpp's; the
// private seam is game_thread_detail.h, whose hot-path rejects stay inline there.

#include "ue_wrap/core/game_thread.h"

#include "game_thread_detail.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/hook_drill.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/pe_diag.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ue_wrap::game_thread {
namespace {

namespace D = detail;

// ProcessEvent's signature (x64), as reflection's ProcessEventFn.
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

ProcessEventFn g_peTrampoline = nullptr;  // trampoline to the real ProcessEvent
void* g_hookTarget = nullptr;
bool g_installed = false;

// The transparent-bypass deadline (steady_clock ms; 0 = off): while it holds, the detour
// forwards straight to the engine, skipping interceptors, observers, the pump, the diagnostics
// and the outer SEH frame. Armed for a world teardown: the transition to the menu fires EndPlay
// through the detour for every dying actor of a 50k-object world, and our observers plus an SEH
// frame that catches without forwarding left half-run EndPlays and hung the swap. It expires on
// its own so the fresh menu world runs with the layer normal again.
std::atomic<long long> g_bypassUntilMs{0};
// The bypass's release condition: when set, the detour clears the bypass the instant this
// UFunction dispatches (the menu's ui_menu_C::Tick) and resumes on that very call; the
// deadline is then a ceiling. Written at arm time, read in the detour.
std::atomic<void*> g_bypassResumeFn{nullptr};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The perf instrumentation (coop/dev/perf_probe diffs the totals per second). g_peCountOn gates
// the per-dispatch counter: off (shipping) the detour pays one relaxed bool load, on it adds a
// relaxed increment. g_peSelfOn also arms the sampled self-timer (one dispatch in
// kSelfSampleMask + 1) that brackets the detour body excluding the engine's ProcessEvent, our
// overhead only.
std::atomic<bool> g_peCountOn{false};
std::atomic<bool> g_peSelfOn{false};
std::atomic<unsigned long long> g_peDispatchCount{0};    // all threads
std::atomic<unsigned long long> g_peDispatchCountGT{0};  // the game-thread subset, where the per-dispatch substrate cost applies
// Dispatches that originated in our code (reflection::CallFunction sets a thread-local depth):
// the game thread runs about 920 dispatches a frame, each executing real Blueprint, so "is the
// game thread busy because of us" is the share of that volume we author; the detour's own
// overhead is small and the Blueprint those calls run is invisible to every bucket here. Counted
// inside Impl, under the SEH firewall: an instrument in the unprotected outer frame once
// hard-crashed the game.
std::atomic<unsigned long long> g_peDispatchCountCoop{0};
std::atomic<unsigned long long> g_peSelfNs{0};
std::atomic<unsigned long long> g_peSelfSamples{0};
constexpr unsigned long long kSelfSampleMask = 0xFF;  // sample 1 dispatch in 256

// Whole-detour timing. The self-timer brackets from inside Impl, so the outer frame, the SEH
// frame and the calls between them are excluded by construction, which is where an unaccounted
// per-dispatch cost would hide (a measured 5 ms/frame gap once summed to under 1 ms in every
// bucket: about 2 us per dispatch at ~2,200 dispatches a frame). These bracket the outer
// function and record the engine's own ProcessEvent on the same sampled dispatches, so whole
// minus engine is the true per-dispatch cost. Sampled only at top level: a nested dispatch's
// time is already inside its parent's bracket.
std::atomic<unsigned long long> g_peWholeNs{0};      // outer detour wall time, sampled
std::atomic<unsigned long long> g_peEngineNs{0};     // engine PE within those same samples
std::atomic<unsigned long long> g_peWholeSamples{0};
std::atomic<unsigned long long> g_peWholeOrd{0};     // drives the 1/256 pick (armed only)
thread_local bool t_sampleWhole = false;             // set by the outer at depth 0
thread_local unsigned long long t_engineNs = 0;      // filled by Impl for that dispatch

// Callback-body timing: a callback that secretly calls an uncached reflection Find* (a walk of
// the GUObjectArray with a wstring per entry) on a hot UFunction costs more than any table
// walk. Each body is bracketed with QPC when counting is armed; the running total and the
// single worst call (with its UFunction, resolved to a name by perf_probe) are kept.
std::atomic<unsigned long long> g_obsBodyNs{0};       // summed cb-body time across all fired observers+interceptors
std::atomic<unsigned long long> g_obsWorstNs{0};      // worst single cb-body call seen (ns)
std::atomic<void*>              g_obsWorstFn{nullptr}; // the UFunction* of that worst call

// QPC ticks per second, cached on first use; 0 until resolved.
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
// One callback-body duration, into the total and the worst; only on a matched dispatch.
inline void RecordCbBodyNs(void* function, unsigned long long ns) {
    g_obsBodyNs.fetch_add(ns, std::memory_order_relaxed);
    if (ns > g_obsWorstNs.load(std::memory_order_relaxed)) {
        g_obsWorstNs.store(ns, std::memory_order_relaxed);
        g_obsWorstFn.store(function, std::memory_order_relaxed);
    }
}

// Fault localisation for the firewalls: an absorbed fault logs its faulting instruction, the access
// address and the containing module and RVA, so it names its own site. A payload-DLL RVA maps to a
// function through the payload's .map; a game-exe hit is a fault inside a ProcessEvent-dispatched
// UFunction on a bad object.
thread_local D::TaskFaultInfo t_lastTaskFault{};

// The SEH filter runs in the faulting context before the unwind, so it only stashes, never
// allocates.
int TaskFaultFilter(EXCEPTION_POINTERS* ep) {
    // A stack overflow is not absorbable: once the guard page has fired it is gone for the thread,
    // and absorbing runs the rest of the frame on an exhausted stack with half-unwound engine state
    // until an unrelated-looking secondary AV kills the process with a useless dump. Passed on, the
    // OS takes it at the true apex, where the minidump names the whole recursive cascade.
    if (ep->ExceptionRecord->ExceptionCode == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW))
        return EXCEPTION_CONTINUE_SEARCH;
    t_lastTaskFault.code       = ep->ExceptionRecord->ExceptionCode;
    t_lastTaskFault.faultingIP = ep->ExceptionRecord->ExceptionAddress;
    t_lastTaskFault.accessAddr =
        (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
         ep->ExceptionRecord->NumberParameters >= 2)
            ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
            : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

// The SEH-wrapped single-callback dispatch. MSVC forbids C++ unwinds (a wstring destructor) in
// a function with __try, so the __try wrapper does only the raw call and a crashed-or-not
// result; the logging lives in a C++ function. The absorbed-AV line names the callback's
// UFunction and the fault site (module and RVA): a site inside the game's own executable is
// the engine's dispatch dereferencing a stale object, not our callback.
//
// The rate latch on identical absorbs: one fault storm once made a 12 MB log of which 78% was
// a single line repeated 33,490 times at ~2,500 a second, burying every other line and turning
// the log into a per-second disk write on the game thread during the very window the game was
// choking. The first kLogFirstN of any distinct (function, ip) log in full, so which
// UFunction, which instruction and which address are never lost; the repeats fold into a
// count that is itself reported. Log policy only: absorbing and forwarding are unchanged.
// Keyed on (function, ip), not on `self`: that storm had a constant ip and a varying self.
constexpr int kLogFirstN   = 5;    // full lines per distinct site before folding
constexpr int kAvSiteSlots = 16;   // distinct sites tracked; LRU-free, oldest wins
struct AvSite { void* fn; void* ip; unsigned long long count; unsigned long long lastReportedAt; };
AvSite g_avSites[kAvSiteSlots]{};
int    g_avSiteNext = 0;

void LogObserverAv(void* function, void* self, const char* phase) {
    // Find or claim a slot; linear over 16, on a fault only.
    AvSite* site = nullptr;
    for (auto& s : g_avSites) {
        if (s.fn == function && s.ip == t_lastTaskFault.faultingIP) { site = &s; break; }
    }
    if (!site) {
        site = &g_avSites[g_avSiteNext];
        g_avSiteNext = (g_avSiteNext + 1) % kAvSiteSlots;
        *site = AvSite{function, t_lastTaskFault.faultingIP, 0, 0};
    }
    ++site->count;

    // Past the first N, a line only on a decade boundary, so a storm's shape (how fast, how far)
    // reaches the log at logarithmic cost.
    if (site->count > kLogFirstN) {
        unsigned long long decade = 10;
        while (decade < site->count) decade *= 10;
        if (site->count != decade || site->count == site->lastReportedAt) return;
        site->lastReportedAt = site->count;
        UE_LOGE("game_thread: PE %s-callback AV at ip=%s x%llu (identical (function,ip) "
                "repeats folded after the first %d; latest self=%p)",
                phase, D::FormatModuleRva(t_lastTaskFault.faultingIP), site->count,
                kLogFirstN, self);
        return;
    }

    const auto fname = reflection::NameOf(function);
    const std::wstring nameStr = reflection::ToString(fname);
    UE_LOGE("game_thread: PE %s-callback AV caught -- function='%ls' (%p) self=%p; "
            "fault code=0x%08lX ip=%s access=%p; absorbing, process continues",
            phase, nameStr.c_str(), function, self,
            t_lastTaskFault.code, D::FormatModuleRva(t_lastTaskFault.faultingIP),
            t_lastTaskFault.accessAddr);
}

// 0 on clean completion, 1 if SEH caught an exception; the callback's own bool comes back
// through outIntercept. __try / __except is the only thing in this function.
int RunInterceptorSEH(UFunctionInterceptor cb, void* self, void* params, bool* outIntercept) {
    __try {
        *outIntercept = cb(self, params);
        return 0;
    } __except (TaskFaultFilter(GetExceptionInformation())) {
        return 1;
    }
}

int RunObserverSEH(ProcessEventObserverFn cb, void* self, void* function, void* params) {
    __try {
        cb(self, function, params);
        return 0;
    } __except (TaskFaultFilter(GetExceptionInformation())) {
        return 1;
    }
}

// The re-entrancy depth probe. A host once died on a script-VM stack overflow, a Blueprint
// destroy cascade dispatching ReceiveDestroyed inside ReceiveDestroyed until the per-level
// alloca exhausted the stack, and the log named nothing about the chain. A thread-local depth,
// incremented and decremented per dispatch, logs the function and self class at each doubling
// from 128 (in a tight cascade those are the cycle members), so the next runaway names itself
// before the stack dies and the WER dump (the overflow now passes through) gets a named lead-in.
constexpr int kPeDepthWarnStart = 128;  // engine-normal nesting is O(10); 128 = pathological
thread_local int t_peDepth = 0;
thread_local int t_peDepthNextWarn = kPeDepthWarnStart;

// The scope is trivial (an int, cannot fault) and the logging is a separate function called
// after construction: if the warn path ever faults (absorbed by RunDetourSEH), the /EHa unwind
// still runs the destructor and the depth cannot drift upward.
struct PeDepthScope {
    PeDepthScope() { ++t_peDepth; }
    ~PeDepthScope() {
        if (--t_peDepth == 0) t_peDepthNextWarn = kPeDepthWarnStart;  // episode over -> re-arm
    }
};

void MaybeWarnPeDepth(void* self, void* function) {
    if (t_peDepth < t_peDepthNextWarn) return;
    t_peDepthNextWarn *= 2;  // raised BEFORE the (allocating) log -- a fault here cannot warn-loop
    const std::wstring fn = function ? reflection::ToString(reflection::NameOf(function)) : L"<null>";
    void* cls = self ? reflection::ClassOf(self) : nullptr;
    const std::wstring cn = cls ? reflection::ToString(reflection::NameOf(cls)) : L"<null>";
    UE_LOGW("game_thread: PE recursion depth=%d -- function='%ls' self=%p class='%ls' "
            "(a dispatch cascade this deep precedes a script-VM stack overflow; the "
            "repeating function/class here names the cycle)",
            t_peDepth, fn.c_str(), self, cn.c_str());
}

// The inner detour body, with every C++ unwind (lock guards, wstrings); the SEH-only outer
// frame below catches any fault in it (callbacks, pumped tasks, the name diagnostics, the
// ToString allocations) and logs it instead of crashing the engine.
void __fastcall ProcessEventDetourImpl(void* self, void* function, void* params) {
    const PeDepthScope depthScope;      // trivial ++ (constructed BEFORE the fallible warn)
    MaybeWarnPeDepth(self, function);
    // The game thread id, recorded on the first dispatch by CAS so a racing worker thread cannot
    // overwrite it.
    if (D::g_gameThreadId.load(std::memory_order_relaxed) == 0) {
        unsigned long expected = 0;
        D::g_gameThreadId.compare_exchange_strong(expected, ::GetCurrentThreadId(),
                                                  std::memory_order_relaxed, std::memory_order_relaxed);
    }

    // The perf probe: one relaxed bool load when off. ord drives the 1-in-256 self sample; t0 is
    // taken before the queue-empty check so the per-dispatch mutex cost is inside the sample, and a
    // dispatch that drains the pump drops its sample (net_pump::Tick runs inside the pump and would
    // dwarf the ~150 ns being measured).
    const bool countOn = g_peCountOn.load(std::memory_order_relaxed);
    unsigned long long ord = 0;
    if (countOn) {
        ord = g_peDispatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (::GetCurrentThreadId() == D::g_gameThreadId.load(std::memory_order_relaxed))
            g_peDispatchCountGT.fetch_add(1, std::memory_order_relaxed);
        if (reflection::InCoopDispatch())
            g_peDispatchCountCoop.fetch_add(1, std::memory_order_relaxed);
    }
    bool sampleSelf = countOn && g_peSelfOn.load(std::memory_order_relaxed) &&
                      ((ord & kSelfSampleMask) == 0);
    LARGE_INTEGER t0{}, t1{}, t2{}, t3{};
    if (sampleSelf) ::QueryPerformanceCounter(&t0);

    // ProcessEvent is also called from task-graph worker threads (parallel animation), and a posted
    // task calls game-thread-only UFunctions, so the queue drains only on the recorded game thread;
    // other threads forward. The lock-free emptiness probe first: the depth load and the in-pump
    // check reject the empty common case without the mutex or the TEB read, and only queued work
    // confirms the game thread and drains (which also holds the spawn-refusal deferral gate).
    if (!D::t_inPump && D::g_queueDepth.load(std::memory_order_acquire) != 0 &&
        ::GetCurrentThreadId() == D::g_gameThreadId.load(std::memory_order_relaxed)) {
        if (D::DrainPostedTasksAtTopLevel())
            sampleSelf = false;  // pump drain time is not per-dispatch detour overhead
    }

    // Interceptors: pre-dispatch hooks; an interceptor returning true replaces the UFunction's body
    // for this call. An O(1) Bloom rejection for a non-intercepted function; on a hit the walk is
    // bounded by the active count, and the callback loads only on a target match.
    if (D::FireInterceptors(self, function, params)) return;  // intercepted; the sample is dropped

    // PRE observers fire before the original, to snapshot state the Blueprint is about to clear
    // (the grab handle's GrabbedComponent before PhysX clears it). The walk is bounded by the
    // active observer count; an empty table exits on one acquire load.
    D::FirePreObservers(self, function, params);

    // The engine's ProcessEvent is bracketed when either timer wants it: the self sample subtracts
    // it, the whole-detour timer turns a wall-clock outer measurement into our share. Recorded at
    // depth 1 only: a nested dispatch runs inside this bracket and would replace the parent's
    // engine time with a fragment of itself.
    const bool timeEngine = sampleSelf || t_sampleWhole;
    if (timeEngine) ::QueryPerformanceCounter(&t1);
    g_peTrampoline(self, function, params);
    if (timeEngine) ::QueryPerformanceCounter(&t2);
    if (t_sampleWhole && t_peDepth == 1)
        t_engineNs = QpcDeltaToNs(t2.QuadPart - t1.QuadPart);

    // POST observers fire after the original, to read what the Blueprint just wrote (the grab
    // handle's target after SetTargetLocation). Bounded like PRE.
    D::FirePostObservers(self, function, params);

    if (sampleSelf) {
        ::QueryPerformanceCounter(&t3);
        // Our overhead is the segment before the original (the empty check, the interceptor and PRE
        // walks) plus the one after (the POST walk); the engine's ProcessEvent is excluded.
        const long long ours = (t1.QuadPart - t0.QuadPart) + (t3.QuadPart - t2.QuadPart);
        if (ours > 0) {
            g_peSelfNs.fetch_add(QpcDeltaToNs(ours), std::memory_order_relaxed);
            g_peSelfSamples.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// The SEH-only outer detour: no C++ destructors, so __try is legal. It catches whatever
// propagates out of Impl (a pumped task, the name diagnostics, the trace log, an observer that
// bypassed the inner wrappers, the engine's own dispatch dereferencing a stale object), logs the
// function and self, and returns normally so the engine continues. The load-bearing crash
// firewall for everything downstream.
int RunDetourSEH(void* self, void* function, void* params) {
    __try {
        ProcessEventDetourImpl(self, function, params);
        return 0;
    } __except (TaskFaultFilter(GetExceptionInformation())) {
        return 1;
    }
}

void __fastcall ProcessEventDetour(void* self, void* function, void* params) {
    // The transparent bypass: straight to the engine with all our logic skipped, so a world
    // teardown runs as with no DLL present; it expires on the deadline.
    const long long until = g_bypassUntilMs.load(std::memory_order_relaxed);
    if (until != 0) {
        // The release condition: the moment the resume function dispatches (the menu world is up),
        // the bypass clears and this very call runs the normal detour, so the MULTIPLAYER-injection
        // observer fires on the first menu frame. One pointer compare per dispatch while armed.
        void* resumeFn = g_bypassResumeFn.load(std::memory_order_relaxed);
        if (resumeFn != nullptr && function == resumeFn) {
            g_bypassUntilMs.store(0, std::memory_order_relaxed);
            g_bypassResumeFn.store(nullptr, std::memory_order_relaxed);
            UE_LOGW("game_thread: transparent bypass RESUMED on its release function "
                    "(menu world up) -- detour normal again");
            // Then the normal detour.
        } else if (NowMs() < until) {
            if (g_peTrampoline) g_peTrampoline(self, function, params);
            return;
        } else {
            g_bypassUntilMs.store(0, std::memory_order_relaxed);   // ceiling hit -> resume
            g_bypassResumeFn.store(nullptr, std::memory_order_relaxed);
        }
    }
    // The whole-detour sample decision, outside the SEH frame, since everything from here to
    // RunDetourSEH's return is what the inner timer cannot see. Top level only: t_peDepth is still
    // 0 before Impl's scope runs. A nested call leaves the sample state alone, or it would clobber
    // the parent's in-flight sample.
    const bool topLevel = (t_peDepth == 0);
    bool whole = false;
    if (topLevel && g_peSelfOn.load(std::memory_order_relaxed)) {
        whole = (g_peWholeOrd.fetch_add(1, std::memory_order_relaxed) & kSelfSampleMask) == 0;
        t_sampleWhole = whole;
        t_engineNs = 0;
    } else if (topLevel) {
        t_sampleWhole = false;
    }
    LARGE_INTEGER w0{}, w1{};
    if (whole) ::QueryPerformanceCounter(&w0);

    if (RunDetourSEH(self, function, params) != 0) {
        // Impl crashed: logged, and the call returns without forwarding, since the engine's caller
        // expects ProcessEvent to return.
        LogObserverAv(function, self, "detour-outer");
    }

    if (whole) {
        ::QueryPerformanceCounter(&w1);
        const long long span = w1.QuadPart - w0.QuadPart;
        // A crashed Impl leaves the engine time at 0, which would report the whole span as ours;
        // those samples are dropped.
        if (span > 0 && t_engineNs > 0) {
            g_peWholeNs.fetch_add(QpcDeltaToNs(span), std::memory_order_relaxed);
            g_peEngineNs.fetch_add(t_engineNs, std::memory_order_relaxed);
            g_peWholeSamples.fetch_add(1, std::memory_order_relaxed);
        }
        t_sampleWhole = false;
    }
}

}  // namespace

// The detail services this TU provides to game_thread.cpp.

namespace detail {

TaskFaultInfo& LastTaskFault() { return t_lastTaskFault; }

// SEH only, no C++ destructors in this frame (`task` is a reference). Under /EHa the __except
// unwind still runs the task's own destructors, which is why the image is built /EHa: the lock
// release the pump relies on survives. Catches structured exceptions and C++ throws (code
// 0xE06D7363). 0 clean, 1 caught.
int RunTaskSEH(const Task& task) {
    __try {
        task();
        return 0;
    } __except (TaskFaultFilter(GetExceptionInformation())) {
        return 1;
    }
}

// A faulting IP as "module+0xRVA": C++ (Win32 plus a thread-local buffer), called only from C++
// bodies, never from the SEH-only frames. The RVA is ASLR-independent, so it maps against the
// payload .map directly.
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

}  // namespace detail

// The public API this TU owns.

// The drill for the absorb rate latch (VOTVCOOP_AV_LATCH_DRILL=1): a latch only ever observed
// not firing is indistinguishable from one wired wrong, and the storm it exists for is not
// reproducible on demand, so synthetic sites drive the real LogObserverAv (the fault path is
// untouched). Expected: site A prints 5 full lines then folds at 10 and 100 (7 lines for 120
// calls); site B prints its own 5, since a new site is never suppressed by an old one's volume.
void RunAvLatchDrill() {
    char v[8]{};
    if (!(::GetEnvironmentVariableA("VOTVCOOP_AV_LATCH_DRILL", v, sizeof(v)) > 0 && v[0] == '1'))
        return;
    // A real UFunction: the full-line path dereferences it through NameOf, and a null once killed
    // the process at boot.
    void* fn = nullptr;
    if (void* cls = reflection::FindClass(L"Actor"))
        fn = reflection::FindFunction(cls, L"K2_DestroyActor");
    if (!fn) {
        UE_LOGW("av_latch_drill: SKIPPED -- no Actor::K2_DestroyActor to name (too early?)");
        return;
    }
    UE_LOGW("av_latch_drill: BEGIN -- 120 calls at site A, then 3 at site B. "
            "PASS = site A prints 7 ERROR lines (5 full + x10 + x100), site B prints 3.");
    t_lastTaskFault.code = 0xC0000005;
    t_lastTaskFault.accessAddr = reinterpret_cast<void*>(0xFFFFFFFFFFFFFFFFull);
    t_lastTaskFault.faultingIP = reinterpret_cast<void*>(0xA000);
    for (int i = 0; i < 120; ++i) LogObserverAv(fn, reinterpret_cast<void*>(uintptr_t{0x1000} + i), "drillA");
    t_lastTaskFault.faultingIP = reinterpret_cast<void*>(0xB000);
    for (int i = 0; i < 3; ++i) LogObserverAv(fn, reinterpret_cast<void*>(uintptr_t{0x2000} + i), "drillB");
    UE_LOGW("av_latch_drill: END -- count the [ERROR] lines tagged drillA / drillB above.");
}

bool Install() {
    if (g_installed) return true;

    void* pe = reinterpret_cast<void*>(reflection::ProcessEventAddr());
    if (!pe) {
        UE_LOGE("game_thread: ProcessEvent unresolved; resolve reflection first");
        return false;
    }
    if (!hook::Init()) return false;
    // ProcessEvent is the one function UE4SS's PolyHook also detours, so the MinHook relay must be
    // followJmp-immune, or the two detours corrupt each other at boot (a reproducible crash whose
    // dump matched the field cohort byte for byte). Unconditional: the corruptible relay has no
    // caller on this hook.
    if (!hook::Install(pe, reinterpret_cast<void*>(&ProcessEventDetour),
                       reinterpret_cast<void**>(&g_peTrampoline), /*followJmpImmune=*/true)) {
        return false;
    }
    UE_LOGI("game_thread: PE relay followJmp-immune (composes with a co-resident PolyHook PE detour)");
    g_hookTarget = pe;
    g_installed = true;
    UE_LOGI("game_thread: ProcessEvent hooked; game-thread dispatcher live");
    // After the hook is live, never before: run at the top of Install the drill destabilised boot
    // (123 reflection lookups and formatted log writes on the loader thread while the engine is
    // still building its object graph), and a drill that kills the process teaches that the latch
    // is broken when it is not.
    RunAvLatchDrill();
    // The double-detour diagnostic (VOTVCOOP_PE_DIAG=1) lives in pe_diag.cpp and needs both
    // TU-locals, final by this line.
    pe_diag::ArmIfEnabled(reinterpret_cast<void*>(&ProcessEventDetour),
                          reinterpret_cast<void*>(g_peTrampoline));
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    ClearAllObservers();
    detail::ClearAllInterceptors();
    // Disable, never remove (hook.h, Retirement): the patch at ProcessEvent lifts so no new
    // dispatch enters, while the trampoline stays allocated for whoever is already inside.
    hook_drill::SampleTrampoline("pre-disable", 0, reinterpret_cast<void*>(g_peTrampoline));
    hook::Disable(g_hookTarget);
    hook_drill::SampleTrampoline("post-disable", 0, reinterpret_cast<void*>(g_peTrampoline));
    g_installed = false;
    g_hookTarget = nullptr;
    // g_peTrampoline is MinHook's trampoline slot, not the engine's entry point, and MH_RemoveHook
    // frees that slot and writes the free-list link over its first bytes, so a removal clobbered
    // the prologue under an in-flight worker at ~250k dispatches a second, with a window of zero.
    // With Disable the prologue is restored and the trampoline intact. The pointer is deliberately
    // not nulled (a racing load could read the null); the Sleep is a drain before the target goes.
    ::Sleep(50);
}

bool IsInstalled() { return g_installed; }

void SetTransparentBypass(int ms) {
    g_bypassResumeFn.store(nullptr, std::memory_order_relaxed);  // pure timer mode
    g_bypassUntilMs.store(ms > 0 ? NowMs() + ms : 0, std::memory_order_relaxed);
    UE_LOGW("game_thread: transparent bypass %s (ms=%d) -- detour forwards straight to "
            "the engine (world-teardown flee)", ms > 0 ? "ARMED" : "cleared", ms);
}

void SetTransparentBypassUntil(void* resumeOnFunction, int maxMs) {
    // The resume function is armed before the deadline, so the detour never sees a live bypass
    // without its release condition; null falls back to the pure timer.
    g_bypassResumeFn.store(resumeOnFunction, std::memory_order_relaxed);
    g_bypassUntilMs.store(maxMs > 0 ? NowMs() + maxMs : 0, std::memory_order_relaxed);
    UE_LOGW("game_thread: transparent bypass %s (resumeFn=%p, ceiling=%dms) -- detour "
            "forwards straight to the engine until the menu world is up",
            maxMs > 0 ? "ARMED" : "cleared", resumeOnFunction, maxMs);
}

void SetPerfCounting(bool countDispatches, bool sampleSelfTime) {
    // Self-timing first, so the first counted dispatch can sample.
    g_peSelfOn.store(countDispatches && sampleSelfTime, std::memory_order_relaxed);
    g_peCountOn.store(countDispatches, std::memory_order_relaxed);
}

unsigned long long PeDispatchCountTotal()   { return g_peDispatchCount.load(std::memory_order_relaxed); }
unsigned long long PeDispatchCountGTTotal() { return g_peDispatchCountGT.load(std::memory_order_relaxed); }
unsigned long long PeDispatchCountCoopTotal() { return g_peDispatchCountCoop.load(std::memory_order_relaxed); }
unsigned long long PeSelfNsTotal()          { return g_peSelfNs.load(std::memory_order_relaxed); }
unsigned long long PeSelfSampleTotal()      { return g_peSelfSamples.load(std::memory_order_relaxed); }
unsigned long long PeWholeNsTotal()         { return g_peWholeNs.load(std::memory_order_relaxed); }
unsigned long long PeEngineNsTotal()        { return g_peEngineNs.load(std::memory_order_relaxed); }
unsigned long long PeWholeSampleTotal()     { return g_peWholeSamples.load(std::memory_order_relaxed); }
unsigned long long PeTopLevelCountTotal()   { return g_peWholeOrd.load(std::memory_order_relaxed); }

unsigned long long PeObserverBodyNsTotal()  { return g_obsBodyNs.load(std::memory_order_relaxed); }
unsigned long long PeObserverWorstNs()      { return g_obsWorstNs.load(std::memory_order_relaxed); }
void*              PeObserverWorstFn()      { return g_obsWorstFn.load(std::memory_order_relaxed); }

}  // namespace ue_wrap::game_thread
