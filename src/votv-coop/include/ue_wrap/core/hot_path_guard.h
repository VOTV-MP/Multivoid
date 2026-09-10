// ue_wrap/core/hot_path_guard.h -- assert the game-thread-only invariant on side-tables that are
// GT-only BY CONVENTION rather than by a lock (g_drives, g_puppets, players::Registry's
// playerBySlot_ and their kind), so the implicit rule is explicit and the first future violator is
// caught. Engine-substrate layer (principle 7): all it knows is which thread is the game thread.
//
// A TRIPWIRE, NOT A LOCK: a mutex over single-thread state would mask the real bug -- an access
// that should never have left the game thread -- instead of surfacing it, so the guard neither
// synchronizes nor changes behaviour. It logs a per-site-bounded ERROR and, in DEBUG builds only,
// breaks into the debugger, so a violation in a shipping build is loud but never fatal.
//
// The test is game_thread::IsDefinitelyOffGameThread(), true ONLY once the game thread's id is
// known AND the caller differs -- never for "not known yet". A guard on a boot path that runs
// before the detour cannot false-fire, while one on a path running AFTER the detour on another
// thread (the session-bringup thread) would: guards belong on per-tick and per-message game-thread
// work, not on one-shot session-start helpers.

#pragma once

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>

namespace ue_wrap::hot_path {

// Per-site log budget: enough to confirm a violation and the small burst that follows it, without
// ever spamming a log. Correct operation never trips it.
inline constexpr int kMaxFiresPerSite = 8;

}  // namespace ue_wrap::hot_path

// Hard developer stop on a violation -- DEBUG builds only. Release (NDEBUG) compiles it out, so a
// shipping build never aborts on a guard.
#if defined(NDEBUG)
  #define UE_WRAP_DEBUG_BREAK() ((void)0)
#else
  #define UE_WRAP_DEBUG_BREAK() __debugbreak()
#endif

// UE_ASSERT_GAME_THREAD(site) -- assert that this side-table access is on the game thread. `site`
// is a short string literal naming the table or accessor, printed as-is so a violation is
// diagnosable. Must be a STATEMENT (it expands to a do/while); place it at the top of every
// accessor of a GT-only-by-convention side-table.
//
// A macro rather than an inline function on purpose: the function-local `static` counter gives each
// call SITE its own budget, so one noisy site cannot exhaust the others', and a silent site costs
// nothing.
//
// Cost on the in-bounds path, which is every call in correct operation: one relaxed atomic load,
// one GetCurrentThreadId() (a TEB read) and a compare, then fall through. No allocation, no lock,
// no log.
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
