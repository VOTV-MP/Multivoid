// ue_wrap/pe_detour.cpp -- the ProcessEvent INTERPOSITION MECHANISM.
//
// Extracted 2026-07-04 from game_thread.cpp (1065 LOC, past the 800 soft cap;
// restated by two audits). This TU owns HOW we sit on ProcessEvent: the MinHook
// install/uninstall, the detour body, the transparent bypass, the SEH crash
// firewalls + absorbed-fault localization, the PE re-entrancy depth probe, and
// the perf self-timing instrumentation. WHAT runs on a dispatch -- the
// observer/interceptor/name-diagnostic registries and the posted-task pump --
// lives in game_thread.cpp; the private seam is game_thread_detail.h (hot-path
// fast rejects stay inline there; only matched/non-empty work crosses the TU).

#include "ue_wrap/core/game_thread.h"

#include "game_thread_detail.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/hook_drill.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ue_wrap::game_thread {
namespace {

namespace D = detail;

// ProcessEvent's signature (x64 ABI). Matches reflection's ProcessEventFn.
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

ProcessEventFn g_peTrampoline = nullptr;  // trampoline to the real ProcessEvent
void* g_hookTarget = nullptr;
bool g_installed = false;

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
// Optional condition-based release for the bypass: when set, the detour clears
// the bypass the instant ProcessEvent dispatches THIS UFunction (the menu's
// ui_menu_C::Tick for the death-flee), resuming on that very call. The maxMs
// deadline above is then just a safety ceiling. Lock-free (game-thread written
// at arm time, read in the detour).
std::atomic<void*> g_bypassResumeFn{nullptr};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Perf instrumentation (MEASURE-first 15-FPS audit; see coop/dev/perf_probe) --
// g_peCountOn gates the per-dispatch counter: OFF (default/shipping) the detour
// pays a single relaxed bool load; ON it adds one relaxed XADD per dispatch.
// g_peSelfOn additionally arms the sampled self-timer (1 dispatch in
// kSelfSampleMask+1) that brackets the detour body EXCLUDING g_peTrampoline -- i.e.
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

// ---- Absorbed-fault localization (firewall diagnosability) -----------------
// The Pump() crash firewall absorbs a faulting task so the host survives, but
// historically it logged only a GENERIC "absorbed exception" line -- it did not
// say WHERE the fault was. That blind spot cost real RE time twice (the bug1
// per-tick AV balloon needed convergent-agent RE; bug2 -- the intermittent
// unpossessed-first-client AV flood -- still can't be pinned without it). These
// pieces capture the faulting instruction pointer + the access address + the
// containing module/RVA, so the next absorbed fault names its own site. A
// payload-DLL RVA maps to a function via the payload .map (multivoid-*.map, /MAP) +
// tools/maprva.py; a game-exe hit means the fault is inside a ProcessEvent-
// dispatched UFunction on a bad object.
thread_local D::TaskFaultInfo t_lastTaskFault{};

// SEH filter -- runs in the faulting context (registers still valid) BEFORE the
// unwind, so it must only stash, never allocate. Returns EXCEPTION_EXECUTE_HANDLER.
int TaskFaultFilter(EXCEPTION_POINTERS* ep) {
    // STATUS_STACK_OVERFLOW is NOT absorbable -- pass it on (2026-07-04 17:09 host
    // death). Once the stack guard page has fired it is GONE for this thread;
    // "absorb and continue" runs the rest of the frame on an exhausted stack with
    // half-unwound engine state, and the process dies ~1 s later on an unrelated-
    // looking secondary AV (17:09:46 SO absorbed at ReceiveDestroyed -> 17:09:47
    // WER c0000005 in FindFunctionChecked, dump useless). CONTINUE_SEARCH instead
    // lets the OS/WER take it AT THE TRUE APEX, where the minidump's call stack
    // names the whole recursive BP cascade -- the diagnostic the absorb destroyed.
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
//
// 2026-07-04 (re-host dangling-save diagnosis): the absorbed-AV line now also
// names the fault SITE (module+RVA via TaskFaultFilter, same mechanism as the
// Pump firewall) -- "VotV-Win64-Shipping.exe+..." means the fault is inside
// the engine's own dispatch (e.g. BP-VM deref of a stale UObject*), not in our
// callback. Diagnosing the re-host crash needed a minidump to learn that; now
// the log says it directly.
// RATE LATCH on identical absorbs (USER-APPROVED 2026-08-23, triage R-1e).
//
// WHY: one absorbed-fault storm turned a 6-minute session's log into 12.35 MB of
// which 9.64 MB -- 78% -- was ONE repeated line: 33,490 copies of the same
// (function, fault-ip) pair at ~2,508/s for 44 s. That is not diagnosis, it is
// diagnosis buried in its own repetition, and every OTHER line in the run became
// unreadable. It also made the log a per-second disk-write load on the game thread
// during the exact window the game was already choking.
//
// WHAT IS PRESERVED: the FIRST kLogFirstN of any distinct (function, ip) still log in
// full, so the forensic content -- which UFunction, which faulting instruction, which
// access address -- is never lost. Only the (N+1)th identical repeat is folded, and it
// is folded into a COUNT that is itself reported, so "this happened 33,490 times" is
// still readable. A storm therefore costs a handful of lines plus one summary instead
// of 9.64 MB, and a NEW fault site is never suppressed by an old one's volume.
//
// WHAT IS NOT: this is log policy, not behaviour. Nothing about absorbing, forwarding
// or the caller's view changes. Deliberately keyed on (function, ip) rather than on
// `self`, because the 2026-08-23 storm had a CONSTANT ip and a VARYING self -- keying
// on self would have suppressed nothing.
constexpr int kLogFirstN   = 5;    // full lines per distinct site before folding
constexpr int kAvSiteSlots = 16;   // distinct sites tracked; LRU-free, oldest wins
struct AvSite { void* fn; void* ip; unsigned long long count; unsigned long long lastReportedAt; };
AvSite g_avSites[kAvSiteSlots]{};
int    g_avSiteNext = 0;

void LogObserverAv(void* function, void* self, const char* phase) {
    // Find or claim a slot. Linear over 16 -- this runs only on a fault.
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

    // Fold: past the first N, report only on a decade boundary, so a storm's shape
    // (how fast, how far) still reaches the log at logarithmic cost.
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

// Returns 0 on clean completion, 1 if SEH caught an exception. cb returns
// its own bool via *outIntercept (only meaningful if return value is 0).
// __try / __except is the ONLY thing in this function -- no C++ destructors.
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

// ---- PE re-entrancy depth probe (2026-07-04, the 17:09 host death) ----------
// The host died on a script-VM stack overflow: a BP destroy cascade dispatched
// ReceiveDestroyed nested inside ReceiveDestroyed until ProcessScriptFunction's
// per-level alloca exhausted the stack. The log named NOTHING about the chain
// (only the absorbed-SO line, one frame). This probe measures the recursion
// live ([[feedback-probe-dont-guess-rule]]): a thread_local depth counter,
// ++/-- per dispatch (TEB-relative, ~free); on each doubling threshold crossing
// (128, 256, 512, ...) it logs the function + self class at that depth -- in a
// tight cascade those ARE the cycle members -- so the NEXT runaway names itself
// in the log long before the stack dies, and the WER dump (the SO now passes
// through, see TaskFaultFilter) gets a named lead-in.
constexpr int kPeDepthWarnStart = 128;  // engine-normal nesting is O(10); 128 = pathological
thread_local int t_peDepth = 0;
thread_local int t_peDepthNextWarn = kPeDepthWarnStart;

// The counter scope is TRIVIAL (an int ++/--, cannot fault) and the logging lives in a
// separate function called AFTER construction completes -- if the warn path ever AVs
// (absorbed by RunDetourSEH), the already-constructed scope's destructor still runs on
// the /EHa unwind, so the depth can never drift upward (audit 2026-07-04 finding 5).
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
            "(a dispatch cascade this deep precedes a script-VM stack overflow -- the "
            "2026-07-04 17:09 host death; the repeating function/class here names the cycle)",
            t_peDepth, fn.c_str(), self, cn.c_str());
}
// ---- end PE re-entrancy depth probe -----------------------------------------

// Inner detour body. Contains all the C++ destructor unwinds (lock_guard,
// std::wstring, etc.) -- MSVC disallows mixing SEH __try/__except with C++
// unwind in the same function. Called via SEH-only outer ProcessEventDetour
// below so any AV anywhere in the detour body (observer callbacks, Pump'd
// tasks, FireNameDiagnostics, ToString allocations, etc.) is caught + logged
// instead of crashing the engine.
void __fastcall ProcessEventDetourImpl(void* self, void* function, void* params) {
    const PeDepthScope depthScope;      // trivial ++ (constructed BEFORE the fallible warn)
    MaybeWarnPeDepth(self, function);
    // Record the game thread id the first time we run here. CAS so that if two
    // threads race the very first dispatch, exactly one wins (a plain load+store
    // could let a worker thread overwrite the real game thread id).
    if (D::g_gameThreadId.load(std::memory_order_relaxed) == 0) {
        unsigned long expected = 0;
        D::g_gameThreadId.compare_exchange_strong(expected, ::GetCurrentThreadId(),
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
        if (::GetCurrentThreadId() == D::g_gameThreadId.load(std::memory_order_relaxed))
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
    // Only when there is queued work do we confirm the game thread and drain
    // (DrainPostedTasksAtTopLevel also holds the spawn-refusal deferral gate).
    if (!D::t_inPump && D::g_queueDepth.load(std::memory_order_acquire) != 0 &&
        ::GetCurrentThreadId() == D::g_gameThreadId.load(std::memory_order_relaxed)) {
        if (D::DrainPostedTasksAtTopLevel())
            sampleSelf = false;  // pump drain time is not per-dispatch detour overhead
    }

    // UFunction interceptors: pre-dispatch hooks on a multi-slot table. If
    // any interceptor for `function` returns true, the original ProcessEvent
    // is SKIPPED -- the UFunction's body is replaced for this call. Cost is
    // an O(1) Bloom rejection for non-intercepted functions; on a Bloom hit
    // the walk is count-bounded by g_interceptorActive (D4-2), with the cb
    // load only happening on a target match.
    if (D::FireInterceptors(self, function, params)) return;  // intercepted -> drops the sample (rare)

    // PRE-observers: fire BEFORE the original. Used to snapshot state the BP
    // is about to clear (e.g. PHC.ReleaseComponent PRE reads handle+176
    // GrabbedComponent before PhysX clears it).
    // D4-2: count-bounded walk -- pays N atomic loads where N is the active
    // observer count (typically <= 30) instead of kMaxObservers=128 per
    // dispatch. Empty-table case exits with a single acquire load.
    D::FirePreObservers(self, function, params);

    // Diagnostic name-prefix sniffer (zero cost when no slot is set).
    D::FireNameDiagnostics(self, function, params);

    // 2026-05-26 deep-RE call trace (one-shot diagnostic). When the
    // trace flag is on, log every ProcessEvent dispatch. Used to
    // capture BP call chains when reflection-invoked BPs don't appear
    // to do anything. The atomic load is relaxed (we don't care about
    // strict ordering -- the trace is best-effort observability).
    if (D::g_callTrace.load(std::memory_order_relaxed) && function) {
        auto fname = reflection::NameOf(function);
        std::wstring nameStr = reflection::ToString(fname);
        UE_LOGI("trace: PE self=%p func=%ls", self, nameStr.c_str());
    }

    if (sampleSelf) ::QueryPerformanceCounter(&t1);
    g_peTrampoline(self, function, params);
    if (sampleSelf) ::QueryPerformanceCounter(&t2);

    // POST-observers: fire AFTER the original. Used to read state the BP just
    // wrote (e.g. PHC.GrabComponentAtLocation POST reads handle+176 to see
    // what was just grabbed; PHC.SetTargetLocation POST sees the per-tick
    // drive target). Count-bounded same as PRE.
    D::FirePostObservers(self, function, params);

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
    } __except (TaskFaultFilter(GetExceptionInformation())) {
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
        // Condition-based release: the moment the armed resume-function dispatches
        // (the menu's ui_menu_C::Tick -> menu world up, teardown past), clear the
        // bypass and FALL THROUGH to the normal detour so this very call runs our
        // logic (the MULTIPLAYER-injection POST observer fires on the first menu
        // frame). A single pointer compare per dispatch while armed -- negligible.
        void* resumeFn = g_bypassResumeFn.load(std::memory_order_relaxed);
        if (resumeFn != nullptr && function == resumeFn) {
            g_bypassUntilMs.store(0, std::memory_order_relaxed);
            g_bypassResumeFn.store(nullptr, std::memory_order_relaxed);
            UE_LOGW("game_thread: transparent bypass RESUMED on its release function "
                    "(menu world up) -- detour normal again");
            // fall through to the normal detour below
        } else if (NowMs() < until) {
            if (g_peTrampoline) g_peTrampoline(self, function, params);
            return;
        } else {
            g_bypassUntilMs.store(0, std::memory_order_relaxed);   // ceiling hit -> resume
            g_bypassResumeFn.store(nullptr, std::memory_order_relaxed);
        }
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

// ---- detail services this TU provides to game_thread.cpp ----------------------

namespace detail {

TaskFaultInfo& LastTaskFault() { return t_lastTaskFault; }

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
// thread-local buffer); called only from C++ bodies (Pump / LogObserverAv),
// never from the SEH-only Run*SEH frames. The logged RVA is ASLR-independent
// (ip - runtime base), so it maps directly against the payload .map's
// preferred-base RVAs.
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

// ---- PE double-detour DIAGNOSTIC (probe; RULE 2 exempt) ------------------------
// WP-2 2026-08-22: proves/refutes the followJmp-divert hypothesis for the boot
// crash WITHOUT needing the ~20% crash. The divert is STRUCTURAL: if UE4SS's
// PolyHook detours ProcessEvent AFTER our MinHook, its followJmp resolves our E9
// and re-points its patch onto OUR DETOUR body -> our detour's prologue is
// overwritten in place, observable on any NORMAL boot. Snapshots the whole hook
// chain (PE prologue / our detour prologue / our trampoline) at install and again
// ~10s later (past UE4SS init). Inert unless VOTVCOOP_PE_DIAG=1.
namespace {

bool PeDiagEnabled() {
    char v[8] = {};
    const DWORD n = ::GetEnvironmentVariableA("VOTVCOOP_PE_DIAG", v, sizeof(v));
    return n > 0 && v[0] == '1';
}

std::string HexBytes(const void* p, size_t n) {
    std::string s;
    const auto* b = static_cast<const uint8_t*>(p);
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        // Read defensively: the pages are code (present), but guard anyway.
        std::snprintf(buf, sizeof(buf), "%02x ", b[i]);
        s += buf;
    }
    return s;
}

// Classify the first byte of a prologue: is it an inline-detour jmp (someone
// hooked it) or the real function body?
const char* JmpKind(const uint8_t* p) {
    if (p[0] == 0xE9) return "E9-rel32-jmp";
    if (p[0] == 0xFF && p[1] == 0x25) return "FF25-riprel-jmp";
    if (p[0] == 0xEB) return "EB-short-jmp";
    if ((p[0] == 0x48 || p[0] == 0x49) && (p[1] == 0xB8 || p[1] == 0xBB)) return "mov-imm64+jmp";
    return "not-a-jmp(real-body?)";
}

void LogHookChainSnapshot(const char* when) {
    auto* pe    = reinterpret_cast<uint8_t*>(reflection::ProcessEventAddr());
    auto* det   = reinterpret_cast<uint8_t*>(&ProcessEventDetour);
    auto* tramp = reinterpret_cast<uint8_t*>(g_peTrampoline);
    UE_LOGI("pe_diag[%-9s] PE      %p : %s | first=%s", when, (void*)pe,
            pe ? HexBytes(pe, 16).c_str() : "(null)", pe ? JmpKind(pe) : "?");
    UE_LOGI("pe_diag[%-9s] detour  %p : %s | first=%s", when, (void*)det,
            det ? HexBytes(det, 16).c_str() : "(null)", det ? JmpKind(det) : "?");
    // 48 bytes: covers the trampoline (0x14) + the MinHook relay opcode (0x14..0x19)
    // + the relay's abs64 target pointer (0x1A..0x21) -- the exact 8 bytes PolyHook's
    // followJmp overwrites when UE4SS hooks PE after us.
    UE_LOGI("pe_diag[%-9s] tramp   %p : %s", when, (void*)tramp,
            tramp ? HexBytes(tramp, 48).c_str() : "(null)");
    // Classify the relay (WP-2 immune-relay aware). The relay lives inside the
    // trampoline slot BEHIND MinHook's own jump-back stub (FF25 00000000 + abs64
    // -> PE+len(stolen)), which shares the legacy relay's encoding -- a blind
    // first-match scan reads the jump-back and mislabels every boot (2026-08-22:
    // it printed LEGACY-CORRUPT on a run whose own byte dump showed the compose
    // working). The abs64/imm64 payload uniquely discriminates: only the relay
    // targets &detour. Locate it ONCE at the install snapshot (nothing has
    // patched it yet), remember the offset, classify THAT offset in every later
    // snapshot:
    //   FF25 00000000 <&detour>      LEGACY-RELAY INTACT
    //   FF25 00000000 <other>        LEGACY-RELAY CORRUPT (PolyHook clobbered the
    //                                  pointer slot -- the old double-detour crash)
    //   48 B8 <&detour> FF E0        IMMUNE-RELAY INTACT (fix on; UE4SS has not
    //                                  armed its PE hook this session)
    //   anything else at the offset  POLYHOOK-COMPOSED -- UE4SS in-place hooked
    //                                  our relay; the fix WORKING (the crash's
    //                                  absence, made visible)
    if (tramp) {
        const uint64_t wantDet = reinterpret_cast<uint64_t>(det);
        static int s_relayOff = -1;  // located at the install snapshot, once per session
        if (s_relayOff < 0) {
            for (int off = 0; off + 14 <= 48; ++off) {
                uint64_t p = 0;
                if (tramp[off] == 0xFF && tramp[off + 1] == 0x25 && tramp[off + 2] == 0 &&
                    tramp[off + 3] == 0 && tramp[off + 4] == 0 && tramp[off + 5] == 0) {
                    std::memcpy(&p, tramp + off + 6, 8);
                    if (p == wantDet) { s_relayOff = off; break; }
                } else if (tramp[off] == 0x48 && tramp[off + 1] == 0xB8 &&
                           tramp[off + 10] == 0xFF && tramp[off + 11] == 0xE0) {
                    std::memcpy(&p, tramp + off + 2, 8);
                    if (p == wantDet) { s_relayOff = off; break; }
                }
            }
        }
        const char* verdict = "UNKNOWN(relay not located at install)";
        if (s_relayOff >= 0) {
            const uint8_t* r = tramp + s_relayOff;
            uint64_t p = 0;
            if (r[0] == 0xFF && r[1] == 0x25 && r[2] == 0 && r[3] == 0 && r[4] == 0 &&
                r[5] == 0) {
                std::memcpy(&p, r + 6, 8);
                verdict = (p == wantDet) ? "LEGACY-RELAY INTACT"
                                         : "LEGACY-RELAY CORRUPT(double-detour hit)";
            } else if (r[0] == 0x48 && r[1] == 0xB8 && r[10] == 0xFF && r[11] == 0xE0) {
                std::memcpy(&p, r + 2, 8);
                verdict = (p == wantDet) ? "IMMUNE-RELAY INTACT(UE4SS not armed on it)"
                                         : "IMMUNE-RELAY PTR-MISMATCH";
            } else {
                verdict = "POLYHOOK-COMPOSED(immune relay in-place hooked -- fix working)";
            }
        }
        UE_LOGW("pe_diag[%-9s] RELAY: %s", when, verdict);
    }
    // WHO-FIRST is decided by what our trampoline HOLDS, not by PE's byte: if the
    // trampoline holds the real PE prologue (40 55 56 57 41 54) we hooked FIRST (PE
    // had real bytes); if it holds an ff25/e9 jmp, UE4SS hooked PE before us.
    const bool weFirst = tramp && tramp[0]==0x40 && tramp[1]==0x55 && tramp[2]==0x56;
    UE_LOGW("pe_diag[%-9s] WHO-FIRST: %s (trampoline holds %s)", when,
            weFirst ? "WE-FIRST (PE had real bytes at our install)"
                    : "UE4SS-FIRST (we relocated its jmp)",
            tramp ? JmpKind(tramp) : "?");
    ue_wrap::log::Flush();
}

DWORD WINAPI PeDiagDelayedThread(LPVOID) {
    ::Sleep(10000);  // past UE4SS init() -> setup_unreal() -> PE detour + slot-2 dispatch
    LogHookChainSnapshot("post-init");
    return 0;
}

}  // namespace

// ---- public API owned by this TU ----------------------------------------------

// DRILL for the absorbed-AV rate latch (VOTVCOOP_AV_LATCH_DRILL=1). A latch that has
// only ever been observed NOT firing is indistinguishable from a latch that is wired
// up wrong, and the storm it exists for is not reproducible on demand -- so drive the
// fold logic directly with synthetic sites. It exercises the real LogObserverAv, so
// what it proves is the real behaviour; it does NOT exercise the fault path, which is
// unchanged. EXPECTED in the log: site A -> 5 full lines then folds at 10 and 100 (7
// lines for 120 calls, not 120); site B -> its own 5 full lines, i.e. a NEW site is
// never suppressed by an old site's volume, which is the property that matters most.
void RunAvLatchDrill() {
    char v[8]{};
    if (!(::GetEnvironmentVariableA("VOTVCOOP_AV_LATCH_DRILL", v, sizeof(v)) > 0 && v[0] == '1'))
        return;
    // A REAL UFunction: the full-line path calls reflection::NameOf(function), which
    // dereferences it. The first cut passed nullptr and killed the process at boot --
    // which is itself the argument for drilling rather than reasoning.
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
    for (int i = 0; i < 120; ++i) LogObserverAv(fn, reinterpret_cast<void*>(0x1000 + i), "drillA");
    t_lastTaskFault.faultingIP = reinterpret_cast<void*>(0xB000);
    for (int i = 0; i < 3; ++i) LogObserverAv(fn, reinterpret_cast<void*>(0x2000 + i), "drillB");
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
    // WP-2 (2026-08-22): PE is the ONE function UE4SS's PolyHook also detours, so
    // its MinHook relay MUST be followJmp-immune (root-cause fix for the UE4SS-lane
    // boot double-detour crash). Default ON -- this is the fix. VOTVCOOP_PE_IMMUNE_RELAY=0
    // forces the LEGACY corruptible relay to reproduce the baseline crash for an A/B
    // (RULE-2-exempt diagnostic escape; retire it when the flag is removed at commit 3).
    bool immuneRelay = true;
    {
        char v[8] = {};
        if (::GetEnvironmentVariableA("VOTVCOOP_PE_IMMUNE_RELAY", v, sizeof(v)) > 0 && v[0] == '0')
            immuneRelay = false;
    }
    if (!hook::Install(pe, reinterpret_cast<void*>(&ProcessEventDetour),
                       reinterpret_cast<void**>(&g_peTrampoline), immuneRelay)) {
        return false;
    }
    UE_LOGW("game_thread: PE relay %s", immuneRelay
            ? "followJmp-immune (fix ON -- composes with a co-resident PolyHook PE detour)"
            : "LEGACY corruptible (VOTVCOOP_PE_IMMUNE_RELAY=0 -- baseline crash repro)");
    g_hookTarget = pe;
    g_installed = true;
    UE_LOGI("game_thread: ProcessEvent hooked; game-thread dispatcher live");
    // AFTER the hook is live, never before. Run from the top of Install() this drill
    // destabilised boot twice (the game died a few seconds in, mid-`cppmod` dispatch
    // census) -- it does 123 reflection lookups + formatted log writes on the loader
    // thread while the engine is still building its object graph and before our own
    // dispatcher exists. The drill is diagnostic-only and env-gated, but a drill that
    // kills the process teaches the next reader that the latch is broken when it is not.
    RunAvLatchDrill();
    if (PeDiagEnabled()) {
        LogHookChainSnapshot("install");
        if (HANDLE t = ::CreateThread(nullptr, 0, PeDiagDelayedThread, nullptr, 0, nullptr)) {
            ::CloseHandle(t);
        }
    }
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    ClearAllObservers();
    detail::ClearAllInterceptors();
    // DISABLE, never remove. This line used to be `hook::Uninstall`, and that
    // function no longer exists -- see hook.h "Retirement" for why. Disable lifts
    // the patch at ProcessEvent so no NEW dispatch enters us, while the trampoline
    // stays allocated and intact for whoever is already inside.
    hook_drill::SampleTrampoline("pre-disable", 0, reinterpret_cast<void*>(g_peTrampoline));
    hook::Disable(g_hookTarget);
    hook_drill::SampleTrampoline("post-disable", 0, reinterpret_cast<void*>(g_peTrampoline));
    g_installed = false;
    g_hookTarget = nullptr;
    // WHAT THE 2026-05-27 AUDIT (C3) GOT WRONG -- it cleared a live UAF by writing
    // "UAF is not possible because g_originalPE points at the engine's PE, a
    // process-lifetime entry point that is never unloaded". Two falsifications:
    //   OBJECT -- `[V]` minhook/hook.c:634 `*ppOriginal = pHook->pTrampoline`. It is
    //     MinHook's slot, not the engine's function. The audit read the NAME; the
    //     name lied. Renamed g_peTrampoline so it cannot be written again.
    //   MECHANISM+TIMELINE -- the old hook::Uninstall called MH_RemoveHook, and
    //     `[V]` hook.c:702 -> buffer.c:282 writes `pSlot->pNext` over the slot (a
    //     MEMORY_SLOT UNIONs that link with the bytes, `[V]` buffer.c:43-50), so the
    //     prologue was clobbered at offset 0 on the line ABOVE, and the Sleep offered
    //     as mitigation ran after the damage. Window ZERO, at ~250k dispatches/s.
    // With Disable: prologue restored so no new dispatch enters, trampoline intact so
    // an in-flight worker calls through live memory. The pointer is still deliberately
    // NOT nulled (a racing load could read the null) -- that part of C3 was right; the
    // Sleep stays as a drain before g_hookTarget goes. Full account: UE4SS_ARC 4c.
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
    // Arm the resume-function BEFORE the deadline so the detour never observes a
    // live bypass without its release condition. A null resumeOnFunction falls
    // back to a pure timer (identical to SetTransparentBypass).
    g_bypassResumeFn.store(resumeOnFunction, std::memory_order_relaxed);
    g_bypassUntilMs.store(maxMs > 0 ? NowMs() + maxMs : 0, std::memory_order_relaxed);
    UE_LOGW("game_thread: transparent bypass %s (resumeFn=%p, ceiling=%dms) -- detour "
            "forwards straight to the engine until the menu world is up",
            maxMs > 0 ? "ARMED" : "cleared", resumeOnFunction, maxMs);
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

}  // namespace ue_wrap::game_thread
