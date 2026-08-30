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
//      through their Sleep + observe the flag and exit -> DISABLE the
//      PE patch (game_thread::Uninstall) so subsequent teardown PE calls
//      hit the ORIGINAL engine code -> lift every remaining patch
//      (hook::Shutdown).
//      NOTHING IS FREED on this path, deliberately: MinHook is never
//      uninitialized and no hook is ever removed, because both free the
//      trampoline a thread may still be returning through -- and the free
//      writes over its first bytes in place. See ue_wrap/core/hook.h,
//      "Retirement".
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

// Run the cleanup sequence: flag -> session.Stop -> sleep -> DISABLE the PE
// patch -> lift every remaining patch. Frees nothing (hook.h, "Retirement").
// Idempotent (subsequent calls no-op). Safe to call from any thread; the
// internal mutex serializes.
//
// NOT callable from DLL_PROCESS_DETACH -- see PersistAtProcessExit below.
void DoShutdown();

// The ONLY teardown work DLL_PROCESS_DETACH may do: write the host's per-peer
// inventory to disk, and nothing else.
//
// WHY IT EXISTS (external source review of the public tree, 2026-08-30, then
// three rounds of measurement). DllMain's detach branch used to call
// DoShutdown() under a comment asserting that "DoShutdown only sets a flag +
// uninstalls our PE detour, both safe under the lock". Both halves of that were
// false. It does far more, and SEVEN of the operations it reaches are hostile
// under the loader lock:
//   1. takes g_slowMu -- at process exit the owner may be a thread Windows has
//      already terminated, and such a mutex never unlocks. hook.cpp:201 is
//      deliberately lock-free for exactly this reason, two frames down.
//   2. Session::Stop -> thread_.join()  (session_start.cpp:444)
//   3. Session::Stop -> CloseConnection + a 20 x 10 ms RunCallbacks/Poll linger
//      loop -- network I/O (:463-477)
//   4. Session::Stop -> signaling_.reset() -> socket close + WSACleanup (:500)
//   5. ::Sleep(100)
//   6. game_thread::Uninstall -> hook::Disable -> MinHook Freeze ->
//      CreateToolhelp32Snapshot + SuspendThread, plus ::Sleep(50)
//   7. hook::Shutdown -> MH_DisableHook(MH_ALL_HOOKS) -> the same freeze, which
//      hook.cpp:229-235 already records as "a documented deadlock risk" on this
//      exact path (filed docs/UE4SS_ARC.md 4c)
//
// None of that work is even USEFUL here. `[V]` cppmod_entry.cpp:318-325 pins the
// module (GET_MODULE_HANDLE_EX_FLAG_PIN) before anything else, so FreeLibrary
// can never unload us and this branch runs ONLY at process exit -- where Windows
// has already terminated every other thread. Quiescing waits on the dead;
// persisting does not.
//
// So: persist, and get out of the way. Deliberately NOT gated on the shutdown
// latch -- if the wndproc path latched and was then terminated mid-teardown,
// its flush may never have happened, and re-flushing is free (FlushSlot
// self-guards on its dirty bit). The flush path takes no locks at all
// (g_hostBySlot has no mutex) and writes a game-thread snapshot, so it is
// loader-lock-clean by construction.
//
// This is NOT the whole answer to durability: a hard crash or TerminateProcess
// delivers no DLL_PROCESS_DETACH at all, so the standing <=15 s write window
// (player_inventory_sync kWriteRate) is a SEPARATE defect and is tracked as one.
void PersistAtProcessExit();

}  // namespace coop::shutdown
