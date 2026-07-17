// ue_wrap/vm_dispatch.cpp -- see ue_wrap/vm_dispatch.h.
//
// Increment 1 of docs/COOP_VM_DISPATCH_PLAN.md: the PERMANENT GNatives[0x45]
// swap + name-keyed registration API + the game-thread name-first filter that
// fires consumer bracket callbacks. Only opcode 0x45 (EX_LocalVirtualFunction) is
// swapped -- 0x46 (EX_LocalFinalFunction) has no measured customer yet (plan R11),
// so its slot is left untouched. Hardening (coverage-gated validation, 1/s slot-
// integrity re-check, loud latches) + the self-bracket TLS opener are increment 1b.

#include "ue_wrap/core/vm_dispatch.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>

namespace ue_wrap::vm_dispatch {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;

// GNatives handler ABI: uintptr exec(UObject* Context, FFrame& Stack, void* Result).
// The dispatcher ignores the return, but execLocalVirtualFunction DOES return a
// value -- forward it unchanged.
using ExecFn = std::uintptr_t(__fastcall*)(void* ctx, void* stack, void* result);

// FFrame +0x20 = Code (bytecode cursor). At wrapper entry for op 0x45 it points AT
// the 12-byte FScriptName operand {ComparisonIndex@0, DisplayIndex@4, Number@8}
// (STEP 1.0 LIVE-MEASURED 2026-07-13). We peek it non-destructively -- a wrong
// decode only mis-FILTERS, never corrupts, because the original handler re-reads
// its own operands from Code and advances the cursor itself.
constexpr std::size_t kFFrameCodeOff = 0x20;
constexpr int kOpcodeLocalVirtual = 0x45;
constexpr int kMaxVerbs = 16;

std::uintptr_t* g_gnatives = nullptr;   // GNatives[256], the exec-handler table base.
ExecFn          g_origVirtual = nullptr;  // GNatives[0x45] original handler.

std::atomic<bool> g_enabled{false};      // the session gate (false during solo SP).
std::atomic<bool> g_resolvedAny{false};  // >=1 verb resolved -> the filter may run.
std::atomic<bool> g_allResolved{false};  // every registered verb resolved (stop re-trying).
std::atomic<bool> g_installed{false};    // the 0x45 swap is live.

// One registered verb. Fixed-size array (no allocation -- the hot path reads it
// lock-free via atomics; the registration mutex guards writes only).
struct VerbEntry {
    const wchar_t* name = nullptr;   // static string from the consumer (string literal).
    int            verbId = 0;
    EntryFn        cb = nullptr;
    std::atomic<std::uint32_t> cmpIdx{0};   // resolved FName.ComparisonIndex (== operand op[0]).
    std::atomic<std::uint32_t> number{0};   // resolved FName.Number         (== operand op[2]).
    std::atomic<bool>          resolved{false};
};
VerbEntry g_verbs[kMaxVerbs];
std::atomic<int> g_verbCount{0};
std::mutex g_regMutex;  // registration only -- NEVER taken on the dispatch hot path.

// Re-entrancy depth + the active-verb window, per thread, published ONLY around a GT
// match (a non-matching dispatch pays nothing). t_currentVerbId / t_currentCtx name
// the innermost active matched verb so a consumer's own Func-seam hooks firing inside
// the verb body can attribute a spawn/destroy to it (CurrentThreadVerb()).
thread_local int   t_matchDepth    = 0;
thread_local int   t_currentVerbId = 0;
thread_local void* t_currentCtx    = nullptr;

// Unwind-safe RAII bracket around g_origVirtual (the verb body): increments depth +
// publishes the active verb, and restores the prior state on ANY exit -- normal
// return OR C++ exception unwind. Without this, an abnormal unwind through the verb
// body would leak the depth and make CurrentThreadVerb() read active-forever, so the
// consumer's containment counter would silently read false-in-window for the rest of
// the session. CAVEAT: a raw SEH unwind under /EHsc still bypasses the destructor --
// but a kerfur verb body faulting via SEH means the process is already crashing, and
// the consumer cross-checks its counter against the raw catch count, so a leak would
// surface as an anomaly rather than a silent lie.
struct MatchScope {
    int   prevVerbId;
    void* prevCtx;
    MatchScope(int verbId, void* ctx) : prevVerbId(t_currentVerbId), prevCtx(t_currentCtx) {
        ++t_matchDepth;
        t_currentVerbId = verbId;
        t_currentCtx = ctx;
    }
    ~MatchScope() {
        --t_matchDepth;
        t_currentVerbId = prevVerbId;
        t_currentCtx = prevCtx;
    }
    MatchScope(const MatchScope&) = delete;
    MatchScope& operator=(const MatchScope&) = delete;
};

// ---- counters (relaxed atomics; the stats reader tolerates torn reads) ----
std::atomic<std::uint64_t> g_gtDispatch{0};
std::atomic<std::uint64_t> g_workerDispatch{0};
std::atomic<std::uint64_t> g_nameMatch{0};
std::atomic<std::uint64_t> g_offGtMatch{0};
std::atomic<std::uint64_t> g_callbackFired{0};

inline const std::uint32_t* PeekOperand(void* stack) {
    void* code = *reinterpret_cast<void**>(reinterpret_cast<char*>(stack) + kFFrameCodeOff);
    return reinterpret_cast<const std::uint32_t*>(code);
}

// Return the index of the registered verb the operand matches, or -1. Both threads
// (an off-GT match is a tripwire the caller reports). op[0]=ComparisonIndex,
// op[2]=Number@byte8 (the CORRECTED decode -- see the header).
inline int MatchIndex(void* stack) {
    const std::uint32_t* op = PeekOperand(stack);
    const std::uint32_t opCmp = op[0];
    const std::uint32_t opNum = op[2];
    const int n = g_verbCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const VerbEntry& e = g_verbs[i];
        if (!e.resolved.load(std::memory_order_acquire)) continue;
        if (opCmp == e.cmpIdx.load(std::memory_order_relaxed) &&
            opNum == e.number.load(std::memory_order_relaxed))
            return i;
    }
    return -1;
}

std::uintptr_t __fastcall WrapperVirtual(void* ctx, void* stack, void* result) {
    // The eternal fast-path tax paid on EVERY 0x45 dispatch forever (the swap is
    // never removed): one relaxed load + a predicted-not-taken branch + tail-call.
    if (!(g_enabled.load(std::memory_order_relaxed) &&
          g_resolvedAny.load(std::memory_order_acquire)))
        return g_origVirtual(ctx, stack, result);

    const bool gt = GT::IsGameThread();
    if (gt) g_gtDispatch.fetch_add(1, std::memory_order_relaxed);
    else    g_workerDispatch.fetch_add(1, std::memory_order_relaxed);

    const int idx = MatchIndex(stack);
    if (idx < 0)
        return g_origVirtual(ctx, stack, result);

    g_nameMatch.fetch_add(1, std::memory_order_relaxed);
    if (!gt) {
        // A watched verb dispatched off the game thread. Skip the callback (engine
        // UFunctions + our reflection are GT-only) and record the tripwire.
        g_offGtMatch.fetch_add(1, std::memory_order_relaxed);
        return g_origVirtual(ctx, stack, result);
    }

    // GT match: fire the consumer's ENTRY callback, then run the real verb body
    // inside an unwind-safe depth+window bracket so a nested matched dispatch sees
    // depth+1 AND the consumer's own FinishSpawningActor / K2_DestroyActor seam hooks
    // -- which fire INSIDE g_origVirtual -- can query CurrentThreadVerb() to attribute
    // the spawn/destroy to this verb. (Increment 1 is observe-only: an entry cb + the
    // published window feed the consumer's containment counter; no state mutation yet
    // -- capture/suppress/converge are 2a-2c.)
    const VerbEntry& e = g_verbs[idx];
    g_callbackFired.fetch_add(1, std::memory_order_relaxed);
    MatchScope scope(e.verbId, ctx);
    if (e.cb) {
        Bracket b{ctx, e.verbId, t_matchDepth};
        e.cb(b);
    }
    return g_origVirtual(ctx, stack, result);  // scope restores depth/window on return or unwind
}

std::uintptr_t* ResolveGNatives() {
    // AOB on a `call [GNatives + opcode*8]` dispatch site (STEP 1.0 / the spike):
    // lea rcx,[GNatives]; ... movzx; ... call [rcx + rax*8]. rel32 at hit+3,
    // GNatives = hit + 7 + rel32.
    const uintptr_t hit = ue_wrap::FindPattern(
        "4C 8D 0D ?? ?? ?? ?? 49 8B D7 0F B6 08 48 FF C0 49 89 47 20 8B C1 49 8B 4F 18 41 FF 14 C1");
    if (!hit) return nullptr;
    const std::int32_t rel = *reinterpret_cast<std::int32_t*>(hit + 3);
    return reinterpret_cast<std::uintptr_t*>(hit + 7 + rel);
}

bool ValidateTable(const std::uintptr_t* tbl) {
    uintptr_t base = 0, size = 0;
    ue_wrap::MainModuleRange(base, size);
    int inRange = 0;
    for (int i = 0; i < 256; ++i) {
        const std::uintptr_t p = tbl[i];
        if (p >= base && p < base + size) ++inRange;
    }
    return inRange >= 200;
}

// Install the 0x45 swap. Idempotent + latched; called under g_regMutex on the
// first RegisterVirtualVerb (the swap is gated on >=1 consumer).
bool EnsureInstalled() {
    if (g_installed.load(std::memory_order_acquire)) return true;

    // v119 perf-audit F-1: AOB resolution is process-immutable -- a failed
    // resolve can never succeed later, so LATCH the failure (log-once) or a
    // per-tick registration retry re-runs the full-image scan forever.
    static std::atomic<bool> s_installFailed{false};
    if (s_installFailed.load(std::memory_order_relaxed)) return false;

    g_gnatives = ResolveGNatives();
    if (!g_gnatives) {
        s_installFailed.store(true, std::memory_order_relaxed);
        UE_LOGE("[vm_dispatch] GNatives AOB unresolved -- substrate NOT installed (latched)");
        return false;
    }
    if (!ValidateTable(g_gnatives)) {
        s_installFailed.store(true, std::memory_order_relaxed);
        UE_LOGE("[vm_dispatch] GNatives@%p failed validation -- substrate NOT installed (latched)",
                (void*)g_gnatives);
        return false;
    }

    // Publish g_origVirtual BEFORE the table pointer so any thread that dispatches
    // 0x45 the instant after the swap reads a valid original to tail-call (x86 TSO
    // orders the stores; VirtualProtect is a full barrier besides).
    g_origVirtual = reinterpret_cast<ExecFn>(g_gnatives[kOpcodeLocalVirtual]);

    DWORD oldProt = 0;
    if (!VirtualProtect(&g_gnatives[kOpcodeLocalVirtual], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        UE_LOGE("[vm_dispatch] VirtualProtect failed -- substrate NOT installed");
        g_origVirtual = nullptr;
        return false;
    }
    g_gnatives[kOpcodeLocalVirtual] = reinterpret_cast<std::uintptr_t>(&WrapperVirtual);
    VirtualProtect(&g_gnatives[kOpcodeLocalVirtual], sizeof(void*), oldProt, &oldProt);

    g_installed.store(true, std::memory_order_release);
    UE_LOGI("[vm_dispatch] GNatives@%p [0x45] swapped -> %p (orig=%p) -- EX_LocalVirtual "
            "substrate INSTALLED (disabled until a coop session enables it)",
            (void*)g_gnatives, (void*)&WrapperVirtual, (void*)g_origVirtual);
    return true;
}

}  // namespace

bool RegisterVirtualVerb(const wchar_t* verbName, int verbId, EntryFn cb) {
    if (!verbName || !cb) return false;
    std::lock_guard<std::mutex> lk(g_regMutex);

    const int n = g_verbCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {  // idempotent per (name, cb)
        if (g_verbs[i].cb == cb && g_verbs[i].name && std::wcscmp(g_verbs[i].name, verbName) == 0)
            return true;
    }
    if (n >= kMaxVerbs) {
        UE_LOGE("[vm_dispatch] verb table full (%d) -- cannot register %ls", kMaxVerbs, verbName);
        return false;
    }
    if (!EnsureInstalled()) return false;

    VerbEntry& e = g_verbs[n];
    e.name = verbName;
    e.verbId = verbId;
    e.cb = cb;
    e.resolved.store(false, std::memory_order_relaxed);
    e.cmpIdx.store(0, std::memory_order_relaxed);
    e.number.store(0, std::memory_order_relaxed);
    g_verbCount.store(n + 1, std::memory_order_release);
    UE_LOGI("[vm_dispatch] registered verb %ls id=%d (slot %d) -- pending GT FName resolve",
            verbName, verbId, n);

    // Prompt one resolve attempt on the GT (belt: the consumer's Tick also drives it).
    GT::Post([] { TickResolvePending(); });
    return true;
}

void TickResolvePending() {
    if (g_allResolved.load(std::memory_order_relaxed)) return;
    if (!GT::IsGameThread()) return;  // StringToFName dispatches ProcessEvent -> GT only.

    const int n = g_verbCount.load(std::memory_order_acquire);
    int resolvedCount = 0;
    for (int i = 0; i < n; ++i) {
        VerbEntry& e = g_verbs[i];
        if (e.resolved.load(std::memory_order_relaxed)) { ++resolvedCount; continue; }
        R::FName f = ue_wrap::fname_utils::StringToFName(e.name);
        if (f.ComparisonIndex != 0) {
            e.cmpIdx.store(static_cast<std::uint32_t>(f.ComparisonIndex), std::memory_order_relaxed);
            e.number.store(static_cast<std::uint32_t>(f.Number), std::memory_order_relaxed);
            e.resolved.store(true, std::memory_order_release);
            g_resolvedAny.store(true, std::memory_order_release);
            ++resolvedCount;
            UE_LOGI("[vm_dispatch] verb resolved: %ls cmpIdx=0x%x number=0x%x id=%d",
                    e.name, f.ComparisonIndex, f.Number, e.verbId);
        }
    }
    if (n > 0 && resolvedCount == n && !g_allResolved.exchange(true, std::memory_order_relaxed))
        UE_LOGI("[vm_dispatch] all %d verb(s) resolved -- name-first filter ARMED", n);
}

void SetEnabled(bool on) {
    const bool was = g_enabled.exchange(on, std::memory_order_release);
    if (was != on)
        UE_LOGI("[vm_dispatch] %s", on ? "ENABLED (coop session active)" : "DISABLED (session ended)");
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }
bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

ActiveVerb CurrentThreadVerb() {
    ActiveVerb v{};
    v.active = t_matchDepth > 0;
    v.verbId = t_currentVerbId;
    v.depth  = t_matchDepth;
    v.ctx    = t_currentCtx;
    return v;
}

Stats GetStats() {
    Stats s{};
    s.gtDispatch     = g_gtDispatch.load(std::memory_order_relaxed);
    s.workerDispatch = g_workerDispatch.load(std::memory_order_relaxed);
    s.nameMatch      = g_nameMatch.load(std::memory_order_relaxed);
    s.offGtMatch     = g_offGtMatch.load(std::memory_order_relaxed);
    s.callbackFired  = g_callbackFired.load(std::memory_order_relaxed);
    const int n = g_verbCount.load(std::memory_order_acquire);
    int resolved = 0;
    for (int i = 0; i < n; ++i)
        if (g_verbs[i].resolved.load(std::memory_order_relaxed)) ++resolved;
    s.registeredVerbs = n;
    s.resolvedVerbs = resolved;
    s.enabled = g_enabled.load(std::memory_order_relaxed);
    s.installed = g_installed.load(std::memory_order_relaxed);
    return s;
}

}  // namespace ue_wrap::vm_dispatch
