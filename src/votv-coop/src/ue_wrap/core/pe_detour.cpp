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
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace ue_wrap::game_thread {
namespace {

namespace D = detail;

// ProcessEvent's signature (x64 ABI). Matches reflection's ProcessEventFn.
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

ProcessEventFn g_originalPE = nullptr;  // trampoline to the real ProcessEvent
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
void LogObserverAv(void* function, void* self, const char* phase) {
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
    g_originalPE(self, function, params);
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
            if (g_originalPE) g_originalPE(self, function, params);
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

// ---- public API owned by this TU ----------------------------------------------

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
    detail::ClearAllInterceptors();
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
