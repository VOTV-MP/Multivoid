# UWorld spawn-refusal window RE + the pump drain-context root (2026-07-04)

Durable RE (exe ad478218, VotV Alpha 0.9.0-n, UE4.27 Shipping). Companion to the
`[spawn-gate]` fix commit `f23ecfdf`; offsets live in `ue_wrap/sdk_profile.h`
("UWorld spawn-refusal window"); the behavioral contract is the Pump-context rule
in `docs/COOP_DISPATCH_VISIBILITY.md`.

## The engine facts [RD, IDA]

`UWorld::SpawnActor` = `sub_142C12D20`. In a Shipping build its failure
early-outs are SILENT (LogSpawn is compiled out), so a refused spawn is
indistinguishable from any other null without knowing the branches:

- `0x142c12df7`: `test byte ptr [world+10Ch], 2` -> **bIsRunningConstructionScript**.
  Refuses unless `SpawnParameters+0x29 & 8` (bAllowDuringConstructionScript) --
  the K2 `BeginDeferredActorSpawnFromClass` path (exec `0x14300B270` ->
  `sub_142B3C120`) never sets that bit (it sets only bDeferConstruction, `|= 4`).
- `0x142c12e07`: `test byte ptr [world+10Dh], 20h` -> **bIsTearingDown**.
- Other early-outs: null/abstract/deprecated class, non-AActor class, template
  class mismatch, NaN transform, name collision ("An actor of name '%s' already
  exists" -- one of the few remaining format strings), level-check failure.

**Sole writer of the 0x10C bit**: `AActor::ExecuteConstruction`
(`sub_1428C5CC0`, the or at `0x1428c5fe4`) with a lambda scope-guard restore --
i.e. the bit brackets the WHOLE SCS+UCS construction of every BP actor, not just
the UserConstructionScript ProcessEvent (a name-keyed UCS bracket in our detour
would therefore under-cover; reading the bit is the exact truth).

**World resolution**: `UEngine::GetWorldFromContextObject` (`sub_142F31680`)
resolves via the virtual `UObject::GetWorld` at **vtable byte offset 0x160**
(`call [vtbl+160h]`). Reading the bits through cached-GameInstance -> that
virtual sees exactly the world state the engine's own spawn check reads.

## The mod-side root this explained [V, smoke log]

Our posted-task pump drained on ANY game-thread ProcessEvent dispatch --
including dispatches nested INSIDE another actor's construction script (BP
construction bodies dispatch plenty of PEs). During a save-load's mass actor
construction (client join: loadObjects/loadPrimitives spawning thousands),
nearly every dispatch for ~5 s is such a nested one, so every spawn a posted
task issued fire-and-failed: 2026-07-04 20:41-precursor smoke, joining client,
19:37:58-19:38:00 -- 871 `engine: BeginDeferredActorSpawnFromClass returned
null` (trash proxies) + 92 `remote_prop::OnSpawn: BeginDeferred returned null`
(keyed mirrors, never retried = ghost-sync for the session) + the puppet
(healed by its 1 s retry at 19:38:03, the window's end). The SAME signature was
seen 2026-06-27 ("970 BeginDeferred null = fresh-boot world-load-window
spawn-nulls", sync-consolidation-refactor-PLAN-2026-06-27.md) and dismissed as
orthogonal/environmental -- it was this, timing-dependent (NOT newly visible,
correcting the first 07-04 hypothesis "settle-phase walks lengthened the load
window").

## The fix shape (one owner: the pump seam)

`ue_wrap/spawn_gate::WorldRefusesSpawns()` (cached GameInstance validated by
IsLiveByIndex -> virtual GetWorld -> two byte reads; fail-open when either is
null) + the PE detour defers the drain while it returns true. FIFO preserved;
every posted task runs at top-level GT context; all spawn callers healed at
once. Smoke proof: nulls 0/0 vs 871+92+puppet, [ERROR] 0/0 vs 967, 73 keyed
mirrors spawned, gate episodes 0-1 ms, no 5 s hold-warns. AS-BUILT + smoke;
hands-on pending (runbook 0p).
