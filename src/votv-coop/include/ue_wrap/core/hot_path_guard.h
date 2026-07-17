// ue_wrap/hot_path_guard.h -- enforce the game-thread-only invariant on
// side-tables that are GT-only BY CONVENTION (no lock), making the implicit
// rule explicit and catching the first future violator.
//
// Engine-substrate layer (principle 7): the only thing it knows is "which
// thread is the game thread" (ue_wrap/game_thread.h). No coop/gameplay state.
//
// WHY THIS EXISTS (audit theme #4, research/findings/votv-architecture-audit-
// 2026-05-29.md): a class of side-tables (g_drives, g_remoteNickBySlot,
// g_puppets, players::Registry's playerBySlot_; threats T-7/T-8/T-10) is
// touched only from the game thread and is therefore safe WITHOUT a mutex --
// but nothing ENFORCES that. Every new subsystem inherits the implicit rule
// and can introduce a silent off-thread access that data-races these tables.
//
// THE FIX IS A TRIPWIRE, NOT A LOCK. Adding a mutex to single-thread state
// would be a RULE-1 crutch: it would mask the real bug (an access that should
// never have left the game thread) instead of surfacing it. So this guard does
// NOT synchronize and does NOT change behaviour. On a PROVEN off-game-thread
// access it:
//   - emits a bounded ERROR log (first kMaxFiresPerSite per call site, then
//     goes silent so a hot loop can't spam the log), and
//   - in DEBUG builds, breaks into the debugger (hard stop for the developer).
// In Release (NDEBUG, our shipping/smoke config) the break compiles out, so a
// violation is a loud-but-non-fatal log line -- exactly what we want in a LAN
// smoke: it shows up without killing the host.
//
// It fires on ue_wrap::game_thread::IsDefinitelyOffGameThread(), which is true
// ONLY when the game thread id is already known AND the caller differs -- never
// for "don't know yet" (boot, before the first ProcessEvent dispatch). So a
// guard placed on a boot-time path that runs before the detour does not
// false-fire; conversely, a boot-time path that runs AFTER the detour on a
// non-game thread (e.g. the harness session-bringup thread) WOULD fire, which
// is why guards go on the per-message/per-tick GT hot path, not on one-shot
// session-start helpers that run on the bringup thread.

#pragma once

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>

namespace ue_wrap::hot_path {

// Per-site log budget: enough to confirm a violation (and a small burst) in a
// smoke log without ever spamming. Correct operation never trips this at all.
inline constexpr int kMaxFiresPerSite = 8;

}  // namespace ue_wrap::hot_path

// Hard developer stop on violation -- DEBUG builds only. Release (NDEBUG)
// compiles it out so the shipping/smoke build never aborts on a guard.
#if defined(NDEBUG)
  #define UE_WRAP_DEBUG_BREAK() ((void)0)
#else
  #define UE_WRAP_DEBUG_BREAK() __debugbreak()
#endif

// UE_ASSERT_GAME_THREAD(site) -- assert that this side-table access is on the
// game thread. `site` is a short string literal naming the table/accessor (it
// lands verbatim in the log so a violation is diagnosable). Must be a STATEMENT
// (it expands to a do/while); place it at the top of every accessor of a
// GT-only-by-convention side-table.
//
// A macro (not an inline fn) on purpose: the function-local `static` counter
// gives each call SITE its own independent budget, so one noisy site can't
// exhaust the log budget of the others, and a silent site costs nothing.
//
// Cost on the in-bounds (always, in correct operation) path: one relaxed
// atomic load + one GetCurrentThreadId() (a TEB read) + a compare, then fall
// through. No allocation, no lock, no log.
#define UE_ASSERT_GAME_THREAD(site)                                            \
    do {                                                                       \
        if (::ue_wrap::game_thread::IsDefinitelyOffGameThread()) {             \
            static ::std::atomic<int> s_gtGuardFires{0};                       \
            if (s_gtGuardFires.fetch_add(1, ::std::memory_order_relaxed) <     \
                ::ue_wrap::hot_path::kMaxFiresPerSite) {                       \
                UE_LOGE("HotPathGuard: %s accessed OFF the game thread -- "    \
                        "GT-only-by-convention invariant violated", (site));   \
            }                                                                  \
            UE_WRAP_DEBUG_BREAK();                                             \
        }                                                                      \
    } while (0)
