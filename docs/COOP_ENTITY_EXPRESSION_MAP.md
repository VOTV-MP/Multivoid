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

**v122 NO-PASSIVE-MINT (2026-07-18, `votv-stable-id-no-passive-mint-DESIGN-2026-07-18.md`) — the
client-identity rule for KEYED props [V smoke A/B]:** a CLIENT's passive census walk key-INDEXES a keyed
prop but mints **no Element** (keyed identity is host-authored by construction: save-loaded props adopt
the host eid by key; client births mint at their EXPRESS seams, which broadcast in the same breath).
Pre-v122 the silent census mint produced ~2200 zombie double-rows per join (local + stacked mirror on
one actor; the reverse stolen by `RegisterMirror`) — measured, gone (sweep universe 2236→1). Guards at
the one bind owner (`CreateOrAdoptPropMirror`): host wall-REJECT over a live host local; client
DISSOLVE of a provisional on host word (the handback receiver); 1:1 actor-row invariant. Host-side, a
peer PropSpawn resolving onto a host-authoritative actor takes the **H handback** at the OnSpawn
resolution points (enroll-if-untracked + re-express under the host eid — replaces the old accidental
stack/rekey corruption the ghost-twin cure rode on). The join sweep's keyed universe = the key index
(element-less half); R2 key-deletes unchanged.

## The families at a glance

| Family | Spawn dispatch | Caught by | Identity | Module(s) |
|---|---|---|---|---|
| Keyed `Aprop_C` props | save-load / spawner / Q-menu (BeginDeferred) / takeObj | seed walk (save-loaded) · Init-POST observer (spawner) · `FinishSpawningActor` POST (Q-menu) · takeObj POST | host eid **+ the BP save Key string** | prop_lifecycle, prop_element_tracker, prop_snapshot, host_spawn_watcher |
| chipPile (ambient trash pile) | garbagePileSpawner-seeded / save-load | seed walk (keyless-pile lane) → connect snapshot / R1 re-seed | **host eid only (KEYLESS)**; the JOIN reconcile of the client's save-loaded native ↔ host proxy is **by SAVE-TIME POSITION** (v86 Path 1c — the frozen value both peers loaded; matched at the POST-QUIESCENCE sweep, NOT world-ready, because world-ready ≠ the native load-tail finished) **[BUILT, audit GO ×2, hands-on take-2 PENDING]** | prop_element_tracker (CollectTrackedPileTransforms), save_transfer (g_blobPileXforms), prop_snapshot (stamp matchX/Y/Z), **coop/props/pile_spawn_bind** (TryDestroyTwin -- the spawn-time twin-destroy) + **coop/element/quiescence_drain** (SweepReconcileSaveTimeTwins -- the post-quiescence retry; was `pile_reconcile`, split 2026-06-30) |
| chipPile/garbageClump CLIENT MIRROR | our own `SpawnActor`/`DestroyActor` (VISIBLE by construction — NO BP dispatch to observe) | **host-auth `AStaticMeshActor` PROXY (`coop/trash_proxy`, Movable, NoCollision):** `OnSpawn` trash-class branch → `SpawnProxy`; re-skin = `OnConvert`→`ReskinProxy`; retire = `OnDestroy`/disconnect→`RetireProxy`. Dup-gone (derived) + visible + carry-FREEZE + carry-JANK + THROW-ARC + ROTATION + SOUND all **[V hands-on]** (`245148c6` + the `!carrying` gate + fixed-delay snapshot interp `df158728` + flight-stream `136ed779`, HEAD `a5282f57`, deployed `015F0AC9590B6B23`, proto v83); carry JANK **FIXED [V hands-on]** (interp PHASE-STALL → fixed-delay snapshot interp; the `key.len=4`→keyless theory DISPROVEN — `GetKey`=`"None"` both forms); THROW ARC **VERIFIED [V hands-on take-29 + harness]** (the arc flies; flight-stream, not a release verb); ROTATION + SOUND **FIXED [V hands-on take-30]** (rot re-read from the settled pile at the land-settle COMMIT `trash_channel.cpp:248`; pickup-cue killed, `OnHostRelease` retired); Z/HEIGHT **FIXED [V harness]** (take-31 read `(0,0,0)` at the BeginDeferred POST → regressed; take-32 re-reads the real transform at the COMMIT, drift=0cm; the native-destroy was INNOCENT); proxy SCALE **AS-BUILT** (re-read `GetActorScale3D` at the same COMMIT); LEVEL-PILE DUP DESTROY **BUILT + VERIFIED [V harness]** (`remote_prop_spawn.cpp:387-410` destroys the co-located native at proxy-spawn, ~1cm + chipType + IsLive, exact-or-skip on >1, join-bracket-gated; harness 12 twins/0 SKIP; the read-only PILE-PROBE is REPLACED, RULE 2); FPS ~4s stutter **FIXED [V harness]** (`net_pump.cpp:559` guards the re-seed on the GUObjectArray high-water mark + ~20s census; rate 0.073/s); the throw-velocity flip is **DEAD — REPLACED by carry/flight stream-continuity** (`136ed779`: both `simulateDrop`+`dropGrabObject` thunks fire ZERO; stream the LIVE clump's flight under E until re-pile, `IsLive`-gated). Option 1 FAILED; option 2 (the `holdPlayer` convert/ctx gate) DISPROVEN by bytecode (`holdPlayer` never cleared). STILL OPEN: WHOOSH throw sound (no ReliableKind; deferred). (The dead `dropGrabObject` thunk: RETIRED 2026-07-10 `fb490e36`.) Harness: `[[reference-pile-test-harness]]`. See `votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md` | **host eid in `g_proxies` + the Prop mirror** (NO blueprint, `AddToRoot` → never self-morphs/GCs/stale) | trash_proxy |
| garbageClump (the carried "ball" — HOST authoring) | `chipPile.playerGrabbed` on grab (EX_LocalVirtualFunction); the clump-spawn + re-pile pile-spawn are `EX_CallMath` (INVISIBLE to PE, Func-thunk-VISIBLE) | **host-auth trash channel (08), BOTH morph directions at the ONE BeginDeferred Func thunk:** grab = **birth certificate** `NoteClumpBorn(clump, pile eid)` **+ MIGRATION-FIRST rebind of E onto the clump AT BIRTH** (v106b `4a280375`: the pile husk then dies eid-less, so the K2_DestroyActor Func seam naturally no-ops on it — pre-v106b the husk death broadcast DESTROY(E) + drained the row = the 11:43 grab regression); the certificate is consumed by the held-edge (`TakeClumpBorn`→carrying? `OnHostRegrab` : `AdoptBornClump`) or by `OnGrabIntent` (puppet hand); an EXPIRING certificate with a live untouched clump broadcasts the ToClump itself (BIRTH-ORPHAN express — a denied grab still converted the world). Re-pile = the same thunk converts E onto the spawned pile the same tick [V]. *(InpActEvt-PRE hand-off RETIRED v106 — use-HOLD grabs bypass press seams; death-watch RETIRED `d19ae4d4`.)* | **host eid only** (E migrates pile→clump→pile; a morph HUSK never owns E at death) | trash_channel, trash_collect_sync, local_streams, ue_wrap/ufunction_hook (08) `[V hands-on 2026-07-07 0ae: client grab/throw lane live-verified after v106b]` |
| Held items — physics GRAB (Aprop_C in `grabbing_actor`, the E-drag of a WORLD prop) | grabbed | `EnsureHeldItemBroadcast` new-held edge (self-heal for untracked; the PRE-QUIESCENCE guard is **CLIENT-only** since v105 — on the host it never lifted and silenced expression forever [V log 2026-07-06 12:40]) + the held-pose stream | the item's Key/eid | trash_collect_sync, local_streams |
| Held items — HOTBAR hand (Aprop_C in `holding_actor`; `updateHold` DESTROYS+RESPAWNS the actor per quick-slot switch) | **NOT a world entity** (v105, RULE 2): routed OUT of the prop pipeline entirely, **AND excluded from the SeedWalk_ census** (v105b: without the boundary the ~20s safety census adopted the recycled-slot hand actor → PropSpawn → frozen dupe at the puppet [V log 2026-07-06 13:44:00 eid=5377]) | owner announces `HandItem{class,name}` on change; peers keep a DISPLAY-ONLY mirror (physics/collision off, `prop_echo_suppress` Mark* so spawn/destroy watchers skip it) **re-driven per tick at the puppet's view-space hold** (head + synced-aim basis; NO attach — the FP-arms rig never anim-ticks on a puppet, a ref-pose socket attach rendered on the BACK [V hands-on 14:2x]) | **no eid, no Key on the wire** — player-slot addressed (player expression, like skin) | coop/player/hand_item (v105b AS-BUILT 2026-07-06, DLL 7EEA4488; mirror look USER-APPROVED live) |
| R-pickup vanish / R-drop appear (the WORLD prop side of a hand transition) | pickup destroy + drop release are EX_CallMath / recycled-actor paths a PE observer + the census high-water guard cannot see | **v106 SEAM-DRIVEN (2026-07-07, replaces the v105b forced-reconcile per RULE 2)**: the pickup destroy broadcasts at the **K2_DestroyActor Func seam** (fires for EVERY dispatch route); an R-drop/place **IS a fresh spawn** (`simulateDrop`→`FinishSpawningActor`@117551, `loadData` restores the Key — **RE 2026-07-08 REFUTES the earlier "NOT a spawn" model**); a SURVIVING ex-hand actor is expressed at the **hand edge** (hand_item), inventory/Q-menu/stack-drop spawns adopt via the **FinishSpawningActor Func seam** (+1 tick, key restored) — BUT a **CLIENT's** fresh R-drop spawn of a pre-existing world rock is UNAUTHORED (`prop_lifecycle:195` host-auth skip) → **host can't see a client-placed rock = OPEN** (as of `13a372a3`). **RE-ROOTED 2026-07-09 (`/qf 15`): the SEAM-catching fixes (v2 FinishSpawn author, v3 destroy-suppress, v4 exclude-hand) are ALL crutches on the wrong layer — a MISSING SINGLE OWNER — and were REVERTED (they duped on grab). Proper fix = the host-authoritative INTENT lane (extend `GrabIntent=78`): `research/findings/join-identity/votv-keyed-prop-grabdrop-intent-lane-DESIGN-2026-07-09.md` + `[[reference-votv-prop-interaction-buttons]]` (E=physics-grab; hold-R=pick into hand / place on ground; R-tap=hotbar). The 0ae [V] below is HOST Q-menu spawns + the grab lane, NOT this client-R-drop case.** The 4s reap + 20s census remain the safety net only | (accelerates the existing per-delta PropDestroy / PropSpawn lanes) | prop_lifecycle (destroy seam) + host_spawn_watcher (spawn seam) + hand_item (release edge); the K2 seam is BIDIRECTIONAL → the formerly-OPEN client-pickup destroy lane is closed by construction `[V hands-on 2026-07-07 0ae: Q-menu spawns appear INSTANTLY on clients — the FinishSpawn drain live-verified; grab lane PASS after v106b migration-first]` |
| NPCs / Characters | BeginDeferred (VISIBLE) | host `BeginDeferred` interceptor + POST; save-loaded via `RegisterExistingWorldNpcs` walk | host eid (no BP key) | npc_sync, npc_mirror, npc_world_enum, npc_adoption |
| wisp_C (wispSwarm event) | BeginDeferred **as `EX_CallMath`** from trigger_wispSwarm's ubergraph (INVISIBLE to PE) | **`ufunction_hook` Func-thunk, SOURCE-GATED** (FFrame::Object class == trigger_wispSwarm_C) → queue → pose-tick drain enroll; **ambient ticker sky wisps host-mirrored too since 2026-07-10 eve** (`ticker_wispSpawner_C` added as an EX-catch source row + the 8 color variants allowlisted; the client's ticker tick is a spawn_authority t3 cancel -- the old per-peer call assumed player-anchoring the dump refutes); PE-invisible self-despawn caught by the **pose-walk dead-retire** | host eid (no BP key) | npc_world_enum (EX-catch + enroll), npc_sync (SyncDestroyedNpcByEid), npc_pose_drive + ue_wrap/wisp (landing edge) |
| WorldActors (event actors) | BeginDeferred (VISIBLE for most; `piramidSpawner_C` = EX_CallMath INVISIBLE) | 2nd `BeginDeferred` interceptor (disjoint, NAME-matched allowlist) + EX Func-thunk drain -> `HostEnrollExSpawn`; destroy = PRE observer + pose-walk dead-retire (SELF-destroys invisible) | host eid | world_actor_sync, npc_world_enum (EX catch), piramid_sync (choreography) |
| Kerfur (prop⇄NPC) | conversion verbs = **`EX_LocalVirtualFunction` ubergraph self-calls** (bytecode-measured 2026-07-13; the INNER spawns are EX_CallMath→**Func-VISIBLE** — "EX_CallMath is a wall" was FALSE, see `docs/COOP_VM_DISPATCH_PLAN.md` §0) | **OWNER (in progress): the form-flip ASSEMBLER on VM verb brackets** (`COOP_VM_DISPATCH_PLAN.md`). **Increment 1 SHIPPED `722fbe18` (observe-only)**: the GNatives[0x45] substrate catches both verbs; containment [V]-confirmed (static body walk — both verbs standalone synchronous, no latent node — + 18/18 hands-on: spawn + self-destroy both IN-bracket, both peers). **FINAL measured fix (2026-07-14 pm — the DRAIN is DEAD, M1 measured-false both roles; earlier A+ drain framing RETIRED).** bug1's relay = the generic `grab_hook[destroy-seam]` CLIENT broadcast (key, eid=0) on every client turn-on. `TryCaptureKerfurPropDestroy` (`prop_destroy_seam.cpp:118`) is the guard on that relay — **REACHED, not dead** (M2 "structurally dead" was a false-negative grep — the destroy-seam line prints no class; RETRACTED, `[[lesson-negative-grep-verify-against-known-positive]]`). It DECLINES because the dying prop's post-destroy anchor (`GetActorLocation(actor)`) reads ~(0,0,0) → the 500 cm proximity finds no B ("1 fresh stamp but NONE qualified", 13:40 take — the take-10 (0,0,0) mechanism, now instrumented). `K2_DestroyActor(self)` IS Func-visible (provenance `vmActive=1 ctxSelf=1`); only the VERB is EX_Local. **FIX = feed the assembler's EXACT in-window captured-B to that guard** (bracket-paired → bypasses the (0,0,0) anchor) for the client suppress + host inline converge, and to `ConvergeAfterConversion(capturedForm)` (foundation `e785cb04`). G1 GREEN (host `formInReqScope=2`, CallFunction route capture-viable). See `[[project-vm-dispatch-2a-capture-2026-07-14]]`. **WIRED `44f8c69b` → 19:09 take DUPED host-own turn_off** (double-EXPRESS: keyed real-key PropSpawn + KerfurConvert; client 6→8, 2 UNCLAIMED). **SOLE-EXPRESS FIX `de3dccb5`** (see `[[project-kerfur-sole-express-dupe-2026-07-14]]`): the Init POST keyed-express suppressor now gates on `kerfur_form_assembler::IsCapturedForm` (deterministic non-consuming peek) — a captured conversion successor is TRACKED but its generic PropSpawn SUPPRESSED (KerfurConvert = sole signal; redesign 10.3 restored). This REPLACES `TryAdoptFreshKerfurProp`'s racy dead-NPC suppression (a PROXY the verb's spawn/destroy order defeated); TryAdopt keeps only its CONVERGE role. Pre-existing 6-month race (silent orphan-NPC ↔ visible orphan-prop) 44f8c69b only exposed. Interim as-built: turn-on destroy-edge `TryCaptureKerfurPropDestroy` (captured-B, now the SOLE which-B path) + the death-watch POLL converge (client request driver + host-own backstop). bug1/bug2 converge measured 19:09; **the turn_off dupe fix VERIFIED GREEN (20:20 take: 14/14 host conversions suppressed, client census 6=6 across 13 samples, user-confirmed no visual dupes).** The stamp-walk proximity FALLBACK inside `TryCaptureKerfurPropDestroy` is **RETIRED `3d9da0cd`** (0 fallback entries across 85 captured-B HITs, both takes/peers → dead as a fallback; the loud in-bracket ORDER ASSERT survives as the terminal genuine-destroy path). ⚠ residual: the request-route turn_off is measured-benign NOT structurally-safe (client twin absorbs the real-key PropSpawn) — see `[[project-kerfur-sole-express-dupe-2026-07-14]]` | host **KerfurId** (spans both forms) + the per-form eid | kerfur_entity, kerfur_convert, kerfur_command, kerfur_prop_adoption |

> **GATING UPDATE 2026-07-05 [V live-failure root-fix, `ff338d87`] — every catch/enroll above is
> HOSTING-gated, NEVER connected()-gated.** Identity/tracking seams (spawn interceptors, the EX-catch +
> its drain, the pose-walk dead-retires, the kerfur conversion poll's host branch) run whenever the
> session exists with role Host — an event actor spawned while the host is ALONE must still enroll, or
> the join connect-snapshot has nothing to re-send (the 0s pyramid failure: joiner saw an empty world
> mid-event). Only wire SENDS are peer-gated. Sibling fix in the same commit: the per-tick npc dead-retire
> had raced the 5 Hz kerfur conversion poll since 2026-07-03 and erased the ALIVE->DEAD evidence before
> the poll could read it as a conversion (even connected) — kerfur-family deaths returned to the poll
> (one owner per death edge). [[lesson-tracking-gates-on-hosting-not-connected]]

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
  host-authoritative. **[V]** Two INTENT doors route around it, host as the sole author: the F2
  **PropDropIntent** (a parked-key pocket→place, v106) and — v114 (L7, `ba8ce297`) — **ReelEjectIntent**
  (a client caddy/reelbox EJECT births a reel in-hand; the unparked reel-class client FinishSpawn in the
  F2 drain sends the intent; the host `HostSpawnPlacedProp`s it born-ASLEEP, class-whitelisted to the
  `Aprop_reel_C` lineage — NOT a general client-spawn door) **[AS-BUILT, smoke]**.
- **Save-scalar at birth (v114):** per-prop save state (`struct_save.mFloat[0]`-class — the reel's
  `Progress @0x364`) rides `PropSpawnPayload.savedScalar` (+flag 0x40) on EVERY birth path — live express,
  join snapshot, container extract, BOTH intent kinds — via ONE shared reader
  (`prop::ReadSavedScalarForClass`); ONE mirror-birth apply. Missing ANY path = a CDO-default mirror whose
  value a peer re-broadcasts as truth (the correctness-audit CRITICAL of the L7 build). **[AS-BUILT]**
- **Destroy:** engine `K2_DestroyActor` **PRE** → `PropDestroy(key,eid)` **[V]**; BP-internal vanishes
  (truck/cull/LifeSpan, EX_CallMath) → the host **reaper death-watch** (`ReapDeadLocalPropElements` →
  explicit `PropDestroy(eid)`, kerfur-skipped) **[V]**.
- **The floppy disc across the laptop slot (v116 `613f2ac4`):** insert = `floppyIn.K2_DestroyActor`
  (uber@7404) — the disc DIES into laptop scalars; that destroy rides the generic seam above (ONE
  owner — the planned lane-side eid capture was dropped once `prop_destroy_seam.cpp` proved to cover
  both roles). Eject = fresh `BeginDeferredActorSpawnFromClass`+`FinishSpawningActor` (uber@8593/8717)
  with `loadData` restore — the spawn rides the SAME birth channels as the reel (host watcher / the F2
  drain); the disc's CONTENT (strings — too big for `savedScalar`) rides the `LaptopState` DiscContent
  side-channel under HOST authority, correlated to a client-eject birth via the adoption eid-binding.
  A held disc's world identity died at pickup (hand-actor keyless+eidless = correctly seam-dropped).
  **[AS-BUILT v116, smoke; RE votv-laptop-pc-RE-2026-07-17.md FD-Q1]**
- **The desk module prop across the physMods sockets (v118 `45a886a4`):** plug =
  `K2_DestroyActor(holdActor)` inside `plugInModule` (in-hand, NO prior release — the native path
  itself destroys held actors) — rides the generic seam above, both roles (a client-held module's
  host twin already died at grab, so receivers no-op). Unplug = `lib.physModToActor(byte)` →
  BeginDeferred+Finish → `player."Hold Object"` (born INTO THE HAND): on the HOST the spawn watcher
  expresses it the same tick (the hand exclusion is only the hotbar view husk) and it rides normal
  carry; on a CLIENT it is invisible-while-held (doctrine) and births at DROP via the kind-104
  fresh-birth whitelist widened to `IsChildOf(Aprop_physModule_C)` (the byte rides the CLASS — no
  scalar channel). The ARRAY state itself is the `PhysModsState` lane (value-ops + host-canonical);
  a raced unplug's ghost is deny-swept, a raced dup-plug is host-REFUNDED (a fresh spawn at the
  desk). **[AS-BUILT v118, smoke + join-canonical in-log; design votv-physmods-L8-impl-DESIGN-2026-07-18.md]**
- **Connect reconcile:** R2 blob-vs-live key-diff deletes → SnapshotBegin/claim-tracking → bracketed
  PropSpawn stream → SnapshotComplete → quiescence-gated **divergence sweep** (membership = the client's
  own local Prop Elements, mirror-excluded; **>50% world-wipe valve**) → `EnsurePileBindIndex` position-bind
  for keyless piles (retires the client-local identity). **[V]**
- **JOIN BARRIER (2026-07-12 `bbf91f39` [V — take-6 hands-on 2026-07-12 13:21: user-verified the
  in-window placed rock present on the client; log-verified: client QUIESCED 13:20:38 STRICTLY before
  `ClientWorldReady announced`, 0 wire prop receives pre-announce, no DEGRADED, sweep fired 2.3 s
  post-arm, bind 872/872, 0 errors both peers] — SUPERSEDES the
  takes-1-4 join-window PROVISIONALITY machinery, RULE 2):** `ClientWorldReady` is announced at
  **load-tail QUIESCENCE** (the `coop/session/world_load_episode` probe latch — the same
  population-stability gate the doom sweep always trusted, now the ONE owner of the "is my world
  settled" axis), not at "world up + registry coherent". The host streams the entire connect replay
  (R2 deletes → snapshot → state lanes → teleport) only after the announce, so **no wire prop
  expression can arrive while loadObjects' churn runs** — nothing is ever provisional, and the
  reliable channel's in-lane FIFO is the only ordering authority (the MTA INITIAL_DATA_STREAM
  shape, CGame.cpp:1393). Retired with it: the in-episode spawn capture + revalidation drain
  (takes 1-2), the wire-order queue netting (take 4), the SnapshotBegin-lost watchdog
  quiescence-by-ceiling (the probe is wire-independent; the lost-bracket flake is a 30 s
  post-announce timeout in the sweep). KEPT: the outbound destroy-broadcast suppression episode
  (host-wipe fix; now self-closing at the probe latch), deferred destroys (travel window +
  probe-deadline DEGRADED mode), b3 pos-corrections, twins/ghost/kerfur retires (save-vs-wire
  STATE reconcile), the doom sweep (claims → doom judges last, take-3 order preserved), the fuzzy
  rekey-steal denial (`ResolveMirrorEidByActor(wireMirrorOnly)`), the dead-row TRIPWIRE + per-doom
  cls/key/loc logs. Design doc:
  `research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md`; the six-root saga RCA:
  `research/findings/join-identity/votv-join-window-placed-prop-RCA-2026-07-11.md`.
- **KEY-UNIQUENESS AUTHORITY (2026-07-11 `2fefd161`, repaired 2026-07-12 `460da7e4` [V — take-6
  2026-07-12: 162 re-keys, 0 `re-key FAILED` (the `460da7e4` SuperStruct-climb WORKS — the `2fefd161`
  form was 162x inert on the same save); UNCHANGED by the join barrier — the key-dedup axis is
  orthogonal to ordering — the `2fefd161` form was INERT: 162x "setKey not found
  on trashBitsPile_C", the family is actor_save_C lineage with setKey declared on the ANCESTOR and
  FindFunction is exact-owner; `460da7e4` = `R::SuperStructOf` + a SuperStruct-CLIMBING
  `ResolveSetKeyFn`, the Aprop_C hardcoded fallback removed]):** VOTV's own save data ships DUPLICATE
  interactable Keys — the live host save carried 85
  `trashBitsPile_C` across FOUR keys (65+12+5+3 save-born clone families; the 2026-06-24 sweep was
  already silently dooming "80 trashBitsPile" — same phenomenon, unnoticed). Every identity layer (key
  index, exact-key converge, keyed churn RE-BIND, R2 key-diff) assumes Key uniqueness; a clone family
  funnels all its wire rows onto ONE actor, the leftovers die at churn, and the revalidation drain then
  re-expresses them INTO already-occupied positions (→ interpenetrating awake physics = the take-3 fps
  storm). Fix: the HOST is the key authority (MTA shape — the server owns element-ID uniqueness). At
  enroll, inside `MarkPropElement` (the ONE enrollment owner — covers the census walk AND the Init-POST
  late-load catch; a census-only detector was an audit HIGH), a keyed actor whose Key is already carried
  by a DIFFERENT live actor is re-keyed with a fresh `rk_<64-bit-random-hex>` key
  (`prop_synth_key::MintFreshKeyForDuplicate`; GT-gated — setKey is a PE dispatch). A DEAD incumbent =
  churn recreate INHERITING its identity (spared by `FindLiveActorByKey` liveness). The game re-saves
  live keys per actor → the fix bakes into the host save + every transferred client world.
  `MarkPropElement` returns the enrolled key; broadcast callers build their payload key from the return.
  CLIENT/SP never re-key. First-enrollment-only (one-way door — documented at the idempotency early-out).

### chipPile + garbageClump (the dupe-critical family) — REDESIGN 2026-06-21, see [docs/piles/08](piles/08-HOST-AUTH-TRASH-CHANNEL.md)

> **NATIVIZATION 2026-06-30→07-01 [V hands-on — DONE] — the resting/re-piled PILE-form CLIENT MIRROR is now a
> ROOTED REAL `actorChipPile_C` NATIVE** (`coop/props/native_pile_mirror::Materialize`), not the bare
> `AStaticMeshActor` proxy (row below). Bound + marked save-native → rides the `IsBoundMirrorNative` machinery; a
> real pile IS `int_player_C` → native hover GUI + collision + rotation + occlusion + movement-block, FREE.
> SHIPPED + VERIFIED: inc 1 spawn-seam (`abfaaed8`), inc 2 re-pile LAND→native (`dabf84de`), chipType via the
> pile's own `init()` (RE export 80 — DEFAULT chipType=0 on a bare spawn, so `SetChipTypeAndRebuild` writes it +
> re-inits; `dabf84de`), rotation consumed host→client on the mesh COMPONENT (`3b72aba0`; `f79bbe84` is the
> delivery half, NOT retirable), and the held-clump-at-join **dup fix** (`fa8bc344`). The in-hand/flying **CLUMP
> stays the bare proxy** (LifeSpan + autonomous re-pile = too live). **[V] the join-window dup was TWO-ACTORS-per-eid
> (adopted save-loaded native @save-pos + convert-beat-spawn proxy @landed-pos); root = the convert-beat-spawn
> branch lacked the LAND-side symmetric half of GRAB's `morphBoundNative`, and `SweepReconcileSaveTimeTwins` is
> structurally BLIND to it (skips `IsBoundMirrorNative` = the leaked half). Fix = create-edge LAND CLAIM
> (`RepositionBoundNative` reuses the bound native, suppresses the parallel spawn). Two-owner map: bound-at-convert
> → create-edge claim; late-load-unbound → the sweep (disjoint).** The camera-cone grab recognition is dead for
> piles (a native IS `lookAtActor`). Deployed `1C242F82` = HEAD `fa8bc344`, 6 ahead of origin. Canonical:
> [docs/piles/11](piles/11-PROXY-TO-NATIVE-NATIVIZATION-2026-06-30.md) · [[project-pile-nativization-2026-06-30]].
> The proxy rows below stay TRUE for the clump + the not-yet-nativized
> pile paths.

> **IDENTITY UPDATE 2026-06-27 [RD] — save-loaded chip/kerfur natives gained a position RE-BIND safety-net.**
> The primary identity is the save-identity ORDINAL bind (Build 3, by saveSlot array index). But UE's incremental
> GC sporadically destroys + re-instantiates a SPARSE handful of save-placed natives mid-join; they re-create at
> their save pos UNBOUND (the cursor was consumed) = ghosts. Variant-1 (`54ee4b06`; the rebind step is
> `save_identity_bind::BindUnboundReCreates`, sequenced by the order owner `coop/element/quiescence_drain::RunReconcile`
> -- was `identity_reconcile`, consolidated 2026-06-30; VERIFIED real log 16:06 2026-06-28 — `RE-BIND by position` x242 on the valve-abort
> path, world ended clean) re-binds them at quiescence by an authoritative host-sent save-position (1cm,
> ambiguous-skip) — the bind seam can't supply the position itself (BeginDeferred POST = `(0,0,0)`, below). See
> `research/findings/join-identity/coop-purge-timing-reconcile-race-DESIGN-2026-06-27.md` + `docs/COOP_STABLE_ID_SIDECAR.md`.
>
> **IDENTITY UPDATE 2026-07-03 [AS-BUILT `2ab718d5`, smoke-V] — identity survives actor churn (the re-bind
> thread; docs/piles/12 status block is canonical).** (1) The chip entry's savePos is IMMUTABLE (a purge
> re-create ALWAYS spawns there — loadObjects replays the save arrays); the host's PropSnapPos position is a
> separate hostPos OVERLAY; the RE-BIND is two-phase (@host first — churn-survivor/resurrect protection,
> excluding natives at any free entry's @save; @save fallback + same-pass pos-correction snap). The earlier
> retrack-savePos-to-@new (take-3) made a purged eid permanently unbindable = the 20:24 client-local dup +
> the pinned 4 Hz drain. (2) Raw IsLive on row-held pointers misreads freed memory — every element liveness
> test is IsLiveByIndex now. (3) Twins retire ONLY on positive evidence (E bound live); the unconfirmed-
> retire arm + >50% cap are GONE. (4) Every drain queue is pass-capped (twins 40 / destroys 8 /
> pos-corrections 40 / kerfur retires 40). (5) KEYED props gained the churn re-bind the chipPile position
> lane already had: the join sweep re-binds an unclaimed keyed re-create onto its dead-actor mirror row by
> KEY (claim transferred), and a HOST-authoritative PropSpawn for an existing mirror row with a different
> actor REBINDS in place (1:1-guarded via EidForActor; displaced actors never destroyed) — the deny-heal
> re-assert is no longer an Install-reject NO-OP.

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
> `research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` + COOP_DISPATCH_VISIBILITY
> "Catching an EX_CallMath call".
>
> **⚠⚠⚠ The CLIENT MIRROR of trash is NO LONGER a real `actorChipPile_C`/`prop_garbageClump_C` BP — it is a
> host-authoritative `AStaticMeshActor` PROXY** (`coop/trash_proxy`, Movable, NoCollision; HEAD `a5282f57`,
> deployed `015F0AC9590B6B23`, proto v83). The durable facts below (KEYLESS, the bytecode mechanic, the
> OBSERVABILITY) describe the GAME's chipPile/clump on the HOST (where the real verbs run); on the CLIENT we no
> longer instantiate the BP at all — we own an `AStaticMeshActor` (NO blueprint, `AddToRoot`, eid→actor
> registry, re-skin in place). This **closed the client mirror-staleness dup (hands-on [V]; piles mirror
> VISIBLY after the Movable fix)**, and the **live clump carry-FREEZE is now [V hands-on] FIXED** via the
> **`!carrying` release-edge gate** in `local_streams.cpp` (the client clump UPDATES through the carry — no
> freeze between E-events). The earlier "carry MIRRORS on a settled join / it was the JOIN RACE" claim was
> **WITHDRAWN as FALSE**; the actual release-edge cause was `updateHold` PUPPET RECREATION (the `heldActor` ptr
> changes with `pendingSettle=0`), NOT a contact-re-pile churn. **Carry JANK — FIXED [V hands-on]** (shipped
> `df158728`): the `key.len=4`→keyless theory was DISPROVEN (BP `GetKey` returns the FName `"None"` for both
> forms → the receiver already guards `keyW != "None"`→eid; keyless was a no-op); REAL root = interp
> PHASE-STALL (`BeginLerpToPose` set `lerpStartMs=nowMs`, `AdvanceLerp` sampled the same `nowMs` → alpha=0 every
> new-pose tick); FIX = fixed-delay snapshot interpolation (2 timestamped poses, render behind, MTA
> `CClientVehicle` shape) → carry now SMOOTH. **THROW ARC — VERIFIED [V hands-on take-29 + harness]** (shipped
> `136ed779`): the arc flies (the autotest does a real directional throw; harness arc-flight-stream PASS),
> streamed THROUGH the release (`!carrying`-SKIP streams the LIVE clump's flight under E until it re-piles,
> `IsLive`-gated; both `simulateDrop` AND `dropGrabObject` thunks fired ZERO → verb-detection ABANDONED).
> **Pile-landing ROTATION + throw SOUND — FIXED [V hands-on take-30]:** rot re-read from the SETTLED pile at the
> land-settle COMMIT (`trash_channel.cpp` TickCarry land-settle COMMIT re-read; thunk passes the clump as fallback in trash_collect_sync.cpp -- line refs shifted by the 2026-07-10 splits);
> the pickup-cue killed (flight branch streams the carry key, no re-StartDrive; a trash eid sends NO
> PropRelease; `OnHostRelease` RETIRED, RULE 2). **Z/HEIGHT — FIXED [V harness]** (regression arc): take-31 read
> the pile transform from `newActor` at the BeginDeferred POST = `(0,0,0)` (unpositioned pre-FinishSpawning) →
> derived piles snapped to world origin; take-32 re-reads the real transform at the land-settle COMMIT
> (`trash_channel.cpp:248-256`), harness drift=0cm; the native-destroy was INNOCENT (harness-confirmed), the bug
> was the `(0,0,0)` loc. **Proxy SCALE — AS-BUILT** (re-read `GetActorScale3D` at the same COMMIT; the prior
> "@protocol.h had scale" was `PropSpawnPayload`, not `PropConvertPayload`). **ORPHAN dup — DESTROY BUILT +
> VERIFIED [V harness]:** DERIVED piles dup GONE [V hands-on]; ORIGINAL (level-placed) piles' NATIVE
> level-loaded chipPile coexisted with the host proxy (the sweep is blind to natives) → now
> `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at proxy-spawn (exact ~1cm + chipType +
> IsLiveByIndex, exact-or-skip on >1, graceful on 0, join-bracket-gated; harness 12 twins/0 SKIP; the read-only
> PILE-PROBE is REPLACED, RULE 2). **FPS ~4s stutter — FIXED [V harness]** (`net_pump.cpp:559` guards the
> steady-world re-seed on the GUObjectArray high-water mark + a ~20s census so the ~237k walk is skipped at
> rest; rate 0.073/s, was ~0.25/s). The **`simulateDrop` throw-velocity flip is DEAD** — see THROW ARC above.
> Host carries fine; other props mirror fine. Option 1 FAILED; option 2 (the `holdPlayer` convert/ctx gate) is
> **DISPROVEN by bytecode** — `holdPlayer` is set ONCE on grab and NEVER cleared in any BP (DEAD, not pending).
> STILL OPEN: the WHOOSH throw sound (no ReliableKind in `protocol.h`; user-deprioritized). (The dead
> `dropGrabObject` thunk: RETIRED 2026-07-10 `fb490e36`.) The autonomous harness:
> `[[reference-pile-test-harness]]`. See the proxy bullet under "The design (08…)" below + the new canonical
> finding `research/findings/piles-trash/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.

**Durable facts (survive the redesign):**
- **chipPile is KEYLESS** — `setKey` is a no-op; the only identity is the host-minted eid. **[V/RD]**
  Caught by the seed-walk keyless-pile lane (`IsChipPile`) → connect snapshot / R1 re-seed. **[V]**
- **IDENTITY MECHANISM UPDATE (2026-06-27/28, sync-consolidation refactor) [RD]:** Prop identity is now ONE
  owner end-to-end — `element::Registry` (the unified `actor→eid` reverse `EidForActor`) + the
  `coop::element::CreateOrAdoptPropMirror` bind keystone. THREE leaked satellites are now DELETED:
  `g_propElementsById` (earlier), `g_boundMirrorNatives` (the is-save-native SET → now `Element::IsSaveNative`,
  `066d0a49`), and `g_actorToPropElementId` (the locals-only actor→eid map → now `EidForActor`, `6aeaf55c`;
  `GetPropElementIdForActor` re-imposes the locals-only contract with an `IsMirror` filter, and idempotency is
  now safer — it also blocks minting a local over a live mirror, a dup the old map couldn't see).
  `RegisterPropMirror` is now a thin forwarder to `CreateOrAdoptPropMirror` (`ecdc527c`). (2) "is this actor a
  bound save-loaded native" (`IsBoundMirrorNative`) resolves `actor→eid` via `EidForActor` → `IsSaveNative()` +
  liveness — atomic with the binding, can never read stale (was the 15:01:49 D1 root). (3) The join reconcile
  (twin-retire / variant-1 / b3 / deferred-destroy / kerfur-retire) is NO LONGER a join-window one-shot: it is the ONE
  order owner `coop/element/quiescence_drain::RunReconcile` (was `identity_reconcile::RunIdentityReconcile`, consolidated
  2026-06-30), driven by the join sweep AND a steady trigger (`OnTick`: pending-twin/b3/destroy/kerfur work OR a 6s
  post-purge window) AND the >50% valve-ABORT path.
  Commits `ce132e0d`/`066d0a49`/`47384057`/`432183ce`/`8b85cb2e`/`ecdc527c`/`6aeaf55c`. **VERIFIED real log
  16:06 2026-06-28: fix #1 (valve-abort reconcile, 242 RE-BIND) + fix #2 (post-purge window, 13 fires) BOTH
  fired, world clean.** Keystone (CreateOrAdopt + satellite retires) build GREEN. NOT-yet-hands-on: D1
  grab-no-dup, D2 kerfur-no-flash (the interaction scenarios). See [[project-sync-module-refactor-2026-06-27]].
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
  research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md]**

**The design (08, host-authoritative state machine):** the eid is a host-minted life-stable logical trash
entity (state PILED/HELD_BY(N)/FLYING; **position is NEVER identity for the GRAB** → the cluster mis-bind is
impossible by construction). Host owns the authoritative state; receivers drive a local visual from it.
Client-grab = **camera-ray cone recognition + `GrabIntent` → host executes the real verb on puppet-N + a
host-AUTH per-eid carry pose stream + `ThrowIntent`** (the FULL CHAIN, AS-BUILT + [V harness], proto v85,
HEAD `29353191`; see the Increment-2 bullet below). A sync-time-context byte rejects stale packets.
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
  CONVERT flip is now VERIFIED [V hands-on take-30] (re-pile + rotation correct at the land-settle COMMIT). The
  proximity death-watch (`WatchClumpForRepile` /
  `FindNearestUntrackedChipPile_`) is RETIRED (RULE 2). **Former known-minor [RESOLVED]:** the ~5s
  vanish-return (reaper-vs-rebind) is gone by construction (the convert lands the new pile under E the same
  tick the clump dies). RE: `research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` §3/§6.
- **GRAB-via-thunk + the eid=0 adopt-miss close — [DESIGN], NOT built:** move the grab (pile→clump) to the
  same thunk (srcObj = tracked chipPile → kToClump), retiring the InpActEvt-PRE + held-edge adopt and
  catching the grab even when the PRE missed (an UNTRACKED clump currently skips the converter). A tightening.
- **Increment 2 (CLIENT-grab direction) — the FULL CHAIN, AS-BUILT + [V harness]. proto v85, HEAD
  `29353191`, deployed `BB94A120A969A51E` (push held).** A client AIMS at a mirrored pile, GRABS, CARRIES,
  THROWS, it re-piles — all verified (0 CRITICAL fail) via the REAL E-press path.
  - **Recognition = a CAMERA-RAY CONE** (`trash_proxy::EidForAimedPileProxy`) in `OnPileGrabPre` — picks the
    most-centered pile proxy on the view-camera forward. The "suppress-native + read `lookAtActor`/
    `lookatActorCurrent`" plan + the proxy QueryOnly collision are **RETIRED (RULE 2)**: a bare AStaticMeshActor
    proxy can NEVER be `lookAtActor` (the `int_player_C` filter at LookAtFunction @3250) AND the worn pile mesh
    has no simple collision body (harness `trace-gate hit=0`). No suppress-native needed (the proxy fails the
    native cast on its own). E-press TOGGLES grab vs throw via the client carry-state.
    `[[lesson-proxy-never-lookatactor-use-camera-cone]]`
  - **Carry visibility = a NEW host-authoritative per-eid pose stream** (`MsgType::TrashCarryPose=34`,
    `TrashClumpPoseSnapshot` 32B, the `StoreRemoteWorldActorBatch` pattern). A client only drives pose slot 0
    + the relay never echoes to the origin, so the host-driven puppet-held clump's pose MUST be host-
    originated; every client drives the proxy via a per-eid `ActiveDrive` interp (`coop/trash_clump_pose_stream`,
    `coop/puppet_carry_drive::Tick` publishes carry + flight). Scales to N simultaneous client grabs.
  - **Throw** = `OnThrowIntent` releases the puppet grab (the re-pile gate reads not-held) + applies physics
    velocity along the puppet aim → the clump flies (flight stream) + self-re-piles via its own ground-hit
    thunk → ToPile. `ThrowIntentPayload` is eid-only (the host derives the direction).
  - **Abort (audit HIGH fix)** = a clump lost mid-carry → `ReleaseClientHold` broadcasts `PropDestroy(eid)`
    so clients retire the frozen proxy + `ClearClientCarry` (else the client was stuck in throw-mode forever).
  - The interp was EXTRACTED to `coop/active_drive.h` (remote_prop 1375→1222, RULE 2026-05-25; carry
    regression confirms zero behaviour change). Finding:
    `research/findings/physics-grab/votv-increment2-clientgrab-FULL-CHAIN-AS-BUILT-2026-06-23.md`. **NEXT (greenlight):**
    a `garbageCollider`-analog SHAPE component on the proxy for occlusion-correct aim + movement-block (the
    cone ignores walls; the proxy is still walk-through).
- **CLIENT MIRROR of trash = a host-authoritative `AStaticMeshActor` PROXY — DUP-FIX (derived) + VISIBILITY +
  CARRY-FREEZE + carry-JANK + THROW-ARC + ROTATION + SOUND all [V hands-on] VERIFIED; Z-fix + LEVEL-PILE
  dup-DESTROY + FPS-fix [V harness]; proxy SCALE AS-BUILT. HEAD `a5282f57`, deployed `015F0AC9590B6B23`,
  proto v83:** the client's mirror of a chipPile/clump is NO LONGER the real self-morphing BP — it is an
  `AStaticMeshActor` WE own (`coop/trash_proxy`): NO blueprint (never self-morphs), `AddToRoot` (never GC'd →
  never stale-index), our eid→actor registry (`g_proxies`), re-skin (`SetStaticMesh`) IN PLACE on convert
  (never spawn-fresh). **Spawn** = `trash_proxy::SpawnProxy` (caught at the `OnSpawn` trash-class branch,
  BEFORE the BP dedup); **identity** = the eid in `g_proxies` + the Prop mirror (`RegisterPropMirror`);
  **re-skin** = `OnConvert` → `IsProxy(E)` → `ReskinProxy`; **destroy** = `OnDestroy`/disconnect →
  `RetireProxy` (`DestroyActor → RemoveFromRoot → unbind`). The proxy's `UStaticMeshComponent` is set
  **Movable** (`SetComponentMobility`, `245148c6`) because a runtime `AStaticMeshActor` defaults to STATIC, on
  which `SetStaticMesh`/`SetActorLocation` silently no-op. This **closed the former OPEN client
  mirror-staleness dup** (a real-BP join-mirror went NOT-LIVE within ~10s → `ResolveLiveActorByEid` null →
  fresh-clump spawn + the original lingering = the dup) — now impossible BY CONSTRUCTION (the 3-verdict
  discriminator / health-poll / serial-check plan is DROPPED as moot); the dup-gone + the resting/landed-pile
  mirror are **hands-on confirmed [V]**. **The LIVE CARRY-FREEZE is [V] FIXED hands-on** via the **`!carrying`
  release-edge gate** in `local_streams.cpp` (`else if (g_lastHeldProp &&
  !coop::trash_channel::IsCarrying(g_lastHeldEid))`) — the client clump UPDATES through the carry now (no freeze
  between E-events). The earlier "carry MIRRORS on a settled join / the failure was the JOIN RACE" claim was
  **WITHDRAWN as FALSE**; the actual release-edge cause was **`updateHold` PUPPET RECREATION** (the `heldActor`
  ptr changes with `pendingSettle=0`), NOT a contact-re-pile churn — which is why the `carrying &&
  HasPendingSettle` gate (`C9F28176`/commit `16ac153f`) FAILED and suppressing the WHOLE carry (`!carrying`) is
  the fix. **Host carries FINE** (native); **OTHER props mirror fine** (pose-stream). **NOW FIXED/BUILT this
  session:** (1) carry **JANK — FIXED [V hands-on]** (shipped `df158728`): the `key.len=4`→keyless theory was DISPROVEN
  by bytecode (BP `GetKey` returns the FName `"None"` for BOTH `prop_garbageClump_C` and `actorChipPile_C`, so
  `key.len=4` is the literal string "None"; the receiver already guards `keyW != "None"`→eid at
  `remote_prop.cpp:403`, so forcing keyless was a no-op). REAL root (code-proven): an interpolation
  PHASE-STALL — `BeginLerpToPose` set `lerpStartMs=nowMs`, `AdvanceLerp` sampled the same `nowMs` → alpha=0
  every new-pose tick at vsync-60. FIX = fixed-delay snapshot interpolation (`remote_prop.cpp` ActiveDrive
  buffers 2 timestamped poses prevLoc/lastLoc + prevPoseMs/lastPoseMs; renders `nowMs-span` behind; alpha by
  real timestamps; MTA `CClientVehicle` shape). User hands-on: carry now SMOOTH like a normal object. (2) proxy
  **SCALE — AS-BUILT** (shipped `df158728`, proto v82→v83; the COMMIT re-reads `GetActorScale3D`): the prior
  "`PropConvertPayload` has `scaleX/Y/Z` @protocol.h:2208" was WRONG (that was `PropSpawnPayload`); added
  `scaleX/Y/Z` to `PropConvertPayload` (static_assert 100→112), `BroadcastConvert` sends per-form
  `GetActorScale3D(newActor)`, new `ue_wrap::engine::SetActorScale3D`, the proxy applies it via `ApplyProxyScale`
  (guarded >0.001) in `SpawnProxy`+`ReskinProxy`; not separately eyeballed (covered by the commit re-read).
  (2b) pile-landing **ROTATION + throw SOUND — FIXED [V hands-on take-30]:** rot is re-read from the SETTLED
  pile at the land-settle COMMIT (`trash_channel.cpp:248`; the thunk passes the clump as a fallback
  `trash_collect_sync.cpp:91`); the pickup-cue-on-throw is killed (the flight branch streams the carry key
  without a re-StartDrive + a trash eid sends NO PropRelease; `OnHostRelease` RETIRED, RULE 2). (2c) the
  **Z/HEIGHT regression arc — FIXED [V harness]:** take-31 read the pile transform from `newActor` at the
  BeginDeferred POST = `(0,0,0)` (pile unpositioned pre-FinishSpawning) → derived piles snapped to world origin;
  the take-32 FIX re-reads the pile's REAL transform at the land-settle COMMIT (`trash_channel.cpp:248-256`),
  harness client-render-matches-host drift=0cm; the native-destroy was INNOCENT (harness-confirmed), the bug was
  the `(0,0,0)` loc. (3) **ORPHAN dup — SPLIT by pile origin; the DESTROY is now BUILT + VERIFIED [V harness].**
  DERIVED (gameplay-born) piles: dup GONE [V hands-on] (born only as a host convert → a proxy on both sides, no
  native twin). ORIGINAL (level-placed) piles: the client's NATIVE level-loaded chipPile coexisted with the
  host proxy (root CONFIRMED: level-piles get an eid+proxy via the eidOnly lane; the divergence sweep is BLIND
  to natives) — now `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at a pile proxy-spawn (exact
  ~1cm + chipType + IsLiveByIndex; exact-or-skip on >1; graceful on 0 matches; GATED on `g_claimTrackingActive`
  = the join bracket only; harness dup-destroy-clean 12 twins / 0 SKIP). NOT adopt (adopt re-introduces the BP
  self-morph the proxy avoids). The read-only PILE-PROBE that confirmed coexistence is REPLACED by this destroy
  (RULE 2, retired). (Supersedes the prior "eid-resolve race / `isProxy=0` spawn-fresh" theory — it was native
  coexistence.) (3b) **FPS — the ~4s stutter FIXED [V harness]:** `net_pump.cpp:559` guards the steady-world
  re-seed on the GUObjectArray high-water mark (NumObjects) + a ~20s safety census, so the ~237k-object walk is
  SKIPPED at rest; harness fps-reseed-rate 0.073/s (was ~0.25/s every 4s). (4) **the `simulateDrop`
  throw-velocity FLIP is DEAD — REPLACED by carry/flight stream-continuity; the THROW ARC is VERIFIED [V
  hands-on take-29 + harness]** (shipped `136ed779`): both the `simulateDrop` thunk AND its successor
  `dropGrabObject` thunk fired ZERO times across 7 grab/release cycles (the clump release uses NEITHER verb —
  the clump rides `grabbing_actor`, the PHC handle, not `holding_actor`) though the same Func-thunk facility
  fired all run; verb-detection ABANDONED. PIVOT: the throw needs no verb/velocity — the host's thrown clump
  really flies (physics) until it re-piles, so `local_streams.cpp`'s release-edge `!carrying`-SKIP branch now
  CONTINUES streaming `g_lastHeldProp`'s pose under the same eid E while it is a LIVE garbageClump (`IsLive` is
  the churn/flight discriminator — a churn re-pile kills the clump → skip; a real release leaves it flying →
  stream); the client's fixed-delay interp shows the arc; it ends when the clump re-piles (ToPile
  re-skins+snaps). The arc FLIES (user: "дуга ЛЕТИТ"; the autotest now does a real DIRECTIONAL throw). The dead
  `dropGrabObject` read-only thunk was RETIRED 2026-07-10 `fb490e36` (RULE 2; retirement notes left
  in trash_collect_sync.cpp).
  **Dead ends:** **Option 1** (`8bc797ef`,
  `SetNotifyRigidBodyCollision(false)` on the held clump) BUILT + FAILED — the live host BP re-arms hit-notify.
  **Option 2 (DISPROVEN by bytecode, NOT pending):** the `holdPlayer` convert/ctx gate is DEAD — `holdPlayer`
  (`@0x0240 AmainPlayer_C*`, CXXHeaderDump-confirmed) is set ONCE on grab (`actorChipPile.json` @8492) and
  NEVER cleared in any BP, so it cannot mark "released." (CLOSE-B latch + land-settle SHIPPED `65AD883A` —
  correct, not the freeze cause.) **Phase 1 = visual + position + re-skin, EXPLICIT NoCollision**; collision
  (the `garbageCollider` hull) is PHASE 2 with Increment 2. Design + AS-BUILT:
  `research/findings/piles-trash/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`; the carry root + fix (the
  canonical doc): `research/findings/piles-trash/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`; the autonomous
  harness: `[[reference-pile-test-harness]]`. **STILL OPEN (NEXT):** a `garbageCollider`-analog SHAPE component
  on the proxy (occlusion-correct aim + movement-block — the client-grab camera cone ignores walls, the proxy
  is walk-through); the WHOOSH throw sound (no ReliableKind; user-deprioritized, best confirmed by hearing);
  the `event_dispatch_trash.cpp` extraction. (The dead `dropGrabObject` thunk: RETIRED 2026-07-10 `fb490e36`.)
  (Increment 2 client-grab FULL CHAIN is AS-BUILT v85 — see the dedicated bullet above.)
  **[V hands-on: dup-fix(derived) + visibility + carry-freeze + carry-JANK + throw-arc + rotation + sound; V harness: Z-fix + level-pile dup-DESTROY + FPS-fix; SCALE AS-BUILT; option 2 DISPROVEN.]**

### NPCs / Characters
- Host `BeginDeferred` **interceptor** (allowlist of 15 ACharacter bases) allocs an `Npc` Element + POST
  binds the actor; **client suppresses its own** local spawn (zero ReturnValue + return true) and
  materializes from the wire (`OnEntitySpawn`). Save-loaded twins (`savePersisted=1`) → **deferred adoption**
  by class+nearest-pose (never by key — the key is random per peer). Pose via `EntityPose` batch (>31
  tracked → fair-share rotation at the publisher, the MTA far-sync shape). Destroy
  via `K2_DestroyActor` PRE → `EntityDestroy`; a bound actor found dead in the pose walk (PE-invisible
  self-destroy) → `SyncDestroyedNpcByEid` retire + `EntityDestroy` (2026-07-03). **[V]**

### wisp_C (the wispSwarm-event swarm — the EX_CallMath NPC; 2026-07-03, smoke 32/32×4 legs)
- **Spawn dispatch is `EX_CallMath` BeginDeferred** from `trigger_wispSwarm_2`'s ubergraph (up to 32, one
  per 0.25–1.0 s, annulus 500–750 m out / 500 m up) — the PE interceptor+POST NEVER fire. Caught by the
  **`ufunction_hook` Func-thunk** on the same UFunction (chains after trash_collect's), **SOURCE-GATED by
  `FFrame::Object`'s class == `trigger_wispSwarm_C`**, queued (fires PRE-Finish, transform unset), enrolled
  at the next pose tick (`npc_world_enum::DrainPendingExSpawns` → the shared `EnrollUntrackedNpcActor`). **[V smoke]**
- **AMBIENT sky wisps: REVERSED to host-auth 2026-07-10 eve** (`08560687`). The old "per-peer by design"
  call assumed player-anchoring; the bytecode dump shows ABSOLUTE map coords (+-60-70k X/Y, Z=80k) ->
  world-anchored. Now: `ticker_wispSpawner_C` IS an EX-catch source row, the 8 color variants
  (`wisp_b/red/bl/w/g/o/p/blu_C` -- direct ACharacter subclasses, NOT wisp_C children) are allowlisted
  (15->23), and the client's ticker tick is cancelled (spawn_authority t3). **[AS-BUILT; smoke: 23/23
  resolve both peers]**
- **Mirror keeps its ACTOR tick** (`DisableMovementTick`, CMC-only park): the wisp's ReceiveTick is
  per-viewer cosmetic design (fade gate, effect bob, shy-despawn on local approach/dawn, 1 Hz bad-camera
  scan is a BeginPlay latent either way). It spawns INVISIBLE; fade-in fires at the landing edge gated on
  **CMC `CurrentFloor.bBlockingHit`** — CMC-tick-computed, stale on a parked CMC — so the pose drive
  replays the edge (`ue_wrap::wisp::DriveWispLanding`: write `landed` + call `dir(true)`) when the
  streamed pose reads grounded. **[V smoke: resolve at +9 s = wisp #1's fall time]**
- **Destroy is an `EX_VirtualFunction` SELF-call** (timeline-reverse-finished → `K2_DestroyActor`; despawn
  check = local camera <100 m OR sun outside the night band, tick-driven, landed-gated) — the K2 PRE never
  sees it. Host pose walk catches the bound-but-dead actor → `SyncDestroyedNpcByEid` → `EntityDestroy`;
  the client mirror may also shy-despawn client-locally first (per-viewer SP design; the wire destroy then
  no-ops on "already not-live"). **[V smoke: 32/32 dead-retired at +6 s after forced midday, 32/32 client teardowns]**

### WorldActors (event actors)
- A **second** `BeginDeferred` interceptor with a **disjoint, NAME-matched** allowlist (17 leaf classes;
  name-match because event classes load lazily). FULL-rotation pose (`WorldActorPose`). Otherwise the NPC
  shape. **[V]**
- **2026-07-04 (piramid lane): two seams the interceptor shape alone missed, both closed generically:**
  (1) an EX_CallMath spawner (`piramidSpawner_C.runTrigger` — ALL its BeginDeferreds invisible to PE)
  is caught by `npc_world_enum`'s source-gated Func-thunk, whose drain feeds
  `world_actor_sync::HostEnrollExSpawn` (same end state as interceptor+POST; reverse-map dedup).
  (2) event-end SELF `K2_DestroyActor` never hits the destroy PRE observer → pose-walk **dead-retire**
  in `TickPoseStream` (bound-but-dead ⇒ retire + WorldActorDestroy broadcast) — this closed a latent
  mirror LEAK for every WA class, not just the pyramid. **[V: piramid e2e 2026-07-04 23:19]**
- `piramid2_C` additionally gets a choreography lane (`coop/creatures/piramid_sync`: client brain
  suppression + PyramidGather replay) — docs/events/piramid.md. **[AS-BUILT; walk at true scale V live
  2026-07-05; facing v100 pending 0s-FACING2]**
- **2026-07-05 (the frozen/small/wrong-facing pyramid arc — three wire truths, all classes):**
  (1) **v99 Scale3D in EntitySpawnPayload** (`419e3894`): the deferred-spawn FTransform CARRIES scale
  (piramid = 2.0) and a mirror spawned without it renders wrong-size + hovers at the wrong altitude
  (the host streams Z from ITS 10000*scaleX hover). All 6 EntitySpawn senders fill it (WA + NPC:
  interceptor / ex-enroll / connect-snapshot each); both receivers apply it into the spawn transform
  through `SanitizeWireScaleAxis` (scale-0 wire = invisible actor). **[V live: «пирамида идёт»]**
  (2) **v100 auxYaw in WorldActorPoseSnapshot** (`75e5ab10`): a class whose VISIBLE heading lives
  OUTSIDE the actor rotation (piramid: root yaw stays 0 forever — [WA-TRACE] live evidence; the
  AnimBP orients the body off the movementVector ArrowComponent) streams that heading as pose-batch
  auxYaw; the class lane consumes it post-apply (`piramid_sync::ApplyMirrorHeadingYaw` → BOTH heading
  components). Deriving it from position deltas was LIVE-REFUTED (native heading keeps easing toward
  the walk target up to 10 s after motion stops). **[AS-BUILT; 0s-FACING2 pending]**
  (3) **[WA-TRACE] permanent 1 Hz telemetry** (`c98c6543`) at all 5 pose hops incl. the
  K2_SetActorLocation/Rotation RETURN values — born because the "first batch" client log proved
  ARRIVAL only and the 07-04 e2e was geometry-blind (the wisp repin ring sat within the arrive radius
  of a FROZEN mirror too). The pose chain itself was then proven alive live (delta ~100 units).

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
- **The 2026-06-29 "two roots" are BOTH CLOSED [status verified against code 2026-07-12]:** ROOT 1
  (the `fromConvert`/`deferKerfur` arg-slot swap) fixed at `kerfur_convert.cpp:332-344` (explicit
  `fromConvert=false, deferKerfur=false` with the ROOT-1 comment); ROOT 2 (mid-join KerfurConvert
  drop) closed by v91 (`retireOffEid` rides the npc EntitySpawn, which the connect snapshot always
  delivers) + the 2026-07-12 JOIN BARRIER (no mid-churn window). Historical design doc:
  `research/findings/kerfur/kerfur-identity-authority-and-module-refactor-DESIGN-2026-06-29.md`.
- **TAKE-9/10 AFTERMATH + THE VM PLAN (2026-07-13):** take-9 exposed the turn-ON twin pair (client
  destroy-seam relayed a kerfur-mirror death → host killed its authoritative prop → «kerfur deleted
  everywhere»; host-own turn-on had structurally NO converge — this same destroy seam drains the prop
  element synchronously with the death, so the poll's premise never holds for a host prop). The take-10
  fix (`873ce32a`: destroy-edge capture + fresh-spawn stamps + eid-first OnDestroy) REGRESSED: the
  capture NEVER fired — the destroy seam is a **POST** hook, `GetActorLocation` on the already-destroyed
  actor reads (0,0,0) and the 5 m filter silently declined every candidate (both silent exits had no
  logging). eid-first OnDestroy (kept — correct fix) flipped the visible symptom from "stale off-prop"
  to "dies on the other peer". **Resolution: /qf 15 design pass (13 rounds, converged) →
  `docs/COOP_VM_DISPATCH_PLAN.md`** — VM verb brackets + the form-flip assembler replace the whole
  compensation family; the key reframing: the conversion verbs are `EX_LocalVirtualFunction` (VM-
  hookable via GNatives swap) and their INNER spawns are EX_CallMath→Func-VISIBLE (the "wall" was only
  ever EX_Local*→script). [[lesson-post-destroy-seam-reads-zeros-and-silent-declines]]
- **KERFUR FIRST-REFUSAL (2026-07-12 `ded3f793` [turn_off converge LIVE-FIRED 2x clean — take-10 log
  12:59:10-11, 0 cm; slated for retirement into the VM assembler per the plan]):**
  the death-watch poll's host turn_off converge premise ("the verb's fresh prop is UNTRACKED") died
  with spawn_authority Inc-1 (2026-07-10) — the per-tick FinishSpawningActor seam drain expressed the
  conversion prop first, the poll's converge found it tracked and silently released the dead NPC with
  NO KerfurConvert (take-8 14:43 five-for-five; the peer kept the NPC mirror + gained a generic prop
  mirror = the toggle dupe). **SUPERSEDED 2026-07-14 (`de3dccb5`):** the SUPPRESSION now gates on the
  DETERMINISTIC `kerfur_form_assembler::IsCapturedForm` (this prop IS the captured conversion successor)
  — NOT the "dead un-handled kerfur-NPC within 5 m" PROXY below, which RACED the `dropKerfurProp` verb's
  spawn-vs-destroy order (NPC still alive at the prop's Init POST → declined → keyed express leaked →
  double-express dupe, 19:09 take). `TryAdoptFreshKerfurProp` keeps only its CONVERGE role (now gated
  behind the capture). The captured prop is TRACKED (so `host_spawn_watcher`/M2 defer) but its generic
  PropSpawn SUPPRESSED — track-but-don't-broadcast. See `[[project-kerfur-sole-express-dupe-2026-07-14]]`.
  **Historical (pre-`de3dccb5`) as-built:** every generic express lane (the shared Init-POST/ExpressSpawnedProp
  body + `BuildPropSpawnPayload_`) offered a fresh kerfur PROP to
  `kerfur_convert::TryAdoptFreshKerfurProp` FIRST: UNTRACKED + a dead un-handled kerfur-NPC watch
  within 5 m ⇒ converge at the spawn edge (silent register → BindFormActor → KerfurConvert); else
  (hand-place / purchase) the generic path keeps same-tick expression. Audit CRITICAL fixed pre-ship:
  the untracked-only guard (without it the Registry-fed builder site could only STEAL a standing
  off-prop's identity). RCA:
  `research/findings/kerfur/votv-kerfur-eyecam-childactor-identity-leak-RCA-2026-07-12.md` (take-7
  half) + the take-8 half in `research/handson_runbook_2026-07-12.md`.
- **KERFUR EYE CAM = CHILD ACTOR, OUT OF THE PROP UNIVERSE (2026-07-12 `c93617be` [V — take-8
  hands-on «камер нету»]):** both kerfur forms carry `prop_camera_good_C` as a `UChildActorComponent`
  child — a keyed Aprop-lineage actor the GAME excludes from its world-object universe
  (`Aprop_C::ignoreSave = ignoreSav || IsChildActor()`, prop_base bytecode). Our identity layer
  enrolling/broadcasting them = the take-7 floating CCTVs + the joiner's own eye cams doomed. ONE
  predicate `ue_wrap::engine::IsChildActor` (raw reflected `AActor.ParentComponent` weak-ptr read),
  SIX surfaces: MarkPropElement, Init-POST catch, destroy seam, FindNearbySameClass, SeedWalk_,
  BuildPropSpawnPayload_. [[lesson-child-actors-excluded-from-world-object-universe]]

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
| client real-BP trash mirror self-morphs/GCs/goes-stale → `OnConvert` NOT-FOUND → spawn-fresh dup | **RETIRED** — the client mirror of trash is no longer a real BP; it is the host-authoritative `AStaticMeshActor` proxy (`coop/trash_proxy`, deployed `c2a5f49cc98add31`): NO blueprint + `AddToRoot` + eid→actor registry + re-skin-in-place → the stale-mirror → spawn-fresh dup path is structurally unreachable. (Discriminator/health-poll/serial-check DROPPED as moot.) **Derived-pile dup-gone + piles mirror visibly = hands-on confirmed.** *(Separate item, not a dup: the live clump CARRY-FREEZE is FIXED hands-on via the `!carrying` release-edge gate in `local_streams.cpp`, and carry JANK is FIXED [V hands-on] via fixed-delay snapshot interp (`df158728`, interp PHASE-STALL root; the keyless theory DISPROVEN). The THROW ARC + ROTATION + SOUND are FIXED [V hands-on take-29/30] (the throw-velocity flip is DEAD — REPLACED by carry/flight stream-continuity `136ed779`: both `simulateDrop`+`dropGrabObject` thunks fire ZERO; stream the LIVE clump's flight under E until re-pile; rot re-read at the land-settle COMMIT; pickup-cue killed). PROXY SCALE AS-BUILT; Z/HEIGHT FIXED [V harness]. Option 2 (the `holdPlayer` convert/ctx gate) DISPROVEN by bytecode (`holdPlayer` never cleared); see the trash CLIENT MIRROR row above + `votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.)* | [V — derived dup-fix + carry-freeze + carry-JANK + throw-arc + rotation + sound hands-on] |
| client NATIVE level-loaded chipPile ↔ host proxy expressed on top (ORIGINAL/level-placed piles) | **DUP-DESTROY BUILT + VERIFIED [V harness].** Level-piles get an eid+proxy (eidOnly lane, shared eid both sides) but the client's NATIVE level-loaded chipPile was NEVER reconciled away → it COEXISTED with the host's proxy (the divergence sweep is BLIND to natives). FIX (shipped): `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at a pile proxy-spawn (exact ~1cm + chipType + IsLiveByIndex; NOT adopt; graceful on 0, exact-or-skip on >1; GATED on `g_claimTrackingActive` = the join bracket only; harness dup-destroy-clean 12 twins / 0 SKIP). The read-only PILE-PROBE is REPLACED by this destroy (RULE 2, retired). | [V harness] |
| client grabs host shared-trash ↔ host author path | 08: client SUPPRESSES the native grab + sends `GrabIntent` → host executes authoritatively (the door `OnRequest` shape) — client never authors shared trash, and no local clump dupe | [DESIGN] |
| NPC interceptor ↔ WorldActor interceptor (same BeginDeferred) | DISJOINT allowlists; multi-interceptor support | [V] |
| nested BeginDeferred steals a pending eid in POST | params-pointer correlation | [V] |
| client kerfur conversion ghost grabbed → client-eid dupe | `ClaimConversionGhosts` parks/freezes immediately (adopt or reap) | [V] |
| host kerfur eye cam (ChildActorComponent child, keyed Aprop lineage) ↔ the whole prop identity universe (enroll/broadcast/sweep/fuzzy) | **CHILD-ACTOR EXCLUSION** — `ue_wrap::engine::IsChildActor` at 6 surfaces mirrors the game's own `ignoreSave \|\| IsChildActor()`; the eye cam never gets an independent wire identity (`c93617be`) | [V — take-8 «камер нету»] |
| kerfur turn_off's fresh prop ↔ the generic spawn-seam drain + any express lane (the drain out-raced the 5 Hz poll converge) | **SOLE-EXPRESS (`de3dccb5`):** the Init POST suppressor gates on `kerfur_form_assembler::IsCapturedForm` (this prop IS the captured conversion successor — DETERMINISTIC, route-independent) → TRACK but SKIP the generic PropSpawn (KerfurConvert = sole signal). REPLACES the `TryAdoptFreshKerfurProp` "dead-NPC-watch within 5 m" proxy, which RACED the verb's spawn/destroy order and DUPED on the NPC-alive-at-Init-POST ordering (19:09 take: client 6→8). TryAdopt keeps CONVERGE only | [dupe fix VERIFIED GREEN — 20:20 take: 14/14 host conversions suppressed, client census 6=6 across 13 samples, no visual dupes. Proximity stamp-walk FALLBACK retired `3d9da0cd` (0/85 fire); `[[project-kerfur-sole-express-dupe-2026-07-14]]`] |

## NEEDS-PROBE (do not encode as truth without it)
- **[ANSWERED]** (trash redesign 08) PROBE-A: the carry slot is **`grabbing_actor`** (the PHC light-grab
  path, NOT `holding_actor`) — confirmed by the carry-churn finding + the autotest. (The old sub-question
  "which `OnPileGrabPre` early-return fires on a live CLIENT" is MOOT — the client-grab recognition is a
  camera-ray cone, NOT suppress-native; no `lookAtActor` read is involved.
  `[[lesson-proxy-never-lookatactor-use-camera-cone]]`.)
- **[ANSWERED — YES (engage), 2026-06-22]** does `playerGrabbed(Player=puppet)` drive the grab on a PUPPET
  (host executes a client's grab intent)? **The grab ENGAGES + HOLDS on an unpossessed puppet**
  (`grabbing_actor := clump`, no `GetController`/PlayerController/possession dependency — RE + the runtime
  probe `VOTVCOOP_RUN_PUPPET_GRAB_PROBE`, commit `32ccd1bc`). **BUT the puppet's tick does NOT drive the
  per-tick PHC** (the clump floats at the spawn spot, `TRACKED=0`), so the host kinematically drives the
  held-clump pose (`coop/puppet_carry_drive`, `81e8e687`). Finding:
  `research/findings/player-puppet/votv-puppet-grab-feasibility-RE-2026-06-22.md`. This is the Increment-2 gate, now
  RESOLVED + the host-side BUILT [V harness].
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
