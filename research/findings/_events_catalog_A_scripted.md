# VOTV scripted-event catalog (A) — coop-sync roadmap

**Date:** 2026-06-11
**Author:** events-catalog agent (A — individual event EFFECTS + output types)
**Sibling:** a separate agent REs the central scheduler
(`struct_event` / `trigger_eventer` / `grayEventController`). This doc
focuses on the **effect** of each event class + the cheapest sync shape.
**Methods:** CXX header dump (`Game_0.9.0n/.../CXXHeaderDump/*.hpp`) +
kismet bytecode disassembly via `tools/bp_reflect.py` (output in
`research/bp_reflection/<name>.json`). VOTV is STOCK UE4.27 — every BP is
kismet bytecode, not native; offsets/calls below are cited from the
disassembly, not guessed.
**Predecessor:** `research/findings/votv-events-RE-2026-05-24.md` (the
original event-system RE — trigger_eventer dispatcher, wire-shape proposal,
strategy taxonomy). This doc supersedes its per-event detail for the 16
classes in scope with bytecode-verified specifics.

---

## TL;DR — the 10-line answer

HIGH-priority events (a shared scare/world-change both players must witness
together), with their OUTPUT TYPE:

1. **skyFallingEvent** — PLAYER-effect + PROP SPAWN + SOUND/FX. The meteor/sky-debris crash: spawns `explosion_C` + `Aprop_skypiece_C` debris, **`punch()` + SetPhysicsLinearVelocity on the main player** (knocks/ragdolls them), flashbang. The single biggest scare in scope.
2. **event_fossilBoarWar** — NPC SPAWN. Spawns `Afossilhound_C` (Character) vs `Agrayboar_C` (prop) packs fighting between two box bounds; RNG positions; camera shake.
3. **event_fleshRain** — PROP SPAWN (rain of `Aprop_garbageClump_C` from sky) + `setEvent` active-event tension music.
4. **event_arirStealsGun** — NPC SPAWN/ACTION. Spawns `Anpc_arirGunStealer_C`, removes the player's `Aprop_arirGun_C` (an inventory/world change).
5. **event_bottomHoleController** — WORLD-STATE + NPC. Activates the bottom-hole room: `AkavotiaPatrolController_C` patrol + `Aprop_rozitalBeacon_C` beacons + laser emitters; overlap-gated.
6. **event_lightsTurnoffer / event_passwordGuesser** — WORLD-STATE (a scripted scare): kills the lightswitch (`use`), slams doors shut (`doorOpen`), re-locks a `ApasswordLock_C` — operates on KEYED interactables.
7. **bedEvent / event_arirTreehouseSleep** — PLAYER-effect (sleep/wake + `teleportWObackrooms` teleport). MED-HIGH; tied to the shared sleep-cycle UX problem.
8. **event_vaccine / event_funnyGascans / event_trashPiles** — PROP SPAWN (overlap-driven loot/gag/garbage). MED — mostly auto-covered by the prop pipeline once the trigger is shared.
9. **event_consoleWrite** — VISUAL/UI only (scripted "hacker" text to the in-game console). LOW-MED, cosmetic per-player.
10. **redSkyEvent** — **DONE** (synced via `coop/weather_redsky` — host observes spawnRedSky/set, client mirrors). Do not redo. **event_arirFuelsAtv** is LOW (single ATV refuel cosmetic).

File: `research/findings/_events_catalog_A_scripted.md`

---

## How the output type maps to a sync shape (legend)

| OUTPUT TYPE | Cheapest sync | Closest precedent in our codebase |
|---|---|---|
| PROP SPAWN (`Aprop_C`-derived) | **free-ish** — the prop rides the existing PropSpawn pipeline once the trigger fires on the host; only the host must run the trigger. | `prop_lifecycle.cpp` / `remote_prop_spawn.cpp` (Inc2 `Aprop_C::Init` POST observer) |
| PROP SPAWN (`AActor`, e.g. garbageClump) | eid-routed via the clump rails (non-keyable, key=None). | `garbage_sync.cpp` + `trash_collect_sync.cpp` |
| NPC SPAWN/ACTION (`ACharacter`) | host-auth spawn → NPC sync manifest mirrors it. | the NPC entity sync layer (kerfur/mannequin precedent) |
| SOUND-only / VISUAL/FX | host-detect + 1 reliable broadcast → client replays the cue locally (no pose). | `firefly_sync.cpp` (ambient spawn), `weather_lightning` (reflected SpawnEmitter) |
| WORLD-STATE flag | host writes → broadcast the state → client mirrors the same field/UFunction. | `weather_sync.cpp`, the keyed-state channels (`interactable_sync` / `power_sync`) |
| PLAYER-effect (teleport/punch/vitals) | host fires on the AUTHORITATIVE player; broadcast a per-target effect packet; client applies to its puppet/local player. | (new — no exact precedent; closest is the ragdoll/throw path) |

**Universal note on OBSERVABILITY (proven repeatedly, incl. the pinecone
today):** every event here does its spawning via
`SpawnActor`/`BeginDeferredActorSpawnFromClass` whose `Init`/BeginPlay runs
as **BP-internal kismet** (`EX_LocalVirtualFunction` →
ProcessInternal-direct, or `EX_CallMath` native-thunk) — these **bypass our
ProcessEvent detour**, so the spawn is NOT catchable at Init. The robust hook
point is the **TRIGGER** (the overlap `BndEvt`, the `runTrigger`, or a
gamemode UFunction reachable via ProcessEvent), NOT the spawned actor's birth.
This is why every recommendation below hooks the trigger and broadcasts, not
the payload.

**Universal note on RNG:** VOTV BP RNG (`RandomFloatInRange` /
`RandomInteger`) is NOT seed-synced across peers (each peer's FRandomStream
seeds from system time). Any event that rolls positions/choices/timings will
**DIVERGE** if both peers run it independently → host-authoritative is
required for those. (Same conclusion as the wind/firefly work: sync the
RESULT, not the seed.)

---

## Per-event entries

### 1. `AskyFallingEvent_C`  (`skyFallingEvent.hpp`, `objects/misc/`)

- **WHAT IT DOES:** The sky-falling / meteor crash scare. Under its own
  `ReceiveTick` it flies a trajectory (Audio approach-loop + PointLight glow
  + `eff_glow_tardis` particles + radarPoint), then at impact: spawns
  `Aexplosion_C` + `Ashake_explosion_C` (camera shake) + `Aui_flashbang_C`
  + `Aprop_skypiece_C` debris, **calls `getMainPlayer().punch(...)` and
  `SetPhysicsLinearVelocity` on the player** (physically knocks/ragdolls the
  player), then `K2_DestroyActor` self. Disasm calls:
  `setEvent`, `getMainPlayer`, `punch`, `SetPhysicsLinearVelocity`,
  `SetFloatParameter`, `K2_DestroyActor`. String params: `damage`, `force`,
  `shake`, `chest`, `opacity`, `time`.
- **TRIGGER:** Spawned by `AtriggerFallingSky_C.Sphere` BeginOverlap (player
  walks into the trigger sphere) → scheduler-adjacent (`triggerFallingSky.hpp`).
- **OUTPUT TYPE:** PLAYER-effect (punch+velocity) **+** PROP SPAWN
  (skypiece, `Aprop_skypiece_C : Aprop_C`) **+** SOUND/VISUAL/FX (explosion,
  flashbang, shake) **+** WORLD-STATE (`setEvent` registers active event).
- **OBSERVABILITY:** the trigger overlap `BndEvt` is ProcessEvent-observable;
  the spawned explosion/skypiece Init is BP-internal (not catchable at birth);
  the `punch`/SetPhysicsLinearVelocity on the player are
  `EX_LocalVirtualFunction`/`EX_VirtualFunction` — NOT catchable at the call
  site from a detour.
- **RNG:** YES — trajectory + impact offset rolled per-peer → diverges.
- **CURRENT COOP STATUS:** NOT synced. (`skyFalling` only appears in the
  2026-05-24 RE doc as §1, never implemented. `Aprop_skypiece_C` would ride
  the prop pipeline if a host spawned it, but the trigger + the player-punch
  are not handled.)
- **RECOMMENDED SYNC SHAPE:** **NEW host-detect + broadcast.** Host hooks the
  `AtriggerFallingSky_C` overlap (suppress on client), broadcasts
  `EventStart(sky_fall, impactLoc, seed)`. Client spawns its own visual-only
  `AskyFallingEvent_C` at impactLoc (cheap: it runs its own timeline; do NOT
  stream pose). The **player-punch must be a per-player effect**: each peer
  applies the punch/velocity to ITS OWN local player when the impact lands
  near them (otherwise only the host player ragdolls). The skypiece debris
  rides PropSpawn. Precedent: firefly_sync (ambient host-detect+broadcast) for
  the spawn; the explosion/shake replay mirrors weather_lightning's reflected
  SpawnEmitter.
- **COOP-RELEVANCE PRIORITY:** **HIGH** — loud, physical, both players must
  see/feel it together.

---

### 2. `Aevent_fossilBoarWar_C`  (`event_fossilBoarWar.hpp`, `objects/misc/`)

- **WHAT IT DOES:** A "boar war" — spawns packs of `Afossilhound_C` (the
  fossilhound Character) and `Agrayboar_C` (a prop-based boar) at random
  locations between two box bounds (`Min`/`Max`) and lets them fight; uses
  `Apiramid PingShake_C` for camera shake; `gibLifespan` controls gib cleanup.
  Disasm: `spawnLoc` (the random-point helper), `setEvent`, `obj_statDyn`,
  `K2_DestroyActor`.
- **TRIGGER:** `ReceiveBeginPlay` (the actor is placed/spawned and self-runs);
  effectively scheduler/story-spawned. `spawnLoc()` rolls each spawn position.
- **OUTPUT TYPE:** NPC SPAWN (`Afossilhound_C : ACharacter`) + PROP-ish NPC
  (`Agrayboar_C : Aprop_C`) + VISUAL/FX (shake/gibs) + WORLD-STATE (`setEvent`).
- **OBSERVABILITY:** BeginPlay + spawns are BP-internal (not catchable at the
  spawned actors' Init).
- **RNG:** YES — `spawnLoc()` rolls positions inside the box per-peer → packs
  would be in different places on each peer.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **Host-authoritative.** Let only the host run
  the event (suppress the client's instance / skip its BeginPlay spawning). The
  fossilhounds replicate via NPC sync; the grayboars via the prop pipeline. If
  NPC sync isn't ready for these classes yet, a fallback is host broadcasts
  `EventStart(boar_war, seed, boxMin, boxMax)` and the client re-rolls with the
  same seed — but seed-replay is fragile (RNG call-count drift); prefer
  host-auth spawn + NPC/prop mirror. Precedent: the ticker host-auth model in
  the 2026-05-24 RE §4.6.
- **COOP-RELEVANCE PRIORITY:** **HIGH** — a combat spectacle; divergent
  positions are very visible.

---

### 3. `Aevent_fleshRain_C`  (`event_fleshRain.hpp`)

- **WHAT IT DOES:** A rain of flesh — loops (`I` counter) spawning
  `Aprop_garbageClump_C` actors falling from the sky (the same garbage-clump
  actor used by the trash system, here as "flesh"); `chipType` set on each;
  `SetLifeSpan` cleans them; `setEvent` registers the active event;
  `piramidPingShake_C` shake; `K2_DestroyActor` self when done.
- **TRIGGER:** `ReceiveBeginPlay` (placed/story-spawned `Aactor_save_C`).
- **OUTPUT TYPE:** PROP SPAWN (`Aprop_garbageClump_C : AActor` — NON-`Aprop_C`,
  the clump actor) + VISUAL/FX (shake) + WORLD-STATE (`setEvent`).
- **OBSERVABILITY:** BeginPlay + per-iteration spawns BP-internal.
- **RNG:** YES — drop positions + count rolled per-peer.
- **CURRENT COOP STATUS:** NOT synced as an event. NOTE: the clump actor
  itself IS eid-routable through the clump rails (`garbage_sync` /
  `remote_prop_spawn` — key=None clumps ride the eid pipeline), so a
  host-spawned clump CAN mirror — but only if the host runs the event and the
  spawn is captured.
- **RECOMMENDED SYNC SHAPE:** **Host-authoritative + reuse clump pipeline.**
  Host runs the event; each spawned `Aprop_garbageClump_C` mirrors via the
  existing eid-routed clump path (the same path trash uses). Broadcast a single
  `EventStart(flesh_rain)` for the shake/active-event tension; the clumps ride
  the prop/clump rails. Precedent: garbage_sync (Inc 3 host-auth gating of
  garbage spawners) + the clump eid routing.
- **COOP-RELEVANCE PRIORITY:** **HIGH** (shared horror set-piece; many falling
  actors that would otherwise differ wildly per-peer).

---

### 4. `Aevent_arirStealsGun_C`  (`event_arirStealsGun.hpp`)

- **WHAT IT DOES:** An ariral steals the player's gun. Per-tick
  (`ReceiveTick`) polls the gamemode/daynightCycle conditions; when armed,
  spawns `Anpc_arirGunStealer_C` (a Character NPC that runs in, grabs the gun)
  and removes/relocates the player's `Aprop_arirGun_C`. Disasm: only
  `getMainGamemode` shows as a call from the tick (the spawn/steal is in the
  ubergraph body); class refs confirm `npc_arirGunStealer_C` + `prop_arirGun_C`.
- **TRIGGER:** Per-tick condition check (`ReceiveTick`) — autonomous, gated on
  game time / gamemode state (no overlap volume; no central scheduler slot).
- **OUTPUT TYPE:** NPC SPAWN/ACTION (`Anpc_arirGunStealer_C : ACharacter`) +
  PLAYER inventory/world change (the gun prop is taken).
- **OBSERVABILITY:** tick is per-frame BP; the spawn + gun-removal are
  BP-internal.
- **RNG:** LIKELY (timing/qualification roll) → diverges per-peer.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **Host-authoritative.** Skip the client's
  per-tick check (peer-role gate, like the ticker model). Host spawns the NPC
  (mirrors via NPC sync) and the gun-removal must broadcast a targeted
  state change (the gun is a keyed `Aprop_C` — its removal/relocation should
  ride the prop lifecycle destroy/move). Precedent: NPC sync + prop lifecycle.
  Watch out: this mutates a PLAYER's inventory — the host must decide WHOSE gun
  (the design must pick the target player; default = host's, or nearest).
- **COOP-RELEVANCE PRIORITY:** **HIGH** (an NPC actively interacts with a
  player + removes an item — a gameplay-affecting scare, not cosmetic).

---

### 5. `Aevent_bottomHoleController_C`  (`event_bottomHoleController.hpp`)

- **WHAT IT DOES:** Controls the "bottom hole" room sequence. On `Box`
  BeginOverlap it activates: an `AholeBottom_C` room + `AholeDevice_C` +
  `AkavotiaPatrolController_C` (a patrolling enemy controller) +
  `Aprop_rozitalBeacon_C` beacons + `AlaserEmitter_C` lasers; manipulates
  props via `getObjectFromKey` / `asProp` / `setPropProps` / `SetMobility` /
  `SetPhysicsLinearVelocity`; `ReceiveDestroyed` tears it down
  (`K2_DestroyActor`). Also references `prop_donut_C` (a set-piece prop).
- **TRIGGER:** `Box` BeginOverlap `BndEvt` (player enters the hole room) —
  overlap-driven, KEY-resolved props (`getObjectFromKey`).
- **OUTPUT TYPE:** WORLD-STATE (room activation + patrol controller) + NPC
  (kavotia patrol) + PROP manipulation (beacons/lasers/donut, keyed).
- **OBSERVABILITY:** overlap `BndEvt` is observable; the prop/NPC activation is
  BP-internal.
- **RNG:** low/none obvious (mostly keyed-deterministic), but the patrol
  controller has its own AI RNG.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **Host-detect + broadcast (room activation as a
  WORLD-STATE flag)**; the patrol enemy via NPC sync; the beacons/lasers/donut
  are KEYED props → ride the keyed-interactable + prop-lifecycle channels.
  Because it's keyed and largely deterministic, the cheapest path is: host
  broadcasts `EventStart(bottom_hole_active)` and the client mirrors the keyed
  prop states + spawns the patrol via NPC sync. Precedent: the keyed-state
  channels (`interactable_sync`/`power_sync`) for the room props + NPC sync.
- **COOP-RELEVANCE PRIORITY:** **HIGH** (a full enemy-room sequence; both must
  share the patrol + hazards). Note: deep location-gated content — schedule
  AFTER the broadly-encountered scares.

---

### 6. `Aevent_lightsTurnoffer_C` & `Aevent_passwordGuesser_C`
(`event_lightsTurnoffer.hpp`, `event_passwordGuesser.hpp` — IDENTICAL field
layout; same `lightswitch`/`door`/`passlock`/`door2`/`GameMode` slots and the
same `lightsTurnoffer_Box` BndEvt)

- **WHAT IT DOES:** A scripted "the lights go out / you're locked in" scare. On
  `Box` BeginOverlap: turns off the `Alightswitch_C` (`use`), shuts the two
  `Adoor_C` doors (`doorOpen` — toggled closed), and engages the
  `ApasswordLock_C` (re-locks). Disasm: `doorOpen`, `use`, `getObjectFromKey`,
  `SetCollisionEnabled`, `getMainGamemode`. `navModifierBox_obs_C` updates AI
  nav (door now blocks). `passwordGuesser` is the variant where the lock is
  *guessed* (a code-entry beat) vs simply forced — same actors, same trigger.
- **TRIGGER:** `Box` BeginOverlap `BndEvt` (player walks into the rigged room).
- **OUTPUT TYPE:** WORLD-STATE (lightswitch off, doors shut, lock engaged — all
  KEYED interactables).
- **OBSERVABILITY:** overlap `BndEvt` observable; the door/light/lock writes are
  `EX_LocalVirtualFunction` (BP-internal) but they ACT on classes our keyed
  channels already mirror.
- **RNG:** none (deterministic given the overlap).
- **CURRENT COOP STATUS:** NOT synced as an event. BUT the underlying
  interactables (`Adoor_C` via door sync, `Alightswitch_C`, `ApasswordLock_C`)
  are ALREADY synced by our keyed channels — so if the host runs the event, the
  resulting door/light/lock STATE CHANGES already propagate. Only the
  client-side double-fire (if the client also overlaps) needs gating.
- **RECOMMENDED SYNC SHAPE:** **Mostly FREE — host-authoritative gate only.**
  Make the event host-authoritative (suppress the client `Box` overlap so it
  doesn't re-fire), and the door/light/lock state already mirrors via the
  existing keyed channels. Optionally broadcast `EventStart(lights_off)` for a
  one-shot scare sound. Precedent: `interactable_sync` (door/lightswitch) +
  `keypad_sync`/passwordlock channel — they already carry the state.
- **COOP-RELEVANCE PRIORITY:** **MED-HIGH** (a shared scare, but the heavy
  lifting is already done by the interactable channels — only the trigger gate
  is new).

---

### 7a. `AbedEvent_C`  (`bedEvent.hpp`)

- **WHAT IT DOES:** The bed/sleep event. On `woken()` (called when the sleep
  cycle ends) it acts on the player: `getMainPlayer`, `teleportWObackrooms`
  (teleports the player — can pull them out of/into the backrooms),
  manipulates a held prop (`asProp`/`setPropProps`/`propThrown_C`), and
  `K2_DestroyActor`. A save-actor (`Aactor_save_C`) so it persists.
- **TRIGGER:** Driven by the sleep cycle — `Atrigger_bedEvent_C.runTrigger`
  (a scheduler slot, `event_bedEvent` on `trigger_eventer`) → calls
  `woken()`. So: scheduler/sleep-driven, NOT overlap.
- **OUTPUT TYPE:** PLAYER-effect (teleport + prop in hand) + WORLD-STATE
  (save-actor flag).
- **OBSERVABILITY:** `runTrigger`/`woken` are BP-internal
  (`EX_LocalVirtualFunction`); the teleport acts on the player.
- **RNG:** possibly (which wake outcome) — low.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **Tied to the shared sleep-cycle design** (an
  open UX question from the 2026-05-24 RE §4.7 / open-Q 7). If sleep is
  synchronous (both sleep together), the host fires `woken` and broadcasts a
  per-player teleport/effect; each client applies it to its own player. If
  sleep is host-only, this only affects the host. **Decision needed first:** do
  both players sleep together? Recommend resolving via the sleep-cycle work
  item before implementing this event. Precedent: none exact — player teleport
  is a new per-player effect packet.
- **COOP-RELEVANCE PRIORITY:** **MED** (gated behind the sleep-cycle design;
  HIGH if/when synchronous sleep ships).

### 7b. `Aevent_arirTreehouseSleep_C`  (`event_arirTreehouseSleep.hpp`)

- **WHAT IT DOES:** The treehouse-sleep variant. `wokeup()` does
  `getObjectFromKey` (the treehouse via `Atreehouse_C`), `getMainPlayer`,
  `teleportWObackrooms` (relocate the player on waking in the treehouse),
  `K2_DestroyActor`. Uses `propSpawner_editor_C` (places set-dressing). Save-actor.
- **TRIGGER:** Sleep-cycle driven (the treehouse sleep beat) — scheduler/sleep.
- **OUTPUT TYPE:** PLAYER-effect (teleport) + WORLD-STATE (save-actor) + PROP
  set-dressing.
- **OBSERVABILITY:** BP-internal.
- **RNG:** low.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** Same as bedEvent — gated behind the shared
  sleep-cycle design; host fires + per-player teleport broadcast.
- **COOP-RELEVANCE PRIORITY:** **MED** (sleep-gated; also a specific
  location/story beat — lower reach than bedEvent).

---

### 8a. `Aevent_vaccine_C`  (`event_vaccine.hpp`)

- **WHAT IT DOES:** Overlap-driven loot/quest drop: spawns a
  `Aprop_container_crate_C` containing a `Aprop_vaccine_C` + a
  `Aprop_notebook_paper_C` (a note). (`text` string param = the note's body.)
- **TRIGGER:** `Box` BeginOverlap `BndEvt` (reuses the `event_funnyGascans_Box`
  delegate signature — note the shared name).
- **OUTPUT TYPE:** PROP SPAWN (all `Aprop_C`-derived: crate/vaccine/notebook).
- **OBSERVABILITY:** overlap `BndEvt` observable; spawned props BP-internal at
  Init.
- **RNG:** none/low.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **Mostly FREE — host-auth gate + prop pipeline.**
  Host runs the overlap (suppress client double-fire); the crate/vaccine/note
  are `Aprop_C` and mirror via PropSpawn. The crate is a `Aprop_container_C` —
  its inventory contents need the container-inventory sync (a known gap, same
  as the drone-sack item gap noted in the economy work). Precedent: prop
  pipeline + container spawn RE (`votv-storage-container-spawn-RE`).
- **COOP-RELEVANCE PRIORITY:** **MED** (a shared pickup; matters because the
  vaccine is a gameplay item, but the spawn is mostly auto-covered).

### 8b. `Aevent_funnyGascans_C`  (`event_funnyGascans.hpp`)

- **WHAT IT DOES:** A gag: spawns `Aprop_gascan_funny_C` gascans on overlap.
- **TRIGGER:** `Box` BeginOverlap `BndEvt`.
- **OUTPUT TYPE:** PROP SPAWN (`Aprop_gascan_funny_C : Aprop_gascan_C : Aprop_C`).
- **OBSERVABILITY:** overlap observable; spawn BP-internal.
- **RNG:** none/low.
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **FREE — host-auth gate + prop pipeline.** Gascans
  are `Aprop_C` → PropSpawn mirrors them once the host fires the overlap.
- **COOP-RELEVANCE PRIORITY:** **MED-LOW** (cosmetic gag; harmless if slightly
  off, but trivially covered).

### 8c. `Aevent_trashPiles_C`  (`event_trashPiles.hpp`)

- **WHAT IT DOES:** Spawns `Aprop_garbageClump_C` trash piles on overlap (same
  garbage-clump actor as fleshRain).
- **TRIGGER:** `Box` BeginOverlap `BndEvt` (the shared
  `event_funnyGascans_Box` delegate name).
- **OUTPUT TYPE:** PROP SPAWN (`Aprop_garbageClump_C : AActor` — clump, NOT
  Aprop_C).
- **OBSERVABILITY:** overlap observable; spawn BP-internal.
- **RNG:** possible (pile positions) — low.
- **CURRENT COOP STATUS:** **PARTIALLY HANDLED.** `garbage_sync.cpp` ALREADY
  registers a host-authoritative cancel on this exact overlap
  (`OnEventTrashPilesOverlapPre` → `event_trashPiles.BndEvt`) as part of Inc 3
  (host-auth gating of garbage spawners). The clump actors ride the eid-routed
  clump rails. So the trigger gate EXISTS; verify the spawned piles mirror.
- **RECOMMENDED SYNC SHAPE:** **Largely DONE** — confirm the host-spawned piles
  propagate via the clump pipeline; no new event packet needed. Precedent:
  `garbage_sync.cpp` (already wired).
- **COOP-RELEVANCE PRIORITY:** **MED** (already gated; low remaining work).

---

### 9. `Aevent_consoleWrite_C`  (`event_consoleWrite.hpp`)

- **WHAT IT DOES:** Scripted "hacker"/story text to the in-game console
  (`Uui_console_C`). The disasm shows the literal lines it prints, with random
  inter-line delays: `>EXECUTE`, `>CONNECT STATION`, `>CONNECT SYSTEM CORE`,
  `>SYSTEM CORE ACCESS`, `>SYSTEM REACTOR CONNECT`, `>REACTOR DISABLE`,
  `>CORE DEACTIVATE`, `>TEMPERATURE FALSE`, `>REMOVE KERNEL`,
  `>MM NS 88 IIW 11 22 AA B`, then garbled/corrupted glyph lines. 16×
  `RandomFloatInRange` = the per-line timing.
- **TRIGGER:** `ReceiveBeginPlay` (placed `AActor`, runs on spawn) +
  `UserConstructionScript`.
- **OUTPUT TYPE:** VISUAL/UI only (console text). No actor, no world change.
- **OBSERVABILITY:** BeginPlay BP-internal; the console writes are BP calls.
- **RNG:** YES — line timing rolled per-peer (but it's just pacing).
- **CURRENT COOP STATUS:** NOT synced.
- **RECOMMENDED SYNC SHAPE:** **LOW priority / per-peer OK.** The console is a
  player-local UI surface; each peer can run the same scripted text locally
  (deterministic content, only timing differs — invisible at gameplay level).
  If exact sync is ever wanted, host broadcasts `EventStart(console_write,
  seed)` and the client replays with the same seed. Recommend **leave per-peer
  (strategy B)** unless the console is shared. Precedent: none needed.
- **COOP-RELEVANCE PRIORITY:** **LOW** (cosmetic UI; per-player console).

---

### 10. `AredSkyEvent_C`  (`redSkyEvent.hpp`) — **DONE**

- **WHAT IT DOES:** `set(bool isred)` swaps the world color curves on
  `AdaynightCycle_C` (+ touches `Anewsky_C`) for the red-sky anomaly.
  Instantiated by `AmainGamemode_C::spawnRedSky()`; pointer stashed at
  `mainGamemode.redSky @0x0888`.
- **TRIGGER:** `mainGamemode.spawnRedSky()` (gamemode UFunction).
- **OUTPUT TYPE:** WORLD-STATE / VISUAL (sky color curves).
- **OBSERVABILITY:** spawnRedSky + `set` are ProcessEvent-observable (that's why
  it was syncable cleanly).
- **RNG:** none.
- **CURRENT COOP STATUS:** **DONE — `coop/weather_redsky.{h,cpp}`.** Host
  observes `spawnRedSky` POST + `redSkyEvent.set` POST, broadcasts
  `RedSkyPayload`; client mirrors (spawnRedSky first time, `set(state)`
  thereafter). Resolved via `MainGamemode_SpawnRedSkyFn` / `RedSkyEvent_SetFn`
  in `sdk_profile.h`.
- **RECOMMENDED SYNC SHAPE:** n/a (shipped). Listed here only to mark DONE.
- **COOP-RELEVANCE PRIORITY:** DONE.

---

### 11. `Aevent_arirFuelsAtv_C`  (+ `_toolbox` subclass)
(`event_arirFuelsAtv.hpp`, `event_arirFuelsAtv_toolbox.hpp` — the toolbox
variant is an empty subclass, identical behaviour)

- **WHAT IT DOES:** An ariral (`AfuelingArir_C`) refuels the ATV. Save-actor
  tracking `fuel`/`health`; on completion writes the ATV's fuel + calls
  `updHealth`/`f_brake` on `AATV_C` (via `atvMap_C` / `svtarget_C`), references
  `prop_radio_atv_C`. `checkVisible()` toggles the cube indicator.
  `K2_DestroyActor` when done.
- **TRIGGER:** `ReceiveBeginPlay` + a `Box` overlap region; gated on the ATV
  being present/low-fuel. Save-actor (persists across reload).
- **OUTPUT TYPE:** WORLD-STATE (ATV fuel/health fields) + NPC (the fueling
  ariral) + minor VISUAL (cube indicator).
- **OBSERVABILITY:** BP-internal.
- **RNG:** low/none.
- **CURRENT COOP STATUS:** NOT synced as an event. NOTE: the ATV itself IS
  synced (`coop/atv_sync` — occupant-authoritative pose stream, protocol v47);
  the ATV's `fuel`/`health` are NOT currently in that stream.
- **RECOMMENDED SYNC SHAPE:** **LOW — host-authoritative + extend the ATV
  state.** Host runs the refuel; the resulting ATV `fuel`/`health` should be
  added to the existing `atv_sync` stream (a couple of extra fields) rather
  than a bespoke event packet. The fueling ariral is an NPC (NPC sync, low
  priority — it's a brief cosmetic helper). Precedent: `atv_sync` (extend it).
- **COOP-RELEVANCE PRIORITY:** **LOW** (one keyed ATV, slow cosmetic refuel;
  fold the fuel field into the existing ATV stream when convenient).

---

## Roadmap summary (priority-ordered worklist)

| Priority | Event(s) | Output type | Cheapest sync | New work? |
|---|---|---|---|---|
| **HIGH** | skyFallingEvent | PLAYER-effect + prop + FX | host-detect trigger + broadcast `EventStart(impactLoc,seed)`; per-player punch; skypiece via PropSpawn | NEW event packet + per-player effect |
| **HIGH** | event_fossilBoarWar | NPC spawn | host-auth spawn; fossilhounds→NPC sync, grayboars→prop pipeline | host-auth gate + NPC sync coverage |
| **HIGH** | event_fleshRain | prop spawn (clump) | host-auth; clumps ride eid clump rails; 1 `EventStart` for shake | host-auth gate (reuse clump pipeline) |
| **HIGH** | event_arirStealsGun | NPC action + inventory | host-auth; NPC sync; gun-removal via prop lifecycle; pick target player | host-auth gate + targeting decision |
| **HIGH** | event_bottomHoleController | world-state + NPC + keyed props | host broadcast room-active flag; patrol→NPC sync; beacons/lasers→keyed channels | host-detect + state flag (location-gated, schedule later) |
| **MED-HIGH** | lightsTurnoffer / passwordGuesser | world-state (keyed) | **host-auth gate only** — door/light/lock already mirror via keyed channels | gate the trigger; state is free |
| **MED** | event_trashPiles | prop spawn (clump) | **already gated** in garbage_sync (Inc 3) — verify piles mirror | confirm only |
| **MED** | event_vaccine / event_funnyGascans | prop spawn (Aprop_C) | host-auth gate + PropSpawn (vaccine crate needs container-inventory sync) | gate + container-inventory (vaccine) |
| **MED** | bedEvent / event_arirTreehouseSleep | player teleport | gated behind shared sleep-cycle design; host fires + per-player teleport | resolve sleep UX first |
| **LOW** | event_consoleWrite | UI/console text | per-peer OK (strategy B); content is deterministic | none (leave per-peer) |
| **LOW** | event_arirFuelsAtv (+toolbox) | ATV fuel/health world-state | fold fuel/health into existing atv_sync stream | extend atv_sync |
| **DONE** | redSkyEvent | world-state/visual | shipped in coop/weather_redsky | — |

**Three cross-cutting observations for the implementer:**

1. **Most of these need a host-authoritative TRIGGER gate, not a bespoke
   per-event packet.** The payloads (props/NPCs) already have sync rails
   (PropSpawn / clump eid / NPC sync / keyed channels). The recurring missing
   piece is: *only the host should run the trigger; the client's copy must be
   suppressed* (an overlap `BndEvt` cancel, like `garbage_sync` already does
   for `event_trashPiles`). A single generic "host-auth event trigger" gate
   (suppress-on-client + optional `EventStart` broadcast) would cover the
   overlap-driven events (vaccine, funnyGascans, lightsTurnoffer,
   passwordGuesser, bottomHole, trashPiles) cheaply.

2. **The two genuinely new mechanisms are (a) per-player physical effects**
   (skyFalling's `punch`/SetPhysicsLinearVelocity; the sleep teleports) — there
   is no existing precedent for "apply this physical effect to each peer's own
   player" — **and (b) the shared sleep-cycle design** which bedEvent +
   arirTreehouseSleep both depend on (open UX question from the 2026-05-24 RE).

3. **`setEvent` (used by skyFalling/fleshRain/fossilBoarWar) registers the
   event as active on the gamemode** (`activeEvents` @0x0E68 /
   `activeEvents_senders` @0x0E70 / `eventsActive` @0x0730) — this is the
   gamemode-level "an event is happening" state that drives tension music /
   HUD. If the host fires these and the client never sets it, the client gets
   no event music. A small `activeEvents` mirror (host broadcasts the active
   set) would make all the "scare" events feel right on the client — worth a
   tiny shared state channel alongside the per-event work.

---

**End of catalog (A).**
