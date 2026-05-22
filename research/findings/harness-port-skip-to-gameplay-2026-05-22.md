# Autonomous test harness ported into the standalone mod — 2026-05-22

**Goal** (user request): port the autonomous test framework
(`tools/probes/coopTestHarness`, a UE4SS Lua mod) INTO the standalone C++ mod,
so it runs with no UE4SS (RULE No.3) and eventually retires the Lua harness
(RULE No.2). First parity target: skip-to-gameplay + screenshot + report.

This is also the first real `reflection::CallFunction` (UObject::ProcessEvent)
invocation, running live on the game-thread context built earlier today
(`game-thread-context-2026-05-22.md`).

## Result — PROVEN LIVE, standalone (no UE4SS), no crash

Boot timeline (scenario=`newgame`, read from `scenario.txt` next to the DLL):

```
harness: timeline start, scenario='newgame'
==== GAME-THREAD CONTEXT: LIVE ====
harness report [menu]:      NumObjects=182755, mainPlayer_C=PRESENT, world=preLoad
harness: skip-to-gameplay (open untitled_1)
engine: console command issued: open untitled_1
harness report [post-load]: NumObjects=257479, mainPlayer_C=PRESENT, world=Untitled_1
engine: console command issued: HighResShot 1920x1080
==== AUTONOMOUS NEWGAME TIMELINE DONE ====
harness report [post-shot]: NumObjects=257479, mainPlayer_C=PRESENT, world=Untitled_1
```

- **skip-to-gameplay works**: the world changed `preLoad` (menu) -> `Untitled_1`
  (the gameplay map). Driven entirely by our own CallFunction -> ProcessEvent;
  no menu navigation, no UE4SS.
- **screenshot works**: `HighresScreenshot00024.png` (1.7 MB, a real rendered
  gameplay frame: night, road, lamp posts, player hands/cart) landed in
  `%LOCALAPPDATA%\VotV\Saved\Screenshots\WindowsNoEditor\`. (Note: HighResShot
  writes there, NOT under the game folder's Saved -- the run-test.ps1 second
  candidate dir.)
- **report works**: NumObjects / mainPlayer_C presence / world name logged.
- No crash through the whole CallFunction chain on the game thread.

(Aside: `mainPlayer_C=PRESENT` already at the menu -- VOTV's `preLoad` menu world
contains a player. The decisive skip-to-gameplay signal is the world-name change
preLoad -> Untitled_1, not the pawn presence.)

## How — the call path

`UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command,
SpecificPlayer)` is the universal lever: BOTH `open <map>` and `HighResShot`
go through it, and it takes only an FString (no FName constructor needed yet).

`ue_wrap/engine::ExecuteConsoleCommand(cmd)`:
- self (dispatch target) = the KismetSystemLibrary CDO
  (`reflection::FindClassDefaultObject(L"KismetSystemLibrary")` ->
  `Default__KismetSystemLibrary`). Static BlueprintCallable UFunctions dispatch
  on the CDO.
- function = `FindFunction(ClassOf(cdo), L"ExecuteConsoleCommand")`.
- WorldContextObject = `FindObjectByClass(L"mainGameInstance_C")` (persists
  across level loads; valid world context), falling back to a live `World`.
- params frame (UE4.27 x64 ABI, `#pragma pack(1)`, size 0x20):
  `{ void* WorldContextObject; FString Command; void* SpecificPlayer; }`
  with FString = `{ wchar_t* Data; int32 Num; int32 Max }`, Num counts the null.
- The Command FString is a local buffer: ExecuteConsoleCommand takes a
  `const FString&`, only reads it (forwards to `GEngine->Exec`), never frees it,
  so no engine allocation is required. (CONFIRMED: no leak/crash across runs.)

New reflection primitives (engine-layer, principle 7):
- `FindObjectByClass(className)` — first live INSTANCE of a class (skips the
  `Default__` CDO). Mirrors UE4SS `FindFirstOf`.
- `FindClassDefaultObject(className)` — the `Default__<Class>` CDO.

## Files

- `src/votv-coop/include/ue_wrap/engine.h` + `src/ue_wrap/engine.cpp` —
  `ExecuteConsoleCommand` (CDO/fn/world-context resolve + cache, param marshal).
- `src/votv-coop/include/harness/harness.h` + `src/harness/harness.cpp` —
  the ported harness: `scenario.txt` read, background timeline, Report (game
  thread), skip-to-gameplay, screenshot.
- `reflection.{h,cpp}` — `FindObjectByClass`, `FindClassDefaultObject`.
- `sdk_profile.h` — added `KismetSystemLibraryClass`, `ExecuteConsoleCommandFn`,
  `GameInstanceClass` to `profile::name` (porting surface).
- `bootstrap/dllmain.cpp` — `harness::Start()` after the dispatcher installs.

## Parity status vs the Lua coopTestHarness

Ported: scenario read, skip-to-gameplay (newgame/open), screenshot, report.
NOT yet ported (so the Lua harness is NOT retired yet, RULE No.2): save loading
(`load:<slot>`), orphan spawn/drive (CTRL+P/O/K -> the Phase 2.1 derisk), widget
inspect. The orphan spawn/drive port lands with `coop::RemotePlayer` (next
roadmap item: SpawnActor + K2_SetActorLocation via CallFunction). Once that
exists, the Lua harness is fully superseded and gets deleted.

## Next

- Port the Phase 2.1 orphan spawn into C++ behind `coop::RemotePlayer`:
  `UWorld::SpawnActor<mainPlayer_C>` + `K2_SetActorLocation`, via CallFunction on
  the game-thread context. This needs param marshaling for SpawnActor (FVector/
  FRotator/FName) and likely the FName constructor (rva 0x1270b20) for level/
  class names. Then retire the Lua harness.
- Then Phase 3: UDP transport -> pose sync.
