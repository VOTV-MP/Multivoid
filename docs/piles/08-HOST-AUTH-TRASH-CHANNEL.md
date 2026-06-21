# 08 — HOST-AUTHORITATIVE TRASH CHANNEL (the pile-sync redesign)

> **Status (2026-06-21 session 38, HEAD `fea04c26`, deployed `BA79E705`, proto v82 — no wire change):**
> - **GRAB (pile→clump) — [V] VERIFIED hands-on** (`[SYNC-MIRROR OK]` in the client log). Driven by the
>   `InpActEvt_use` PRE seam (a real input event → ProcessEvent-VISIBLE) + the held-object edge adopt.
> - **RE-PILE (clump→pile) — now the DETERMINISTIC `UFunction::Func` thunk converter — [AS-BUILT]; the
>   thunk DETECTION is [V] VERIFIED hands-on, the CONVERT flip is deployed-pending-hands-on** (commit
>   `d19ae4d4`). A read-only observe pass (deployed `B7EEB1BF`) showed many CLEAN `[REPILE]` and the thunk's
>   `*Result` was ptr-for-ptr the SAME pile the old death-watch's FindNearest found on every isolated re-pile
>   (two independent paths agreeing). The convert now fires the SAME tick the pile is constructed → **zero
>   proximity, no reaper race, so the ~5s vanish-return is gone by construction.** The proximity death-watch
>   is **RETIRED** (RULE 2, same commit).
> - **Triple grab-cue — FIXED [AS-BUILT], deployed-pending-hands-on** (commit `fea04c26`): the ctx-gate was
>   split by packet kind (a carry pose requires `ctx == known`, a release keeps `ctx >= known`) so an
>   ahead-of-convert carry pose no longer drives the pre-convert pile + re-fires the grab cue.
> - **DETECTION verified, CONVERT pending the hands-on:** the next hands-on confirms the grab cue is SINGLE
>   (not triple) and the re-pile no longer vanish-returns. Do NOT mark the convert/sound VERIFIED until then.
> - **CLIENT-grab direction (Increment 2) — [DESIGN], NOT built** (proto v83).
> - **OPEN — the client mirror-staleness dup (the ROBUSTNESS track):** the client's join-mirror of a pile
>   goes NOT-LIVE on its own within ~10s → `OnConvert` `ResolveLiveActorByEid` returns null → "mirror
>   NOT-FOUND" → a fresh clump is spawned while the original lingers = a dup the user sees. The thunk flip
>   does NOT fix this (it is client-side staleness, NOT the host-side cluster mis-bind the flip fixes). See
>   `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.
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
  PropDestroy{eid}                        // collect/despawn (no re-pile)             // [V] AS-BUILT
CLIENT → HOST (intent/request, NOT a state push):                                    // [DESIGN] Increment 2
  GrabIntent{eid}   ThrowIntent{eid}   PileResyncRequest{}
```

**Host-grab** (host is authority AND grabber) — **[V] VERIFIED** (grab) **+ [AS-BUILT]** (re-pile):
`InpActEvt_use` PRE resolves the aimed pile's eid → records it as a pending grab → the held-object edge
adopts the spawned clump onto E, bumps ctx, broadcasts `PropConvert{kToClump}`. Carry streams `PropPose`
(ctx-stamped). Throw → `PropRelease`. Land caught by the **`UFunction::Func` thunk converter** (commit
`d19ae4d4`; detection VERIFIED, convert deployed-pending-hands-on) → `PropConvert{kToPile, xform}`.

**Client-grab** (the dead direction — the door `OnRequest` pattern verbatim) — **[DESIGN] Increment 2**:
```
client InpActEvt_use PRE: SUPPRESS the native BP grab for THIS dispatch (null lookAtActor — the
   device_screen ClearAimForDispatch analog — so icast(lookAtActor)→playerGrabbed fails, NO local
   clump spawns; this kills the local eid=0 dupe at the source), then send GrabIntent{eid}.
host OnGrabIntent (role==Host): validate state==PILED && !HELD (per-peer guard, door holdOpen_ analog);
   EXECUTE the real grab on puppet-N: reflection::CallFunction(pile, playerGrabbed, {puppetN, hit});
   PILED→HELD_BY(N); bump ctx; broadcast PropConvert{kToClump} to ALL incl. the requester.
every peer mirrors the host's authoritative convert ⇒ ONE actor per eid, no local dupe.
```
This single move fixes BOTH the dead client direction AND the local-clump dupe (the proven door fix).

**MTA precedent (cited):** `CObjectSync.cpp` single-syncer (`GetSyncer`/`SetSyncer` :47/:140, syncer-gated
`Packet_ObjectSync` :214, re-broadcast :234); `CStaticFunctionDefinitions.cpp` `AttachElements`/`Detach`
:1602/:1656 (host-broadcast carry transition by ID :1644 + ctx on transition :1689); `CElement.cpp`
`GenerateSyncTimeContext`/`CanUpdateSync` :1281/:1300. In-tree proven instance: the door channel
(`interactable_channel.h:220`, `interactable_sync.cpp:221-290`). All driven by reflected UFunctions + state
push — no BP-asset edits, no Replicated props/RPCs, no pak edits (A6 respected).

---

## AS-BUILT — Increment 1 (HEAD `fea04c26`, deployed `BA79E705`, proto v82; the thunk landed `d19ae4d4`)

**Grab via the VISIBLE InpActEvt seam; re-pile via the deterministic `UFunction::Func` thunk (the
BeginDeferred-POST ProcessEvent link DISPROVEN + removed; the proximity death-watch RETIRED).** Per
[[feedback-docs-piles-living-knowledge-base]] "AS-BUILT" ≠ "VERIFIED": the GRAB is VERIFIED (a real hands-on
`[SYNC-MIRROR OK]`); the RE-PILE thunk DETECTION is VERIFIED (a read-only observe pass agreed ptr-for-ptr
with the old death-watch); the RE-PILE CONVERT flip + the triple-sound fix are AS-BUILT, deployed,
hands-on-PENDING.

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
  the old death-watch's FindNearest pile on every isolated re-pile); the CONVERT flip is hands-on-PENDING.
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
  death-watch atomically deleted in `d19ae4d4` (RULE 2 — no parallel paths). The CONVERT itself is
  hands-on-PENDING (the user tests next).

When the GRAB direction moves to the same thunk too (srcObj = a tracked chipPile, newActor = clump →
kToClump), it retires the InpActEvt-PRE + held-edge adopt AND closes the eid=0 adopt-miss gap (an UNTRACKED
clump from a grab the PRE missed currently skips the converter) — the NEXT tightening, unbuilt.

---

## Increment 2 — the CLIENT-grab direction (DESIGN, NOT built, proto v83)

The suppress-native + `GrabIntent` → host-executes-on-puppet-N path (the door `OnRequest` shape, above) + the
PILED/HELD/FLYING state machine + the drain-resync (`PileResyncRequest`). Protocol v83. **[DESIGN.]** The
PROBE-A diagnostic (`OnPileGrabPre` logs the carry slot `grabbing_actor` vs `holding_actor`) feeds this — the
client suppress + GrabIntent send is added at that exact seam.

Open [?] (verify during Increment 2): does `reflection::CallFunction(pile, playerGrabbed, {puppetN, hit})` on
a PUPPET drive `pickupObjectDirect` so puppet-N visibly holds the clump on the host (the verb is
VISIBLE-re-entrant → exercisable in the smoke)?

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
   the RE-PILE thunk DETECTION cleared the read-only gate; the RE-PILE CONVERT + the triple-sound fix are
   deployed-PENDING — the next hands-on confirms the grab cue is SINGLE and the re-pile no longer
   vanish-returns.)

---

## OPEN — the client mirror-staleness dup (the ROBUSTNESS track)

Separate from everything above (the host-side cluster mis-bind the thunk flip fixes). On the CLIENT, a
join-mirror of a pile (a real `actorChipPile_C`) goes **NOT-LIVE on its own within ~10s** → on the next
`OnConvert`, `ResolveLiveActorByEid` returns null → "mirror NOT-FOUND" → the client spawns a FRESH clump
while the original lingers = the dup the user sees. The thunk flip does NOT touch this (it is client-side
staleness, not a host author/identity problem). Design + RCA:
**`research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`**.
