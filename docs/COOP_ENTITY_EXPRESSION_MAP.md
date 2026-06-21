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
| chipPile/garbageClump CLIENT MIRROR | our own `SpawnActor`/`DestroyActor` (VISIBLE by construction — NO BP dispatch to observe) | **host-auth `AStaticMeshActor` PROXY (`coop/trash_proxy`, Movable, NoCollision):** `OnSpawn` trash-class branch → `SpawnProxy`; re-skin = `OnConvert`→`ReskinProxy`; retire = `OnDestroy`/disconnect→`RetireProxy`. Dup-gone + visible **[V]** hands-on (`245148c6`, deployed `70f1f04b`, HEAD `7f1b29ba`); the live carry-driven re-skin/follow MIRRORS on a settled join — mechanism **smoke-proven** (convert adopt `known` 0->1, mesh `dirtball`, `GRAB-IN`, drive `#1..#540 [proxy]`, LAND re-skin), on-screen VISUAL **[?] hands-on PENDING** | **host eid in `g_proxies` + the Prop mirror** (NO blueprint, `AddToRoot` → never self-morphs/GCs/stale) | trash_proxy |
| garbageClump (the carried "ball" — HOST authoring) | `chipPile.playerGrabbed` on grab (EX_LocalVirtualFunction); the clump-spawn + re-pile pile-spawn are `EX_CallMath` (INVISIBLE) | **host-auth trash channel (08):** grab = `InpActEvt_use` PRE + held-edge adopt [V]; re-pile = the **`UFunction::Func` thunk converter** (`ue_wrap/ufunction_hook`) — reads the source clump + spawned pile off the `EX_CallMath BeginDeferred`, converts E the same tick [AS-BUILT; detection [V] read-only `B7EEB1BF`]. *(The BeginDeferred-POST ProcessEvent link was DISPROVEN — EX_CallMath, 0 fires, `0e56ca39`; the proximity death-watch that briefly replaced it is RETIRED, RULE 2, `d19ae4d4`.)* | **host eid only** (re-skins the pile's E) | trash_channel, trash_collect_sync, local_streams, ue_wrap/ufunction_hook (08) — *pile_morph + death-watch RETIRED* |
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
>
> **⚠⚠⚠ The CLIENT MIRROR of trash is NO LONGER a real `actorChipPile_C`/`prop_garbageClump_C` BP — it is a
> host-authoritative `AStaticMeshActor` PROXY** (`coop/trash_proxy`, Movable, NoCollision; deployed
> `70f1f04b`). The durable facts below (KEYLESS, the bytecode mechanic, the OBSERVABILITY) describe the
> GAME's chipPile/clump on the HOST (where the real verbs run); on the CLIENT we no longer instantiate the BP
> at all — we own an `AStaticMeshActor` (NO blueprint, `AddToRoot`, eid→actor registry, re-skin in place).
> This **closed the client mirror-staleness dup (hands-on [V]; piles mirror VISIBLY after the Movable fix)**,
> and the **live clump carry MIRRORS on a settled join — mechanism smoke-proven, on-screen VISUAL [?] hands-on
> PENDING** (a host grab re-skins/drives the client's mirror; the smoke proves the wire/drive, not the
> pixels). See the proxy bullet under "The design (08…)" below + the **CARRY-MIRROR — RESOLVED at the
> mechanism level** section of
> `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.

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
- **CLIENT MIRROR of trash = a host-authoritative `AStaticMeshActor` PROXY — DUP-FIX + VISIBILITY [V] hands-on
  VERIFIED; the LIVE CLUMP CARRY MIRRORS on a settled join (mechanism smoke-proven; on-screen VISUAL [?]
  hands-on PENDING). Deployed `70f1f04b` (HEAD `7f1b29ba`):** the client's mirror of a chipPile/clump is NO
  LONGER the real self-morphing BP — it is an `AStaticMeshActor` WE own (`coop/trash_proxy`): NO blueprint
  (never self-morphs), `AddToRoot` (never GC'd → never stale-index), our eid→actor registry (`g_proxies`),
  re-skin (`SetStaticMesh`) IN PLACE on convert (never spawn-fresh). **Spawn** = `trash_proxy::SpawnProxy`
  (caught at the `OnSpawn` trash-class branch, BEFORE the BP dedup); **identity** = the eid in `g_proxies` +
  the Prop mirror (`RegisterPropMirror`); **re-skin** = `OnConvert` → `IsProxy(E)` → `ReskinProxy`; **destroy**
  = `OnDestroy`/disconnect → `RetireProxy` (`DestroyActor → RemoveFromRoot → unbind`). The proxy's
  `UStaticMeshComponent` is set **Movable** (`SetComponentMobility`, `245148c6`) because a runtime
  `AStaticMeshActor` defaults to STATIC, on which `SetStaticMesh`/`SetActorLocation` silently no-op. This
  **closed the former OPEN client mirror-staleness dup** (a real-BP join-mirror went NOT-LIVE within ~10s →
  `ResolveLiveActorByEid` null → fresh-clump spawn + the original lingering = the dup) — now impossible BY
  CONSTRUCTION (the 3-verdict discriminator / health-poll / serial-check plan is DROPPED as moot); the dup-gone
  + the resting/landed-pile mirror are **hands-on confirmed [V]**. **The LIVE CARRY MIRRORS on a settled
  join:** two clean instrumented smokes (runs `b97z33gyh`/`b7oxr23uy`) show the ToClump convert adopt (`known`
  0->1), the clump mesh resolve (`mesh-src=dirtball`, NOT a pile-fallback), `GRAB-IN`, the drive `#1..#540
  [proxy]` tracking the host 1:1, and the LAND re-skin — so the km-walk lerp/freeze (the prior "BUILT but
  UNEXERCISED") now runs. The earlier "stays a PILE / `0` `GRAB-IN` / 2 fps" was the **JOIN RACE** (the
  autotest grabbed before the client expressed its 875-proxy join snapshot), NOT a sync bug — killed by the
  autotest's new puppet-live settle gate; the smoke's old "functionally green" was withdrawn as render-blind +
  raced, but the carry MECHANISM is now smoke-proven. **STILL PENDING: a hands-on for the on-screen VISUAL**
  (the smoke proves the drive computes the right targets, not that the pixels follow) + to characterize the
  user's earlier "2 fps" (most likely the same race). **Phase 1 = visual + position + re-skin, EXPLICIT
  NoCollision**; collision (the `garbageCollider` hull) is PHASE 2 with Increment 2. Design + the CARRY-MIRROR
  — RESOLVED at the mechanism level: `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.
  **[V dup-fix + visibility; carry MECHANISM smoke-proven, [?] on-screen VISUAL hands-on PENDING.]**

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
| client real-BP trash mirror self-morphs/GCs/goes-stale → `OnConvert` NOT-FOUND → spawn-fresh dup | **RETIRED** — the client mirror of trash is no longer a real BP; it is the host-authoritative `AStaticMeshActor` proxy (`coop/trash_proxy`, deployed `70f1f04b`): NO blueprint + `AddToRoot` + eid→actor registry + re-skin-in-place → the stale-mirror → spawn-fresh dup path is structurally unreachable. (Discriminator/health-poll/serial-check DROPPED as moot.) **Dup-gone + piles mirror visibly = hands-on confirmed.** *(Separate item, not a dup: the live clump CARRY MIRRORS on a settled join — mechanism smoke-proven, on-screen VISUAL hands-on pending; see the trash CLIENT MIRROR row above.)* | [V — dup-fix hands-on] |
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
