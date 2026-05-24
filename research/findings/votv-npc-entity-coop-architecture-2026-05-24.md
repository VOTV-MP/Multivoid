# VOTV NPC / entity coop sync — proposed architecture

**Date:** 2026-05-24
**Status:** research phase — **NO code yet**. Awaiting user review.
**Prerequisites read:**
- `research/findings/mta-npc-entity-sync-2026-05-24.md` — MTA precedent (what to copy / adapt)
- `research/findings/votv-npc-entity-survey-2026-05-24.md` — VOTV entity inventory + sync-relevant fields
- `docs/COOP_METHODOLOGY.md` — architectural principles
- `docs/COOP_SCOPE.md` — what's in scope; this doc proposes a new entry
- `research/findings/votv-physics-interaction-deep-re-2026-05-23.md` — already-shipped Stage 1-4 grab wire

## TL;DR

VOTV is a single-player UE4.27 game with **zero replication on its blueprints**. ~17 NPC classes (kerfur-omega, npc_zombie, the ariral family, krampus, funguy, etc.), ~1200 prop subclasses, doors/triggers/keypads, two vehicle-ish classes (ATV, drone), and a parade of timeline-driven UFO/event actors. The universal cross-peer ID is **`Key` (FName)** — already used by props, present on triggers, doors, switches, save-system actors.

The MTA model — **single-syncer per entity, server-relayed, AOI-streamed, sync-time-context-stamped** — ports cleanly. The substrate shift: MTA's "server" maps to our **host**; MTA's "syncer" maps to **whichever peer is closest** (initially: host always, until we add proximity arbitration).

**Recommended staging:** 5 phases over the next few weeks. **Phase 5N1** (host-authoritative NPC pose stream) gives the visible coop NPC sync the user is asking about; everything past 5N3 (interactables) is incremental polish until 5N5 (event/transient effects).

## Architectural model

### Authority (1-host-1-client)

| Entity class | Authority owner | Why |
|---|---|---|
| Player pawn | self (each peer owns its pose) | Stage 3 wire already does this |
| Physics prop (held) | the grabber | Stage 4 wire already does this; release returns to physics-everywhere |
| **NPCs (kerfur, zombie, ariral, etc.)** | **HOST always (P1)** | NPC AI runs in BP TickEvent on the host's instance; client mirrors it. Simpler than MTA's syncer model — at 2 peers there's no contention. |
| Doors, switches, triggers | **HOST always (P1)** | One-shot state, lowest-latency wins; host arbitrates concurrent presses |
| Spawnable entities (insomniac, fossilhound, etc.) | **HOST always (P1)** | Spawn is host-driven; client receives spawn event with Key + class + transform |
| Vehicles (ATV, drone) | DRIVER if occupied, else HOST | MTA's "syncer = driver" rule |
| Transient events (explosion, projectile) | INITIATOR | Reliable one-shot broadcast |
| Sounds / particles (one-shot) | INITIATOR | Unreliable, AOI-culled |

Authority is per-entity per-tick. Future: switch to MTA's proximity-based syncer when 3+ peers added (out of scope for now).

### Identity (cross-peer ID)

VOTV's `Key` FName (already used by props) is the canonical cross-peer entity ID. Two flavours:

1. **World-loaded entities** (level-spawned NPCs, doors, save-system actors): `Key` is baked into the cooked content / save UUID. **Cross-peer stable when both peers load the same save** (proven for props in Stage 4).
2. **Runtime-spawned entities** (insomniac, ariral via `launchEvent`, projectiles, explosions): `Key` is assigned by the spawning machine. **NOT cross-peer stable** — host must route the spawn through the wire, telling the client both the Key AND the spawn parameters; client spawns its own copy locally with the SAME Key.

For runtime-spawned entities, **host = the spawning machine**. Client never spawns; it receives spawn packets from host.

### Whole-map sync (USER DECISION 2026-05-24)

**Rule:** entities (enemies, NPCs) AND objects (props) sync across the
WHOLE MAP, not bound by any radius. No AOI culling.

This supersedes the AOI radii proposal earlier in this doc. Updated
rationale:

- VOTV's map is moderate-sized; even with all entities streaming, the
  cumulative bandwidth fits the LAN budget at typical entity counts.
- Players legitimately need to see distant events (a kerfur fleeing
  toward the far side of the map, a UFO crossing the sky) -- AOI culling
  would make those invisible.
- Persistent world consistency over bandwidth efficiency: both peers
  hold the same world state at all times.

Bandwidth back-of-envelope (host-side outgoing):
- NPCs: assume up to 100 active at peak. Pose stream = 60 Hz x 28 bytes
  + flag-byte delta state at lower rate = ~1.8 KB/s per NPC = 180 KB/s
  peak. Fine on LAN; tight on 2 Mbit upstream WAN.
- Objects: only IN-MOTION props stream (PhysX is deterministic-enough
  at rest -- a settled crate at host's coords IS at the same coords on
  client). Per-tick pose only when grabbing/throwing/being-pushed.
- Events: all explosions / projectiles / one-shot effects ARE
  broadcast regardless of distance.

Concrete implementation:
- Replace "AOI cull check" with a "is-this-entity-active check" --
  active NPCs (ticking, alive, not in cryo) stream; inactive don't.
- For objects: use PhysX's sleep state. AnyPrimitiveComponent with
  `IsAnyRigidBodyAwake()` true streams per-tick; sleeping bodies are
  authoritative-at-rest and need no per-tick wire.
- For events: broadcast unconditionally.

Optimization later if bandwidth becomes a concern: delta encoding +
varying-rate (fast NPCs at 60Hz, slow/idle at 10Hz). Out of scope for
v5 baseline.

### Wire taxonomy (protocol v5)

Building on v4 (PoseSnapshot + PropPose + PropRelease), v5 adds:

| MsgType | Reliability | Direction | Payload |
|---|---|---|---|
| `NpcSpawn` | reliable | host → client | spawn an NPC (Key + class FName + spawn pose) |
| `NpcDespawn` | reliable | host → client | despawn an NPC (Key) |
| `NpcPose` | unreliable | host → client | per-tick pose stream (Key + transform + anim state) |
| `NpcState` | reliable | host → client | discrete state change (Key + field bitmask + values: health/mode/target) |
| `EntityEvent` | reliable | host → client | door opened, button pressed, trigger fired, etc. (Key + event-id + params) |
| `EffectFire` | unreliable | initiator → peer | one-shot explosion / projectile / sound / particle (class + transform + params) |
| `VehicleEnter`/`Exit` | reliable | initiator → host | authority handoff for vehicle |
| `VehiclePose` | unreliable | driver → others | per-tick vehicle drive |

Reliable for: state changes whose loss would desync world (spawn, despawn, door state, vehicle authority).
Unreliable for: per-tick pose / per-tick state (latest-wins via RFC1982 seq, same as PoseSnapshot).
AOI-culled (don't even SEND if peer >100m from event): EffectFire.

### Staging — 5 phases

**Phase 5N1 — NPC pose stream (kerfur, zombie, ariral subset)**
Goal: a kerfur on host visibly walks around; client sees its puppet kerfur walking the same path.

Scope:
- HOST: walk GUObjectArray once per tick, find NPCs in `m_nearbyNpcs` set (filtered by class derives from ACharacter AND class FName in {kerfurOmega_C, npc_zombie_C, npc_ariral_*}), emit NpcPose per tick.
- CLIENT: on first NpcPose for a Key, walk GUObjectArray for matching Aprop-style lookup (need a `FindNpcByKey` helper). If found locally (world-loaded), drive it kinematically. If not (runtime-spawned), receive an NpcSpawn packet first.
- Anim sync: pass yaw + speed + headLook the way mainPlayer's PoseSnapshot already does.

This is the user-visible win. Architecture mirrors player pose sync.

**Phase 5N2 — NPC spawn / despawn (host-authoritative)**
Goal: when an NPC spawns on host (e.g. host's tick spawns insomniac), client also spawns it; when killed, both despawn.

Scope:
- HOST: hook UWorld::SpawnActor for NPC classes via reflection observer (or, simpler: observe BP `Spawn Bad Sun`-style functions on mainGamemode_C). On spawn, generate a Key (or read the spawned actor's Key field), broadcast NpcSpawn.
- CLIENT: receive NpcSpawn → spawn local copy via reflection (same as we do for our player puppet). Stamp the local actor's Key field to match.
- Despawn: observe AActor::Destroy / engine-side death, broadcast NpcDespawn.

Hook surface needs RE — Stage 5N2 is research-heavy.

**Phase 5N3 — Interactables (doors, switches, triggers, keypads)**
Goal: opening a door on host opens it on client; pressing a button on host registers on client; client can also initiate (sent to host who arbitrates).

Scope:
- Bidirectional: client press → request to host → host applies + broadcasts state.
- Use the EntityEvent reliable channel.
- Hook the `fireTrigger` / `runTrigger` BP UFunctions (per VOTV survey) — they ARE BP-dispatched (impure, exec-pin).
- Multi-peer keypad UX problem: `ApasswordLock_C.isFocused` should be replicated so two players don't type into the same keypad simultaneously.

**Phase 5N4 — Vehicles (ATV, drone)**
Goal: host driving the ATV; client sees the ATV move + the host puppet on it.

Scope:
- AATV_C has Pawn-style controller-input → physics drive. Driver-authoritative.
- The kerfur passenger field (`kerfur_passenger`) needs replication.
- Drone is timeline-driven (no controller); simpler — just stream pose.

**Phase 5N5 — Transient events (explosions, projectiles, sound/particle)**
Goal: when host shoots an alien dart, client sees the dart fly; when an explosion fires, both peers hear/see it.

Scope:
- AOI-cull: only broadcast events within `MAX_EXPLOSION_SYNC_DISTANCE` of the recipient.
- Reliable for discrete events (explosion = state change in the world).
- Unreliable for cosmetic (footstep sound).
- Reuse the Aprop_C.thrown(player) dispatch pattern shipped for grab.

### Implementation patterns to copy from MTA (per the research note)

1. **`sync time context` byte** — every entity has a 1-byte counter bumped on every server-authoritative state change. In-flight unreliable packets stamped with the old context are silently dropped. We already have RFC1982 seq for pose; for entities we should add a per-entity context byte stamped on spawn/teleport/state-change.

2. **Flag-byte delta encoding** — `CPedSync` sends 1 byte indicating which of 8 fields are present, then only those fields' bytes. NpcState would do the same: 1 byte for which of {health, mode, target, anim, etc.} is present + only those values. Saves ~80% wire bandwidth on entities whose state barely changes.

3. **Near/far rate split** — `CPedSyncPacket` sends per-packet for nearby peers, 2-second batched for far peers. For our 1-host-1-client topology this collapses to a single tier, but the AOI cull is still relevant (don't stream the basement insomniac when the client is on the satellite).

4. **AOI streaming** — fixed-grid sector AOI with hysteresis. For VOTV's map size (a few km) and entity count (likely <100 active at once), even a flat O(N) check per peer is fine; sector grid is premature optimisation.

5. **Reliable+sequenced spawn / despawn** — same as our current PropRelease channel. No re-engineering.

6. **Server validates `pEntity->GetSyncer() == sender`** — for VOTV, in 5N1-5N2 only host emits (validation = "is this packet from the host?"). For 5N3 client-initiated events, host validates the requesting peer has legitimate proximity to the entity.

### Hook surface (what RULES require RE'ing FIRST)

Per `feedback_re_related_functions`, before any of Phase 5N1-5N5 is implemented, RE each of these:

| Function | Class | Phase | RE needed |
|---|---|---|---|
| `aiTick`, `BehaviourTreeUpdate` | npc_zombie_C, kerfurOmega_C | 5N1 | What ticks the AI? When does it run? Can we observe its outputs (target / mode changes) without intercepting it? |
| `TakeDamage`, `Die`, `OnDeath` | all NPC classes | 5N2 | Death event dispatch — for despawn replication |
| `fireTrigger`, `runTrigger`, `setActiveTrigger` | AtriggerBase_C | 5N3 | Are these ProcessEvent-dispatched (impure) or BP-pure? |
| `doorOpened`, `Open`, `Close` | Adoor_C | 5N3 | Same |
| `Aexplosion_C.BeginPlay`, `postExplosion` | Aexplosion_C | 5N5 | Engine SpawnActor hook — observe spawn at this class |
| `mainGamemode_C.spawnPropThroughGamemode` | mainGamemode_C | 5N2 | Hook for host-routed spawn |
| `AnailProjectile_C / AgrimeProjectile_C` | projectile classes | 5N5 | Lifecycle: spawn → tick → hit/destroy |
| `AATV_C.input` / occupancy | AATV_C | 5N4 | Driver detection, input replication |

The MTA findings doc has the corresponding MTA file:line refs for each. The VOTV survey has the SDK header field offsets.

### Inventory is per-player private (USER DECISION 2026-05-24)

**Rule:** each peer's inventory is unique / private. No inventory contents
ever cross the wire.

VOTV's inventory is an actor (`Aprop_inventoryContainer_player_C : Aprop_container_C`,
size 0x42A, with `getData`/`loadData` save-system hooks). Each player carries
their own instance. The host's `inventoryContainer_player` is host-local;
the client's `inventoryContainer_player` is client-local. **Neither is replicated.**

WHAT DOES need to sync (when inventory state intersects the shared world):

1. **World pickup → inventory transition**: when host picks up a flashlight
   from the world floor, both peers must see the world flashlight disappear
   from the shared world. But ONLY host's inventory gets the item.
   - Wire: reliable `EntityEvent` of "prop X was picked up into a player's
     inventory" → both peers despawn the world prop.
   - NOT sent: the receiving player or which inventory slot it went to.

2. **Inventory → world dump transition**: when host drops a flashlight
   onto the floor, the WORLD gets a new prop. Both peers must see it spawn.
   - Wire: reliable `NpcSpawn`-style spawn packet (same channel as Phase 5N2)
     with the spawned prop's Key + class + transform.
   - NOT sent: the dropping player.

3. **Use / interaction events** (e.g. flashlight click on/off): the held
   item's STATE if it has world-visible effect (e.g. light cone). For
   flashlight specifically: just replicate the light-on bool.
   - Wire: reliable `EntityEvent` with item Key + state.

WHAT STAYS LOCAL (never on the wire):

- Inventory open/close UI
- Inventory slot reorder, splitting stacks, examining items
- Reading notes / using consumables that have no world effect
- Stats, achievements, save-system credits for collecting items

This is the only way a coop-shared world reconciles with private
inventories. It mirrors MTA's pickup model (server-authoritative world
pickup events, peer-local "what's in my pockets"). Implementation goes
in Phase 5N3 (interactables / world events), NOT a new phase.

### Risks / open questions

1. **AI in two places**: if HOST runs AI for NPCs and CLIENT's local copy also runs AI (because the same blueprint loads on both), the client's NPC will diverge from host's. **Solution:** suppress AI tick on the client's NPC copy via reflection (set `SetActorTickEnabled(false)` on the controller, or null out the AIController), only drive it from the wire. This is the analog of how we disable physics on client-side props during grab.

2. **Save persistence**: when host's NPC dies, the death is persisted in the save. Does the client's save reflect this? Currently the client loads its own save independently — the death wouldn't persist unless we propagate save events. **Likely out of scope** for the initial phase; coop runs are ephemeral.

3. ~~**Mass scale**: VOTV has 1200+ prop subclasses, ~17 NPC classes. Streaming everything is wasteful. **Solution:** AOI radius per entity type — props 50m, NPCs 200m, large vehicles 500m, explosions 300m.~~  **SUPERSEDED 2026-05-24 by user decision: whole-map sync, no AOI**. Bandwidth managed by activity gating (PhysX-sleep for props, AI-active for NPCs) and possible varying-rate per-entity in a future optimisation pass.

4. **Cross-class instantiation parity**: client receiving an NpcSpawn for `Anpc_zombie_C` must have that class registered. Cooked content has them all baked; runtime spawn via FindClass should work, but the BP graph's `BeginPlay` may have side effects we don't want (e.g. NPC plays a spawn sound, decides on a target). **Solution:** spawn the class but suppress its tick + AI until it's been "synced into existence".

5. **Sync-time-context vs key**: an entity could (rare) be despawned + respawned with the same Key. The sync-time-context byte handles this — bumped on spawn → stale packets dropped.

### Decisions needed from user before code starts

1. **Phase ordering**: 5N1 first (NPC pose stream) seems right. Confirm?
2. **Authority model**: host-always for 5N1-5N3 is the simplest. Driver-authoritative for vehicles in 5N4 OK?
3. ~~**AOI defaults**~~ -- DECIDED 2026-05-24: whole-map sync, no AOI cull (see "Whole-map sync" section).
4. **Suppression strategy**: kill client-side AI tick (preferred) vs run AI on both + correct via host stream (more bandwidth but smoother on packet loss)?
5. **Save persistence**: in/out of scope for 5N?
6. ~~**Inventory**~~ -- DECIDED 2026-05-24: per-peer private, world-transition events only (see "Inventory is per-player private" section).

### Cross-refs

- `research/findings/mta-npc-entity-sync-2026-05-24.md` — MTA patterns
- `research/findings/votv-npc-entity-survey-2026-05-24.md` — VOTV inventory
- `research/findings/votv-physics-interaction-deep-re-2026-05-23.md` — Stage 1-4 grab (the precedent for this design)
- `research/findings/votv-throw-sound-path-2026-05-24.md` — Aprop_C.thrown pattern (re-usable for NPC throw events)
- `docs/COOP_SCOPE.md` — for scope-add (see next section)
- `docs/COOP_METHODOLOGY.md` — RULE №1/2/3 and architectural principles
- [[project-physics-object-pickup]] memory — what's already shipped
