# COOP — Entity Expression Map

> **The single source of truth for: how does each synced entity get a cross-peer identity, which seam
> expresses it, what gates apply, who owns its identity, and how its destroy propagates.** This is the
> map that, had it existed, would have saved the pile-morph's 3 reworks (the question "which seam
> expresses a clump/pile?" is answered here, definitively). Synthesized 2026-06-20 from a code-verified
> trace (4 agents, cross-checked against live code + the IDA RE). Companion:
> [COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md).
>
> Confidence: **[V]** verified-from-code · **[RD]** from-comment/RE-doc · **[?]** needs-probe.

## Identity ranges (all families)

`coop::element::Registry`: **HOST eids = `[1, kHostRangeSize=32768)`; CLIENT/peer eids = `[32768, …)`**.
`IsAllowedHostAllocatedEid` / `IsAllowedPeerAllocatedEid` are the trust gates; every wire receiver is
host-authoritative (`senderPeerSlot != 0` ⇒ drop, except the either-range cases — PropDestroy/PropConvert).
**[V]** (`kHostRangeSize` literal is in `coop/element/registry.h`).

## The families at a glance

| Family | Spawn dispatch | Caught by | Identity | Module(s) |
|---|---|---|---|---|
| Keyed `Aprop_C` props | save-load / spawner / Q-menu (BeginDeferred) / takeObj | seed walk (save-loaded) · Init-POST observer (spawner) · `FinishSpawningActor` POST (Q-menu) · takeObj POST | host eid **+ the BP save Key string** | prop_lifecycle, prop_element_tracker, prop_snapshot, host_spawn_watcher |
| chipPile (ambient trash pile) | garbagePileSpawner-seeded / save-load | seed walk (keyless-pile lane) → connect snapshot / R1 re-seed | **host eid only (KEYLESS)** | prop_element_tracker, prop_snapshot, remote_prop_spawn (EnsurePileBindIndex) |
| garbageClump (the carried "ball") | `chipPile.playerGrabbed` on grab (EX_LocalVirtualFunction); the clump-spawn + re-pile pile-spawn are `EX_CallMath` (INVISIBLE) | **host-auth trash channel (08):** grab = `InpActEvt_use` PRE + held-edge adopt [V]; re-pile = the **`UFunction::Func` thunk converter** (`ue_wrap/ufunction_hook`) — reads the source clump + spawned pile off the `EX_CallMath BeginDeferred`, converts E the same tick [AS-BUILT; detection [V] read-only `B7EEB1BF`]. *(The BeginDeferred-POST ProcessEvent link was DISPROVEN — EX_CallMath, 0 fires, `0e56ca39`; the proximity death-watch that briefly replaced it is RETIRED, RULE 2, `d19ae4d4`.)* | **host eid only** (re-skins the pile's E) | trash_channel, trash_collect_sync, local_streams, ue_wrap/ufunction_hook (08) — *pile_morph + death-watch RETIRED* |
| Held items (Aprop_C in hand) | grabbed | `EnsureHeldItemBroadcast` new-held edge (self-heal for untracked) + the held-pose stream | the item's Key/eid | trash_collect_sync, local_streams |
| NPCs / Characters | BeginDeferred (VISIBLE) | host `BeginDeferred` interceptor + POST; save-loaded via `RegisterExistingWorldNpcs` walk | host eid (no BP key) | npc_sync, npc_mirror, npc_world_enum, npc_adoption |
| WorldActors (event actors) | BeginDeferred (VISIBLE) | 2nd `BeginDeferred` interceptor (disjoint, NAME-matched allowlist) | host eid | world_actor_sync |
| Kerfur (prop⇄NPC) | conversion verbs (EX_CallMath, INVISIBLE) | **conversion death-watch POLL** + KerfurConvert broadcast | host **KerfurId** (spans both forms) + the per-form eid | kerfur_entity, kerfur_convert, kerfur_command, kerfur_prop_adoption |

## Per-family detail

### Keyed Aprop_C props
- **Spawn→catch:** save-loaded placed props are caught by the one-shot/throttled **GUObjectArray seed walk**
  (`SeedKnownKeyedProps`/`ReSeedKnownKeyedProps`), NOT the Init-POST observer **[V/RD]**; spawner-spawned
  props DO fire the **Init-POST observer** (`GrabObserver_Aprop_Init_POST_Body`) **[V]**; Q-menu/toolgun
  BeginDeferred spawns are caught at **`FinishSpawningActor` POST** (their `init()` is BP-internal →
  Init-POST never fires) **[V]**; container `takeObj` is caught at **takeObj POST** (the nested Init defers
  via `g_takeObjInFlight`) **[V]**.
- **Identity:** the **BP save Key string** is the cross-peer id (the game mints it via `NewGuid`; we READ it,
  and on a receiver write the host's Key via `setKey` before FinishSpawningActor) **[V/RD]**; a host eid
  rides alongside.
- **Client never authors a save-loaded Aprop_C** (`prop_lifecycle.cpp:193` `IsDescendantOfProp → return`) —
  host-authoritative. **[V]**
- **Destroy:** engine `K2_DestroyActor` **PRE** → `PropDestroy(key,eid)` **[V]**; BP-internal vanishes
  (truck/cull/LifeSpan, EX_CallMath) → the host **reaper death-watch** (`ReapDeadLocalPropElements` →
  explicit `PropDestroy(eid)`, kerfur-skipped) **[V]**.
- **Connect reconcile:** R2 blob-vs-live key-diff deletes → SnapshotBegin/claim-tracking → bracketed
  PropSpawn stream → SnapshotComplete → quiescence-gated **divergence sweep** (membership = the client's
  own local Prop Elements, mirror-excluded; **>50% world-wipe valve**) → `EnsurePileBindIndex` position-bind
  for keyless piles (retires the client-local identity). **[V]**

### chipPile + garbageClump (the dupe-critical family) — REDESIGN 2026-06-21, see [docs/piles/08](piles/08-HOST-AUTH-TRASH-CHANNEL.md)

> **⚠ The "MORPH" (pile_morph: held-object adopt + PROXIMITY land-watch, docs/piles/07) is RETIRED.** A
> real hands-on (2026-06-21) refuted its smoke "VERIFIED": the proximity land-watch
> (`FindNearestChipPile(lastPos,100cm)`) consumes a NEIGHBOR pile in a cluster → eid mis-binds → divergence,
> and the client grab never armed. The current design is the **host-authoritative trash channel** (08).
>
> **⚠⚠ The 08 "BeginDeferred-POST is observable" claim (the s35 "observability reversal") is ALSO FALSE
> (corrected 2026-06-21, commit `0e56ca39`).** The chipPile grab clump-spawn + the clump re-pile pile-spawn
> are dispatched `EX_CallMath` (a native thunk below `UObject::ProcessEvent`) → INVISIBLE to our hook;
> `host_spawn_watcher`'s POST observer registered fine but logged **0 fires** across 870 piles + every
> re-pile. The GRAB syncs via the VISIBLE InpActEvt-PRE seam; the RE-PILE deterministic zero-proximity catch
> is now the **`UFunction::Func` thunk patch — AS-BUILT** (commit `d19ae4d4`, `ue_wrap/ufunction_hook`;
> detection verified read-only `B7EEB1BF`, convert deployed-pending-hands-on). See
> `research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` + COOP_DISPATCH_VISIBILITY
> "Catching an EX_CallMath call".

**Durable facts (survive the redesign):**
- **chipPile is KEYLESS** — `setKey` is a no-op; the only identity is the host-minted eid. **[V/RD]**
  Caught by the seed-walk keyless-pile lane (`IsChipPile`) → connect snapshot / R1 re-seed. **[V]**
- **VERIFIED mechanic (bytecode):** `actorChipPile_C` = CARRY-AND-THROW (E → `playerGrabbed` spawns a
  clump in hand + destroys the pile; the clump re-piles on its **2nd ground contact** at the clump's
  **sphere-traced RESTING point** — NOT `lastPos`, NOT the source pile pos → why proximity was doomed).
  `trashBitsPile_C` = a SEPARATE COLLECT entity (keyed-Aprop lane). Only `chipType@0x0238` (cosmetic
  variant) carries. **[V/RD]**
- **OBSERVABILITY (corrected 2026-06-21):** `playerGrabbed`/`pickupObjectDirect`/`K2_DestroyActor(self)` are
  INVISIBLE (`EX_LocalVirtualFunction`). **The clump-spawn (grab) + the pile-respawn (re-pile) are ALSO
  INVISIBLE** — the chipPile/clump ubergraphs issue `BeginDeferredActorSpawnFromClass`/`FinishSpawningActor`
  as **`EX_CallMath`** (a native thunk below ProcessEvent). `host_spawn_watcher` catches that UFunction only
  from the NATIVE/SPAWNER caller (pinecone/`garbagePileSpawner`); for the BP-ubergraph caller it logged **0
  fires** (870 piles + every re-pile, hands-on 2026-06-21, commit `0e56ca39`). **Visibility is the CALLER's
  opcode, not the callee.** The 2026-06-08 `pass2` RE had this RIGHT; the s35 "BeginDeferred observable"
  reversal was the regression. `WorldContextObject == EX_Self` (the source) is bytecode-confirmed for both
  transitions; it is READ via a `UFunction::Func` thunk patch (AS-BUILT, `ue_wrap/ufunction_hook`, commit
  `d19ae4d4` — the thunk reads `FFrame::Object`@0x18 = `EX_Self`), NOT a ProcessEvent observer.
  **[V: COOP_DISPATCH_VISIBILITY.md "Catching an EX_CallMath call"; host_spawn_watcher.cpp:118-122;
  research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md]**

**The design (08, host-authoritative state machine):** the eid is a host-minted life-stable logical trash
entity (state PILED/HELD_BY(N)/FLYING; **position is NEVER identity for the GRAB** → the cluster mis-bind is
impossible by construction). Host owns the authoritative state; receivers drive a local visual from it.
Client-grab = **suppress-native + `GrabIntent` → host executes the real verb** (DESIGN, Increment 2). A
sync-time-context byte rejects stale packets (AS-BUILT, proto v82).
- **GRAB (pile→clump) — [V] VERIFIED hands-on (`[SYNC-MIRROR OK]`), proto v82, commit `0e56ca39`:**
  `trash_collect_sync::OnPileGrabPre` (the `InpActEvt_use` PRE — a real input event, ProcessEvent-VISIBLE)
  records the aimed pile's eid (`trash_channel::NotePendingGrab`); `local_streams`' held-edge adopts the
  spawned clump onto E (`AdoptPendingGrabClump → trash_channel::OnHostConvert(kToClump)`). Identity is the
  host eid end-to-end; NO proximity.
- **RE-PILE (clump→pile) — [AS-BUILT], the DETERMINISTIC `UFunction::Func` thunk converter (commit
  `d19ae4d4`):** a transparent forwarder patched onto `BeginDeferredActorSpawnFromClass`'s `Func`
  (`UFunction+0xD8`, via `ue_wrap/ufunction_hook`) fires on the clump's `EX_CallMath` re-pile spawn →
  `OnBeginDeferredSpawnObserve` reads `FFrame::Object`(@0x18 = source clump = `EX_Self`) + `*Result`(= new
  pile); a TRACKED clump → `OnHostConvert(kToPile)` converts E onto the EXACT spawned pile the SAME tick →
  the client re-skins its ONE mirror (no destroy+spawn dupe). **Zero proximity, no reaper race.** Detection
  VERIFIED hands-on (read-only `B7EEB1BF`: thunk `*Result` ptr-for-ptr == the old death-watch's pile); the
  CONVERT flip is deployed-pending-hands-on. The proximity death-watch (`WatchClumpForRepile` /
  `FindNearestUntrackedChipPile_`) is RETIRED (RULE 2). **Former known-minor [RESOLVED]:** the ~5s
  vanish-return (reaper-vs-rebind) is gone by construction (the convert lands the new pile under E the same
  tick the clump dies). RE: `research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` §3/§6.
- **GRAB-via-thunk + the eid=0 adopt-miss close — [DESIGN], NOT built:** move the grab (pile→clump) to the
  same thunk (srcObj = tracked chipPile → kToClump), retiring the InpActEvt-PRE + held-edge adopt and
  catching the grab even when the PRE missed (an UNTRACKED clump currently skips the converter). A tightening.
- **Increment 2 (CLIENT-grab direction) — [DESIGN], NOT built:** suppress-native + GrabIntent +
  host-executes-on-puppet-N + the PILED/HELD/FLYING state machine (proto v83).
- **OPEN — client mirror-staleness dup (ROBUSTNESS track):** the client's join-mirror of a pile goes
  NOT-LIVE within ~10s → `OnConvert`'s `ResolveLiveActorByEid` returns null → fresh-clump spawn + the
  original lingers = a dup. NOT the host-side mis-bind the thunk fixes. Design:
  `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.

### NPCs / Characters
- Host `BeginDeferred` **interceptor** (allowlist of 14 ACharacter bases) allocs an `Npc` Element + POST
  binds the actor; **client suppresses its own** local spawn (zero ReturnValue + return true) and
  materializes from the wire (`OnEntitySpawn`). Save-loaded twins (`savePersisted=1`) → **deferred adoption**
  by class+nearest-pose (never by key — the key is random per peer). Pose via `EntityPose` batch. Destroy
  via `K2_DestroyActor` PRE → `EntityDestroy`. **[V]**

### WorldActors (event actors)
- A **second** `BeginDeferred` interceptor with a **disjoint, NAME-matched** allowlist (16 leaf classes;
  name-match because event classes load lazily). FULL-rotation pose (`WorldActorPose`). Otherwise the NPC
  shape. **[V]**

### Kerfur (the hardest — prop form ⇄ NPC form)
- The game gives a kerfur NO stable id (random key per peer per load). We keep ONE host-only **KerfurId (K)**
  per logical kerfur spanning both forms; the rendered form is a normal `Npc` or `Prop` mirror at a per-form
  eid; K is preserved in place across conversions (`BindFormActor`, the SOLE `KerfurConvert` broadcaster).
  **[V]**
- Conversions are **INVISIBLE** (EX_CallMath) → caught by a **5 Hz death-watch poll** (element-present +
  bound-actor-dead == a local conversion), client-quiescence-gated. Client RELAYS the turn-on/off
  (`KerfurConvertRequest`); host runs the real verb authoritatively + broadcasts `KerfurConvert`; the client
  **claims-and-adopts** its conversion ghost (parks/freezes, never destroy+respawn). The historical dupe came
  from the mid-join turn-on window + the invisible spawn leaving untracked ghosts. **[V/RD]**

## Dupe matrix (every two-seam overlap + its dedup) — the high-value reference

| Overlap | Dedup | Conf |
|---|---|---|
| Init-POST observer ↔ FinishSpawningActor POST (Q-menu Aprop) | shared `HasProcessedInit` latch | [V] |
| Init-POST (nested) ↔ takeObj POST | `g_takeObjInFlight` defer | [V] |
| connect drain ↔ live Init-POST/re-seed during a join | receiver `OnSpawn` exact-key/eid dedup → `RegisterPropMirror` idempotent | [V] |
| R1 re-seed ↔ a 2nd peer's connect drain | bracket-free additive; receiver dedups; no sweep re-arm | [V] |
| kerfur converge ↔ R1 re-seed re-expressing the kerfur | `MarkKnownKeyedProp` + `ExpressIncrementalSpawn` kerfur-skip + reaper kerfur-skip | [V] |
| ~~pile_morph land-watch ↔ host re-seed~~ | **RETIRED** — the morph + its 100cm any-pile proximity land-watch are gone (08); the proximity death-watch that briefly replaced them is ALSO retired (RULE 2). The landed pile is now claimed by eid via the **`UFunction::Func` thunk converter** (commit `d19ae4d4`): the thunk reads the exact `(source clump, spawned pile)` off the `EX_CallMath BeginDeferred` and converts E onto the spawned pile the same tick → zero proximity, no re-seed/reaper race (the rebind re-points E onto the live pile). *(The convert-spawn-POST ProcessEvent link was DISPROVEN — EX_CallMath; the thunk is the deterministic catch.)* | [AS-BUILT; detection [V]] |
| client save-loaded pile (own eid) ↔ host pile eid (connect bracket) | position-bind retires the client-local identity (`UnmarkKnownKeyedProp`); 08 replaces this with host re-stream on the drain edge (`PileResyncRequest`) | [V] → [redesign] |
| client grabs host shared-trash ↔ host author path | 08: client SUPPRESSES the native grab + sends `GrabIntent` → host executes authoritatively (the door `OnRequest` shape) — client never authors shared trash, and no local clump dupe | [DESIGN] |
| NPC interceptor ↔ WorldActor interceptor (same BeginDeferred) | DISJOINT allowlists; multi-interceptor support | [V] |
| nested BeginDeferred steals a pending eid in POST | params-pointer correlation | [V] |
| client kerfur conversion ghost grabbed → client-eid dupe | `ClaimConversionGhosts` parks/freezes immediately (adopt or reap) | [V] |

## NEEDS-PROBE (do not encode as truth without it)
- **[?]** (trash redesign 08) PROBE-A: which `OnPileGrabPre` early-return fires on the live client
  (hands-full vs `kInvalidId`) + the carry slot (`grabbing_actor` vs `holding_actor`). One log line + 1 grab.
- **[?]** (trash 08 Increment 2) does `CallFunction(pile, playerGrabbed, {puppetN, hit})` drive
  `pickupObjectDirect` on a PUPPET (host executes a client's grab intent)?
- **[ANSWERED — NO, 2026-06-21]** does the `BeginDeferred`/`FinishSpawning` POST fire for the clump↔pile
  convert? **It does NOT** — the chipPile/clump caller issues it `EX_CallMath` (0 fires, 870 piles + every
  re-pile, commit `0e56ca39`). The deterministic catch is the **`UFunction::Func` thunk patch — now AS-BUILT**
  (`ue_wrap/ufunction_hook`, commit `d19ae4d4`; offsets pinned + read-only-validated).
- **[ANSWERED — RESOLVED, 2026-06-21]** the thunk-hook offsets on the shipping binary: `FFrame::Object`@0x18,
  `FFrame::Locals`@0x28, `UFunction::Func`@0xD8 — IDA-pinned + validated by the read-only log pass
  (`B7EEB1BF`); the spawned actor is read from `*Result` (not `Locals+returnOff`). Shipped in
  `ue_wrap/ufunction_hook`. *(RETIRED morph [?]s removed: the "clump→holding_actor" link [it's grabbing_actor] + the land-claim/re-seed timing margin.)*
- **[?]** NPC/WorldActor client-suppression assumes spawner BPs null-check before `FinishSpawningActor`
  (UE4 convention; not IDA-confirmed on VOTV spawners).
- **[?]** WorldActor 16-name allowlist completeness / all-spawn-via-BeginDeferred — smoke-pending.
