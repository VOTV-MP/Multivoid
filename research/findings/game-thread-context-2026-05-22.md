# Game-thread execution context (standalone) — 2026-05-22

**Goal**: a guaranteed game-thread callback so `reflection::CallFunction`
(UObject::ProcessEvent) can run safely. ProcessEvent MUST NOT be called from
our boot thread — it races the engine and crashes. Standalone (RULE No.3): no
UE4SS, our own hooking.

## Result — PROVEN LIVE (no UE4SS)

The standalone mod hooks `UObject::ProcessEvent` (the function we already
AOB-resolve) and uses its detour as a per-frame, guaranteed-game-thread
callback. A boot-posted self-test task ran on the game thread and read engine
state safely:

```
boot: BootThread tid=29212
hook: MinHook initialized
hook: installed on 00007FF7ADEC5930 (trampoline 00007FF7ACA50FC0)
game_thread: ProcessEvent hooked; game-thread dispatcher live
game-thread self-test: task ran on tid=17700 (boot tid=29212, DIFFERENT thread -- OK); NumObjects()=106469 read from game thread
==== GAME-THREAD CONTEXT: LIVE ====
```

- Task ran on tid 17700 — a DIFFERENT thread than the boot thread (29212): it is
  the engine's game thread (the thread ProcessEvent runs on).
- `NumObjects()` read 106469 from that thread (vs 17770 at boot — the menu had
  finished loading), proving engine reads are safe there.
- ProcessEvent hooked at the same rva the health check resolves (0x1465930).
- UE4SS absent (deployed with `deploy-loader.ps1 -Standalone`).

## Mechanism

Why hook ProcessEvent (rather than `GameEngine::Tick`):
- We already AOB-resolve ProcessEvent — no new signature to derive/maintain.
- The engine calls it constantly during play (and at the menu), always on the
  game thread — a free, frequent game-thread callback.
- It is exactly the function our CallFunction uses, so a task that calls a
  UFunction re-enters our own detour; the design handles that cleanly.

Detour (`ue_wrap/game_thread.cpp`):
1. Record the game thread id on first entry.
2. If not already pumping AND the task queue is non-empty (cheap locked check),
   set a thread-local re-entrancy guard, drain the queue (running each task
   unlocked so a task may Post more), clear the guard.
3. Forward to the real ProcessEvent via the MinHook trampoline.

Re-entrancy: a task that calls `CallFunction` re-enters ProcessEvent → our
detour, but the guard is set, so it skips the pump and just forwards. No
recursion, no deadlock. The guard is `thread_local` so a (hypothetical)
cross-thread ProcessEvent call stays correct.

`Post(task)` is thread-safe (mutex + deque); the boot thread posts, the game
thread runs. `IsGameThread()` is available for asserting before touching engine
state directly.

## Hooking substrate — MinHook (WP13)

- Vendored as a git submodule: `src/votv-coop/third_party/minhook`
  (TsudaKageyu/minhook, MIT, commit 05c06c5). Established library; we do not
  hand-roll trampolines.
- Compiled into our own static target (`minhook`) in our CMakeLists rather than
  `add_subdirectory`, so it inherits the SAME static-CRT runtime as the mod (a
  runtime mismatch breaks the link). Needed `project(... C)` for its `.c` files.
- Thin C++ facade `ue_wrap/hook.{h,cpp}` (`Init`/`Install`/`Uninstall`/
  `Shutdown`) — the rest of ue_wrap never touches MinHook types directly, so the
  substrate stays swappable (same discipline as `sig_scan` for the scanner).

## Files

- `src/votv-coop/include/ue_wrap/hook.h` + `src/ue_wrap/hook.cpp` — MinHook facade.
- `src/votv-coop/include/ue_wrap/game_thread.h` + `src/ue_wrap/game_thread.cpp`
  — ProcessEvent hook + task queue + reentrancy-guarded pump.
- `src/bootstrap/dllmain.cpp` — after the health check: install the dispatcher,
  post the game-thread self-test task.
- `CMakeLists.txt` — minhook static lib + new sources; `project(votv_coop CXX C)`.

## Gotcha logged for next time

Reading `votv-coop.log` while the game holds it open: `cp` fails on Windows with
"Device or resource busy" and a poll loop then reads a STALE temp copy (false
negative — looked like the marker never appeared). Kill the game first, then
read the log directly. The run had actually succeeded.

## Next

This unblocks the first real `CallFunction`. Next milestone (user request): port
the autonomous test framework (`tools/probes/coopTestHarness`) into the
standalone mod — skip-to-gameplay, screenshot, report — driven from this
game-thread context via CallFunction. Clean path: both skip-to-gameplay
(`open untitled_1`) and screenshot (`HighResShot`) reduce to
`KismetSystemLibrary::ExecuteConsoleCommand` (FString-only params, no FName
constructor needed). See ROADMAP "Standalone test harness (ported)".
