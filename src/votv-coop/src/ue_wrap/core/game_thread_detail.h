// ue_wrap/game_thread_detail.h -- PRIVATE seam between the two game_thread TUs.
// NOT public API: nothing outside src/ue_wrap/{game_thread,pe_detour}.cpp may
// include this (subsystems use ue_wrap/game_thread.h).
//
// The 2026-07-04 modularity extraction split the old 1065-LOC game_thread.cpp
// along its one clean concept boundary:
//   - pe_detour.cpp    -- the INTERPOSITION MECHANISM: the MinHook install, the
//     detour body, the transparent bypass, the SEH crash firewalls + absorbed-
//     fault localization, the PE re-entrancy depth probe, and the perf
//     self-timing instrumentation.
//   - game_thread.cpp  -- the DISPATCHER SERVICES the detour drives: the
//     observer/interceptor/name-diagnostic registries (+ their Bloom presence
//     probes) and the posted-task pump (+ the spawn-refusal drain gate).
//
// HOT-PATH CONTRACT: the detour fires on EVERY ProcessEvent dispatch
// (~85-100k/sec measured). The split must not add per-dispatch cost, so the
// fast REJECTS stay inline in this header, reading extern atomics defined in
// game_thread.cpp -- the exact same loads the pre-split code paid:
//   - registry probes: active-count acquire load (+ Bloom word on a count hit);
//     only a MATCHED dispatch (<1%) crosses the TU boundary into *Matched().
//   - pump probe: t_inPump TLS + queue-depth acquire load + thread-id compare;
//     only a non-empty queue crosses into DrainPostedTasksAtTopLevel().

#pragma once

#include "ue_wrap/core/game_thread.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::game_thread::detail {

// ---- registry presence state (defined in game_thread.cpp) --------------------
// O(1) observer/interceptor presence probe (perf, 2026-06-04): per-table Bloom
// bitmask; bits only SET on register (never cleared per-Unregister), so a live
// registrant's bit is ALWAYS set => no false negatives; stale bits are harmless
// false positives that fall through to the count-bounded slot walk.
constexpr int kBloomWords = 64;  // 4096 bits; ~75 entries => ~2% false positive
constexpr unsigned kBloomBits = kBloomWords * 64u;
extern std::atomic<uint64_t> g_postBloom[kBloomWords];
extern std::atomic<uint64_t> g_preBloom[kBloomWords];
extern std::atomic<uint64_t> g_intcBloom[kBloomWords];
extern std::atomic<int> g_interceptorActive;
extern std::atomic<int> g_postObserverActive;
extern std::atomic<int> g_preObserverActive;
extern std::atomic<int> g_nameDiagAnySet;
extern std::atomic<bool> g_callTrace;

inline unsigned BloomBit(void* fn) {
    // UFunction pointers are >= 16-aligned; drop the dead low bits before masking.
    return static_cast<unsigned>((reinterpret_cast<uintptr_t>(fn) >> 4) & (kBloomBits - 1));
}
inline bool BloomMaybe(const std::atomic<uint64_t>* bloom, void* fn) {
    const unsigned b = BloomBit(fn);
    return (bloom[b >> 6].load(std::memory_order_acquire) & (1ull << (b & 63))) != 0;
}

// ---- pump state (defined in game_thread.cpp) ----------------------------------
extern std::atomic<int> g_queueDepth;                // lock-free emptiness probe
extern thread_local bool t_inPump;                   // re-entrancy guard
extern std::atomic<unsigned long> g_gameThreadId;    // first-dispatch CAS records it

// ---- cold halves in game_thread.cpp (called only past the inline rejects) ----
// Count-bounded slot walks; fire every matching registrant via the SafeCall*
// SEH wrappers below. FireInterceptorsMatched returns true if any interceptor
// cancelled the original dispatch.
bool FireInterceptorsMatched(void* self, void* function, void* params);
void FireObserversMatched(bool post, void* self, void* function, void* params);
// Name-prefix diagnostic walk (resolves the function name; gated by anySet).
void FireNameDiagnosticsMatched(void* self, void* function, void* params);
// The queue drain at TOP-LEVEL game-thread context: checks the spawn-refusal
// gate (defers + episode-logs while the world refuses spawns -- the 2026-07-04
// join-window null-burst fix), else pumps under the t_inPump guard. Returns
// true iff Pump() ran (the detour drops its self-time sample then).
bool DrainPostedTasksAtTopLevel();
// Registry teardown for Uninstall() (ClearAllObservers is public API already).
void ClearAllInterceptors();

// ---- inline hot-path fast rejects (same loads as the pre-split detour) --------
inline bool FireInterceptors(void* self, void* function, void* params) {
    const int active = g_interceptorActive.load(std::memory_order_acquire);
    if (active <= 0) return false;
    if (!BloomMaybe(g_intcBloom, function)) return false;  // O(1) reject
    return FireInterceptorsMatched(self, function, params);
}
inline void FirePreObservers(void* self, void* function, void* params) {
    const int active = g_preObserverActive.load(std::memory_order_acquire);
    if (active <= 0) return;
    if (!BloomMaybe(g_preBloom, function)) return;  // O(1) reject
    FireObserversMatched(false, self, function, params);
}
inline void FirePostObservers(void* self, void* function, void* params) {
    const int active = g_postObserverActive.load(std::memory_order_acquire);
    if (active <= 0) return;
    if (!BloomMaybe(g_postBloom, function)) return;  // O(1) reject
    FireObserversMatched(true, self, function, params);
}
inline void FireNameDiagnostics(void* self, void* function, void* params) {
    if (g_nameDiagAnySet.load(std::memory_order_acquire) == 0) return;
    if (!function) return;
    FireNameDiagnosticsMatched(self, function, params);
}

// ---- SEH / absorbed-fault services in pe_detour.cpp ---------------------------
// The Pump() crash firewall and the per-callback wrappers live with the detour
// (one SEH concept, one owner); the pump + registry walks in game_thread.cpp
// call through these. See pe_detour.cpp for the full firewall rationale.
struct TaskFaultInfo {
    void*         faultingIP = nullptr;  // EXCEPTION_RECORD::ExceptionAddress
    void*         accessAddr = nullptr;  // ExceptionInformation[1] on an AV
    unsigned long code       = 0;        // 0xC0000005 == EXCEPTION_ACCESS_VIOLATION
};
// The current thread's last-absorbed-fault stash (filled by the SEH filters).
TaskFaultInfo& LastTaskFault();
// SEH-only task runner (no C++ destructors in its frame). 0 clean, 1 caught.
// STATUS_STACK_OVERFLOW is never absorbed (CONTINUE_SEARCH -- WER dumps at the
// recursion apex).
int RunTaskSEH(const Task& task);
// Resolve a faulting IP to "module+0xRVA" (thread-local buffer; C++ callers only).
const char* FormatModuleRva(void* ip);
// SEH-wrapped single-callback dispatchers (log + absorb an AV in one registrant
// instead of taking down the engine; time the cb body when perf counting is on).
bool SafeCallInterceptor(UFunctionInterceptor cb, void* self, void* params, void* function);
void SafeCallObserver(ProcessEventObserverFn cb, void* self, void* function, void* params,
                      const char* phase /* "PRE" or "POST" */);

}  // namespace ue_wrap::game_thread::detail
