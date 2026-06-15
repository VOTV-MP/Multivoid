# VOTV TRIGGER classes — coop-sync catalog (group D: trigger volumes + condition triggers) — 2026-06-11

Catalog of the `AtriggerBase_C` family for a coop-sync roadmap. Source of truth: the CXX
header dump (`Game_0.9.0n/.../CXXHeaderDump/trigger_*.hpp`) + kismet-bytecode disassembly
(`tools/bp_reflect.py` -> `research/bp_reflection/trigger_*.json`). VOTV is STOCK
non-nativized UE4.27 -> every trigger is Blueprint bytecode, NOT in IDA. Cite, don't guess.

---

## 0. The architecture every trigger shares (read first)

`AtriggerBase_C : AActor` (`triggerBase.hpp`). The whole family is **level-placed actors**
with a save `Key` (`@0x0260`), an `Objects[]` link array (`@0x0230`), and the
`Iint_ttrigger_C` interface. The universal fire verb is:

- **`runTrigger(AActor* Owner, int32 Index)`** — the BlueprintEvent every concrete trigger
  overrides to produce its effect. The body is a stub (`EX_LocalFinalFunction` ->
  `ExecuteUbergraph_<name>` at a fixed EntryPoint); the real logic lives in the ubergraph.
- `runAll(int32 allIndex)` — base helper; iterates `Objects[]` and calls `runTrigger` on each.
- `setActiveTrigger(AActor*, bool)` — arm/disarm.

### Two firing paths (this is the whole coop story)

1. **Overlap-driven volumes** (`trigger_box` family, `teleporter`, `forestMoan`,
   `locationAmbience`): a `UBoxComponent`/`USphereComponent` overlap fires a
   `BndEvt__...ComponentBeginOverlapSignature` delegate. `trigger_box::ExecuteUbergraph`
   then calls `runAll` -> `runTrigger` on its linked `Objects[]`. So `trigger_box` is the
   **generic dispatcher volume**: walk in, it fires a *list* of linked effect-triggers.

2. **Scheduler-driven story events** (the `event_*` family): the **`trigger_eventer_C`**
   (the scheduler — **OWNED BY A SIBLING AGENT, internals SKIPPED here**) holds named
   pointers to ~38 event triggers (`event_arirShip`, `event_solar`, `event_agrav`,
   `event_bigmRoar`, `event_wisps`, `event_bedEvent`, `event_arirEgg`, …) and calls each
   one's `runTrigger` from `ExecuteUbergraph_trigger_eventer` / `runSpecialEvent`.

### OBSERVABILITY — the decisive fact (binary-proven, do not re-derive)

Our hook is a MinHook detour on `UObject::ProcessEvent` (`0x141465930`). A UFunction is
observable **iff its invocation reaches ProcessEvent** (per
`votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md` §1 +
`votv-firefly-host-mirror-RE-2026-06-09.md` §1):

- **BP->BP calls via `EX_LocalVirtualFunction` (CALLVIRT) -> `ProcessInternal` directly ->
  UNOBSERVABLE.** Verified: `trigger_eventer` dispatches **EVERY** `runTrigger` call via
  CALLVIRT (53 in the ubergraph + 34 in `runSpecialEvent`). `trigger_box::ExecuteUbergraph`
  calls `runAll`/`runTrigger` via CALLVIRT too. **=> A POST observer on `runTrigger` will
  NOT fire for scheduler- OR box-dispatched effects.** (Same class of bug as the
  doors-`sent=0` and `playerGrabbed`-never-fires findings.)
- **Multicast-delegate entries (`BndEvt__...ComponentBeginOverlap`) ARE ProcessEvent-
  dispatched -> OBSERVABLE.** (`OnComponentBeginOverlap.Broadcast -> ProcessMulticastDelegate
  -> ProcessEvent`, the same path the clump hit-handler uses.) So for *overlap* volumes the
  observable edge is the **begin-overlap delegate**, not `runTrigger`.
- **Engine-driven entries (BeginPlay, Tick, timer/latent resumes) -> OBSERVABLE.**

**Practical consequence for sync design:** you cannot cheaply observe "an event fired" by
hooking `runTrigger`. The robust shapes are: (a) observe the **begin-overlap delegate** on
the local player for overlap volumes; (b) **host-authoritative re-dispatch** — the host runs
the scheduler/effect natively and the mod *broadcasts a cue* (the firefly/lightning
precedent), then every peer re-invokes the effect verb via reflected ProcessEvent; or (c)
the effect **already rides an existing synced pipeline** (a spawned prop, a jammed door, a
light) and needs nothing new.

### COOP STATUS across the whole family: NONE handled today

`grep -ri trigger src/votv-coop/src/coop` -> no trigger volume is synced. The only adjacent
work: `interactable_sync.cpp` mirrors **doors / lightswitch-groups / containers / garage /
appliances** (state polling), `keypad_sync`, `weather_sync`, `power_sync`. Triggers are
greenfield. Scope explicitly anticipates them: `docs/COOP_SCOPE.md` Phase 5N3 (interactables:
doors/switches/triggers/keypads) + 5N5 (transient events: one-shot sound/particle/scares).

---

## GROUP 1 — SHARED-WORLD, effect NOT yet covered -> NEEDS NEW SYNC (the short list)

These produce a persistent or perceptible **shared-world** effect that no current subsystem
mirrors. If only one peer triggers, the other never sees it.

| Trigger | What it does (effect, bytecode-confirmed) | Fire condition | Observable edge | Sync shape recommendation | Prio |
|---|---|---|---|---|---|
| **trigger_breakDish** | `ExecuteUbergraph` -> casts to a `dish` and calls **`breakServer`** -> permanently breaks a satellite dish/server (story sabotage). `Index` @0x0298 = which dish; save-restored via `getTriggerData`/`loadTriggerData`. | scheduler (`runTrigger`); save-gated once | runTrigger UNOBSERVABLE (CALLVIRT). The dish-break itself may be catchable on the dish actor. | **Host-authoritative cue.** Host runs it; broadcast a `DishBreak{dishIndex}` reliable; each peer calls `dish->breakServer` (or mirrors the broken dish state). Alternatively the dish is a keyed actor -> a dish-state channel. | HIGH (visible permanent world change) |
| **trigger_jamDoor** | `ExecuteUbergraph` -> casts `Objects[]` to `Door` and calls **`jam`** (door becomes unopenable) **+ `addEmail`** (story email, per-player). | scheduler (`runTrigger`) | runTrigger UNOBSERVABLE | Door jam is a **door-state extension**: fold `jammed` into the existing door channel (`interactable_sync` door adapter) so the poll mirrors it. Email is per-player (skip). | HIGH (a jammed door blocks both peers; today only the trigger-er is blocked) |
| **trigger_spawnProp** | Spawns a prop of class `prop` (FName) at the trigger, optional `frozen/Static/Active`, links it to an `onSpawned_trigger` + a `door`. The spawned actor is a **shared-world prop**. | scheduler/box; save-gated (`spawned`/`spawned_key`) | runTrigger UNOBSERVABLE; the spawn is `SpawnActor` (native, unobservable) | **Route through the existing PROP pipeline.** Host spawns -> the prop-lifecycle / EntitySpawn path already mirrors world props by key. Likely "covered transitively IF the spawned prop is caught by prop_lifecycle"; verify the spawn class is one prop_snapshot tracks. Else add a host-spawn cue. | HIGH |
| **trigger_solarBoom** | `ExecuteUbergraph` -> **`PlaySound2D`** (the solar-flare BOOM, a global 2D world sound) + **`solar`** -> drives linked `trigger_lightRoot[]` dark (blackout). | scheduler (eventer `event_solar`) | runTrigger UNOBSERVABLE | The **lightRoot blackout** is covered-transitively IF lights are synced (see G2). The **2D boom sound is NOT covered** -> needs a one-shot **sound-cue broadcast** (a generic `WorldSoundCue{soundId,2D}` reliable; each peer PlaySound2D). | HIGH (a scare-class global sound; both must hear) |
| **trigger_bigmRoar** | One-shot: plays the "bigman" roar (a global scare sound/VO). Bare ubergraph (`runTrigger` only). | scheduler (eventer `event_bigmRoar`) | runTrigger UNOBSERVABLE | Generic **sound-cue broadcast** (same `WorldSoundCue` vehicle as solarBoom). | HIGH (jump-scare; both must hear) |
| **trigger_forestMoan** | Sphere overlap -> plays the forest "moan" ambient-scare. **OVERLAP-driven** (`BndEvt__Sphere...BeginOverlap`). | LOCAL player sphere overlap; repeatable | **OBSERVABLE** (begin-overlap delegate) | Borderline. The moan is positional ambience but scare-flavored. If treated as shared: observe the overlap on the local player -> broadcast `WorldSoundCue` (3D at trigger pos). If treated as per-player ambience -> NO-SYNC. **Recommend MED**: broadcast so a co-located peer shares the scare. | MED |
| **trigger_wispSwarm** | Spawns/triggers the wisp-swarm creature event (bare ubergraph). | scheduler (eventer `event_wisps`) | runTrigger UNOBSERVABLE | If wisps are NPC/particle actors -> **route through NPC/entity pipeline** (host spawns, EntitySpawn mirrors). Else host-cue + per-peer spawn. Verify what `solar`/wisp spawns. | MED |
| **trigger_spawnFollowingArir** | Spawns a "following ariral" creature that pursues the player (bare ubergraph; eventer `event_arirFollower`). | scheduler | runTrigger UNOBSERVABLE | **NPC pipeline.** A following-creature is a host-authoritative AI NPC -> the existing NPC sync (host spawns + EntitySpawn + pose stream) should own it. Per-player "follows ME" semantics need a target decision (whom does it chase?). | MED (depends on NPC sweep) |
| **trigger_bloodSkeleton** | Spawns/reveals the blood-skeleton scare prop (bare ubergraph). | scheduler | runTrigger UNOBSERVABLE | Likely a spawned scare actor -> prop/entity pipeline OR host-cue. | MED |
| **trigger_arirEgg** | Reveals/activates the ariral egg (a `snd` billboard + ubergraph). | scheduler (eventer `event_arirEgg`) | runTrigger UNOBSERVABLE | Spawned/activated world object -> prop pipeline or host-cue. | LOW-MED |
| **trigger_eventt_arirShip** | The arir-SHIP story event: `Active` flag + `alarmLamp_C[]` (drives alarm lamps on). | scheduler (eventer `event_arirShip`) | runTrigger UNOBSERVABLE | Alarm-lamp state is shared-world -> a small **alarmLamp state channel** (or fold into a generic keyed-toggle once alarmLamp gets a key). The ship spawn (if any) -> entity pipeline. | MED |
| **trigger_alarm** | Drives `alarmLamp_C[]` SetActive + a sound + `setEvent` (a base alarm). | scheduler/key | runTrigger UNOBSERVABLE | Same as above: **alarmLamp state channel** + the alarm sound via `WorldSoundCue`. | MED |
| **trigger_vehtp** | **Teleports the ATV** (`Vehicle` = `AATV_C`) to `Pos`: SetWorldLocation + visibility/shadow + **`teleportVehicle`**. The ATV is a SHARED synced actor (AtvState v47). | scheduler/box | runTrigger UNOBSERVABLE | The ATV already has a pose stream (`atv_sync`). A teleport is a discrete jump -> either the pose stream catches the new transform (occupant-authoritative) OR add a `VehicleTeleport{key,transform}` reliable so a non-occupant peer's mirror snaps. **Coordinate with the ATV owner.** | MED |
| **trigger_tpChamberSpawn** | Spawns/reveals the teleporter-chamber (light timeline + `chamberAppear` audio + a blur UI + a `physEvent`). | scheduler/overlap | runTrigger UNOBSERVABLE | The chamber prop/light is shared-world (spawn -> prop pipeline; the blur UI is per-player). The appear-audio -> `WorldSoundCue`. | LOW-MED |

### The clean primitive these mostly want: a generic one-shot cue channel

Many GROUP-1 effects are **one-shot, non-stateful, scare-class**: a sound, a particle, a
"this happened" pulse. Rather than a bespoke reliable per trigger, the high-leverage build is
**one generic `WorldEventCue` reliable** carrying `{cueType, key/index, optional transform}`
(MTA shape: a server-broadcast event the client replays). solarBoom-boom, bigmRoar,
forestMoan, alarm-sound, chamber-appear, dish-break, door-jam all collapse onto it. This is
the recommended first build for group D and the natural home for the eventer's story events
(host runs the scheduler natively, the mod taps the *result* and broadcasts the cue —
exactly the firefly/lightning pattern, since the scheduler's `runTrigger` dispatch is
unobservable so we sync at the RESULT level, not the call).

---

## GROUP 2 — COVERED TRANSITIVELY (effect rides an already-synced subsystem; verify, don't rebuild)

| Trigger | Effect | Rides which synced subsystem | Action |
|---|---|---|---|
| **trigger_lightRoot** | `SetActive`/`flick` on linked `ceilingLamp_C[]` + `ambientLight_C[]` (a room's lights on/off, incl. flicker). | The **light** subsystem. NOTE: today's light channel is *lightswitch groups* — confirm `ceilingLamp_C`/`ambientLight_C` driven by a lightRoot are caught by the same state poll. If not, lightRoot is a **gap** (extend the light channel to these classes). | VERIFY the light channel covers ceilingLamp/ambientLight. If yes: covered. If no: HIGH (lights desync). |
| **trigger_solarBoom** (light half) | `solar` drives lightRoots dark (blackout). | Same as lightRoot. | Covered IFF lightRoot is. (The boom *sound* half stays in GROUP 1.) |
| **trigger_spawnProp** (prop half) | Spawns a world prop by class+key. | The **prop lifecycle / EntitySpawn** pipeline (mirrors world props by Key). | VERIFY prop_snapshot/prop_lifecycle tracks the spawned class. If yes: covered. If no: host-spawn cue (GROUP 1). |
| **trigger_jamDoor** (door half) | `jam` on a `Door`. | The **door** channel (`interactable_sync`). | Extend the door state to carry `jammed` (a door-channel field add), then covered by the existing poll. |
| **trigger_vehtp** (ATV transform) | Teleports `AATV_C`. | The **ATV pose stream** (`atv_sync`, v47). | The occupant-authoritative pose stream may already carry the post-teleport transform. Verify; add a discrete teleport reliable only if the snap is too coarse. |
| **trigger_box / _box_AB / _box_N / _box_rain** | Generic dispatcher volume: overlap -> `runAll` -> `runTrigger` on linked `Objects[]`. `_AB` = friend-check gate, `_N` = N-count gate, `_box_rain` = toggles `rain` (a weather flag). | **No effect of its own** — it just *dispatches to its linked triggers*, each of which is catalogued on its own row. `_box_rain` rides the **weather** subsystem (rain flag). | The box itself is plumbing — DON'T sync the box; sync the **linked effect-triggers** it fires (their rows above). For determinism, see "double-fire" note below. `_box_rain` -> weather (covered). |
| **trigger_TBoxActivator** | Arms/disarms a linked `trigger_box` (`TriggerBox` + key). | Plumbing for a box -> no direct effect. | No sync (covered via the box's linked effects). |

### Double-fire / dedup note for the box dispatchers (the "both peers overlap" case)

A `trigger_box` is a **deterministic, level-placed volume** — if BOTH peers walk through, BOTH
locally fire `runAll` -> the linked effect runs **twice**. For idempotent effects (lights,
door open) that's harmless. For **non-idempotent** ones (spawn a prop, break a dish, play a
one-shot scare) it **double-spawns / double-breaks / double-plays**. Whatever sync shape the
linked effect uses MUST be **host-authoritative + dedup'd by the trigger Key** (the host owns
"this keyed event fired once", like the firefly/lightning host-relay). This is the strongest
argument for routing all GROUP-1 effects through ONE host-owned cue channel keyed by trigger
Key, rather than letting each peer fire locally.

---

## GROUP 3 — PER-PLAYER LOCAL -> NO SYNC (and why)

| Trigger | Effect | Why per-player (no sync) |
|---|---|---|
| **trigger_achievement** | Grants achievement `achiv` (FName) to the overlapping player. | Achievements are per-player by design; each peer earns their own. |
| **trigger_notif** | Shows on-screen notification `Text` (FText) to the player. | A HUD message for the one player who triggered it. Per-player UI. |
| **trigger_agrav** | Anti-gravity field: lifts the **player's own held/nearby props** (`props[]`, `vel`, a timeline + cloak) inside `Bounds` for that player. | Acts on the local player's physics/held objects; the agrav-on-self is inherently local. (If it lifts *shared* world props, a held-prop pose stream already mirrors those — verify, but default NO-SYNC.) |
| **trigger_ambientSound** | Positional ambient `Audio` with distance volume bands (`Volume_0..3`, `volume_3_isBlackHole`); `upd`/`setVol2`. | Ambient soundscape each player hears based on their own position. Per-player by design. |
| **trigger_sound_DUPL_1** (`trigger_sound_C`) | Plays `sound_data` (Fstruct_sound) via an `Audio` component at the trigger. `SetSound`/`makeSoundData`. | A placed ambient/positional sound source. Per-player audition (each client's audio engine plays it locally based on proximity). NOTE: if a specific instance is a *scare* one-shot wired to a story beat, that beat fires via the eventer -> handle the SCARE at the event level (GROUP 1 cue), not by syncing this ambient emitter. |
| **trigger_locationAmbience** | Sphere overlap -> `Activate` a location ambience `Audio` (Fstruct_sound), `played` latch. | Per-player ambient zone audio. Overlap-observable but no shared effect. |
| **trigger_teleporter** (`trigger_box` child) | Overlap -> `K2_GetActorLocation`/`K2_SetActorLocation`: teleports the **overlapping player's own pawn**. | Moves the local pawn only. Each peer who enters teleports themselves; the peer's resulting position is already carried by the normal player pose stream. NO new sync. |
| **trigger_lockerLooker** | `ReceiveTick` watches if the player is looking (a `cube` + `Run` flag) — drives a per-player look-gated micro-event. | Per-player gaze condition; local. |
| **trigger_bedEvent** | Fires the "sleep in bed" dream/event for the player who used the bed (bare ubergraph). | Sleeping is a per-player action (one player sleeps). The *dream* is that player's local sequence. (If sleeping advances SHARED world time, that rides the **time_sync** subsystem — covered there, not here.) |
| **trigger_fakeLmaos** | Spawns a FAKE/illusory `dish` (`dishID`) visible as a hallucination. | If it's a per-player hallucination (VOTV scare style), it's local. VERIFY: if the fake dish is meant to be a shared world object, promote to GROUP 1. Default NO-SYNC (hallucination). |
| **trigger_forceObject** | Sets a "force object" FName (`forceObject`) — forces a held/interaction object context for the player. | Per-player interaction-context hint; local. |
| **trigger_destroyByKeys** | On fire, destroys actors whose keys are in `objectKeys[]` (cleanup). | A world-cleanup. **Borderline:** if it destroys SHARED props, it should ride the prop-destroy pipeline (host runs cleanup -> PropDestroy mirrors). But it's save/script cleanup, deterministic on both peers -> default covered-by-determinism; if a desync appears, route the destroys through PropDestroy. |
| **trigger_destroyInRadius** | Destroys actors of `Filter[]` classes within `Radius` (a sphere cleanup). | Same as destroyByKeys: deterministic radial cleanup. Covered-by-determinism; promote to host-authoritative PropDestroy only if a divergence is observed. |
| **trigger_delay_DUPL_1** (`trigger_delay_C`) | Pure timing relay: waits `Delay` then fires its `Objects[]`. | Plumbing (no effect of its own); the *linked* effect is what matters. No sync for the delay itself. |
| **trigger_button** (`trigger_button_C`) | **NOT a volume** — it's a grabbable/steppable PHYSICAL button PROP (`playerGrabbed`, `playerStepped`, `player_use`, container/food interfaces). Inherits triggerBase but behaves as a prop that, when pressed, fires its linked `Objects[]`. | The button *object* itself rides the **prop pipeline** (it's a held/placed prop). Its press fires linked effect-triggers -> those are the rows above. The button is NOT a sync target on its own; its press-effect is. |

---

## Appendix — full class inventory (33 in scope) with parent + key fields

```
trigger_achievement      : AtriggerBase_C   achiv(FName)                                 PER-PLAYER (achievement)
trigger_agrav            : AtriggerBase_C   Bounds, props[], timeline                    PER-PLAYER (self-physics)
trigger_alarm            : AtriggerBase_C   Active, alarms[](alarmLamp_C)                SHARED (alarm lamps+sound)   G1
trigger_ambientSound     : AtriggerBase_C   Audio, Volume_0..3, isBlackHole             PER-PLAYER (ambient)
trigger_arirEgg          : AtriggerBase_C   snd(Billboard)                               SHARED (egg reveal)          G1
trigger_bedEvent         : AtriggerBase_C   (bare)                                       PER-PLAYER (dream) / time-sync
trigger_bigmRoar         : AtriggerBase_C   (bare)                                       SHARED (roar sound)          G1
trigger_bloodSkeleton    : AtriggerBase_C   (bare)                                       SHARED (skeleton scare)      G1
trigger_box              : AtriggerBase_C   Box, Filter[], IsActive  [DISPATCHER]        plumbing -> linked effects   G2
trigger_box_AB           : trigger_box_C    checkForFriend()                             plumbing (friend-gated)      G2
trigger_box_N            : trigger_box_C    N(int)                                       plumbing (count-gated)       G2
trigger_box_rain         : trigger_box_C    rain(bool)                                   weather (rain flag)          G2/weather
trigger_breakDish        : AtriggerBase_C   Index -> dish::breakServer                   SHARED (dish broken)         G1 HIGH
trigger_button           : AtriggerBase_C   PHYSICAL PROP (playerGrabbed/Stepped)        prop pipeline (the button)
trigger_delay_DUPL_1     : AtriggerBase_C   Delay -> linked Objects[]                    plumbing (timer)
trigger_destroyByKeys    : AtriggerBase_C   objectKeys[]                                 cleanup (determinism/PropDestroy)
trigger_destroyInRadius  : AtriggerBase_C   Sphere, Filter[], Radius                     cleanup (determinism/PropDestroy)
trigger_eventer          : AtriggerBase_C   ~38 event_* ptrs + runEvent  [SCHEDULER]     *** SIBLING AGENT OWNS ***
trigger_eventt_arirShip  : AtriggerBase_C   Active, alarms[]                             SHARED (ship event+lamps)    G1
trigger_fakeLmaos        : AtriggerBase_C   dishID, dish(fake)                           PER-PLAYER (hallucination?)  verify
trigger_forceObject      : AtriggerBase_C   forceObject(FName)                           PER-PLAYER (interaction ctx)
trigger_forestMoan       : AtriggerBase_C   Sphere -> moan  [OVERLAP, OBSERVABLE]        SHARED-ish (scare sound)     G1 MED
trigger_jamDoor          : AtriggerBase_C   Door::jam + addEmail                         SHARED (door jam)            G1 HIGH -> door chan
trigger_lightRoot        : AtriggerBase_C   ceilingLamp[]/ambientLight[] SetActive       lights (covered? verify)     G2
trigger_locationAmbience : AtriggerBase_C   Box, Audio, played                           PER-PLAYER (zone ambience)
trigger_lockerLooker     : AtriggerBase_C   cube, Run (ReceiveTick gaze)                 PER-PLAYER (gaze gate)
trigger_notif            : AtriggerBase_C   Text(FText)                                  PER-PLAYER (HUD notif)
trigger_solarBoom        : AtriggerBase_C   roots[] + PlaySound2D + solar                SHARED (boom sound + lights) G1 HIGH(sound)/G2(lights)
trigger_sound_DUPL_1     : AtriggerBase_C   Audio, sound_data                            PER-PLAYER (ambient emitter)
trigger_spawnFollowingArir: AtriggerBase_C  (bare)                                       SHARED (chasing NPC)         G1 MED -> NPC pipeline
trigger_spawnProp        : AtriggerBase_C   prop(FName), spawned, frozen/Static          SHARED (world prop spawn)    G1 HIGH -> prop pipeline
trigger_TBoxActivator    : AtriggerBase_C   TriggerBox, triggerBoxKey                    plumbing (arms a box)        G2
trigger_teleporter       : trigger_box_C    overlap -> SetActorLocation(self)            PER-PLAYER (self teleport)
trigger_tpChamberSpawn   : AtriggerBase_C   light tl + chamberAppear audio + blur        SHARED (chamber) + PP(blur)  G1 LOW-MED
trigger_vehtp            : AtriggerBase_C   Vehicle(ATV) teleportVehicle                 SHARED (ATV teleport)        G1 MED -> atv_sync
trigger_wispSwarm        : AtriggerBase_C   (bare)                                       SHARED (wisp creatures)      G1 MED -> NPC/particle
```

(`trigger_eventer` internals deliberately omitted — owned by the scheduler agent; it is the
CALLVIRT dispatcher that makes `runTrigger` unobservable, which is why GROUP-1 effects must
be synced host-authoritatively at the RESULT level, not by hooking `runTrigger`.)
