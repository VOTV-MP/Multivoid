// coop/shutdown.h -- centralized X-close / process-exit handling.
//
// Root cause (deep-RE 2026-05-26, votv-coop X-close 99% RAM hang):
//   * Our PE detour stays installed through UE4 engine teardown. The engine
//     keeps firing ProcessEvent calls during GC/RHI/World teardown after
//     WM_CLOSE; those land in our trampoline; the observer body reads
//     half-destroyed UObject memory; faults inside the trampoline interact
//     badly with the loader lock; the OS spins the process with the engine's
//     entire working set (hundreds of MB) still mapped -> "99% RAM".
//   * Several detached worker threads (TimelineThread, dev hotkey threads,
//     screenshot WatcherThread) loop with `for (;;) { ::Sleep(N); }` and
//     have no exit condition -- they keep ticking after the game thread
//     is gone, posting GT::Post lambdas into a dead pump.
//
// Fix (RULE 1, per the RE doc):
//   1) Subclass the game HWND's wndproc; on WM_CLOSE, run shutdown
//      BEFORE the engine starts its teardown PE calls.
//   2) Shutdown order: set g_shuttingDown -> stop the net session
//      (joins NetThread) -> sleep ~100 ms so polling worker loops fall
//      through their Sleep + observe the flag and exit -> uninstall the
//      PE detour (game_thread::Uninstall) so subsequent teardown PE
//      calls hit the ORIGINAL engine code, not our trampoline ->
//      MH_Uninitialize (hook::Shutdown).
//   3) Every infinite worker loop in the project polls IsShuttingDown()
//      via `while (!coop::shutdown::IsShuttingDown()) { ... Sleep(N); }`.
//
// What this is NOT:
//   * NOT a join-all-threads-from-DllMain pattern. Detached workers are
//     LEFT to observe the flag and self-exit; we do not WaitForSingleObject
//     from DllMain (loader-lock deadlock).
//   * NOT a try/catch band-aid around the PE detour. RULE 1 root-cause
//     fix means removing the detour BEFORE teardown PE calls fire, not
//     swallowing the fault.

#pragma once

#include <atomic>

namespace coop::net { class Session; }

namespace coop::shutdown {

// Read the global shutdown flag. Tripped once (by the wndproc WM_CLOSE subclass, the
// DLL_PROCESS_DETACH last-resort path, or a manual DoShutdown()) and NEVER cleared
// after -- the process is going down. The flag itself is file-private to shutdown.cpp
// (internal linkage; the sole writer is DoShutdown()); this is its only public accessor.
bool IsShuttingDown();

// Install the WM_CLOSE subclass on the game window. Idempotent. Called
// once at boot AFTER the game window exists (harness boot path locates
// the HWND via EnumWindows). Holds a reference to the session pointer
// so the WM_CLOSE handler can Stop() it.
void Install(coop::net::Session* session);

// Set the window title to "VotV (Host)" / "VotV (Client)" depending on
// the session role. Idempotent: only fires once, AFTER the session has
// been Started (cfg_.role is the default Host before Start, so we have
// to defer until running()==true). Call per-tick from the boot loop;
// no-op until the conditions are satisfied. Splits from Install()
// because Install runs before Session.Start() and would always see
// Role::Host (bug observed 2026-05-26).
void UpdateWindowTitle();

// Run the cleanup sequence: flag -> session.Stop -> sleep -> uninstall PE
// hook -> MH_Uninitialize. Idempotent (subsequent calls no-op). Safe to
// call from any thread; the internal mutex serializes.
void DoShutdown();

}  // namespace coop::shutdown
