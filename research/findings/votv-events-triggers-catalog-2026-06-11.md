# VOTV events + triggers — MASTER coop-sync catalog & roadmap (2026-06-11)

> **Status:** synthesis of four parallel bytecode-RE passes (all landed
> 2026-06-11). This is the roadmap; the per-class detail lives in the four
> section files (keep them — this doc does not duplicate every offset):
> - `research/findings/_events_catalog_A_scripted.md` — the `event_*` effect
>   classes (what each does + cheapest sync shape).
> - `research/findings/_events_catalog_B_scheduler.md` — the central scheduler
>   (`list_events` table, `saveSlot::settime`, `trigger_eventer`). **The keystone.**
> - `research/findings/_events_catalog_C_spawners.md` — the `*Spawner_C` family
>   (33 ambient spawners) + the shared host-detection seam.
> - `research/findings/_events_catalog_D_triggers.md` — the `AtriggerBase_C`
>   family (33 trigger volumes + condition triggers).
>
> Predecessor (superseded for the in-scope classes): `votv-events-RE-2026-05-24.md`.
> Method for all four: STOCK non-nativized UE4.27 → every BP is kismet bytecode
> (NOT in IDA); RE via `tools/bp_reflect.py` → `research/bp_reflection/*.json` +
> the CXX header dump. Cite, don't guess.

---

## TL;DR — the unified architecture in 12 lines

VOTV has **no single central event-roll**. "Events" are **two independent
subsystems with opposite sync needs** (agent B's keystone):

1. **Scripted story events** (`list_events`, 69 rows) fire **deterministically
   on the game clock** — `saveSlot::settime` compares `now > row.time` and calls
   `eventer->runEvent(name)`. **Zero RNG.** Reproducible from synced clock +
   fired-set. The host clock IS the roller (foundation: `time_sync` v36).
2. **Ambient tickers** (`Aticker_base_C` × 33 + per-hour rolls in
   `daynightCycle`) each roll **independent local RNG** per peer → guaranteed
   divergence; **no shared roll to intercept**. Must sync at the **output**.

Both fire via `EX_LocalVirtualFunction`/`EX_CallMath` → **bypass our ProcessEvent
detour** (the `playerGrabbed`/firefly trap). The only two ProcessEvent-observable
seams in the whole system are **`BeginDeferredActorSpawnFromClass`** (every actor
spawn) and **`ComponentBeginOverlap` BndEvt** (overlap volumes). Everything else
is synced at the **STATE/RESULT level**, never by hooking the fire.

→ **Three mechanisms + two primitives** cover the entire catalog (§2). Build
**M2 (HostSpawnWatcher) first** — it is independent of the unverified clock
seams, and it delivers the pinecone scare the user asked for.

---

## 1. The two subsystems (the foundational split — agent B)

| | (A) Scripted story events | (B) Ambient tickers |
|---|---|---|
| **What** | `list_events.uasset` — 69 `Fstruct_event{time(h,m,DAY), specialTrigger}` rows, keyed by FName (no enum) | `Aticker_*Spawner_C : Aticker_base_C` (33) + `daynightCycle` per-hour `RandomBoolWithWeight` rolls |
| **Decision** | `saveSlot::settime`: `conv(now) > conv(row.time)` → `eventer->runEvent(name, special)`. **Deterministic — the clock is the roll.** | each actor self-arms a random interval + RNG-gates a spawn at a random position. **Local RNG, per-peer.** |
| **Dedupe** | persistent `saveSlot.passEvents` (fired set) / `allEvents` (all names); past-day rows pre-marked passed at boot | none — continuous |
| **Fire dispatch** | `runEvent`/`runTrigger` via `EX_LocalVirtualFunction` → **unobservable + un-suppressable by a PE hook** | `BeginDeferredActorSpawnFromClass` (**observable**) or `SpawnEmitterAtLocation` (`EX_CallMath`, **un**observable) |
| **Sync leverage** | sync the **CLOCK + fired-set**; events reproduce from `runEvent(name)` | sync the **OUTPUT** (detect spawned actor/PSC, mirror); no roll to sync |
| **Precedent** | `time_sync` v36 (host clock) + save-transfer v56 (passEvents) | `firefly_sync` v51 (PSC-diff), `npc_sync` (BeginDeferred POST) |

`lib_C::getEvent/setEvent` + `mainGamemode.activeEvents` (0x0E68) is **not a
roller** — it's a refcount of currently-running events that gates tension/threat
music (`mainGamemode` ubergraph [3027-3032]). It is **state to mirror**, and it
is set as a side-effect of `runEvent` replay (§2, P2).

---

## 2. The unified architecture — 3 mechanisms + 2 primitives

Every in-scope class maps onto one of these. None requires editing a game asset
(RULE 1/A6); all reuse an existing pipeline or precedent.

### M2 — HostSpawnWatcher  *(SHIPPED 2026-06-11 — delivers the pinecone + Q-menu props)*

> **STATUS: SHIPPED + smoke-proven.** `coop/host_spawn_watcher.{h,cpp}` runs TWO POST
> observers on the GameplayStatics spawn seam (zero npc_sync changes — the weather_lightning
> multi-observer precedent):
> - **`BeginDeferredActorSpawnFromClass` POST → KEYLESS ambient** (pinecone/stick/crystal,
>   `kAmbientPropSpawnMirrorClasses`): keyless PropSpawn-by-eid + death-watch. Reads the
>   SpawnTransform PARAM (the actor isn't positioned until FinishSpawningActor). Mirror
>   simulates locally (user: local fall physics needn't sync). = the pinecone scare.
> - **`FinishSpawningActor` POST → KEYED sandbox** (Q-menu / toolgun, RE
>   `votv-sandbox-Q-propspawn-RE-2026-06-11.md`): the keyed Aprop_C's init() is BP-internal
>   so prop_lifecycle's Init-POST observer misses it; observe at FinishSpawningActor (Key
>   minted there) → `prop_lifecycle::ExpressSpawnedProp` REUSES the canonical keyed broadcast
>   (no duplication; HasProcessedInit dedupes vs the keyless set). Same Key both peers.
>
> Both audited 0 CRIT/HIGH. The original M2 description below is the keyless half; the keyed
> FinishSpawningActor half is the Q-menu extension. NPC creature spawners (the catalog's
> "add to kNpcAllowlist") remain the next M2 increment.



**One POST observer on `BeginDeferredActorSpawnFromClass`, class-routed.** This is
the single most valuable finding (agent C), bytecode-verified across 9 spawners:
**every actor-spawning spawner AND every spawning event** materializes through the
GameplayStatics two-step `BeginDeferredActorSpawnFromClass → FinishSpawningActor`,
dispatched cross-object through **ProcessEvent — observable.** (The 2026-06-11
"spawner Init is unobservable" pinecone finding is true but irrelevant: Init is
BP-internal, the *spawn call* is not. Catch at FinishSpawning, never at Init.)

```
HostSpawnWatcher  (generalize npc_sync's NpcSpawn_POST into a shared, class-routed observer)
  on host BeginDeferredActorSpawnFromClass POST:
    cls = params[g_npcSpawnActorClassParamOff]
    route by static class set:
       cls ∈ kNpcAllowlist        -> EntitySpawn  (NPC pipeline; EXISTING)
       cls ∈ kMirroredPropSpawnSet -> PropSpawn-by-eid (prop pipeline; the v54 trash-clump payload)
       else                        -> ignore (cosmetic / out of scope)
  on client (for mirrored classes): suppress the local BeginDeferred (npc_sync ALREADY does this for NPCs)
```

- **`npc_sync` already IS this detector for creatures** (`kNpcAllowlist`:
  goreSlither/insomniac/fossilhound/antibreather). The work is (1) lift the
  routing out into `coop/host_spawn_watcher.{h,cpp}`; `npc_sync` becomes one
  consumer; (2) add a `kMirroredPropSpawnSet` route → `PropSpawn`; (3) add plain
  creatures to the allowlist + give them pose entries.
- **Delivers the pinecone:** `prop_food_pinecone_C` ∈ the prop route →
  PropSpawn-by-eid. Same path mushroom/stick/crystal/garbage-clump take.
- **Flip `ambient_spawner_suppress`** from *client-cancel-only* to *client-cancel
  + host-mirror*: keep cancelling the client's own RNG roll (so it can't spawn
  divergent local copies), but now the host's result mirrors via the prop route.
  Net: identical pinecones/mushrooms on both peers, host-authoritative.
- **One exception** (cannot use this seam): emitter-only spawners
  (`ticker_fireflySpawner`) — `SpawnEmitterAtLocation` is `EX_CallMath`, bypasses
  PE. Those keep the v51 firefly_sync PSC-diff. Every *actor* spawner uses
  BeginDeferred; only *particle* spawners need the diff.
- **Hot-path:** event-driven (fires only on an actual spawn, seconds apart) — a
  set lookup per spawn, NOT a per-frame GUObjectArray walk. Safe.

### M1 — EventFire  *(scripted scheduler events; verify seams §4 before building)*

**Host is the clock-authority; the scripted fire is reproduced from the row name.**
Target design (agent B):

1. Host owns the clock (`time_sync` v36). Only the host's `settime` is
   authoritative for event firing.
2. Host detects a fire (a row enters the fired-set) and broadcasts
   `EventFire{ rowName : FName, special : FName }` (tiny).
3. Every peer calls `eventer->runEvent(rowName, special)` via a **reflected**
   ProcessEvent invoke (CDO/Local-ctx — the weather_lightning / firefly-spawn
   precedent; our active call DOES route through PE even though the game's own
   EX_LocalVirtualFunction call does not). The event reproduces locally.
4. **Spawns within the replayed event ride M2** (client's local spawn suppressed,
   host's mirrored) — so RNG-divergent spawn positions never matter.
5. **Dedupe** via `saveSlot.passEvents`/`allEvents` — initial state rides
   save-transfer join (v56), live fires ride the broadcast.

Covers **~40 heterogeneous story events in ONE mechanism** (ships, satellites,
treehouse builds, obelisk, prank interactions, dish-break, door-jam, solarBoom
sound, bigmRoar, lightsTurnoffer…) instead of 40 bespoke detectors — because the
scripted fire is reproducible from a single FName, unlike ambient RNG.

⚠ **Two load-bearing seams MUST be verified first — see §4.** The biggest:
`runEvent` is `EX_LocalVirtualFunction`, so the client's own clock-driven
`settime` may already fire (or be impossible to suppress by a PE hook). The real
shape of M1 depends on whether `time_sync` already drives the client's `settime`
in lockstep. Do not build M1 on agent B's "suppress the client runEvent" line
until that is resolved.

### M3 — Overlap-trigger gate  *(overlap-volume events, NOT scheduler-driven)*

For `event_*` / `trigger_box` effects fired by a **player overlapping a volume**
(vaccine, funnyGascans, trashPiles, forestMoan, lightsTurnoffer-via-box,
bottomHole): the `ComponentBeginOverlap` BndEvt **is** ProcessEvent-observable
(agent D). Shape: observe the begin-overlap on the local player → **host-
authoritative gate** (suppress the client's double-fire so a deterministic
level-placed volume both peers walk through doesn't fire twice) → spawned props
ride **M2**, one-shot sounds ride a small cue. **`garbage_sync` already does this
for `event_trashPiles`** — it's the precedent. Most payloads already ride
existing pipelines (door/light/lock keyed channels, PropSpawn).

### P1 — Per-player physical effect  *(the one genuinely new primitive)*

`skyFalling`'s `punch()` + `SetPhysicsLinearVelocity` on the player, and the
sleep-event `teleportWObackrooms` teleports, have **no existing precedent**: an
effect that each peer must apply to **its own local player**. skyFalling is the
hardest single event — its RNG impact location must be host-rolled and broadcast
(`EventFire{skyFalling}` + `impactLoc`), and each peer applies the punch to its
own player gated on that shared impact. This is "host rolls → broadcast RESULT"
at the parameter level. Build as a dedicated payload after M1/M2 land.

### P2 — activeEvents mirror  *(tension music / HUD — mostly automatic)*

`setEvent(true/false)` increments/decrements `mainGamemode.activeEvents`. When a
peer replays `runEvent` (M1) the event's own `setEvent` runs locally, so
`activeEvents` is set **for free** on that peer → threat music works. **Verify**
the replay path reaches `setEvent`; add an explicit `activeEvents` mirror only if
some event sets it outside the replayed graph. Low effort, high feel-payoff.

---

## 3. The hard constraint — only two observable seams (do not re-derive)

Our hook is a MinHook detour on `UObject::ProcessEvent`. A UFunction is
observable **iff its dispatch reaches ProcessEvent**:

| Dispatch | Observable? | Used by |
|---|---|---|
| `EX_LocalVirtualFunction` / `EX_LocalFinalFunction` → ProcessInternal-direct | **NO** | `runEvent`, `runTrigger`, `Init`, `spawn()`, all BP→self verbs |
| `EX_CallMath` → native thunk on CDO | **NO** | `SpawnEmitterAtLocation` (fireflies) |
| **`BeginDeferredActorSpawnFromClass` / `FinishSpawningActor`** (cross-object GameplayStatics) | **YES** | every actor spawn (M2) |
| **`BndEvt__...ComponentBeginOverlap`** (multicast delegate) | **YES** | overlap volumes (M3) |
| BeginPlay / Tick / timer-delegate / latent resume | **YES** | engine-driven (ambient_spawner_suppress hooks these) |

**Consequence:** you cannot passively learn "event E fired" by hooking the fire.
Sync at the **result** — detect the spawn (M2), observe the overlap (M3), diff the
state (M1 fired-set / P2 activeEvents), or actively re-invoke via reflected PE.

---

## 4. MUST-VERIFY-BEFORE-BUILD (load-bearing seams for M1)

RULE 1 — do not build M1 on these until confirmed. (M2 is independent; build it
regardless.)

1. **Does the client's time-synced `settime` already fire scripted events?**
   If `time_sync` v36 drives the client's `daynightCycle`/`saveSlot` clock in
   lockstep, the client's `settime` already crosses the same row timestamps and
   already calls `runEvent` locally — so M1's job is NOT "make the client fire"
   but "ensure both fire the same set + dedupe the RNG-divergent details (M2/P1)".
   If the client clock is independent, agent B's broadcast-driven model is needed.
   **Check what `time_sync` writes** (display-only vs. authoritative clock field
   `daynightCycle.timeZ` @0x02D0 → `settime`).
2. **Can the client's `runEvent` be prevented at all?** It is
   `EX_LocalVirtualFunction` → a ProcessEvent interceptor will NOT fire for the
   game's own call (the trap). If the client must be stopped from double-firing,
   suppression has to happen at a *different* layer (freeze the client's
   event-clock advance, or gate inside a hookable upstream verb) — NOT by
   intercepting `runEvent`. Resolve before committing M1's suppression step.
3. **Does `settime` write the fired row to `passEvents` synchronously on fire?**
   Agent B's host-detection diffs `passEvents` each minute-pulse. Confirm the
   `Add` happens at fire time (bytecode `settime` region [4298-4389]), not only at
   save. If not, detect via a different state delta, OR (cleaner) **compute the
   schedule ourselves** from `list_events` + the synced clock and own the firing
   entirely (host computes due-rows → broadcast + reflected `runEvent` on all
   peers). The self-computed-schedule variant sidesteps seams (2) and (3) and is
   worth evaluating as the primary M1 design.

---

## 5. Master priority / status table

Effect-type legend: SPAWN (rides M2) · OVERLAP (rides M3) · SCHED (rides M1) ·
PPLAYER (P1) · STATE (keyed channel / weather / etc., mostly done) · LOCAL (no
sync) · DONE.

### HIGH — shared scares/world-changes both players must witness

| Event/trigger | Effect type | Mechanism | New work |
|---|---|---|---|
| `skyFallingEvent` | PPLAYER + SPAWN + FX | **P1** + M1 + M2 | host-roll impactLoc; per-player punch; skypiece→M2 |
| `event_fossilBoarWar` | SPAWN (NPC+prop) | M2 (+M1 trigger) | add fossilhound*/grayboar to allowlist + pose |
| `event_fleshRain` | SPAWN (clump) | M2 (+M1) | clump route in M2; 1 cue for shake |
| `event_arirStealsGun` | SPAWN (NPC) + inventory | M2 + P1 | NPC via M2; gun-removal via prop lifecycle; pick target |
| `event_bottomHoleController` | OVERLAP + NPC + keyed | M3 + M2 + STATE | room-active flag; patrol→M2; beacons→keyed (location-gated, later) |
| `ghostcarSpawner` / `hillRollerSpawner` / `ticker_mannequinSpawner` | SPAWN (scare) | M2 | add to allowlist + pose |
| `trigger_breakDish` | STATE (permanent) | M1 (or dish-state) | host-auth `breakServer` dedup by key |
| `trigger_jamDoor` | STATE (door) | door channel | add `jammed` field to door adapter |
| `trigger_solarBoom` / `trigger_bigmRoar` / `trigger_alarm` | FX (2D sound) + lights | M1 (replay) / M3 cue | sound reproduced by `runEvent` replay; lights via light channel |
| `lightsTurnoffer` / `passwordGuesser` | STATE (keyed) | M1/M3 gate only | door/light/lock already mirror; just gate the trigger |

\* fossilhound/insomniac/goreSlither/antibreather **already** in `kNpcAllowlist`
→ host-detected today; gap is pose quality + the spawner's non-NPC state.

### MED — world-shared but lower stakes / partly covered

| Event/trigger | Effect type | Mechanism | New work |
|---|---|---|---|
| `pineconeSpawner` | SPAWN | **M2** | **the pinecone — prop route + flip suppress→mirror** |
| `mushroomMaster` / `mushroomSpawner` | SPAWN | M2 | prop route + flip suppress→mirror |
| `undergroundGarbageSpawner` / `ticker_deer/wisp/bp7/hexahive/susHole` | SPAWN | M2 | allowlist/prop route + pose |
| `grayBoarSpawner` / `blackballSpawner` / `piramidSpawner` | SPAWN | M2 | allowlist/prop route |
| `event_trashPiles` | OVERLAP (clump) | M3 | **already gated in `garbage_sync`** — verify piles mirror |
| `event_vaccine` / `event_funnyGascans` | OVERLAP (prop) | M3 + M2 | gate + PropSpawn (vaccine crate needs container-inventory sync) |
| `trigger_forestMoan` | OVERLAP (sound) | M3 cue | observe overlap → broadcast 3D cue |
| `trigger_wispSwarm` / `trigger_spawnFollowingArir` / `trigger_bloodSkeleton` | SPAWN (NPC/prop) | M2 | route by class |
| `trigger_eventt_arirShip` / `trigger_alarm` | STATE (alarmLamp) | small lamp channel | alarmLamp key/state |
| `trigger_vehtp` | STATE (ATV transform) | atv_sync | verify pose stream catches teleport; else discrete reliable |
| `bedEvent` / `event_arirTreehouseSleep` | PPLAYER (teleport) | P1 | **resolve shared-sleep UX first** |

### LOW / LOCAL / DONE

- **LOW (cosmetic, fold in when convenient):** `event_consoleWrite` (per-peer OK),
  `event_arirFuelsAtv` (fold fuel into atv_sync), `autumnLeaf`/`birch`/`tree`/
  `beehive`/`greenFire`/`furfurAltar` ambient spawners, GROUP-E one-shots
  (`bloodSkeleton`/`arirBuster`/`hexahive`/`batch*` — verify deterministic-at-
  placement → likely NO sync).
- **LOCAL (no sync — per-player by design, agent D GROUP 3):**
  `trigger_achievement`, `trigger_notif`, `trigger_agrav`, `trigger_ambientSound`,
  `trigger_sound`, `trigger_locationAmbience`, `trigger_teleporter`,
  `trigger_lockerLooker`, `trigger_forceObject`, `trigger_fakeLmaos`
  (hallucination). Plumbing (no own effect): `trigger_box*`, `trigger_delay`,
  `trigger_TBoxActivator`, `trigger_button` (the button-prop rides PropSpawn; its
  press fires linked effects = their own rows).
- **DONE:** `redSkyEvent` (`weather_redsky`), fireflies (v51), wind (v50),
  lightning, sky/stars (v44), time/clock (v36), weather (v43).

---

## 6. Build order

1. **M2 — HostSpawnWatcher.** Generalize `npc_sync`'s BeginDeferred POST observer
   into `coop/host_spawn_watcher.{h,cpp}` (class-routed: NPC→EntitySpawn,
   prop→PropSpawn). Add `prop_food_pinecone_C` + mushroom/stick/crystal/clump to
   the prop route. Flip `ambient_spawner_suppress` to client-cancel + host-mirror.
   **Delivers the pinecone** (user's explicit ask) and is independent of the §4
   unknowns. Then add plain creatures (deer/grayBoar/ghostcar/hillRoller/mannequin)
   to the allowlist + pose entries (FREE detection — they already hit the observer).
2. **Verify §4 seams**, then **M1 — EventFire** (or the self-computed-schedule
   variant). Covers the ~40 scripted story events at once.
3. **M3 — Overlap-trigger gate.** Generalize `garbage_sync`'s overlap-cancel into a
   reusable host-auth gate for vaccine/funnyGascans/forestMoan; one-shot
   `WorldSoundCue` for overlap sounds.
4. **P1 — skyFalling** (host-rolled impactLoc + per-player punch) — the one bespoke
   per-player-effect event.
5. **P2 — activeEvents** verify/mirror (tension music) — likely free via M1 replay.
6. Per-event tail by priority (bottomHole, sleep events behind the sleep-UX
   decision, alarmLamp channel, dish-break, door-jam field).

**Recurring pattern across all of it:** host rolls/decides → broadcasts the
RESULT → peers mirror. Never seed-sync RNG (shared stream + divergent call counts
+ no asset edits). Never hook the unobservable fire — sync the observable
spawn/overlap/state.

---

## 7. Cross-cutting notes for the implementer

- **The pinecone is not special.** It is one `prop_food_pinecone_C` on the M2 prop
  route. The 2026-06-11 `pinecone_probe` (force-spawn, ini-gated, now =0) proved
  the spawner Init is unobservable; M2 catches the *spawn call* instead. Keep the
  probe code (dev-only) until M2 is hands-on-verified, then it can go (RULE 2).
- **`prop_food_C` in `RegisterExtraKeyedInitObservers`** (prop_lifecycle.cpp) is a
  SEPARATE, genuine fix for *player-dropped* food props (Init owner + late load) —
  keep it; it is not the spawner path.
- **Double-fire on deterministic volumes** (both peers overlap the same
  level-placed `trigger_box`) is why M3 must be host-authoritative + key-deduped,
  not per-peer-local.
- **File sizes:** `npc_sync.cpp` is the M2 source-of-truth to extract from; create
  `coop/host_spawn_watcher.{h,cpp}` as a new single-feature pair (don't grow
  npc_sync past the 800 soft cap). New reliable kinds (`EventFire`, `WorldSoundCue`)
  go in `protocol.h` (the constants header may grow).

---

**End of master catalog.** Per-class bytecode detail: the four `_events_catalog_*`
section files. Authoritative session record: `votv-connection-selector-events-
pinecone-2026-06-11.md`.
