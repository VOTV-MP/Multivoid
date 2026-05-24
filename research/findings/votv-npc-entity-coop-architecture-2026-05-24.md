# VOTV NPC / entity coop sync — proposed architecture

**Date:** 2026-05-24
**Status:** research phase — **NO code yet**. Audit-fixed; awaiting user review of remaining open decisions.

## User-decision changelog (3rd revision)

| Change | Source | Section |
|---|---|---|
| Enemies must target BOTH peers, not just host | User 2026-05-24 | New "Enemies target both peers" section. Puppet design changes to mainPlayer_C orphan (per Option A in the memory). Per-NPC targeting-logic RE listed for Phase 5N1. |

## Final-audit-fix changelog (2nd revision)

| Change | Source | Section |
|---|---|---|
| EntityManifest packet for session-init + reconnect | Issue 1 | Wire taxonomy |
| Direct-memory transform write demoted to optional optimization; baseline = SetActorLocation | Issue 2 | Scale section item 5 |
| Tagged-union sequential-parse contract documented | Issue 3 | Audit refinements section |
| Spawn-time NpcState bootstrap rule | Issue 4 | Audit refinements section |
| Forward-scope note for N≥3 syncerPeerId protocol bump | Issue 5 | Audit refinements section |
| `Aprop_C.thrown` cosmetic-only RE TODO for Phase 5N5 | Issue 6 | Audit refinements + Hook surface table |
| Reconnect-mid-session = manifest replay; ephemeral = cross-session only | Issue 7 | Audit refinements section |

## Audit-fix changelog (this revision)

| Change | Source | Section |
|---|---|---|
| AI suppression replaced with deferred-spawn AutoPossessAI=0 + AIControllerClass=null | Flaw 1 + RE doc | Phase 5N1 / Phase 5N2 |
| Dropped-prop Key policy specified (runtime Key by host) | Flaw 2 + RE doc | Inventory section |
| Spawner-tick race fixed via per-spawner PRE-observer | Flaw 3 + RE doc | Phase 5N2 + Hook-surface table |
| kerfur target field made into tagged-union (remote-player / local-player / entity-key) | Flaw 4 + RE doc | NpcState packet spec |
| Explosion/damage AActors NEVER spawn on client; client spawns only cosmetic VFX+SFX | Flaw 5 + RE doc | Phase 5N5 |
| Phase 5N3 scope explicitly lists inventory-transition events | Inconsistency A | Phase 5N3 |
| Phase 5N5 "AOI-cull for transient effects" removed | Inconsistency B + whole-map decision | Phase 5N5 |
| Wire taxonomy table: EffectFire AOI annotation removed; reliability tier for explosion clarified | Inconsistency D | Wire taxonomy |
| Bandwidth estimate caveated for swarm worst cases (wisp, goreSlither pack) | Inconsistency C | Whole-map sync |
| Open decisions #1, #2 confirmed closed | Audit | Decisions section |
| NpcPose aggregated-bitstream design noted (gap to MTA) | MTA Gap 1 | Wire taxonomy |
| Re-activation packet on NPC wake-up specified | MTA Gap 2 | NpcState section |
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
| `NpcSpawn` | reliable | host → client | spawn an NPC (Key + class FName + spawn pose + assigned 4-byte session-id) |
| `NpcDespawn` | reliable | host → client | despawn an NPC (4-byte session-id) |
| `EntityManifest` | reliable | host → client | **SESSION INITIALIZATION** -- on session-Connected edge AND on every client reconnect, host sends a full manifest of every active entity (id + Key + class FName + current transform). Client's EntityPoseBatch processing requires the manifest first; entries with unknown id are PROTOCOL ERROR, not graceful fallback. Closes Issue 1 of final audit. |
| `EntityPoseBatch` | unreliable | host → client | **AGGREGATED** per-tick pose stream (1 packet for N entities; see 100-entity scale memo). Entry = 4-byte id + quantized transform (~26 B). 100 entries fit in single UDP packet. Sequential parsing -- no offset-indexed access; the per-entry quantized fields are fixed-width, but the batch itself is variable-N. |
| `NpcState` | reliable | host → client | discrete state change (4-byte id + field bitmask + values: health / mode / target as tagged-union). Re-activation packet (Gap 2 mitigation) -- when NPC wakes from idle, push a full state snapshot before pose stream resumes. |
| `EntityEvent` | reliable | host or client | door opened, button pressed, trigger fired, **inventory pickup, inventory drop, item-state with world effect**. Client-initiated requests routed via host arbitration. (Key + event-id + params) |
| `EffectFire` | reliable | initiator → peer | one-shot explosion / projectile / sound / particle. **Client receivers spawn ONLY cosmetic VFX+SFX (PlaySoundAtLocation + SpawnEmitterAtLocation); the damage-radius AActor is NEVER spawned on non-initiator peers** -- the spawning peer's `Aexplosion_C.ReceiveBeginPlay` is the sole damage application. Per Flaw 5 RE. |
| `VehicleEnter`/`Exit` | reliable | initiator → host | authority handoff for vehicle |
| `VehiclePose` | unreliable | driver → others | per-tick vehicle drive |

Reliable for: state changes whose loss would desync world (spawn, despawn, door state, vehicle authority, **explosions** -- per Flaw 5 RE, NOT unreliable as the taxonomy table previously listed).
Unreliable for: per-tick pose batches (latest-wins via RFC1982 seq).
**No AOI culling anywhere** -- per the whole-map sync user decision.

### Staging — 5 phases

**Phase 5N1 — NPC pose stream (kerfur, zombie, ariral subset)**
Goal: a kerfur on host visibly walks around; client sees its puppet kerfur walking the same path.

Scope:
- HOST: walk GUObjectArray once per tick, find NPCs (class derives from ACharacter AND class FName in {kerfurOmega_C, npc_zombie_C, npc_ariral_*} + activity-gate per the whole-map sync rule), emit NpcPose per tick.
- CLIENT (no broad-suppression -- per Flaw 1 RE):
  Each WORLD-LOADED NPC on the client is "born" with full AI. We disable it AT SPAWN-TIME by writing `actor + 0x231 = 0` (AutoPossessAI::Disabled) and `actor + 0x238 = nullptr` (AIControllerClass) AT THE DEFERRED-SPAWN WINDOW (BeginDeferredActorSpawnFromClass -> write -> FinishSpawningActor). For world-loaded NPCs (already past BeginPlay when we join), we use the existing `DetachFromControllerPendingDestroy` pattern from RemotePlayer.
  On first NpcPose for a Key, walk GUObjectArray for matching lookup (need a `FindNpcByKey` helper -- sibling of `prop_wrap::FindByKeyString`). Drive transform kinematically; AI is already nulled-out.
- Anim sync: pass yaw + speed + headLook the way mainPlayer's PoseSnapshot already does. The kerfur AnimBP-satellite pattern (Bug 2 / project_bug2_locomotion_anim) is directly reusable -- NPC puppets share the same anim infrastructure.

This is the user-visible win. Architecture mirrors player pose sync + already-proven AI-block pattern from RemotePlayer spawn.

**Phase 5N2 — NPC spawn / despawn (host-authoritative)**
Goal: when an NPC spawns on host (e.g. host's tick spawns insomniac), client also spawns it; when killed, both despawn.

Scope:
- HOST: hook UWorld::SpawnActor for NPC classes via reflection observer (or, simpler: observe BP `Spawn Bad Sun`-style functions on mainGamemode_C). On spawn, generate a Key (or read the spawned actor's Key field), broadcast NpcSpawn.
- CLIENT: receive NpcSpawn → spawn local copy via reflection (same as we do for our player puppet). Stamp the local actor's Key field to match.
- Despawn: observe AActor::Destroy / engine-side death, broadcast NpcDespawn.

Hook surface needs RE — Stage 5N2 is research-heavy.

**Phase 5N3 — Interactables (doors, switches, triggers, keypads) + inventory transitions**
Goal: opening a door on host opens it on client; pressing a button on host registers on client; client can also initiate (sent to host who arbitrates). Inventory pickups/drops sync as world events without crossing inventory contents.

Scope:
- Bidirectional: client press → request to host → host applies + broadcasts state.
- Use the EntityEvent reliable channel.
- Hook the `fireTrigger` / `runTrigger` BP UFunctions (per VOTV survey) — they ARE BP-dispatched (impure, exec-pin).
- Multi-peer keypad UX problem: `ApasswordLock_C.isFocused` should be replicated so two players don't type into the same keypad simultaneously.
- **Inventory transitions (per user decision, Phase 5N3 owns these per Inconsistency A fix):**
  - World-pickup despawn: `EntityEvent("pickup", propKey)` -- receiver despawns its local copy of the prop.
  - Inventory-drop spawn: reliable `NpcSpawn`-style packet with class FName + transform + **host-generated runtime Key** (`"rt_<sessionToken>_<counter>"`, per Flaw 2 RE) -- receiver spawns its local copy with the same Key.
  - In-hand item state with world effect: `EntityEvent("itemState", propKey, bitmask)` -- e.g. flashlight on/off light cone.
  - Item state with NO world effect: stays local, never on wire.

**Phase 5N4 — Vehicles (ATV, drone)**
Goal: host driving the ATV; client sees the ATV move + the host puppet on it.

Scope:
- AATV_C has Pawn-style controller-input → physics drive. Driver-authoritative.
- The kerfur passenger field (`kerfur_passenger`) needs replication.
- Drone is timeline-driven (no controller); simpler — just stream pose.

**Phase 5N5 — Transient events (explosions, projectiles, sound/particle)**
Goal: when host shoots an alien dart, client sees the dart fly; when an explosion fires, both peers hear/see it.

Scope (per whole-map decision + Flaw 5 RE):
- **No AOI cull** -- all events broadcast regardless of distance (per whole-map user decision).
- Reliable for damage-bearing events (explosions, projectiles that hit).
- Unreliable for purely-cosmetic (one-shot particle, footstep sound).
- **Damage authority lives ONLY on the spawning peer** (Flaw 5 RE):
  - HOST runs the actual `Aexplosion_C` AActor with full damage radius.
  - CLIENT receives `EffectFire` and spawns ONLY the cosmetic VFX+SFX (PlaySoundAtLocation, SpawnEmitterAtLocation -- both are engine-native ProcessEvent-dispatched UFunctions).
  - Client NEVER spawns `Aexplosion_C` -- so `ReceiveBeginPlay` never fires on the client side -- so damage never double-applies.
  - Damage effects on remote actors propagate via `NpcState` health updates.
- Same pattern for `AlightningStrike_C`, projectile classes (`AnailProjectile_C`, `AgrimeProjectile_C`, `AplungerProjectile_C`, `AalienJump_C`).
- Reuse the Aprop_C.thrown(player) dispatch pattern shipped for grab where event has a natural prop dispatcher.

### Implementation patterns to copy from MTA (per the research note)

1. **`sync time context` byte** — every entity has a 1-byte counter bumped on every server-authoritative state change. In-flight unreliable packets stamped with the old context are silently dropped. We already have RFC1982 seq for pose; for entities we should add a per-entity context byte stamped on spawn/teleport/state-change.

2. **Flag-byte delta encoding** — `CPedSync` sends 1 byte indicating which of 8 fields are present, then only those fields' bytes. NpcState would do the same: 1 byte for which of {health, mode, target, anim, etc.} is present + only those values. Saves ~80% wire bandwidth on entities whose state barely changes.

3. **Near/far rate split** — `CPedSyncPacket` sends per-packet for nearby peers, 2-second batched for far peers. For our 1-host-1-client topology this collapses to a single tier, but the AOI cull is still relevant (don't stream the basement insomniac when the client is on the satellite).

4. **AOI streaming** — fixed-grid sector AOI with hysteresis. For VOTV's map size (a few km) and entity count (likely <100 active at once), even a flat O(N) check per peer is fine; sector grid is premature optimisation.

5. **Reliable+sequenced spawn / despawn** — same as our current PropRelease channel. No re-engineering.

6. **Server validates `pEntity->GetSyncer() == sender`** — for VOTV, in 5N1-5N2 only host emits (validation = "is this packet from the host?"). For 5N3 client-initiated events, host validates the requesting peer has legitimate proximity to the entity.

### Hook surface (what RULES require RE'ing FIRST)

Per `feedback_re_related_functions`, before any of Phase 5N1-5N5 is implemented, RE each of these:

| Function | Class | Phase | RE status |
|---|---|---|---|
| AutoPossessAI / AIControllerClass deferred-spawn block | APawn (engine) | 5N1, 5N2 | **DONE (Flaw 1 RE)** -- offsets 0x231/0x238 in sdk_profile.h; engine path confirmed (PostInitializeComponents → SpawnDefaultController). Pattern proven via RemotePlayer puppet. |
| `launchEvent` (impure) -- ariral spawn fire | AariralRepEventHandler_C | 5N2 | **DONE (Flaw 3 RE)** -- impure BP fn, ProcessEvent-dispatchable, signature verified in SDK dump. |
| `tickEvent` / per-spawner spawn-fire function | Aticker_*Spawner_C (~14 classes) | 5N2 | **PER-CLASS RE TODO** -- each spawner header needs a quick scan to identify the impure fire-function name (varies). |
| `TakeDamage`, `Die`, `OnDeath` | all NPC classes | 5N2 | TBD -- impure vs pure determines hookability. |
| `fireTrigger`, `runTrigger`, `setActiveTrigger` | AtriggerBase_C | 5N3 | TBD -- BP-callable functions usually impure. |
| `doorOpened`, `Open`, `Close` | Adoor_C | 5N3 | TBD. |
| `Aexplosion_C.ReceiveBeginPlay` | Aexplosion_C | 5N5 | **DONE (Flaw 5 RE)** -- Damage @ 0x0280, Radius @ 0x027C, BeginPlay is damage trigger. Fix: don't spawn the actor on client, only VFX+SFX. |
| `mainGamemode_C.spawnPropThroughGamemode` | mainGamemode_C | 5N2 | TBD -- host-routed spawn entry point. |
| `kerfurOmega.moveToServ`, `kerfur.moveTo @ 0x05D0` (UObject*) | AkerfurOmega_C | 5N1 | **DONE (Flaw 4 RE)** -- moveTo is UObject* (player or world entity). Use tagged-union NpcState target encoding. |
| `Aprop_inventoryContainer_player_C.extract(int32 Index)` | Aprop_inventoryContainer_player_C | 5N3 | **DONE (Flaw 2 RE)** -- extract returns prop to world; host generates runtime Key + broadcasts spawn. |
| `AATV_C.input` / occupancy | AATV_C | 5N4 | TBD -- driver detection, input replication. |
| `AnailProjectile_C / AgrimeProjectile_C / *` | projectile classes | 5N5 | TBD -- lifecycle: spawn → tick → hit/destroy. |

The MTA findings doc has the corresponding MTA file:line refs for each. The VOTV survey has the SDK header field offsets. The Flaws 1-5 RE doc (`research/findings/votv-npc-entity-RE-2026-05-24.md`) has the closed RE items.

### Enemies target both peers (USER DECISION 2026-05-24)

User: **"Also, the enemies should target host and client, not just host"**.

Full design in `memory/project_coop_enemies_target_both.md`. Key implication
for the architecture: **the puppet (each peer's local representation of the
OTHER peer) must be AI-perceivable as a player**. Currently the puppet is an
`ASkeletalMeshActor` (bare visual, not a Pawn) -- host's NPCs don't see it.

**Recommended path:** switch the puppet from `ASkeletalMeshActor` back to a
`mainPlayer_C` orphan (the design from earlier Phase 1, already proven viable
with multi-mainPlayer_C spawn). Block input + AI possession via the
deferred-spawn pattern (`AutoPossessPlayer=Disabled, AutoPossessAI=Disabled,
AIControllerClass=nullptr`) so the orphan can't act as a real player, but
keep the player-class identity that AI perception and `Cast to mainPlayer_C`
checks rely on.

Per-tick local-player-only subsystem suppression (PostProcess gamma stomp,
inventory tick, input handling) is a SEPARATE task -- much was done for the
earlier orphan design; revisit the catalog in
[[project-remote-player-hijack-and-pose]].

**Pre-Phase-5N1 RE TODO:** for each active enemy class, identify the
target-selection BP logic:
- AIPerception-based -- works with mainPlayer_C puppet automatically.
- `Cast to mainPlayer_C` -- works with mainPlayer_C puppet automatically.
- `GetPlayerPawn(0)` / `Find Player` BP nodes -- if VOTV BPs use only
  index 0, those NPCs see only the FIRST mainPlayer (the host's). Need to
  check whether VOTV iterates all players or hardcodes index 0. If hardcoded,
  per-NPC hook is needed.

Class list to RE: `Anpc_zombie_C`, `AkerfurOmega_C`, `Akrampus_C`,
`Afunguy_C`, `AgoreSlither_C`, `Ainsomniac_C`, `Afossilhound_C`,
`Aantibreather_C`, `Aorborb_C`, ariral family, UFO event classes.

### Scale: 100 entities (USER DECISION 2026-05-24)

User: **"When there's 100 entities/npcs during coop - we should be able to handle that"**.

Full design in `memory/project_coop_scale_100_entities.md`. Key forces:

1. **Aggregated bitstream**: 1 `EntityPoseBatch` packet per tick covering N entries; not 1 packet per entity. At 100 entities × 26 B per entry × 30 Hz aggregate = ~78 KB/s. Without aggregation: 6000 packets/sec from host (impossible).
2. **Activity-rate gating**: per-NPC rate by AI state (5 Hz idle / 15 Hz roam / 30 Hz alert / 60 Hz combat). Typical 100 active = ~12 Hz average.
3. **WireKey → 4-byte session-id mapping**: after NpcSpawn establishes the Key, subsequent pose entries use the 4-byte id. Saves ~28 B per entry per tick.
4. **Host-side `g_activeNpcs` set**: event-driven insertion (spawn / state-change observers) -- no per-tick GUObjectArray walk (237k entries is too costly).
5. **Receiver-side transform write -- BASELINE**: use `SetActorLocation` + `SetActorRotation` via reflection (proven path; same as RemotePlayer puppet). Estimated cost at 100 NPCs × 30 Hz = 3k ProcessEvent dispatches/sec on receiver -- tolerable. The architecture COMMITS to this path for Phase 5N1.
   **OPTIONAL OPTIMIZATION (deferred until profile-driven evidence justifies)**: direct write to USceneComponent.RelativeLocation @+0x011C bypasses ProcessEvent dispatch, BUT requires also triggering `USceneComponent::UpdateComponentToWorld` -- otherwise the render transform stays at the previous position (per Issue 2 of final audit). RE prerequisite for this optimization: identify the `UpdateComponentToWorld` vtable index OR confirm the `bWantsUpdateTransform` flag offset. Not blocking baseline; do not pursue without measured need.

Budget at peak: ~80-150 KB/s wire, ~5% host CPU, ~3% client CPU. Comfortable.

### NPC state encoding (NpcState packet detail per Flaw 4 RE)

`NpcState` packet field `target` is a **tagged union** (per Flaw 4 RE):

```
struct NpcState {
    uint32 entityId;        // session-local 4-byte id (NpcSpawn established mapping)
    uint16 fieldBitmask;    // which fields below are present
    // FLAG: health
    [if bit 0] uint16 health;
    // FLAG: mode (kerfurCommand enum / kerfurState enum / npc_zombie state)
    [if bit 1] uint8 mode;
    // FLAG: target tagged-union
    [if bit 2] uint8 targetTag;       // 0=none, 1=remote-player, 2=local-player, 3=entity-by-key
                [if targetTag == 3] WireKey targetKey;
    // FLAG: re-activation snapshot (Gap-2 mitigation: full state when NPC wakes from idle)
    [if bit 3] FVector lastKnownPos;
    // ... add fields as needed per NPC class
};
```

Receiver resolves the target tag to the right local object:
- `none` → moveTo = nullptr
- `remote-player` (host pointing at host-mainPlayer) → moveTo = client's host-puppet (`g_orphan.actor()`)
- `local-player` (host pointing at host's representation of client) → moveTo = client's local mainPlayer (`g_netLocal`)
- `entity-by-key` → `prop_wrap::FindByKeyString(targetKey)` → moveTo = resolved actor

This closes the "kerfur chases wrong player on client" bug Flaw 4 identified.

### Audit refinements applied (final audit follow-up)

**Spawn-time NpcState bootstrap (Issue 4 of final audit):** On every `NpcSpawn`,
the host immediately follows with a full `NpcState` snapshot (re-activation bit
set) for that entity, regardless of whether the AI state actually changed.
This gives the receiver an explicit initial mode/target/rate before the first
`EntityPoseBatch` arrives. Without this, receiver has no basis to pick an
expected stream rate.

**Tagged-union sequential-parse contract (Issue 3 of final audit):** The
`NpcState` packet's `targetTag + optional WireKey` field MUST be parsed
sequentially -- no offset-indexed access into the bitmask-gated fields. The
receiver's parser walks fields in declared order, consuming exactly the bytes
each present field requires. Same shape as MTA's CPedSync `WritePedInformation`.

**Future N≥3 migration path (Issue 5 of final audit, RULE 2 hygiene):**
The current wire format encodes "host is always the syncer." When 3+ peers
are added (future): `EntityPoseBatch` and `EntityEvent` will need a
`syncerPeerId` byte added (protocol version bump). Host validates
`pEntity->GetSyncer() == sender` before applying any peer's authoritative
packet (MTA pattern). This is documented as a forward-scope note, NOT a
crutch -- nothing about the current 2-peer wire prevents adding it cleanly.

**Cosmetic-only verification for `Aprop_C.thrown` extension (Issue 6 of
final audit):** Phase 5N5 proposes reusing `Aprop_C.thrown(player)` for
NPC throw events. The throw-impulse shipped path (commit 8832e56) calls
`thrown(localPlayer)` on receiver for sound + particles. **Before extending
this pattern to NPC throws in Phase 5N5**, RE the `Aprop_C.thrown` BP body
via UE4SS Lua dump to confirm it has NO side effects beyond cosmetics
(no inventory mutation, no stat tracking with world effect, no AI trigger).
If non-cosmetic side effects exist, refactor to use only the underlying
PlaySoundAtLocation + SpawnEmitterAtLocation pattern (same as explosions
in Phase 5N5).

**Reconnect-mid-session clarification (Issue 7 of final audit):** The
"save persistence out of scope" decision applies only to CROSS-SESSION
state. WITHIN-SESSION reconnect (client disconnect + rejoin during the
same coop game) is handled by the `EntityManifest` packet: host detects
the new session-Connected edge and replays the full active-entity manifest.
Runtime Keys for in-flight props (drops mid-session) are re-broadcast as
part of the manifest. Reconnect is a clean re-sync, not a state divergence.

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

1. ~~**Phase ordering**~~ -- DECIDED (default 5N1 first, no objection).
2. ~~**Authority model**~~ -- DECIDED (host-always 5N1-5N3, driver-authoritative 5N4, initiator-authoritative damage 5N5 per Flaw 5).
3. ~~**AOI defaults**~~ -- DECIDED 2026-05-24: whole-map sync, no AOI cull.
4. ~~**Suppression strategy**~~ -- DECIDED 2026-05-24: block AI possession at deferred-spawn time (Flaw 1 RE). NO broad post-spawn suppression.
5. **Save persistence**: still open. The Phase-5N3 inventory transitions imply ephemeral session is fine. **Recommendation: OUT of scope** -- pickup/drop replicates within a session; on disconnect, world state diverges per peer (ok).
6. ~~**Inventory**~~ -- DECIDED 2026-05-24: per-peer private, world-transition events only.
7. **Scale target** -- DECIDED 2026-05-24 (user): handle 100 entities/NPCs. Architecture forces aggregated bitstream + activity-rate gating + direct-memory writes (see scale section).

Only #5 (save persistence) remains; my recommendation is "out of scope for v5".

### Cross-refs

- `research/findings/mta-npc-entity-sync-2026-05-24.md` — MTA patterns
- `research/findings/votv-npc-entity-survey-2026-05-24.md` — VOTV inventory
- `research/findings/votv-physics-interaction-deep-re-2026-05-23.md` — Stage 1-4 grab (the precedent for this design)
- `research/findings/votv-throw-sound-path-2026-05-24.md` — Aprop_C.thrown pattern (re-usable for NPC throw events)
- `docs/COOP_SCOPE.md` — for scope-add (see next section)
- `docs/COOP_METHODOLOGY.md` — RULE №1/2/3 and architectural principles
- [[project-physics-object-pickup]] memory — what's already shipped
