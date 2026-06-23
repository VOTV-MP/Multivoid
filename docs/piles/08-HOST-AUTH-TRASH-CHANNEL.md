# 08 — HOST-AUTHORITATIVE TRASH CHANNEL (the pile-sync redesign)

> **UPDATE 2026-06-23 (LATEST, decisive hands-on) — L1 ROOT IS A JOIN-WINDOW DUP, not a join orphan.**
> A controlled hands-on (6 items: 2 kerfur + 4 pile, host moves them onto asphalt DURING the join-load
> window) proved: a chipPile MOVED in the window [scratch-save ... client-load-100%] DUPS on the client
> (scratch-save native @old + broadcast-convert proxy @new, >1cm apart → the 1cm position-dedup can't
> match → 2 objects). Kerfur (single channel = save only) does NOT dup = the clean control. The dup is
> CLIENT-LOCAL (host state correct) + PERSISTENT (two real objects, not self-resolving). **Pre-connect
> divergence does NOT exist** (the fresh scratch save at connect captures it), so the earlier "[PILE-CENSUS]
> orphan census + absence-removal AT THE JOIN" direction is CANCELLED (it hunted join-orphans the save-
> transfer makes impossible — hence every census=0, correct). FIX (designed, not built, CLIENT-side):
> (a) hold the broadcast convert until client load-100%, or (b) dedup match-by-eid not 1cm. Canonical:
> `research/findings/votv-pile-dup-join-window-two-channel-RE-2026-06-23.md`. **The L1 bits in the banner
> below (level-pile client->host / absence-removal) are HISTORICAL.**

> **UPDATE 2026-06-23 (HEAD `54a3a332`) — a real two-peer HANDS-ON of the v85 chain found 5 layers; this
> session drove them. Corrections to the status below:**
> - **L3 carry-JITTER + L4 wild-THROW: FIXED [V hands-on + V harness]** (`92a76f27`). The carry was NOT
>   smooth in real hands-on (it shook): the puppet-held clump was a SIMULATING physics body (the PHC spring
>   fought our per-tick teleport) — fix = `SetActorSimulatePhysics(clump,false)` on grab (kinematic). And E
>   FLUNG the clump (a fixed ~871 cm/s constant) — fix = INHERIT the hold-point hand velocity (still=soft
>   drop). Harness `maxDriftCm`=0, still-release `|vel|`=0.
> - **FPS — the "~4s stutter FIXED [V harness]" claim below is DOWNGRADED. NOT closed.** The `net_pump:559`
>   re-seed guard + a 4-pass walk crusade (gating -> incremental tail-scan of all 7 `*_sync` walks,
>   `5081d32e`+`54a3a332`) genuinely KILLED the periodic 237k walks (`[WALK-TIME]` 69->12), but a hands-on
>   shows the ~3-4s hitch PERSISTS -> the walks were NEVER its root (period coincidence). Barely noticeable
>   (cosmetic), likely UE GC (unconfirmed). **PARKED — backlog.** See
>   [[lesson-periodic-hitch-not-the-walk-by-period-coincidence]].
> - **L1 (level-pile client->host) + L2 (proxy interaction window/ERHHH/offset): the open functional bugs.**
>   L1: adopt-native is DEAD (3 gates); the "level-pile dup-DESTROY" below DOES fire (~801×, the "12/871" was
>   a log-throttle artifact); the real bug is ~70 host state-drift orphan natives — fix = reconcile after
>   `HasLoadTailQuiesced` (the sweep already doom-removes un-mirrored chipPiles `remote_prop_spawn.cpp:1123`),
>   `[PILE-DELTA]` probe BUILT but needs a host-drift scenario to populate. L2: mod-DRIVE the native
>   `ui_UI_C` window via `gamemode`, suppress `PlaySound2D(use_deny)`, eye-anchor (drop the +33 head bone).
>   See [[project-session-L3L4L5-L1L2-reckoning-2026-06-23]].

> **Status (2026-06-23, HEAD `29353191`, deployed `BB94A120A969A51E`, proto v85 — committed, push held).
> The CLIENT-grab FULL CHAIN is AS-BUILT + [V harness] (see the Increment-2 bullet below). Prior (still
> true): host carry/throw/re-pile, HEAD `a5282f57`, proto v83:**
> - **GRAB (pile→clump) — [V] VERIFIED hands-on** (`[SYNC-MIRROR OK]` in the client log). Driven by the
>   `InpActEvt_use` PRE seam (a real input event → ProcessEvent-VISIBLE) + the held-object edge adopt.
> - **RE-PILE (clump→pile) — the DETERMINISTIC `UFunction::Func` thunk converter — VERIFIED [V hands-on
>   take-30]** (commit `d19ae4d4`; the re-pile + ROTATION are correct at the land-settle COMMIT). The thunk
>   DETECTION was first VERIFIED read-only (deployed `B7EEB1BF`: many CLEAN `[REPILE]`, the thunk's `*Result`
>   ptr-for-ptr the SAME pile the old death-watch's FindNearest found); the CONVERT flip is now hands-on
>   confirmed. The convert fires the SAME tick the pile is constructed → **zero proximity, no reaper race, so
>   the ~5s vanish-return is gone by construction.** The proximity death-watch is **RETIRED** (RULE 2, same
>   commit).
> - **Triple grab-cue — FIXED, covered by the take-30 [V hands-on] sound verification** (commit `fea04c26`):
>   the user confirmed the pickup cue is clean ("звук-пикап УШЁЛ") — an extra grab cue would have been audible.
>   The ctx-gate was
>   split by packet kind (a carry pose requires `ctx == known`, a release keeps `ctx >= known`) so an
>   ahead-of-convert carry pose no longer drives the pre-convert pile + re-fires the grab cue.
> - **CLIENT mirror of trash = the host-authoritative `AStaticMeshActor` PROXY — phase 1 [AS-BUILT];
>   DUP-FIX (derived) + VISIBILITY + CARRY-FREEZE + carry-JANK + THROW-ARC + ROTATION + SOUND all VERIFIED
>   [V hands-on]; Z-fix + LEVEL-PILE dup-DESTROY + FPS-fix [V harness]; proxy SCALE AS-BUILT** (HEAD
>   `a5282f57`, deployed `015F0AC9590B6B23`, proto v83).
>   The client's mirror of a chipPile/clump is now an `AStaticMeshActor` WE own (NO blueprint, `AddToRoot`, our
>   eid→actor registry) instead of the real self-morphing BP — so the staleness dup below is impossible BY
>   CONSTRUCTION. **The DUP FIX works (hands-on): no doubled piles; resting + landed piles mirror correctly +
>   VISIBLY** (`0` `mirror NOT-FOUND` in the smoke, 875 proxies, user confirmed). A runtime `AStaticMeshActor`
>   defaults to STATIC mobility, on which `SetStaticMesh`+`SetActorLocation` silently no-op (the proxies were
>   INVISIBLE in the smoke — which is render-blind); FIXED with `engine::SetComponentMobility(Movable)`
>   (`245148c6`), user then confirmed "works visually." **The LIVE CARRY-FREEZE is [V] FIXED hands-on** — the
>   live fix is the **`!carrying` release-edge gate** in `local_streams.cpp` (`else if (g_lastHeldProp &&
>   !coop::trash_channel::IsCarrying(g_lastHeldEid))`): the client clump UPDATES through the carry now (no
>   freeze between E-events). The earlier "carry MIRRORS on a settled join / the failure was the JOIN RACE"
>   claim was **WITHDRAWN as FALSE**; the actual release-edge cause was **`updateHold` PUPPET RECREATION** (the
>   `heldActor` ptr changes with `pendingSettle=0`, so a `HasPendingSettle` gate couldn't catch it) —
>   suppressing the WHOLE carry (`!carrying`) is the fix, not a `holdPlayer`/re-pile gate. **NOW FIXED/BUILT
>   this session:** (1) carry **JANK — FIXED [V]** (shipped `df158728`): the `key.len=4`→keyless theory was
>   DISPROVEN by bytecode (BP `GetKey` returns the FName `"None"` for BOTH `prop_garbageClump_C` and
>   `actorChipPile_C`, so `key.len=4` is the literal string "None"; the receiver already guards
>   `keyW != "None"`→eid at `remote_prop.cpp:403`, so forcing keyless was a no-op). REAL root (code-proven): an
>   interpolation PHASE-STALL — `BeginLerpToPose` set `lerpStartMs=nowMs`, `AdvanceLerp` sampled the same
>   `nowMs` → alpha=0 every new-pose tick at vsync-60. FIX = fixed-delay snapshot interpolation
>   (`remote_prop.cpp` ActiveDrive buffers 2 timestamped poses prevLoc/lastLoc + prevPoseMs/lastPoseMs; renders
>   `nowMs-span` behind; alpha by real timestamps; MTA `CClientVehicle` shape). User hands-on: carry now SMOOTH
>   like a normal object; (2) proxy **SCALE — AS-BUILT** (shipped `df158728`, proto v82→v83; the land-settle
>   COMMIT re-reads `GetActorScale3D`): the prior "`PropConvertPayload` has `scaleX/Y/Z`" was WRONG (that was
>   `PropSpawnPayload`); added `scaleX/Y/Z` to `PropConvertPayload` (static_assert 100→112), `BroadcastConvert`
>   sends per-form `GetActorScale3D(newActor)`, new `ue_wrap::engine::SetActorScale3D`, the proxy applies via
>   `ApplyProxyScale` (guarded >0.001) in `SpawnProxy`+`ReskinProxy`; not separately eyeballed (covered by the
>   commit re-read); (2b) pile-landing **ROTATION + throw SOUND — FIXED [V hands-on take-30]:** rot re-read from
>   the SETTLED pile at the land-settle COMMIT (`trash_channel.cpp:248`; the thunk passes the clump as fallback
>   `trash_collect_sync.cpp:91`); the pickup-cue-on-throw killed (flight branch streams the carry key without a
>   re-StartDrive + a trash eid sends NO PropRelease; `OnHostRelease` RETIRED, RULE 2); (2c) **Z/HEIGHT — FIXED
>   [V harness]** (regression arc): take-31 read the pile transform from `newActor` at the BeginDeferred POST =
>   `(0,0,0)` (unpositioned pre-FinishSpawning) → derived piles snapped to world origin; take-32 re-reads the
>   real transform at the land-settle COMMIT (`trash_channel.cpp:248-256`), harness drift=0cm; the native-destroy
>   was INNOCENT (harness-confirmed), the bug was the `(0,0,0)` loc; (3) **ORPHAN dup — SPLIT by pile origin;
>   the DESTROY is BUILT + VERIFIED [V harness].** DERIVED (gameplay-born) piles: dup GONE [V hands-on] (born
>   only as a host convert → a proxy on both sides, no native twin). ORIGINAL (level-placed) piles' NATIVE
>   level-loaded chipPile coexisted with the host proxy (root CONFIRMED: eidOnly lane; the sweep is BLIND to
>   natives) → now `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at a pile proxy-spawn (exact
>   ~1cm + chipType + IsLiveByIndex; NOT adopt; graceful on 0, exact-or-skip on >1; GATED on
>   `g_claimTrackingActive` = the join bracket only; harness dup-destroy-clean 12 twins / 0 SKIP). The read-only
>   PILE-PROBE is REPLACED by this destroy (RULE 2, retired). (Supersedes the "eid-resolve race / `isProxy=0`
>   spawn-fresh" theory.); (3b) **FPS — the ~4s stutter FIXED [V harness]:** `net_pump.cpp:559` guards the
>   steady-world re-seed on the GUObjectArray high-water mark (NumObjects) + a ~20s census so the ~237k walk is
>   SKIPPED at rest; harness fps-reseed-rate 0.073/s (was ~0.25/s); (4) the **`simulateDrop` throw-velocity FLIP
>   is DEAD — REPLACED by carry/flight stream-continuity; the THROW ARC is VERIFIED [V hands-on take-29 +
>   harness]** (shipped `136ed779`): BOTH the `simulateDrop` thunk AND its successor `dropGrabObject` thunk
>   fired ZERO times across 7 grab/release cycles (the clump release uses NEITHER verb — the clump rides
>   `grabbing_actor`, the PHC handle, not `holding_actor`) though the same Func-thunk facility fired all run;
>   verb-detection ABANDONED. PIVOT: the host's thrown clump really flies (physics) until it re-piles, so
>   `local_streams.cpp`'s release-edge `!carrying`-SKIP branch now CONTINUES streaming `g_lastHeldProp`'s pose
>   under the same eid E while it is a LIVE garbageClump (`IsLive`-gated — a churn re-pile kills the clump →
>   skip; a real release leaves it flying → stream); the client's fixed-delay interp shows the arc; it ends when
>   the clump re-piles (ToPile re-skins+snaps). The arc FLIES (user: "дуга ЛЕТИТ"; the autotest does a real
>   DIRECTIONAL throw). The dead `dropGrabObject` read-only thunk (`trash_collect_sync.cpp:45,99-126,396`) is
>   STILL PRESENT — to be retired RULE 2 next. **Host carries FINE** (native); **OTHER physics props mirror
>   fine** (pure pose-stream). The proxy is NoCollision (the client-grab aim is a camera-ray cone, not
>   collision). **STILL OPEN (NEXT):** a `garbageCollider`-analog SHAPE component on the proxy (occlusion-
>   correct aim + movement-block — the cone ignores walls, the proxy is walk-through); the WHOOSH throw sound
>   (no ReliableKind; user-deprioritized, best confirmed by hearing); retire the dead `dropGrabObject` thunk
>   (RULE 2). **(Increment 2 client-grab full chain is AS-BUILT v85 — see the bullet below.)** **Dead
>   ends:** option 1
>   (`8bc797ef`, `SetNotifyRigidBodyCollision(false)` on the held clump)
>   BUILT + FAILED (the live host BP re-arms hit-notify); option 2 (the `holdPlayer` convert/ctx gate) is
>   **DISPROVEN by bytecode** — `holdPlayer` (`@0x0240`) is set ONCE on grab (`actorChipPile.json` @8492) and
>   NEVER cleared in any BP, so it cannot mark "released" (DEAD, NOT pending / NOT design-locked). See
>   **"AS-BUILT — the client trash MIRROR is the host-authoritative `AStaticMeshActor` proxy"** below + the new
>   canonical finding **`research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`**.
> - **DUP-FIX (derived) + VISIBILITY + CARRY-FREEZE + carry-JANK + THROW-ARC + ROTATION + SOUND all VERIFIED
>   [V hands-on]; Z-fix + LEVEL-PILE dup-DESTROY + FPS-fix [V harness]; SCALE AS-BUILT:** the derived-pile dup
>   is gone + the resting/landed piles mirror visibly (user-confirmed); the carry-freeze is fixed by the
>   `!carrying` release-edge gate, the carry JANK by fixed-delay snapshot interp, the throw arc by the
>   flight-stream (`136ed779`), and rotation/sound at the land-settle COMMIT (take-30) — all hands-on. The
>   ORIGINAL level-pile native-coexistence dup is now closed by the native-DESTROY (`remote_prop_spawn.cpp:387-410`,
>   harness-verified). Verification this session ran on the autonomous log-truth harness (`tools/pile-test-assert.ps1`,
>   13 invariants, VERDICT PASS) — `[V harness]` = asserted against real autonomous-run logs, NOT a human
>   hands-on. Option 1 failed; option 2 (the `holdPlayer` convert/ctx gate) is DISPROVEN by bytecode
>   (`holdPlayer` never cleared). STILL OPEN: the WHOOSH throw sound (no ReliableKind); retire the dead
>   `dropGrabObject` thunk (RULE 2). (The s38 grab cue / re-pile vanish-return checks were a separate track,
>   now folded into the take-30 verified state.)
> - **CLIENT-grab direction (Increment 2) — FULL CHAIN AS-BUILT + [V harness]** (proto **v85**, HEAD
>   `29353191`, deployed `BB94A120A969A51E`, push held). A client AIMS at a mirrored pile, GRABS, CARRIES,
>   THROWS, it re-piles — all verified on the log-truth harness (0 CRITICAL fail) via the REAL E-press path.
>   **Recognition = a CAMERA-RAY CONE** (`trash_proxy::EidForAimedPileProxy`) in `OnPileGrabPre` — the
>   trace/`lookatActorCurrent`/proxy-collision approach is **RETIRED (RULE 2)**: a bare AStaticMeshActor proxy
>   can never be `lookAtActor` (the `int_player_C` filter at LookAtFunction @3250) AND the worn pile mesh has
>   no simple collision body (harness `trace-gate hit=0`). **Carry visibility = a NEW host-authoritative
>   per-eid pose stream** (`MsgType::TrashCarryPose=34`, the `StoreRemoteWorldActorBatch` pattern): a client
>   only drives slot 0 + the relay can't echo to the grabber, so the host ORIGINATES the clump's carry/flight
>   pose; every client drives the proxy via a per-eid `ActiveDrive`. **Throw** = `OnThrowIntent` releases the
>   puppet grab + applies physics velocity → the clump self-re-piles via the existing thunk → ToPile. The
>   **faithful next increment** (greenlight): a `garbageCollider`-analog SHAPE component on the proxy for
>   occlusion-correct aim + movement-block (the cone ignores walls; the proxy is still walk-through).
>
> Supersedes **07** (the MORPH V2 re-skin + proximity land-watch — RETIRED 2026-06-21: it false-fired in
> dense pile clusters and never wired the client→host direction; the autonomous smoke that "verified" it was
> a false positive on a sanitized single pile). Status lifecycle: DESIGN → AS-BUILT → VERIFIED (real
> hands-on — NEVER a smoke). [[feedback-docs-piles-living-knowledge-base]]

---

## ⚠ THE "OBSERVABILITY REVERSAL" WAS FALSE — corrected 2026-06-21 (read this first)

The earlier-this-session draft of this doc claimed an **"observability reversal":** that the chipPile/clump
`BeginDeferredActorSpawnFromClass` POST (and `FinishSpawningActor` POST) is **observable** to our
ProcessEvent hook, so `host_spawn_watcher` could catch the grab (pile→clump) and the re-pile (clump→pile) at
that POST as a "deterministic, zero-proximity link". **That claim is FALSE.** It was the central design pillar
of Increment 1; it never worked.

- **Why it's false (the dispatch truth):** the chipPile clump-spawn (inside `actorChipPile_C::playerGrabbed`)
  and the pile-respawn (inside the `prop_garbageClump_C` ubergraph) are dispatched in the bytecode as
  **`EX_CallMath`** — a native thunk invoked via `UObject::CallFunction → UFunction::Invoke →
  (Context->*UFunction::Func)(...)`, which is **one layer BELOW `UObject::ProcessEvent`**, our sole hook seam.
  An `EX_CallMath` call never re-enters ProcessEvent, so a POST observer registered on `BeginDeferred` for
  this caller **never fires.** [V]
- **Visibility is a property of the CALLER's opcode, not the callee UFunction.** `host_spawn_watcher` DOES
  catch `BeginDeferred` for the `garbagePileSpawner` / ambient (pinecone) caller — because THAT caller's
  dispatch reaches ProcessEvent. The chipPile/clump caller issues the SAME UFunction as `EX_CallMath` →
  invisible. "Catches it for the pinecone, therefore catches it for the clump" was a category error. [V]
- **The 2026-06-08 pass-2 RE already had this right** (`findings/votv-clump-lifecycle-observability-and-
  robust-design-2026-06-08-pass2.md` §1.1 + §2 row 4: `EX_CallMath … Observable: NO`). The s35/08 redesign
  CONTRADICTED our own earlier correct RE. **[RD → confirmed]**
- **Live proof (hands-on 2026-06-21):** `host_spawn_watcher` registered the POST observer fine but logged
  **0 fires** across 870 piles + every re-pile. Committed in `0e56ca39`; full RE in
  `research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`. **[V]**

**Consequence:** `host_spawn_watcher` was reverted to owning the AMBIENT/pinecone BeginDeferred POST ONLY (the
chipPile/clump convert link was removed from it). The grab + re-pile now sync via the **VISIBLE** seams
(below). The ZERO-proximity catch is still achievable — but only by patching `UFunction::Func` (the thunk),
NOT by a ProcessEvent observer (the DESIGN in the last section).

> **WorldContextObject is still the right data — it was the SEAM that was wrong.** WorldContextObject ==
> `EX_Self` (the source actor) is bytecode-confirmed for BOTH transitions (the chipPile playerGrabbed
> clump-spawn; the prop_garbageClump ubergraph pile-spawn). Reading it gives the source entity for free. The
> blocker is purely that the spawn is `EX_CallMath` (invisible to ProcessEvent) — addressed by the thunk hook.

---

## The VERIFIED mechanic (byte-exact — bytecode + SDK + live log)

**`actorChipPile_C` = CARRY-AND-THROW** (never collected — every collect/pickup/hold verb is
hard-false/no-op). **`trashBitsPile_C` = COLLECT** (a SEPARATE entity: `playerTryToCollect` → `trashToProp` →
spawn one `Aprop_C` + decrement count + self-destroy at 0; rides the existing keyed-Aprop lane — do NOT force
it through the carry channel).

```
PILE (actorChipPile_C; keyless; chipType@0x0238; fresh-load pos is map-deterministic)
  └─[E-press; lookAtActor==pile]→ playerGrabbed @2607: BeginDeferred(clump) @2748 ·
       clump.chipType:=pile.chipType @2790 · clump.holdPlayer:=player @2831 · FinishSpawning @3013 ·
       player.pickupObjectDirect(clump) @3051 · K2_DestroyActor(self=pile) @2563
       ⇒ CLUMP-HELD   [spawn 1 clump, destroy 1 pile; ONLY chipType carried]
CLUMP-HELD (prop_garbageClump_C; keyless; holdPlayer set; held via PHC grabHandle)
  ├─ convert ABORTS while the holder is still grabbing (@2927 gate)
  ├─[E-press on a NEW pile]→ grabbing_actor reassigned; THIS clump silently RELEASED (NO merge) ⇒ FLYING
  └─[grab decays / drop]→ grabbing_actor:=null; PHC release ⇒ FLYING  (residual sim vel ~34cm/s —
       NOT a thrown impulse; the PHC just lets go as it settles)
CLUMP-FLYING (simulating; grabbing_actor null)
  └─[OnComponentHit #1]→ delayOnHit:=false @2716   (ARM ONLY — no convert on the 1st contact)
  └─[OnComponentHit #2; canConvert && !holder.grabbing_actor && Dot(N,Up)>0.75 && !hitComp.IsSimulating]
       SphereTrace(10cm)→restingXform @27 · BeginDeferred(pile,restingXform) @1722 ·
       pile.chipType:=clump.chipType @1764 · FinishSpawning @2557 · pile.SetLifeSpan @2595 ·
       K2_DestroyActor(self=clump) @2640
       ⇒ PILE (NEW, at the clump's SPHERE-TRACED RESTING POINT — NOT lastPos, NOT the source pile pos)
```

**Why proximity-from-lastPos was doomed:** the re-pile is at the clump's **resting point**, not `lastPos`; a
2nd grab while holding **replaces, never merges**; only `chipType` (a cosmetic variant @0x0238) carries — it
is NOT a unique id. Piles are keyless (`Key=None`); identity must be our eid. **[V/RD]**

## OBSERVABILITY (corrected — every grab/spawn/destroy verb is INVISIBLE; the inputs + the hit delegate are VISIBLE)

- **INVISIBLE** (inner BP-VM calls, below our ProcessEvent detour — `EX_LocalVirtualFunction` →
  ProcessInternal, or `EX_CallMath` → native thunk):
  - `playerGrabbed`, `pickupObjectDirect`, both `K2_DestroyActor(self)` (`EX_*VirtualFunction`). **[V/RD]**
  - **`BeginDeferredActorSpawnFromClass` + `FinishSpawningActor`** when called from the chipPile/clump
    ubergraphs (`EX_CallMath`). **THIS is the FALSE-claim correction** — the grab clump-spawn and the
    re-pile pile-spawn are NOT observable to our hook (§"observability reversal was FALSE" above). **[V]**
  - A reflection `CallFunction(InpActEvt_use)` fires our PRE/POST observer but NOT the input-gated BP body
    (see COOP_DISPATCH_VISIBILITY observe-vs-drive). **[V]**
- **VISIBLE** (OUTER entries — reach ProcessEvent, our detour fires):
  - `InpActEvt_use` PRE/POST — the one observable grab-INTENT seam (pile alive at PRE). **This is the seam
    the AS-BUILT grab uses.** **[V]**
  - `prop_garbageClump_C::BndEvt__…ComponentHit` — the land trigger (a multicast-delegate broadcast →
    ProcessEvent). Candidate-VISIBLE (pass-2 §1.2). Used only as the thunk-hook FALLBACK (not on the AS-BUILT
    path). **[RD]**
  - `BeginDeferredActorSpawnFromClass` POST **for the `garbagePileSpawner` / ambient (pinecone) caller** —
    host_spawn_watcher catches THIS caller (its dispatch reaches PE). The chipPile/clump caller does NOT
    reach here. **[V]**
  - `reflection::CallFunction(player, playerGrabbed, frame)` re-enters PE and runs the REAL verb — the
    mechanism for the host to execute a client's grab intent (Increment 2). **[V]**

## THE ARCHITECTURE — host-authoritative trash-entity state machine

MTA single-syncer + sync-time-context, **structurally cloned from the shipped door channel**
(`interactable_channel.h:220 OnRequest`, `Channel::Mode::HostAuth`). Principle 6 / the door rule: the
non-authority moves ONLY on host echoes.

**Data model — the eid is a host-minted, life-stable logical trash entity** (re-skinned in place
pile↔clump↔pile; keep `oldEid==newEid==E`). **Position is NEVER identity anywhere** → the cluster mis-bind is
impossible by construction.

```
TrashEntity { eid; state(PILED@xform | HELD_BY(slot N) | FLYING(vel)); chipType; xform; ctx(uint8) }
```
The host owns the authoritative state; every receiver (incl. a client that initiated a grab) drives a LOCAL
visual actor from that state — never advances state locally, never guesses identity by proximity.

**Cluster-safe identity, NO proximity:**
1. **eid end-to-end.** The clump↔pile link rebinds the SAME E in place (`oldEid==newEid==E`); no spatial
   search keys identity. (AS-BUILT: the grab uses the InpActEvt-PRE eid; the re-pile is now the
   `UFunction::Func` thunk converter — it reads the exact `(source clump, spawned pile)` pair off the
   `EX_CallMath BeginDeferred` and converts E onto the spawned pile the same tick, zero spatial search. The
   former proximity death-watch is RETIRED.)
2. **MTA sync-time-context (`ctx` byte).** Stamp every PropConvert/Pose/Release; bump on EVERY transition;
   receivers drop stale-ctx packets (port `CElement::GenerateSyncTimeContext`/`CanUpdateSync`, MTA
   `CElement.cpp:1281/1300`). A late pose/land packet for eid E after a transition is REJECTED — never
   re-applied to a neighbor. (The single guard the morph lacked.) **[V — AS-BUILT, proto v82.]**
3. **Drain survival (MTA EntityAdd-on-rescope).** On the shadow-drain edge the client sends
   `PileResyncRequest`; the host re-streams `PropSpawn` per live pile (host eid preserved). **[DESIGN —
   Increment 2.]**

**Packets (both directions symmetric through the host):**
```
HOST → ALL (authoritative state change):
  PropConvert{eid, kind(kToClump|kToPile), ctx, xform, chipType}   // [V] AS-BUILT
  PropPose{eid, ctx}                      // carry (existing held-pose stream + ctx) // [V] AS-BUILT
  PropRelease{eid, vel, ctx}              // throw                                    // [V] AS-BUILT
  PropSpawn{eid, class, xform, chipType}  // (re)scope-in incl. resync reply          // [V] existing
  PropDestroy{eid}                        // collect/despawn AND carry-abort          // [V] AS-BUILT
  TrashCarryPose{[eid,xform,ctx]...}      // v85 host-AUTH per-eid carry/flight batch // [V] AS-BUILT
CLIENT → HOST (intent/request, NOT a state push):                                    // [V] AS-BUILT (v85)
  GrabIntent{eid}   ThrowIntent{eid}                  // PileResyncRequest{} still [DESIGN] (phase 3)
```

**Host-grab** (host is authority AND grabber) — **[V] VERIFIED** (grab) **+ [AS-BUILT]** (re-pile):
`InpActEvt_use` PRE resolves the aimed pile's eid → records it as a pending grab → the held-object edge
adopts the spawned clump onto E, bumps ctx, broadcasts `PropConvert{kToClump}`. Carry streams `PropPose`
(ctx-stamped). Throw → `PropRelease`. Land caught by the **`UFunction::Func` thunk converter** (commit
`d19ae4d4`; detection VERIFIED, convert VERIFIED [V hands-on take-30]) → `PropConvert{kToPile, xform}`.

**Client-grab** (the door `OnRequest` pattern) — **[V] AS-BUILT, the FULL CHAIN, v85** (HEAD `29353191`):
```
client OnPileGrabPre (role!=Host): a CAMERA-RAY CONE (trash_proxy::EidForAimedPileProxy) picks the aimed
   pile proxy -> SendGrabIntent{eid}. NO suppress-native needed (the bare AStaticMeshActor proxy fails the
   native int_player_C cast, so the local grab no-ops on its own). E-press TOGGLES: carrying -> ThrowIntent.
   (The "null lookAtActor + read it" suppress/recognize plan is RETIRED, RULE 2 -- a proxy can't be
   lookAtActor [int_player_C filter] + the pile mesh has no collision body; harness trace-gate hit=0.)
host OnGrabIntent (role==Host): validate !HELD (per-peer guard, door holdOpen_ analog) + the eid is a live
   chipPile; EXECUTE playerGrabbed(Player=puppetN); HELD_BY(N); bump ctx; broadcast PropConvert{kToClump}.
carry: puppet_carry_drive hand-drives the clump + PUBLISHES TrashCarryPose (host-AUTH, per-eid); every
   client drives its proxy via a per-eid ActiveDrive interp (a client only drives slot 0 + the relay can't
   echo to the grabber -> the carry pose MUST be host-originated, the WorldActor-batch pattern).
host OnThrowIntent: release the puppet grab (re-pile gate reads not-held) + physics velocity along the
   puppet aim; the clump flies (flight stream) + self-re-piles via its own ground-hit thunk -> ToPile.
abort: a clump lost mid-carry -> ReleaseClientHold broadcasts PropDestroy(eid) so clients retire the proxy +
   clear the carry toggle (audit HIGH: else the client is stuck in throw-mode forever).
```
Every peer mirrors the host's authoritative converts ⇒ ONE actor per eid, no local dupe. The faithful next
increment (greenlight): a `garbageCollider`-analog SHAPE component on the proxy (occlusion-correct aim +
movement-block — the cone ignores walls; the proxy is still walk-through).

**MTA precedent (cited):** `CObjectSync.cpp` single-syncer (`GetSyncer`/`SetSyncer` :47/:140, syncer-gated
`Packet_ObjectSync` :214, re-broadcast :234); `CStaticFunctionDefinitions.cpp` `AttachElements`/`Detach`
:1602/:1656 (host-broadcast carry transition by ID :1644 + ctx on transition :1689); `CElement.cpp`
`GenerateSyncTimeContext`/`CanUpdateSync` :1281/:1300. In-tree proven instance: the door channel
(`interactable_channel.h:220`, `interactable_sync.cpp:221-290`). All driven by reflected UFunctions + state
push — no BP-asset edits, no Replicated props/RPCs, no pak edits (A6 respected).

---

## AS-BUILT — Increment 1 (the s38 thunk re-pile + grab-cue; HEAD was `fea04c26` on `BA79E705`, now folded into the deployed proxy build `69405445`, proto v82; the thunk landed `d19ae4d4`)

**Grab via the VISIBLE InpActEvt seam; re-pile via the deterministic `UFunction::Func` thunk (the
BeginDeferred-POST ProcessEvent link DISPROVEN + removed; the proximity death-watch RETIRED).** Per
[[feedback-docs-piles-living-knowledge-base]] "AS-BUILT" ≠ "VERIFIED": the GRAB is VERIFIED (a real hands-on
`[SYNC-MIRROR OK]`); the RE-PILE thunk DETECTION is VERIFIED (a read-only observe pass agreed ptr-for-ptr
with the old death-watch); the RE-PILE CONVERT flip + the triple-sound fix are now VERIFIED [V hands-on
take-30] (re-pile + rotation correct at the land-settle COMMIT).

### What shipped

- **Protocol v82** (`coop/net/protocol.h`): a per-eid MTA sync-time-context `ctx` byte on
  `PropConvertPayload`, `PropPoseSnapshot` (60→64), `PropReleasePayload` (56→60). `Session::SendPropRelease`
  takes a `ctx` param. **[V] KEPT — this part holds, unchanged by the disproof.**
- **`coop/trash_channel.{cpp,h}`** (ctx generator + stale-packet guard + the per-eid rebind primitive):
  `OnHostConvert` (bump ctx + rebind E in place + broadcast PropConvert), `OnHostRelease` (bump on throw),
  `NotePendingGrab` / `AdoptPendingGrabClump` (the grab eid hand-off), `CtxForEid` (carry stamp),
  `AdoptInboundConvertCtx` / `IsInboundStreamCtxFresh` (receiver drop-if-stale, wrap-aware int8, 0 =
  no-enforcement sentinel). **[V]**
- **GRAB (pile→clump) — [V] VERIFIED:** `trash_collect_sync::OnPileGrabPre` (the `InpActEvt_use` PRE observer
  — a REAL input event, ProcessEvent-VISIBLE, which is why it works) reads the aimed pile (alive at PRE) and,
  on the host, records its eid via `trash_channel::NotePendingGrab`. `local_streams`' new-held edge adopts the
  spawned clump onto that eid via `trash_channel::AdoptPendingGrabClump → OnHostConvert(kToClump)`. Identity
  is the host eid end-to-end; NO proximity. (`[SYNC-MIRROR OK]` in the client log.)
- **RE-PILE (clump→pile) — [AS-BUILT], the DETERMINISTIC `UFunction::Func` thunk converter (commit
  `d19ae4d4`):** a process-lifetime patch on `BeginDeferredActorSpawnFromClass`'s `Func` (`UFunction+0xD8`)
  installs a transparent forwarder (`ue_wrap/ufunction_hook`). On a host re-pile the clump's
  `EX_CallMath BeginDeferred(self=clump, pile)` fires the thunk → `OnBeginDeferredSpawnObserve` reads
  `FFrame::Object` (@0x18 = the re-piling clump = `WorldContextObject` = `EX_Self`) + `*Result` (the new
  pile); if the clump is a TRACKED trash entity (eid E) it `OnHostConvert(E, kToPile)` converts E onto the
  EXACT spawned pile the SAME tick it is constructed → the client re-skins its ONE mirror (no destroy+spawn
  dupe), **zero proximity, no reaper race**. An UNTRACKED clump (the grab-adopt miss, eid=0) is skipped. The
  thunk DETECTION is VERIFIED (read-only pass `B7EEB1BF`: many CLEAN `[REPILE]`, `*Result` ptr-for-ptr ==
  the old death-watch's FindNearest pile on every isolated re-pile); the CONVERT flip is VERIFIED [V hands-on take-30].
- **The proximity death-watch RETIRED (RULE 2, same commit):** `WatchClumpForRepile` / `Tick` /
  `FindNearestUntrackedChipPile_` / `g_watchedClumps` + the `local_streams` enroll + the `subsystems` tick +
  the `trash_collect_sync.h` decls are DELETED — no window with two live converters. A thread-local
  re-entrancy guard (`t_inCb`) in `ufunction_hook.cpp` keeps a nested spawn from double-converting.
- **`host_spawn_watcher` — the chipPile/clump link REMOVED** (reverted to the ambient/pinecone BeginDeferred
  POST only; the comment at `:118-122` records why: EX_CallMath, invisible to the ProcessEvent hook). **[V]**
- **MORPH DELETED (RULE 2):** `coop/pile_morph.{cpp,h}` git-removed; `trash_collect_sync::OnPileGrabPre` is
  now PROBE-A + the host pending-grab note (logs role / aimed eid / the carry slot `grabbing_actor` vs
  `holding_actor` for Increment 2); `local_streams` carries E's pose (ctx-stamped) for an adopted clump;
  `remote_prop::OnConvert` adopts ctx + drops stale; `subsystems` ticks `TickPendingGrab` + adds
  `trash_channel::OnDisconnect`. **[V]**

### KNOWN minor — RESOLVED by the thunk (2026-06-21)

The interim ~5 s vanish-return (the reaper death-watch racing the convert rebind) is **gone by
construction**: the thunk converter rebinds E onto the new pile the SAME tick it is constructed, so the
reaper never sees E dead between the clump's death and the pile's rebind. (Confirm absent at the next
hands-on alongside the single-grab-cue check.)

---

## AS-BUILT — the deterministic re-pile via a `UFunction::Func` thunk hook (committed `d19ae4d4`)

Catch the `EX_CallMath BeginDeferred` itself by patching the callee's thunk (a ProcessEvent observer provably
can't — that's the whole point of the disproof). Full RE + the IDA-pinned offsets + the validation result:
**`research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`** §3 (now AS-BUILT). As built:

- **`ue_wrap/ufunction_hook.{h,cpp}`** — the standalone Func-patch facility (principle 7, engine substrate).
  `InstallPostHook(ufn, cb)` saves the original `Func` (@`UFunction+0xD8`) and writes a STAMPED transparent
  thunk (`NativeThunk<N>`, one per slot, ≤4) that reads `FFrame::Object` (@0x18) BEFORE forwarding, runs the
  original (transparent — the spawn proceeds), then passes `(srcObj, *Result)` to the callback under an SEH
  guard + a thread-local re-entrancy guard. Refuses to patch if `Func` reads null (wrong offset for the
  build). Offsets `off::UFunction_Func`/`off::FFrame_Object` pinned in `sdk_profile.h`.
- **`trash_collect_sync::OnBeginDeferredSpawnObserve`** is the converter: installed in `Install` via
  `ufunction_hook::InstallPostHook(BeginDeferredActorSpawnFromClass, …)`. Filters
  `IsGarbageClump(srcObj) && IsChipPile(newActor) && GetPropElementIdForActor(srcObj) != invalid`; on a
  TRACKED clump re-pile → `OnHostConvert(E, kToPile, newActor, loc=clumpResting, rot, chipType)`. The grab
  case (srcObj=pile) is skipped here (the host grab stays on the InpActEvt-PRE + held-edge adopt). **Zero
  proximity, same tick.** Game-thread (BeginDeferred is GT-only), process-lifetime, host-only.
- **VALIDATION — DONE (read-only pass, deployed `B7EEB1BF`, 2026-06-21):** the thunk ran as a pure logger;
  the host log showed many CLEAN `[REPILE]` (worldCtx a tracked garbageClump + Result a chipPile, eid
  cross-check perfect) and the thunk's `*Result` was ptr-for-ptr the SAME pile the death-watch's FindNearest
  found on every isolated re-pile. Two independent paths agreeing → the convert was flipped on + the
  death-watch atomically deleted in `d19ae4d4` (RULE 2 — no parallel paths). The CONVERT itself is now
  VERIFIED [V hands-on take-30] (re-pile + rotation correct at the land-settle COMMIT).

When the GRAB direction moves to the same thunk too (srcObj = a tracked chipPile, newActor = clump →
kToClump), it retires the InpActEvt-PRE + held-edge adopt AND closes the eid=0 adopt-miss gap (an UNTRACKED
clump from a grab the PRE missed currently skips the converter) — the NEXT tightening, unbuilt.

---

## Increment 2 — the CLIENT-grab direction (the FULL CHAIN, AS-BUILT + [V harness], proto v85)

The `GrabIntent` → host-executes-on-puppet-N path (the door `OnRequest` shape, above) + the carry pose
stream + `ThrowIntent`. **The client-INITIATED path is now BUILT** (camera-ray cone recognition, NOT the
originally-planned suppress-native): the host arm (below, v84) carries it, plus the v85 carry-visibility +
throw + camera-cone recognition. See the consolidated AS-BUILT finding
`research/findings/votv-increment2-clientgrab-FULL-CHAIN-AS-BUILT-2026-06-23.md`.

> **HOST-SIDE — AS-BUILT + VERIFIED [V harness] (2026-06-22, commits `81e8e687` + MEDIUM-1 `2dc5d06e`,
> deployed `AAEC4D8F3B4341F8`, proto v84, push held).** The full client→host wire + router + handler + drive,
> verified end-to-end on the autonomous log-truth harness (synthetic `GrabIntent`, no human). What shipped:
> - **Proto v84:** `GrabIntent`(78) CLIENT→HOST + STAGED `ThrowIntent`(79)/`PileResyncRequest`(80) (IDs
>   reserved, no handler). 3-place router (`event_feed.cpp` fall-through + `event_dispatch_state.cpp` handler).
> - **`trash_channel::OnGrabIntent`** (the door `OnRequest` arm): gates (role==Host, slot in range, not
>   carrying, sender not already holding) → resolve puppet-N + the pile (Element Registry, IsLiveByIndex) →
>   `playerGrabbed(Player=puppet)` (the probe's `ParamFrame`) → read `grabbing_actor` synchronously →
>   `OnHostConvert(kToClump)` (broadcast) → register HELD_BY + the hand-drive. `SendGrabIntent` (client send).
> - **`coop/puppet_carry_drive.{cpp,h}`** (NEW): per-tick kinematic drive of each puppet-held clump to
>   `GetHeadPosition() + GetSyncedAimDirection()*grabLen` — because the host streams the clump's pose to all,
>   it must BE at the puppet hand on the host (the probe proved the puppet tick won't position it).
> - **Lifetime (audit MEDIUM-1, `2dc5d06e`):** the client hold (`g_heldBy`) is cleared on the land COMMIT,
>   on disconnect, AND (via `ReleaseClientHold`) whenever the drive drops a clump that died without a land —
>   so an eid can never strand as un-grabbable.
>
> **Harness VERDICT PASS:** `grab-intent-roundtrip` (1 RECEIVED→1 SUCCESS→0 DENIED), `puppet-drive-active`
> (126 DRIVING ticks), `grab-intent-client-echo` (client proxy re-skinned to CLUMP, SYNC-MIRROR OK no dup),
> no-crash. Carry-test regression 16/16 PASS (no regression to the existing pile work). The harness CAUGHT a
> real bug pre-ship: an over-strict `g_ctx` "tracked" gate denied a never-transitioned resting pile (removed).
> Test: `VOTVCOOP_RUN_GRAB_INTENT_TEST=1` (the client sends a synthetic GrabIntent). Findings:
> `research/findings/votv-increment2-clientgrab-host-side-DESIGN-2026-06-22.md` +
> `votv-puppet-grab-feasibility-RE-2026-06-22.md`.
>
> **AS-BUILT (2026-06-23, v85, HEAD `29353191`) — the CLIENT-INITIATED path + carry visibility + throw.**
> Recognition is a **camera-ray cone** (`trash_proxy::EidForAimedPileProxy` in `OnPileGrabPre`), NOT the
> originally-planned suppress-native + read-`lookAtActor`: a bare AStaticMeshActor proxy can never be
> `lookAtActor` (the `int_player_C` filter) AND the worn pile mesh has no simple collision body (harness
> `trace-gate hit=0`) — that approach is RETIRED (RULE 2). Carry visibility = the new host-authoritative
> per-eid `TrashCarryPose` stream. Throw = `OnThrowIntent`. Verified via the REAL E-press path (the harness
> injects InpActEvt_use, the cone recognizes). **STILL OPEN (greenlight): a `garbageCollider`-analog SHAPE
> component** on the proxy — for occlusion-correct aim AND movement-block (the cone ignores walls; the proxy
> is still walk-through). Plus the FEEL (PHC-jitter, head-vs-camera offset) at a real hands-on.

**The gating `[?]` is RESOLVED (2026-06-22, RE + runtime probe — `[V harness]`).** Full result:
`research/findings/votv-puppet-grab-feasibility-RE-2026-06-22.md`. The puppet-grab probe
(`harness/autotest_chippile.cpp::RunPuppetGrabProbe`, env `VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1`, deployed
`CDBDCFB2996A68DC`) drove `pile.playerGrabbed(Player=puppet)` on a confirmed unpossessed slot-1 puppet
(`GetController()==null`) and asserted the host log:

- **`reflection::CallFunction(playerGrabbed)` on a puppet ENGAGES the grab** — `grabbing_actor := clump` at
  poll 0, HELD 40/40 polls (~4 s). The RE is confirmed: the grab path touches NO `GetController` /
  PlayerController / possession state — it is all `mainPlayer_C`-member / component-relative, which a puppet
  has. The eid identity binding the trash channel needs (`grabbing_actor`/`holdPlayer` = puppet) works.
- **BUT the puppet does NOT auto-track the clump to its hand** — `TRACKED=0` / `FLOATING`: the clump stayed at
  the pile's spawn spot (`dist=678cm` constant, `dZ=-68cm` ground level, `grabLen` frozen). An unpossessed
  puppet's `ReceiveTick` does not drive the per-tick PHC `SetTargetLocationAndRotation`, so the clump never
  reaches the puppet's hand on its own. Since the trash channel streams the clump's HOST-side pose to all
  peers, "floats at the spawn spot on the host" = every peer renders it there — wrong.

**Refined Increment-2 build (the probe added step 4):**
1. wire `GrabIntent`/`ThrowIntent`/`PileResyncRequest` (proto v83→**v84**, the 3-place ReliableKind router
   per `[[feedback-reliablekind-router-checklist]]`);
2. **[AS-BUILT v85, corrected]** client recognition = a CAMERA-RAY CONE (`EidForAimedPileProxy`) at the
   `OnPileGrabPre` seam → send `GrabIntent{eid}`. (The planned suppress-native + read-`lookAtActor` is
   RETIRED — a bare proxy can't be `lookAtActor` + the pile mesh has no collision body; harness `trace-gate
   hit=0`.) No suppress needed (the proxy fails the native `int_player_C` cast on its own);
3. host `OnGrabIntent` (role==Host) → validate `!HELD` + the eid is a live chipPile → `playerGrabbed` on
   puppet-N → bump ctx → broadcast `PropConvert{kToClump}`;
4. **[AS-BUILT v84]** the host KINEMATICALLY drives the puppet-held clump's hold pose each tick
   (`GetHeadPosition() + GetSyncedAimDirection()*grabLen`) AND **[AS-BUILT v85]** PUBLISHES it into the
   host-authoritative per-eid `TrashCarryPose` batch so every client renders the carry/flight (a client only
   drives slot 0 + the relay can't echo to the grabber → the pose MUST be host-originated);
5. **[AS-BUILT v85]** `ThrowIntent` → `OnThrowIntent` releases the puppet grab + applies physics velocity →
   the clump self-re-piles via its own ground-hit thunk → `PropConvert{kToPile}`.

**STILL OPEN (greenlight):** a `garbageCollider`-analog SHAPE component on the proxy (occlusion-correct aim +
movement-block — the cone ignores walls, the proxy is walk-through); the whoosh-on-throw cue; the
`event_dispatch_trash.cpp` extraction; `PileResyncRequest` (phase 3, still [DESIGN]).

---

## How to VERIFY for real (the smoke the morph faked)

The morph's smoke false-passed by calling `playerGrabbed` directly on ONE sanitized pile — never the cluster,
never the input seam, never the client direction. The real gates:
1. **Real input seam** (drive `InpActEvt_use` / key-inject so the PRE actually runs) — an interaction smoke,
   not a join smoke ([[feedback-interaction-smoke-not-join-smoke]]). **(GRAB met: `[SYNC-MIRROR OK]`.)**
2. **A CLUSTER** — 4+ piles within ~30cm. Grab one, carry, throw to re-pile among neighbors; assert exactly
   ONE actor per eid on BOTH peers + the re-pile bound to the CORRECT eid.
3. **BOTH directions** — host-grab→client-mirror (DONE) AND client-grab→host-executes→both-mirror (Increment
   2); assert the host pile actually disappears on a client grab.
4. **Pre-handoff checklist (RULE 2026-05-27):** hot-path re-entry table, `wc -l` modularity, deploy ×4, ≥30s
   LAN smoke via the named-window launchers, host+client log diff clean, RSS stable; audit agents.
5. **User hands-on is the final gate.** "Works" only after that, NEVER from a smoke. (GRAB cleared this;
   the RE-PILE thunk DETECTION cleared the read-only gate; the RE-PILE CONVERT + rotation + sound are now
   CLEARED [V hands-on take-30]; the carry-smooth + throw-arc are CLEARED [V hands-on take-29]. This session
   ALSO adopted an autonomous log-truth harness (`tools/pile-test-assert.ps1`, 13 invariants) as a `[V harness]`
   gate for things the user can't easily eyeball — the Z-fix, the level-pile dup-destroy, the FPS re-seed rate.)

---

## AS-BUILT — the client trash MIRROR is the host-authoritative `AStaticMeshActor` proxy (phase 1) — DUP-FIX (derived) + VISIBILITY + CARRY-FREEZE + carry-JANK + THROW-ARC + ROTATION + SOUND hands-on VERIFIED; Z-fix + LEVEL-PILE dup-DESTROY + FPS-fix [V harness]; proxy SCALE AS-BUILT

**HEAD `a5282f57`, deployed `015F0AC9590B6B23` (proto v83 — all committed, push held). [AS-BUILT].** Commits `06685a9c` (core) +
`1011e512` (leak) + `3d371349` (HIGH-1/2 + MEDIUM-1) + `095dbf44` (lerp/freeze/teardown) + `8a17faeb` (HOT-1)
+ `245148c6` (the VISIBILITY/Movable fix) + `8bc797ef` (option 1 — BUILT + FAILED) + `65AD883A` (CLOSE-B latch
+ land-settle) + the `!carrying` release-edge gate (the CARRY-FREEZE FIX) in `local_streams.cpp` +
`df158728` (carry JANK fix = fixed-delay snapshot interp + proxy SCALE) + `136ed779` (throw arc =
carry/flight stream-continuity) + the land-settle COMMIT rot/scale re-read (`trash_channel.cpp:248`, take-30) +
the LEVEL-PILE native-DESTROY (`remote_prop_spawn.cpp:387-410`) + the FPS re-seed guard (`net_pump.cpp:559`);
harness `4a1f42a6` + `f1177589` + `cfdd7745` + `tools/pile-test-assert.ps1`. Per
[[feedback-docs-piles-living-knowledge-base]] "AS-BUILT" ≠ "VERIFIED" (and `[V harness]` = asserted by the
autonomous log-truth harness against real autonomous-run logs, NOT a human hands-on).
- **DUP FIX (DERIVED/gameplay-born piles) — [V] VERIFIED hands-on.** No doubled DERIVED piles; resting +
  landed piles mirror correctly. (Smoke: `0` `mirror NOT-FOUND`, 875 proxies, no crash/leak, 300 s stable; the
  user confirmed it works visually.) **ORIGINAL (level-placed) piles — DUP DESTROY BUILT + VERIFIED [V
  harness]:** the client's NATIVE level-loaded chipPile coexisted with the host's proxy (root confirmed this
  session); now `remote_prop_spawn.cpp:387-410` destroys the co-located native at proxy-spawn; see open-item
  (3) below.
- **VISIBILITY — [V] VERIFIED hands-on (`245148c6`).** A runtime-spawned `AStaticMeshActor` defaults to
  STATIC mobility, on which `SetStaticMesh`+`SetActorLocation` silently no-op → the proxies were INVISIBLE in
  the (render-blind) smoke. Fixed with `engine::SetComponentMobility(Movable)`; user then confirmed "works
  visually." [[lesson-runtime-staticmeshactor-must-be-movable]]
- **THE LIVE CLUMP CARRY-FREEZE — [V] FIXED hands-on (the `!carrying` release-edge gate).** The client clump
  now UPDATES through the carry instead of freezing between E-events. The live fix is the **`!carrying`
  release-edge gate** in `local_streams.cpp` (`else if (g_lastHeldProp &&
  !coop::trash_channel::IsCarrying(g_lastHeldEid))`) — it suppresses the WHOLE carry's release path. The
  earlier "carry MIRRORS on a settled join / the failure was the JOIN RACE" claim was **WITHDRAWN as FALSE**;
  the actual release-edge cause was **`updateHold` PUPPET RECREATION** (the `heldActor` ptr changes with
  `pendingSettle=0`), NOT a chipPile re-pile, which is why the `carrying && HasPendingSettle` release gate
  (`C9F28176`/commit `16ac153f`) BUILT + FAILED — `HasPendingSettle` couldn't catch a flicker that wasn't a
  re-pile. **Host carries FINE** (native; seamless); **OTHER physics props mirror fine**. **NOW FIXED/BUILT
  this session:** (1) carry **JANK — FIXED [V]** (shipped `df158728`): the `key.len=4`→keyless theory was
  DISPROVEN by bytecode (BP `GetKey` returns the FName `"None"` for BOTH `prop_garbageClump_C` and
  `actorChipPile_C`, so `key.len=4` is the literal string "None"; the receiver already guards
  `keyW != "None"`→eid at `remote_prop.cpp:403`, so forcing keyless was a no-op). REAL root (code-proven): an
  interpolation PHASE-STALL — `BeginLerpToPose` set `lerpStartMs=nowMs`, `AdvanceLerp` sampled the same
  `nowMs` → alpha=0 every new-pose tick at vsync-60. FIX = fixed-delay snapshot interpolation (`remote_prop.cpp`
  ActiveDrive buffers 2 timestamped poses prevLoc/lastLoc + prevPoseMs/lastPoseMs; renders `nowMs-span` behind;
  alpha by real timestamps; MTA `CClientVehicle` shape). User hands-on: carry now SMOOTH like a normal object.
  (2) proxy **SCALE — AS-BUILT** (shipped `df158728`, proto v82→v83; the land-settle COMMIT re-reads
  `GetActorScale3D`): the prior "`PropConvertPayload` has `scaleX/Y/Z`" was WRONG (that was `PropSpawnPayload`);
  added `scaleX/Y/Z` to `PropConvertPayload` (static_assert 100→112), `BroadcastConvert` sends per-form
  `GetActorScale3D(newActor)`, new `ue_wrap::engine::SetActorScale3D`, the proxy applies via `ApplyProxyScale`
  (guarded >0.001) in `SpawnProxy`+`ReskinProxy`; not separately eyeballed (covered by the commit re-read).
  (2b) pile-landing **ROTATION + throw SOUND — FIXED [V hands-on take-30]:** rot re-read from the SETTLED pile
  at the land-settle COMMIT (`trash_channel.cpp:248`; the thunk passes the clump as fallback
  `trash_collect_sync.cpp:91`); the pickup-cue-on-throw killed (flight branch streams the carry key without a
  re-StartDrive + a trash eid sends NO PropRelease; `OnHostRelease` RETIRED, RULE 2). (2c) **Z/HEIGHT — FIXED
  [V harness]** (regression arc): take-31 read the pile transform from `newActor` at the BeginDeferred POST =
  `(0,0,0)` (unpositioned pre-FinishSpawning) → derived piles snapped to world origin; take-32 re-reads the
  real transform at the land-settle COMMIT (`trash_channel.cpp:248-256`), harness drift=0cm; the native-destroy
  was INNOCENT (harness-confirmed). (3) **ORPHAN dup — SPLIT by pile origin; the DESTROY is BUILT + VERIFIED [V
  harness].** DERIVED (gameplay-born) piles: dup GONE [V hands-on] (born only as a host convert → a proxy on
  both sides, no native twin). ORIGINAL (level-placed) piles' NATIVE level-loaded chipPile coexisted with the
  host proxy (root CONFIRMED: eidOnly lane, shared eid both sides; the divergence sweep is BLIND to natives) →
  now `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at a pile proxy-spawn (exact ~1cm +
  chipType + IsLiveByIndex; NOT adopt — adopt re-introduces the BP self-morph the proxy avoids; graceful on 0,
  exact-or-skip on >1; GATED on `g_claimTrackingActive` = the join bracket only; join-time position index built
  once; harness dup-destroy-clean 12 twins / 0 SKIP). The read-only PILE-PROBE that confirmed coexistence is
  REPLACED by this destroy (RULE 2, retired). (Supersedes the "eid-resolve race / `isProxy=0` spawn-fresh"
  theory.) (3b) **FPS — the ~4s stutter FIXED [V harness]:** `net_pump.cpp:559` guards the steady-world re-seed
  on the GUObjectArray high-water mark (NumObjects) + a ~20s safety census so the ~237k walk is SKIPPED at rest;
  harness fps-reseed-rate 0.073/s (was ~0.25/s). (4) the **`simulateDrop` throw-velocity FLIP is DEAD —
  REPLACED by carry/flight stream-continuity; the THROW ARC is VERIFIED [V hands-on take-29 + harness]** (shipped
  `136ed779`): BOTH the `simulateDrop` thunk AND its successor `dropGrabObject` thunk fired ZERO times across 7
  grab/release cycles (the clump release uses NEITHER verb — the clump rides `grabbing_actor`, the PHC handle,
  not `holding_actor`/equipment; RE pinned `dropGrabObject` as the PHC release verb but it STILL never fired)
  though the same Func-thunk facility (BeginDeferred slot 0) fired all run; verb-detection ABANDONED. PIVOT: the
  host's thrown clump really flies (physics) until it re-piles, so `local_streams.cpp`'s release-edge
  `!carrying`-SKIP branch now CONTINUES streaming `g_lastHeldProp`'s pose under the same eid E while it is a LIVE
  garbageClump (`IsLive` is the churn/flight discriminator — a churn re-pile kills the clump → skip; a real
  release leaves it flying → stream); the client's fixed-delay interp shows the arc; it ends when the clump
  re-piles (ToPile re-skins+snaps). The arc FLIES (user: "дуга ЛЕТИТ"; the autotest does a real DIRECTIONAL
  throw; harness arc-flight-stream PASS). The carry main branch (heldActor-keyed) is byte-identical (additive,
  audited zero-CRITICAL). The dead `dropGrabObject` read-only thunk (`trash_collect_sync.cpp:45,99-126,396`) is
  STILL PRESENT — to be retired RULE 2 next. **Dead ends:** **Option 1** (`8bc797ef`,
  `SetNotifyRigidBodyCollision(false)` on the held clump) BUILT + FAILED — the live host BP re-arms hit-notify.
  **Option 2 (DISPROVEN by bytecode, NOT pending):** the `holdPlayer` convert/ctx gate is DEAD — `holdPlayer`
  (`@0x0240 AmainPlayer_C*`, CXXHeaderDump-confirmed) is set ONCE on grab (`actorChipPile.json` @8492) and
  **NEVER cleared in any BP**, so it cannot mark "released." (CLOSE-B latch + land-settle SHIPPED `65AD883A` —
  correct, but not the freeze cause.) The root + the open-item fixes: the new canonical finding
  **`research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`**. NEXT = USER HANDS-ON take-29
  (carry-smooth + the throw arc + the level-pile native-destroy).

### The dup this fixes (the ROBUSTNESS track — was OPEN, now addressed BY CONSTRUCTION)

The client's mirror of a trash entity USED to be a **real `actorChipPile_C` / `prop_garbageClump_C`
blueprint**. That BP runs its OWN ubergraph → it self-morphs / self-destructs / is GC-eligible (unrooted) on
its own schedule, independent of the host. Within ~10 s it went **NOT-LIVE** → on the next `OnConvert`,
`ResolveLiveActorByEid` returned null → "mirror NOT-FOUND" → the client spawned a FRESH clump while the
original lingered = the visible DUP (RCA: the eid=4424 lifecycle in the design finding §1). This is a
CLIENT-side staleness problem — **distinct** from the host-side cluster mis-bind the thunk flip fixes.

### The fix — change the mirror's RULES OF EXISTENCE (NEW `coop/trash_proxy.{cpp,h}`)

The client mirror of trash (chipPile/clump + variants) is now an **`AStaticMeshActor` WE own**, NOT the real
BP:
- **NO blueprint** → never self-morphs / self-destructs.
- **`AddToRoot`** → never GC'd → never stale-index (`trash_proxy.cpp:119`).
- **our eid→actor registry** (`g_proxies`) → on convert we **re-skin (`SetStaticMesh`) IN PLACE**, never
  spawn-fresh → the "mirror NOT-FOUND → spawn fresh" dup path is **structurally unreachable**.

So the dup is impossible by construction; the earlier 3-verdict discriminator / health-poll / serial-check
plan (design finding §4) is **DROPPED as moot**.

**Phase 1 scope: visual + position + re-skin, EXPLICIT NoCollision** (`SetActorRootCollisionEnabled(actor,
0)`, `trash_proxy.cpp:120`). The proxy is a kinematic host-driven follower; the client player TEMPORARILY
passes through mirrored trash (an accepted phase-1 regression, design §Q1). Collision (the `garbageCollider`
double-duty hull — Pawn-block + grab-trace) is **PHASE 2** with the client-grab direction (Increment 2).
Scope: trash only; `Aprop_C` + kerfur mirrors unchanged.

### As-built pieces (file:line)

- **`coop/trash_proxy.{cpp,h}`** — owns the eid→proxy registry + the rooting. `SpawnProxy` (idempotent: a
  same-eid re-spawn re-skins instead of leaking a second, `:107-112`) / `ReskinProxy` (in place, binding
  untouched, `:130-138`) / `RetireProxy` (`ClearAnyDriveFor → DestroyActor → RemoveFromRoot → unbind`, the
  GC-window order, `:140-163`) / `IsTrashProxyClass` / `IsClumpClass` (one cached `ClassKind` lookup) /
  `IsProxy` / `OnDisconnect` (all proxies) / `OnDisconnectForSlot` (per-slot, the CRITICAL-1 fix).
- **ue_wrap foundation:** `reflection::RemoveFromRoot`, `engine::SetStaticMesh` +
  `engine::GetStaticMeshComponent`, `prop::ResolvePileMesh` (the game's OWN `getChipPileType(chipType)`
  resolver, last-good-cached).
- **Wiring:**
  - `remote_prop_spawn.cpp` `OnSpawn` — trash class → `SpawnProxy` + `RegisterPropMirror`, branched BEFORE the
    BP dedup/converge/physics machinery (`:343-364`).
  - `remote_prop.cpp` `OnConvert` — `IsProxy(E)` → `ReskinProxy` in place → return — **THE dup fix**.
  - `remote_prop.cpp` `OnDestroy` — `IsProxy(E)` → `RetireProxy` → return (un-roots the rooted actor).
  - `subsystems.cpp` `DisconnectSlot` — `OnDisconnectForSlot(slot)` before the generic per-slot mirror drain;
    `DisconnectAll` — `OnDisconnect()` before `ForceRelease`.

### Audit `a249b005` (post-ship): CRITICAL fixed; then ALL the pre-smoke fixes + the lerp BUILT

- **CRITICAL-1 — FIXED (`1011e512`):** a PER-SLOT disconnect drained a proxy's Prop Element WITHOUT
  `RetireProxy` → the rooted `AStaticMeshActor` leaked. Fixed via `OnDisconnectForSlot(slot)` (retire by
  `ownerSlot`, stamped from `senderSlot` at spawn) running BEFORE the generic drain. (The design's Q2 claimed
  this "structurally impossible" — it was the gap.) MEDIUM-2 (cache the component) + MEDIUM-3 (fold the class
  cache) also FIXED.
- **HIGH-1/HIGH-2/MEDIUM-1 — ALL BUILT (`3d371349`):** HIGH-1 = `OnConvert` spawns the proxy in the
  unambiguous `wantClump` form on a convert-before-spawn AND `SpawnProxy` convergence no longer re-skins (the
  form is owned by the ctx-ordered convert channel, so a stale trailing spawn can't flip a clump back to a
  pile). HIGH-2 = bytecode-VERIFIED `setTex` (`prop_garbageClump.json` export 64 = `StaticMesh.SetMaterial(0,
  getChipPileType(chipType).GetMaterial(0))` on the fixed dirtball — a MATERIAL swap); new
  `engine::SetComponentMaterial` + `GetStaticMeshMaterial` (SDK-exact); `SkinProxy` clump = dirtball + the
  material, pile = mesh + `SetMaterial(0,null)`. MEDIUM-1 = Cube last-ditch fallback (`StaticLoadObject` deemed
  unwarranted — a loaded trash class pins its meshes resident).
- **R4 km-walk lerp + reliable carry-end release — ALL BUILT (`095dbf44` + HOT-1 `8a17faeb`):** MTA-style
  interpolation scoped to the proxy (`BeginLerpToPose`/`AdvanceLerp`, advanced every tick; non-proxy keeps
  teleport); the 500 ms timeout-release gated to `!isProxy` so a host-authoritative proxy FREEZES on a network
  gap and releases only on the explicit reliable edge (throw / ToPile convert / disconnect); proxy throw =
  freeze + the ToPile convert repositions to the landed pile; the destroy-only-via-`RetireProxy` invariant
  hardened (ForceRelease + OnDisconnectForSlot proxy-aware). HOT-1 dirty-gate skips sub-epsilon writes.
  **Carry-freeze update (2026-06-22):** the freeze (the client clump not updating between E-events) is FIXED by
  the **`!carrying` release-edge gate** in `local_streams.cpp` — the actual release-edge cause was `updateHold`
  PUPPET RECREATION, NOT a re-pile churn, so suppressing the whole carry's release path fixes it. The lerp/freeze
  scaffolding is retained; the remaining carry JANK (the keyless-pose fix #1) is what makes the carry drive
  smoothly, NOT a `holdPlayer` gate (option 2 is DISPROVEN — `holdPlayer` is never cleared).
- **Hot-path audit `aa8e7d9a` — GO (no CRITICAL/HIGH); HOT-1 folded.**
- **SMOKE — PARTIAL; the earlier "functionally green" is WITHDRAWN (the smoke is RENDER-BLIND + the autotest
  grabbed DURING the client join). SHA `f2344bab`, 2026-06-21.** What the smoke DID prove (matching real log):
  **the dup is GONE — `0` `mirror NOT-FOUND` / `spawn fresh`**, 875 proxies spawn (`AStaticMeshActor`,
  rooted), `0` proxy/drive errors, no crash/SEH/OOM, 300 s stable. The lan-test exit-1 was a harness PASS-gate
  bug (host-centric `puppet=` slot check), fixed `f1177589`+`cfdd7745`. **What the smoke did NOT prove + the
  hands-on then DISPROVED:**
  - **The proxies were INVISIBLE in the smoke.** `f2344bab` spawned them STATIC-mobility, on which
    `SetStaticMesh` no-ops at runtime; the smoke can't see render (log markers + black screenshots, the
    no-op'd `Call()` still returns true) so it passed anyway. FIXED: `SetComponentMobility(Movable)`
    (`245148c6`, build `69405445`); the user hands-on then confirmed resting + landed piles mirror VISIBLY.
  - **This raced smoke did NOT exercise the carry — the JOIN RACE, not a broken drive.** Client log:
    **`0` `GRAB-IN`** (no live proxy to drive), `reskinINPLACE=0`/`spawn-on-convert=0` (no ToClump convert
    applied), carry poses ctx-HELD. The autotest grabbed at host+40 s but the client's 875-proxy join finished
    LATER → the grab RACED the join, so this smoke never cleanly tested a post-join carry (the client joined
    AFTER the whole grab→carry→land and correctly showed the final pile). So **"the grab→carry→throw→re-pile
    cycle worked end-to-end" was WITHDRAWN at the time** — this run evidences only the dup-gone + the final
    landing. **An interim 2026-06-22 "RESOLVED — it was the JOIN RACE" conclusion (from a clean settled-join
    smoke) was WRONG and is WITHDRAWN.** That smoke drove a clean `#1..#540 [proxy]` carry only because
    `autotest_chippile.cpp` grabs via `playerGrabbed` → the clump rides `grabbing_actor` → the native re-pile
    gate aborts (`@2927`) → the autotest clump never re-piles while held → no churn. A real **E-press** carries
    the clump in `holding_actor` → the gate never fires → the held clump re-piles on cluster contact ~1/s → the
    game auto-re-grabs → CHURN, which each convert teleports on the client (0.5–2 fps). So the user's hands-on
    ("old pile not removed" / "2 fps") was REAL at that time. **UPDATE (2026-06-22): the carry-FREEZE is now
    FIXED, hands-on VERIFIED** — but the diagnosis above was itself superseded: the actual release-edge flicker
    was `updateHold` PUPPET RECREATION (the `heldActor` ptr changing with `pendingSettle=0`), NOT the
    contact-re-pile churn, which is why the `carrying && HasPendingSettle` gate (`C9F28176`/commit `16ac153f`)
    FAILED and the **`!carrying`** release-edge gate (suppress the WHOLE carry) is the real fix. Root + the
    item status (carry JANK **FIXED [V hands-on]** via the fixed-delay interp; the THROW ARC **VERIFIED [V
    hands-on take-29 + harness]**; ROTATION + SOUND **FIXED [V hands-on take-30]**; Z/HEIGHT **FIXED [V
    harness]** (take-31 regressed → take-32 land-settle re-read); proxy SCALE **AS-BUILT**; ORPHAN dup
    **DESTROY BUILT + VERIFIED [V harness]** — derived gone [V hands-on], ORIGINAL level-placed native destroyed
    at proxy-spawn; FPS ~4s stutter **FIXED [V harness]**): the canonical finding
    `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md` (option 1 FAILED; option 2, the
    `holdPlayer` convert/ctx gate, is DISPROVEN by bytecode — `holdPlayer` never cleared). The final verified
    state + the autonomous harness are in `research/handson_runbook_2026-06-22_regression_and_harness.md` (take-32)
    + `[[reference-pile-test-harness]]` (`tools/pile-test-assert.ps1`, 13 invariants, VERDICT PASS).

Full design + RCA: **`research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`** (§2 the
proxy design, §6 the four mesh/collision requirements, §7 the phase split + C1/C2/C3 + Q1/Q2 + the
AS-BUILT-phase-1 section).
