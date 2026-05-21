# Finding: Phase 2.1 orphan spawn — PASS (idle, 65s soak)

**Date**: 2026-05-22
**Game version**: Alpha 0.9.0-n
**Phase**: 2.1 / 2.4 (first data point)
**Method**: autonomous — `coopTestHarness` `orphan` scenario, captured via
UE4SS.log + PrintWindow screenshot. Engine-native; no menu nav, no clicks.

## Result

Spawning a **second `mainPlayer_C`** in the live game via
`UWorld:SpawnActor` (our path — unpossessed, NOT split-screen /
`CreatePlayer`) **does not crash VOTV** and the orphan **survives the
soak**:

```
EnterGameplay(new) -> OpenLevel(untitled_1) -> local pawn = mainPlayer_C
spawn 2nd mainPlayer_C: ORPHAN SPAWNED
  mainPlayer_C /Game/maps/untitled_1.Untitled_1:PersistentLevel.mainPlayer_C_2147466038
T+60s  orphan alive=true  pos=(-37495, 69978, 6420)
T+90s  orphan alive=true  pos=(-37495, 69978, 6420)
T+120s orphan alive=true  pos=(-37495, 69978, 6420)
```

- Orphan lived from spawn (~T+55) through T+120 = **~65s, gate (>=60s) met**
  for the idle case.
- Game process stayed alive; **no new crash dump** (only an unrelated 2024
  dump exists).
- Position is constant — correct for an unpossessed, input-less idle pawn
  (methodology Phase 2 deliverable: "orphan ticks without crashing, in idle
  pose").

This de-risks the entire project's hardest assumption: UE's multi-pawn
support holds for VOTV's player class, and `SpawnActor<mainPlayer_C>` is a
viable orphan primitive for `coop::RemotePlayer`.

### Extended soak + singleton check (2nd run, 4 min)

A follow-up run soaked the idle orphan to T+240s and checked per-player
singletons (methodology 2.3):

```
pre-spawn   mainPlayer_C count=1 | gamemode.origPawn=nil
ORPHAN SPAWNED (mainPlayer_C_2147466125)
post-spawn  mainPlayer_C count=2 | gamemode.origPawn=nil
T+62/120/180/240s  orphan alive=true (pos constant); local pawn still mainPlayer_C
T+240s      mainPlayer_C count=2 | game stable, no crash
```

- **No clobber**: count went 1->2 cleanly; the **local player kept its
  original pawn** (orphan did not steal possession or replace the player).
- **4-minute soak passed**: orphan alive throughout, HUD intact, no crash.
- Note: `gamemode.origPawn` was `nil` even pre-spawn (not populated in a
  fresh New Game), so it is not a useful clobber indicator in this state;
  the meaningful signals are the stable count and unchanged local pawn.

## Honest caveats (NOT yet validated)

1. **Unpossessed + idle.** The orphan was not possessed by a controller and
   received no input. Its `BeginPlay`-as-a-real-player paths (camera setup,
   HUD binding, input) largely did not run. Driving it (possess / feed
   input) will exercise new code paths that may hit SP assumptions.
2. **Short soak (~65s).** Methodology wants several minutes for late
   crashes (anim transitions, scripted events). Needs a longer soak.
3. **Singleton conflicts unverified.** A 2nd `mainPlayer_C` BeginPlay may
   have touched host-authoritative singletons (`AmainGamemode_C::origPawn`,
   the HUD `ui_UI_C`, save hooks). The local player + game stayed stable,
   but subtle state clobbering isn't ruled out. Per principle 4, each will
   get a per-site fix when found.
4. **Not visually confirmed.** `mainPlayer_C` is first-person (no 3rd-person
   body), so the orphan isn't visible in the capture; existence is proven by
   reflection (valid object + queryable transform), not pixels.

## Next (Phase 2 continuation)

- Longer soak (5+ min) on the idle orphan; watch for late crashes/leaks.
- Possess/drive the orphan (the input-replication path, 4.1) and re-soak.
- Check `AmainGamemode_C::origPawn` and per-player registries for clobber
  when the 2nd pawn spawns (methodology 2.3 registry mirror).
- Then it becomes the network-driven `coop::RemotePlayer` target (Phase 3.5).

## Repro

`./tools/run-test.ps1 -Scenario orphan` (fresh New Game per the fresh-save
rule), then read `UE4SS.log` + a `capture-window.ps1` screenshot.
