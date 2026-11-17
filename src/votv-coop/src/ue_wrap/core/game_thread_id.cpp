// ue_wrap/core/game_thread_id.cpp -- the game thread's id and the "provably not the game thread"
// answer built on it. The id is recorded by the ProcessEvent detour on its first dispatch
// (pe_detour.cpp); nothing here reaches the engine, so a program with no game behind it links this
// file and sees an id that is never recorded: IsDefinitelyOffGameThread is then always false.

#include "ue_wrap/core/game_thread.h"

#include "game_thread_detail.h"

#include <windows.h>

#include <atomic>

namespace ue_wrap::game_thread {

namespace detail {
// The game thread id, recorded by the detour on its first dispatch.
std::atomic<unsigned long> g_gameThreadId{0};
}  // namespace detail

bool IsDefinitelyOffGameThread() {
    // An id of 0 means not known yet, so nothing can be proven and the answer is false: the guard
    // must not fire at boot before the first dispatch records the game thread id. Once known, it
    // fires only on a real mismatch.
    const unsigned long gt = detail::g_gameThreadId.load(std::memory_order_relaxed);
    return gt != 0 && gt != ::GetCurrentThreadId();
}

}  // namespace ue_wrap::game_thread
