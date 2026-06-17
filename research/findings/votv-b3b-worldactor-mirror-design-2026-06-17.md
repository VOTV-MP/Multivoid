# B3b — host-authoritative WorldActor transform mirror (design, 2026-06-17)

Part of the "sync ALL events" arc (`votv-all-events-coop-sync-classification-2026-06-17.md`,
channel B3). Mirrors the ~14 NON-Character event actors (gray saucers, Rozital mothership, ariral
ships, sky UFO, space jellyfish, firetank, …) that the Character-only NPC mirror can't replicate.

## The finding (from agent ac176fda + reading the pipeline)
- The NPC mirror's client drive (`npc_pose_drive.cpp::ApplyToEngine`) is `SetActorLocation` +
  **yaw-only** `SetActorRotation({0,yaw,0})` + `DriveCharacterMovement` (CMC@0x288, ACharacter-only)
  + kerfur drives. A raw `AActor` would TRACK pos+yaw (the "stuck" claim was overstated) but (a)
  loses pitch/roll (ships can't bank) and (b) the Character-specific PARKING
  (`npc_mirror`: `DisableCharacterTicks`/`NeutralizeAiTimers` read CMC@0x288) MISREADS a non-
  Character's layout -- on a large BP actor offset 0x288 is some other property; if it happens to
  be a live UObject* the wrong component gets its tick killed. So `kNpcAllowlist` must stay
  Character-only.
- The `Npc` Element (`coop/element/npc.h`) is heavily kerfur/Character-coupled (bodyYaw, lookAt,
  kerfState, `DriveKerfur*`) -> generalizing it with a "kind" flag pollutes it. The element
  architecture already anticipates siblings (npc.h: Npc is the "FIRST subclass of Element", the
  MTA CClientEntity adoption). MTA precedent: `CClientStreamElement` siblings share `CClientEntity`
  + the stream manager; the per-type create/destroy/process differ. So:
- `ue_wrap::game_thread::RegisterInterceptor` supports **multiple interceptors per UFunction**
  (game_thread.h:115 "stops at the first returning true"). The NPC + WorldActor allowlists are
  DISJOINT (a class is one or the other), so a SECOND interceptor on `BeginDeferredActorSpawnFromClass`
  is conflict-free: on the host both run, the matching one broadcasts, both return false (spawn
  proceeds); on the client the matching one suppresses. No npc_sync surgery.

## Decision
A **self-contained `coop/world_actor_sync` module** + a **`coop::element::WorldActor` Element
subclass** (transform-only) + its own wire packets + `kWorldActorAllowlist`. NOT extend `Npc`
(kerfur-coupled), NOT refactor npc_sync (critical subsystem -- risk). Independent + safe; each
piece is SIMPLER than npc_sync's (no kerfur, no CMC, no save-persist branch).

## Wire (3 new ReliableKinds, proto bump)
- `WorldActorSpawn{eid, ClassName(64), locXYZ, rotPYR}` -- host->all on a host spawn (the
  interceptor reads the SpawnTransform exactly like npc_sync's EntitySpawn; FULL rotation kept).
- `WorldActorPose{eid, locXYZ, rotPYR}` -- the per-frame host transform stream (a batch like
  EntityPose, but FULL rotation, no speed/stateBits/kerfur). Unreliable lane (pose stream).
  (Decision: a SEPARATE small batch packet, NOT a bit on EntityPose -- keeps the NPC pose lean.)
- `WorldActorDestroy{eid}` -- host->all on actor destroy.
All HOST-AUTHORITATIVE: NOT in IsClientRelayableReliableKind, NOT pre-world. eid from the host
range (the shared element Registry; a `MirrorManager<WorldActor>`).

## The element (`coop/element/world_actor.{h,cpp}`)
`class WorldActor : public Element` -- pos + FULL rot (pitch/yaw/roll), LerpWindow interp on pos +
shortest-arc on each angle. `ApplyToEngine` = `SetActorLocation(curPos)` +
`SetActorRotation(curPitch,curYaw,curRoll)` ONLY (no CMC, no kerfur). `Tick()` like Npc::Tick
(advance interp -> ApplyToEngine when dirty). Snap-on-teleport like Npc.

## The module (`coop/world_actor_sync.{h,cpp}`) -- the npc_sync shape, simpler
- Install: resolve `kWorldActorAllowlist` classes + the BeginDeferred UFunction/offsets (reuse the
  reflected names) + register a 2nd interceptor + POST + K2_DestroyActor PRE (its OWN
  `g_actorToWorldActorId` reverse map + bypass slot).
- Interceptor: HOST -> alloc WorldActor element + `WorldActorSpawn` broadcast + stash pending for
  POST; CLIENT -> suppress (zero return) unless the bypass slot matches (wire-received mirror).
- POST: bind the spawned AActor* to the element + reverse map.
- Destroy PRE: Take element + `WorldActorDestroy` broadcast.
- Host pose stream (`TickPoseStream`, host-only ~sendHz): iterate the host WorldActor elements,
  read `GetActorLocation`+`GetActorRotation` (FULL), diff vs last-sent, batch `WorldActorPose`.
- Client mirror: OnWorldActorSpawn (materialize via the bypass-slot + BeginDeferred + FinishSpawning,
  then GENERIC park: `SetActorTickEnabled(false)` -- NO CMC read), OnWorldActorPose (SetTargetPose),
  OnWorldActorDestroy (K2_DestroyActor the mirror), Tick (drive interp). Connect-snapshot:
  QueueConnectBroadcastForSlot re-sends live WorldActorSpawn+Pose to a joiner (turbine shape).

## kWorldActorAllowlist (the ~14, verified : AActor/APawn in CXXHeaderDump)
`ufoDropper_body_C`, `ufoDropper_car_C`, `ufoDropper_tank_C`, `rozitBorg_C`, `arirShip_C`,
`skyUfo_C`, `jellyfish_C`, `morningUfo_C`, `tentacleBallsFollower_C`, `soltomiaCleaning_C`,
`kocker_C`, `skyFallingEvent_C`, `superEgger_C`, `igetis_C`, `firetank_C`(APawn),
`piramidSubpawn_C`(APawn). (Append-only is NOT required here -- class is matched by name, not a
wire id; but keep the list curated.)

## Ship plan
v1: build the infra + a HIGH-VALUE subset in the allowlist (ufoDropper_body/car_C, rozitBorg_C,
morningUfo_C, arirShip_C, skyUfo_C) -> audit -> smoke (dev-trigger `morningGay`/`borgRozital`/
`fallbody_0` on the host, confirm the actor mirrors on the client with pos+rotation) -> then add
the rest. Caveats from RE: confirm kavotia/piramidTest spawn via BeginDeferred (vs possession)
before relying on them; keyed-prop payloads (grayboar_C, dropped corpse/jeep) ride prop_lifecycle
separately (B6 if keyless). arirBuster ChildActors are not BeginDeferred -> separate (low value).

## RULE-2 / Principle-7 note
WorldActor is a peer of Npc under Element (the intended subclass design), NOT a parallel
re-implementation -- they share Element + the Registry + LerpWindow. The lifecycle DUPLICATION
(interceptor/POST/destroy) is the cost of not refactoring the critical npc_sync; if a third actor-
mirror type ever appears, factor the lifecycle into a template parameterized by <element, allowlist,
packet-sender> then (one extraction, RULE 2) -- noted, not done now.
