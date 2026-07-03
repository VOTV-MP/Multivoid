# VOTV `*Spawner_C` catalog — coop-sync roadmap (spawner OUTPUTS + shared host-detection)

Date: 2026-06-11. Game: VOTV Alpha 0.9.0-n, STOCK non-nativized UE4.27 (all BP = kismet bytecode).
Sources (cite, don't guess):
- CXX header dump: `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/*.hpp` (field/function SIGNATURES).
- Bytecode disassembly: `tools/bp_reflect.py <name>` -> `research/bp_reflection/<name>.json` (the LOGIC; dumped this session for mushroomSpawner, goreSlithersSpawner, ticker_deerSpawner, ticker_wispSpawner, undergroundGarbageSpawner, autumnLeafSpawner, ticker_beehiveSpawner, ghostcarSpawner, bloodSkeletonSpawner).
- Existing coop precedents: `src/votv-coop/src/coop/{npc_sync.cpp, firefly_sync.cpp, ambient_spawner_suppress.cpp, garbage_sync.cpp}`.
- NPC allowlist: `src/votv-coop/include/ue_wrap/sdk_profile.h:716` (`kNpcAllowlist`).

This agent catalogs spawner OUTPUTS + the shared host-detection seam. A sibling agent REs the central scheduler/ticker (`Aticker_base_C`).

---

## TL;DR — the single most valuable finding (read the SHARED HOST-DETECTION section)

The "spawner-spawned actor's `Init()` is BP-internal/unobservable" finding (2026-06-11) is TRUE but is about the **physics-prop Init**, NOT the spawn itself. **Every spawner in scope materializes its actor through `BeginDeferredActorSpawnFromClass` + `FinishSpawningActor`** (the GameplayStatics two-step), dispatched as a cross-object call through **ProcessEvent** — the SAME observable seam `npc_sync` already hooks. So the spawn CALL is catchable on the host even though the spawned actor's later `Init()` is not. Bytecode-verified across 9 dumped spawners: 0 use raw `SpawnActor`/`AddInstance`/`SpawnEmitter`; 100% use `BeginDeferredActorSpawnFromClass`.

Corollary already shipped: **creature spawners are ALREADY half-handled.** `kNpcAllowlist` already contains `npc_goreSlither_C`, `insomniac_C`, `fossilhound_C`, `antibreather_C` — exactly what `goreSlithersSpawner` / `ticker_insomniacSpawner` / `ticker_fossilhoundSpawner` / `antibreatherSPawner` spawn — so `npc_sync`'s `NpcSpawn_POST` observer on `BeginDeferredActorSpawnFromClass` already catches those host spawns and emits `EntitySpawn`. The remaining creatures (deer/boar/bunny/blackball/grayBoar/...) just need their classes added to that allowlist + pose-streamed.

---

## Inheritance / trigger taxonomy (what determines the seam)

| Parent class | Identity / lifecycle | Implication for sync |
|---|---|---|
| `Aticker_base_C` (the ticker family) | Gamemode-spawned scheduler actor; `GameMode` ptr @0x0230; has `intComs_*` event bus | The spawner itself is host+client-identical (each peer has its own). Its OUTPUT is what diverges. |
| `AActor` (plain) | World-placed or gamemode-spawned | Same — own copy per peer. |
| `Aactor_save_C` | SAVE-PLACED, **already KEYED** (`Key`) + `getData`/`loadData` | The spawner ITSELF rides the snapshot/prop pipeline already; only its runtime spawn OUTPUT needs sync. |
| `Aprop_C` / `Aprop_notebook_C` / `AtoolObject_C` | A held/dropped PROP that spawns on use | Rides the existing PROP pipeline (PropSpawn by eid) for itself; output is a prop too. |

Trigger mechanism (read off the function signatures + bytecode), each maps to a different "when does the host emit":

1. **TICKER-tick** — `ReceiveTick` only, inherits `Aticker_base_C`. The ticker base ticks it; body re-rolls `SetActorTickInterval(random)` + RNG-gates a spawn. (deer, wisp, yellowWisp, bp7, fossilhound, hexahive, insomniac, mannequin, susHole, tree, firefly.)
2. **Self-arming TIMER** — a `Spawn()` UFunction armed by `K2_SetTimerDelegate` (looping). (mushroomMaster/mushroomSpawner, beehive, batchSpawner, undergroundGarbage via `Timer()`.)
3. **Plain `ReceiveTick`** (non-ticker AActor) — pinecone, autumnLeaf (`spawnLeaves`), greenFire, blackball, furfurAltar.
4. **Trigger-VOLUME overlap** — `BndEvt__..._ComponentBeginOverlapSignature`. (ghostcar, antibreather, birch.)
5. **Construction / BeginPlay one-shot** — `UserConstructionScript` and/or `ReceiveBeginPlay` spawns at load. (bloodSkeleton, arirBuster, ABplush, bs_spawner, hexahive, hillRoller's static parts.) These are deterministic-at-placement; usually NOT runtime-divergent.

ALL of 1–5 funnel their actor materialization through `BeginDeferredActorSpawnFromClass` (verified) — so the seam is uniform regardless of trigger.

---

## GROUP A — TICKER-driven ambient (`Aticker_*Spawner_C : Aticker_base_C`)

All share: `UberGraphFrame@0x0238`, a single `ReceiveTick(float)`, `ExecuteUbergraph_*`. The ticker base drives `ReceiveTick`; the body re-rolls its own `SetActorTickInterval` (deer/wisp dumps show 2 `SetActorTickInterval` calls each) and RNG-gates a `BeginDeferredActorSpawnFromClass`. Each peer's RNG diverges -> different spawn position+time per peer = the reason host-authority is required.

| Spawner | WHAT IT SPAWNS | TRIGGER (cited) | Observability of spawn | Is it an NPC? | Coop status | Priority |
|---|---|---|---|---|---|---|
| `ticker_deerSpawner` | `deer` (`/Game/objects/deer`, NameMap[5] in dump) — ambient woodland creature | `ReceiveTick` re-rolls `SetActorTickInterval` (2x in dump), gates spawn; `K2_ProjectPointToNavigation` for placement | BeginDeferred via ProcessEvent — **observable** | creature (not in allowlist yet) | unhandled | MED |
| `ticker_bp7Spawner` | bp7 anomaly prop | `ReceiveTick` | observable | likely prop | unhandled | MED |
| `ticker_fossilhoundSpawner` | `fossilhound_C` — hostile dog anomaly (SCARE) | `ReceiveTick` | observable | **YES (in allowlist)** -> NPC pipeline already catches host spawn | **PARTIAL (NPC)** | HIGH |
| `ticker_hexahiveSpawner` | hexahive (bee swarm hive) | `ReceiveTick` | observable | swarm/prop | unhandled | MED |
| `ticker_insomniacSpawner` | `insomniac_C` — creature; has `insomniacDest(DestroyedActor)` death cleanup | `ReceiveTick` + death delegate | observable | **YES (in allowlist)** | **PARTIAL (NPC)** | HIGH |
| `ticker_mannequinSpawner` | mannequin (Don't-look anomaly, SCARE) | `ReceiveTick` | observable | creature/prop | unhandled | HIGH |
| `ticker_susHoleSpawner` | "sus hole" anomaly | `ReceiveTick` | observable | prop/anomaly | unhandled | MED |
| `ticker_treeSpawner` | tree (ambient flora, grows trees) | `ReceiveTick` | observable | static prop | unhandled | LOW |
| `ticker_wispSpawner` | wisp variants — `wisps` `TMap<TSubclassOf<AActor>,float>` @0x0240 weighted table (wisp_C weight 100 of ~108 + 8 colored, cooked CDO read 2026-07-03) | `ReceiveTick` re-rolls interval; `skipFirst` bool | observable | anomaly prop | **PER-PEER BY DESIGN** (deliberately NOT suppressed — ambient_spawner_suppress.cpp:71-74; the EVENT-swarm wisp_C is what mirrors, via the source-gated EX-catch `17cde303`) | — |
| `ticker_yellowWispSpawner` | yellow wisp -> spawns `deer` (`deer` array @0x0260); uses `mats` PhysicalMaterials to pick surface; `P` FVector spawn pt | `ReceiveTick` | observable | anomaly + creature | **HANDLED** (client ReceiveTick PRE-cancelled — ambient_spawner_suppress.cpp:75,96; killerwisp_C npc-allowlisted -> host-authoritative + mirrored) | — |
| `ticker_beehiveSpawner` | trees into `pinesComponent` HISM + `spawnableTrees`; **NOTE: timer-driven, not tick** — `Spawn()` armed by `K2_SetTimerDelegate` (6 SetTimer refs in dump), `spawnTime`@0x0258, `maxInst`@0x0254 | self-arming TIMER (Group B mechanic on a ticker class) | observable | foliage/HISM | unhandled | LOW |
| `ticker_fireflySpawner` | `eff_fireflies` particle emitter | `ReceiveTick` (TickInterval=30s) | spawn is `EX_CallMath SpawnEmitterAtLocation` -> **NOT observable at the call** (the EXCEPTION) -> captured via PSC-set diff | **DONE (v51 firefly_sync)** | — | — |

### Relationship of `ticker_fireflySpawner` to the v51 `firefly_sync`
SAME class. `firefly_sync.cpp` already handles it, PEER-SYMMETRICALLY (every peer runs its own spawner, captures its own spawn by PRE+POST-diffing the live `ParticleSystemComponent` set across the 30 s `ReceiveTick`, broadcasts the position, others mirror via reflected `SpawnEmitterAtLocation`). It is the **one true exception** to the BeginDeferred seam in this whole catalog, because fireflies are spawned as a *particle emitter* (`SpawnEmitterAtLocation`, `EX_CallMath` -> bypasses ProcessEvent) rather than an *actor* (`BeginDeferredActorSpawnFromClass` -> ProcessEvent). It is the template for the rare "emitter / EX_CallMath" cases; the actor-spawning majority use the simpler BeginDeferred-POST seam below.

---

## GROUP B — Self-arming TIMER ambient (`Spawn()` armed by `K2_SetTimerDelegate`)

| Spawner | WHAT IT SPAWNS | TRIGGER (cited) | Observability | Coop status | Priority |
|---|---|---|---|---|---|
| `mushroomMaster_C` | mints `mushroomSpawner_C` children w/ `SetLifeSpan(1800)` | `Spawn()` looping timer 15 s (CDO), armed in `ReceiveBeginPlay` (ambient_spawner_suppress.cpp:5-9) | BeginDeferred via ProcessEvent — observable; but currently SUPPRESSED on client | **SUPPRESSED on client** (ambient_spawner_suppress) | MED |
| `mushroomSpawner_C` | `prop_food_C` mushroom caps (`SetLifeSpan(1800)`) | `Spawn()` looping timer 2 s (CDO); dump: 3 BeginDeferred + 1 SetTimerDelegate | observable; SUPPRESSED on client | **SUPPRESSED on client** | MED |
| `batchSpawner_C` | a fixed batch of `spawned` `TArray<AActor*>` once (`isSpawned` latch); `intComs_storeMiddleman` | `Spawn()` once; `Aactor_save_C` keyed | observable; deterministic ONE-SHOT at load | save-placed -> snapshot covers the spawner; output unhandled | LOW |
| `batchSpawner_babygame` / `_waterlocker` | static-mesh batches (the StaticMesh1..30 are templates); waterlocker is a trigger w/ `runTrigger`/cords | `Spawn()` / trigger | one-shot deterministic | LOW | LOW |
| `undergroundGarbageSpawner_C` | weighted loot props (`Items`/`Weights`/`loot` TMap), placed around player/landscape (`aroundPlayer`/`skipLandscape`); `Duration`@0x02B0 | `Timer()` UFunction (6 `K2_SetTimer` in dump) + `prepareItems(Loc)` | BeginDeferred via ProcessEvent — observable | unhandled | MED |

NOTE on suppression: `ambient_spawner_suppress.cpp` cancels mushroomMaster/mushroomSpawner/pinecone `ReceiveTick`/`Spawn` ON THE CLIENT to avoid per-peer RNG divergence. Consequence: the client sees NONE of those unless host-mirrored. The roadmap should REPLACE suppression with host->client mirroring (suppress the client roll AND mirror the host result) so both peers see the same mushrooms/pinecones — otherwise the client world is barren of them (a known divergence). This is the same "client suppresses its own spawner" shape firefly_sync explicitly moved AWAY from (peer-symmetric), but for *physics props* the host-authoritative roll+mirror is correct (props can't be peer-symmetric — they're shared world objects with one identity, unlike camera-relative fireflies).

---

## GROUP C — Plain `ReceiveTick` ambient (non-ticker `AActor`)

| Spawner | WHAT IT SPAWNS | TRIGGER (cited) | Observability | Coop status | Priority |
|---|---|---|---|---|---|
| `pineconeSpawner_C` | `prop_stick_C` / `prop_food_pinecone_C` / `prop_crystal_C` (5 branches, each `SetLifeSpan(600)`) | `ReceiveTick` w/ in-body `SetActorTickInterval(random)` (ambient_spawner_suppress.cpp:10-16) | BeginDeferred via ProcessEvent — observable; SUPPRESSED on client. **Empirically (2026-06-11): no Init-POST fires** -> confirms Init unobservable, but the BeginDeferred is the seam | **SUPPRESSED on client** | MED |
| `autumnLeafSpawner_C` | falling leaf actors via `spawnLeaves()`; `Spread`/`Frequency`/`minSlope`/`minDistance` | `ReceiveTick` re-rolls interval (2x SetTickInterval in dump) -> `spawnLeaves` -> BeginDeferred (6 in dump) | observable | unhandled | LOW (cosmetic) |
| `greenFireSpawner_C` | `Agreenfire_C` (green flame anomaly) | `ReceiveTick`; `Aactor_save_C` keyed | observable | save-placed | LOW |
| `blackballSpawner_C` | orbiting blackball (`Angle`/`laps`/`maxlaps`/`MinRadius`/`vec`) | `ReceiveTick`; `Aactor_save_C` keyed w/ getData/loadData | observable | save-placed (keyed) | MED |
| `furfurAltarSpawner_C` | spawns into a `Adirthole_item_C hole` (`ChildActor`); `Dest(DestroyedActor)` cleanup | `ReceiveTick` + death delegate; `Aactor_save_C` keyed | observable | save-placed | LOW |

---

## GROUP D — Trigger-VOLUME overlap (`BndEvt__...BeginOverlap`)

These fire when a PLAYER overlaps a sphere/box. Coop subtlety: only the overlapping peer's volume fires -> a remote peer's overlap won't spawn on YOUR client. Host-authority needed so both peers see the result.

| Spawner | WHAT IT SPAWNS | TRIGGER (cited) | Observability | Coop status | Priority |
|---|---|---|---|---|---|
| `ghostcarSpawner_C` | ghost car (drives past; SCARE) | `BndEvt__..._spawn_...BeginOverlap` -> sets `spawned` latch -> BeginDeferred (6 in dump); `GameMode` ptr | observable | unhandled | HIGH |
| `antibreatherSPawner_C` | `antibreather_C` (`brether`@0x0268) via `cencipede()`; `bLock()` blocks cave; `canPlay` music | `BndEvt__..._Sphere1_...BeginOverlap` (cave entry) | observable | **antibreather IS in allowlist** -> NPC catches the creature; the bLock/music/blockRock state is separate | **PARTIAL (NPC)** | HIGH |
| `birchSpawner_C` | `Abirch_C` trees (`set()`, `multR`, `spawnArea`); begin+end overlap | `ReceiveTick` + overlap (`Overlap` bool) | observable | unhandled | LOW |

---

## GROUP E — Construction / BeginPlay one-shot (deterministic-at-placement)

These spawn at UCS/BeginPlay from a FIXED layout -> the same on every peer IF the placement seed is the same. Usually NOT runtime-divergent (no per-frame RNG), so often **need no sync** — but verify each spawns deterministically and not from `Rand` at BeginPlay.

| Spawner | WHAT IT SPAWNS | TRIGGER | Observability | Coop status | Priority |
|---|---|---|---|---|---|
| `bloodSkeletonSpawner_C` | bones (30 `UChildActorComponent` templates) — spawns actual bone actors at `ReceiveBeginPlay` (BeginDeferred 6 in dump) | `ReceiveBeginPlay` one-shot | observable | likely deterministic -> may need NO sync; verify RNG | LOW |
| `arirBusterSpawner_C` | ariral-buster via 7 ChildActorComponents | `ReceiveBeginPlay` | observable | deterministic | LOW |
| `ABplushSpawner_C` | a plush (`rad` radius); `Aactor_save_C` keyed | `UserConstructionScript`/`ReceiveBeginPlay` | observable | save-placed | LOW |
| `bs_spawner_C` | `Abs_C bs` (`rad`, object-type query) | `UserConstructionScript`/`ReceiveBeginPlay` | observable | save-placed-ish | LOW |
| `hexahiveSPawner_C` | hexahive cells via `gen(Spawn)`; `Size`@0x0230 | `UserConstructionScript`/`ReceiveBeginPlay` | observable | deterministic grid | LOW |
| `hillRollerSpawner_C` / `_instantSpawn` | hill-roller alien-on-wheels (SCARE); rolls down a hill on overlap/audio-finished | `Spawn()` + `BndEvt` Box overlap + Audio-finished | observable (BeginDeferred 6) | unhandled | HIGH (scare) — but instant variant fires at BeginPlay |
| `goreSlithersSpawner_C` | `npc_goreSlither_C` (`Spawn()`, `debug` bool, `GameMode` ptr) | `Spawn()` (BeginDeferred 25 refs — multi-spawn) + tick | observable | **goreSlither IS in allowlist** | **PARTIAL (NPC)** | HIGH |
| `grayBoarSpawner_C` | gray boar (`SpawnCount`/`maxBoar`/`combat`); full `intComs_*` bus | `ReceiveTick` + intComs | observable | creature (not in allowlist) | MED |
| `bunnySpawner_C` | bunny (`Aactor_save_C`) | `ReceiveBeginPlay` | observable | creature (not in allowlist) | LOW |
| `piramidSpawner_C` | `Apiramid2_C` (`piramid`); `runTrigger`/cord-driven puzzle (`walkFinish`/`walkLeave`) | trigger (`setActiveTrigger`/`runTrigger`) | observable | save/trigger-placed | MED |

---

## GROUP F — Prop/tool spawners (ride the existing PROP pipeline)

These ARE props themselves (held/dropped/used), so they + their outputs ride the existing `PropSpawn`-by-eid pipeline; no new detector needed — they appear when the prop appears.

| Spawner | WHAT IT SPAWNS | Notes | Priority |
|---|---|---|---|
| `tool_garbageSpawner_C` (`:AtoolObject_C`) | trash-clump chips (`chip_type` enum) via toolgun `Init(toolgun)` | The toolgun + clump output ALREADY ride the trash-clump / prop pipeline (`trash_collect_sync`, `PropConvert` v52). | LOW (covered) |
| `prop_broomspawner_C` (`:Aprop_C`) | a broom prop | held prop -> PropSpawn pipeline | LOW |
| `prop_tvSpawner_C` (`:Aprop_C`) | a TV prop | held prop -> PropSpawn pipeline | LOW |
| `prop_notebook_busterSpawner_C` (`:Aprop_notebook_C`) | spawns at `Loc` on `actionOptionIndex` (player interaction) | held notebook prop -> PropSpawn pipeline | LOW |

---

## ★ SHARED HOST-DETECTION MECHANISM — one detector for the whole family ★

This is the most valuable output. Three findings make a single shared detector possible:

**(1) The universal seam is `BeginDeferredActorSpawnFromClass` POST (ProcessEvent-dispatched).**
Bytecode-verified across all 9 dumped spawners: every actor-spawning spawner uses the GameplayStatics two-step `BeginDeferredActorSpawnFromClass(class, transform, ...)` -> [set exposed props] -> `FinishSpawningActor(actor)`. These are dispatched cross-object through ProcessEvent (the `EX_FinalFunction`/`EX_CallMath`-on-a-static form), which our MinHook ProcessEvent detour SEES — proven by the fact that `npc_sync` ALREADY hooks `BeginDeferredActorSpawnFromClass` with a POST observer (`NpcSpawn_POST`, `npc_sync.cpp:706`) + a pre-`FinishSpawningActor` suppression interceptor and reliably catches gamemode/ticker-driven NPC spawns. **The 2026-06-11 "Init unobservable" finding does NOT apply to the spawn call** — Init is dispatched BP-internally (`EX_LocalVirtualFunction` -> `ProcessInternal`) from the spawned actor's own UCS AFTER FinishSpawning, but BeginDeferred/FinishSpawning themselves are NOT. Catch the spawn at FinishSpawning, never at Init.

**(2) `npc_sync` already IS this detector for the NPC subset — extend, don't rebuild.**
`npc_sync` resolves `BeginDeferredActorSpawnFromClass` once, reads the class param at `g_npcSpawnActorClassParamOff`, hierarchy-matches it against `kNpcAllowlist`, and on a host match emits `EntitySpawn` (allocating an Element/eid). The POST observer reads the just-spawned actor's transform off `FinishSpawningActor`'s return/param. **For every creature spawner (deer, grayBoar, bunny, mannequin-if-character, blackball-if-character, ghostcar, hillRoller-alien), the ONLY work is: add the spawned class to the allowlist + give it a pose-stream entry.** goreSlither/insomniac/fossilhound/antibreather are ALREADY in the allowlist -> their spawners are already host-detected; the gap is only pose interp quality, not detection.

**(3) For PHYSICS-PROP spawners (pinecone/mushroom/leaf/garbage/stick/crystal), the SAME BeginDeferred-POST seam feeds the PROP pipeline instead of the NPC pipeline.**
Generalize the npc_sync detector into a class-routed host spawn-watcher:

```
HostSpawnWatcher  (one POST observer on BeginDeferredActorSpawnFromClass, shared)
  read spawnedClass = params[classOff]
  route by a small static class->channel table:
     class in kNpcAllowlist            -> EntitySpawn        (npc pipeline; EXISTING)
     class in kMirroredPropSpawnSet     -> PropSpawn(by eid)  (prop pipeline; the trash-clump precedent)
     else                               -> ignore (cosmetic / out-of-scope)
  POST: read the finished actor's transform (off FinishSpawningActor) -> fill the payload
```

- The PROP route reuses the **trash-clump precedent exactly**: host allocates an eid, broadcasts `PropSpawn{eid, WireClassName, transform, ...}` (the 200-byte v54 payload already carries class identity + transform + physics flags), client spawns the mirror by eid with tick/physics off. Pinecones/mushrooms self-expire via `SetLifeSpan` on the HOST; the host then broadcasts `PropDestroy{eid}` (or the client mirror runs the same lifespan, since the payload could carry remaining-life). This REPLACES the `ambient_spawner_suppress` client-cancel: keep suppressing the CLIENT's own roll (so its RNG can't spawn divergent local copies), but now MIRROR the host's result so the client actually sees them. Net: identical mushrooms/pinecones on both peers, host-authoritative.

**Why ONE shared observer (not per-spawner hooks):** `BeginDeferredActorSpawnFromClass` is a SINGLE GameplayStatics UFunction; every spawner calls THAT one function. A single POST observer on it sees ALL spawns from ALL spawners (and from the gamemode, and from anything else). Routing is a cheap class-pointer set lookup. This is strictly better than N per-class `ReceiveTick`/`Spawn` interceptors (which is what `ambient_spawner_suppress` does for suppression and `firefly_sync` does for the emitter exception) because: (a) one slot in the observer table, not N; (b) no per-spawner-class resolution loop; (c) automatically covers spawners we haven't catalogued; (d) zero per-frame GUObjectArray walk (the observer is event-driven, fires only on an actual spawn). The hot-path cost is a set lookup per spawn event — spawns are rare (seconds apart), so this is far below the per-frame-scan footgun.

**The one exception that CANNOT use this seam:** `ticker_fireflySpawner` (and any future emitter-only spawner) — fireflies are `SpawnEmitterAtLocation` (`EX_CallMath` -> native thunk on the GameplayStatics CDO, bypasses ProcessEvent). Those keep the firefly_sync PSC-set-diff capture. But that is a *particle* exception; every *actor* spawner uses BeginDeferred.

### Recommended build order
1. **Extend `npc_sync` allowlist + pose entries** for the plain creatures (deer, grayBoar, bunny, ghostcar, hillRoller-alien, mannequin/blackball if they're Characters). FREE detection — they already hit the existing POST observer. (HIGH: scares — ghostcar, hillRoller, mannequin.)
2. **Generalize the host spawn-watcher to a class-routed `HostSpawnWatcher`** (lift the npc_sync routing out so PROP classes route to `PropSpawn`). New `coop/host_spawn_watcher.{h,cpp}`; npc_sync becomes one consumer.
3. **Mirror the suppressed ambient props** (pinecone, mushroomMaster/Spawner) host->client via the PROP route; flip `ambient_spawner_suppress` from "client-cancel only" to "client-cancel + host-mirror". (MED.)
4. **Per-spawner state extras** that aren't the spawned actor: antibreather `bLock`/music/blockRock, ghostcar one-shot latch, undergroundGarbage loot determinism — handle as small state syncs AFTER the actor mirror works.
5. Defer GROUP E one-shots (bloodSkeleton, arirBuster, hexahive, batch) — verify they're deterministic-at-placement; if so, NO sync needed (both peers build the same layout from the same save). Only sync if a `Rand` at BeginPlay is found in their bytecode.

### Priority rollup
- **HIGH (creature/scare, both peers must see):** ghostcarSpawner, hillRollerSpawner, ticker_mannequinSpawner, ticker_fossilhoundSpawner*, ticker_insomniacSpawner*, goreSlithersSpawner*, antibreatherSPawner* (*= NPC already detected; need pose + the spawner's non-NPC state).
- **MED (ambient cosmetic but world-shared):** pineconeSpawner, mushroomMaster/mushroomSpawner, undergroundGarbageSpawner, ticker_deer/wisp/yellowWisp/bp7/hexahive/susHoleSpawner, grayBoarSpawner, blackballSpawner, piramidSpawner.
- **LOW:** autumnLeaf, birch, tree, beehive, greenFire, furfurAltar, bunny, bloodSkeleton, arirBuster, ABplush, bs_spawner, hexahiveSPawner, batch*, all GROUP F props (already covered by prop pipeline).
