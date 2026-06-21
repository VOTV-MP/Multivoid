# 08 — HOST-AUTHORITATIVE TRASH CHANNEL (the pile-sync redesign)

> **Status (2026-06-21, committed `0e56ca39`, deployed `872ca8ee`, proto v82):**
> - **GRAB (pile→clump) — [V] VERIFIED hands-on** (`[SYNC-MIRROR OK]` in the client log). Driven by the
>   `InpActEvt_use` PRE seam (a real input event → ProcessEvent-VISIBLE) + the held-object edge adopt.
> - **RE-PILE (clump→pile) — [AS-BUILT], hands-on NO-DUPE** (user confirmed no dupes). Driven by a host
>   death-watch convert onto the fresh UNTRACKED pile at the clump's resting point. One **known minor**
>   glitch: a ~5 s vanish-return on some re-piles (the reaper racing the convert rebind).
> - **The deterministic re-pile via a `UFunction::Func` thunk hook — [DESIGN], IDA-gated, NOT built.**
> - **CLIENT-grab direction (Increment 2) — [DESIGN], NOT built** (proto v83).
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
   search keys identity. (AS-BUILT, the grab uses the InpActEvt-PRE eid; the re-pile finds the fresh pile by
   position but ONLY among UNTRACKED piles, then converts E onto it — see AS-BUILT below. The deterministic
   zero-search source is the thunk hook, DESIGN.)
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

**Host-grab** (host is authority AND grabber) — **[V] AS-BUILT, VERIFIED**: `InpActEvt_use` PRE resolves the
aimed pile's eid → records it as a pending grab → the held-object edge adopts the spawned clump onto E,
bumps ctx, broadcasts `PropConvert{kToClump}`. Carry streams `PropPose` (ctx-stamped). Throw → `PropRelease`.
Land caught by the death-watch convert (AS-BUILT) / the thunk hook (DESIGN) → `PropConvert{kToPile, xform}`.

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

## AS-BUILT — Increment 1 (committed `0e56ca39`, deployed `872ca8ee`, proto v82)

**Re-pile via VISIBLE seams (the BeginDeferred-POST link DISPROVEN + removed).** Per
[[feedback-docs-piles-living-knowledge-base]] "AS-BUILT" ≠ "VERIFIED": the GRAB is VERIFIED (a real hands-on
`[SYNC-MIRROR OK]`); the RE-PILE is AS-BUILT with a hands-on no-dupe observation (user confirmed no dupes) +
one known-minor glitch.

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
- **RE-PILE (clump→pile) — [AS-BUILT], hands-on NO-DUPE:** at adopt, `WatchClumpForRepile(E, clump)` enrolls
  the clump in a host-tick death-watch (`g_watchedClumps`, `lastPos` follows carry + flight). `Tick` (host
  net-pump): the tick the clump dies (the BP re-piled it — `EX_CallMath` spawn + BP-internal destroy, both
  invisible), `FindNearestUntrackedChipPile_(lastPos, 250cm)` finds the fresh pile and `OnHostConvert(E,
  kToPile)` converts E onto it in place → the client re-skins its ONE mirror (no destroy+spawn dupe). **Gated
  to UNTRACKED piles only** (a tracked neighbour is excluded → NOT the s35 cluster mis-bind). The rebind
  re-points E onto the live new pile, so the reaper sees E alive. No fresh pile near → `PropDestroy(eid)`.
- **`host_spawn_watcher` — the chipPile/clump link REMOVED** (reverted to the ambient/pinecone BeginDeferred
  POST only; the comment at `:118-122` records why: EX_CallMath, invisible to the ProcessEvent hook). **[V]**
- **MORPH DELETED (RULE 2):** `coop/pile_morph.{cpp,h}` git-removed; `trash_collect_sync::OnPileGrabPre` is
  now PROBE-A + the host pending-grab note (logs role / aimed eid / the carry slot `grabbing_actor` vs
  `holding_actor` for Increment 2); `local_streams` carries E's pose (ctx-stamped) for an adopted clump;
  `remote_prop::OnConvert` adopts ctx + drops stale; `subsystems` ticks `TickPendingGrab` + adds
  `trash_channel::OnDisconnect`. **[V]**

### KNOWN minor [?] (interim, removed by the thunk hook)

A ~5 s vanish-return on SOME re-piles: the host reaper death-watch can race the convert rebind — the clump
dies, and a PropDestroy can fire before/alongside the convert landing the new pile under E. Cosmetic,
self-corrects. Removed by the deterministic thunk hook (DESIGN, below), which converts the SAME tick the new
pile is constructed (no death-watch window).

---

## PLANNED — the deterministic re-pile via a `UFunction::Func` thunk hook (DESIGN, IDA-gated, NOT built)

Catch the `EX_CallMath BeginDeferred` itself by patching the callee's thunk (a ProcessEvent observer provably
can't — that's the whole point of the disproof). Full design + the IDA anchors + the validation plan:
**`research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`** (durable-RE). Summary:

- **Patch `UFunction::Func` (`UFunction+0xD8`) of `BeginDeferredActorSpawnFromClass`** → our native thunk:
  read `WorldContextObject` (= source clump, `EX_Self`) from `FFrame.Locals + worldCtxOff`; forward to the
  original (transparent); read `ReturnValue` (= new pile) from the thunk's `Result` arg (NOT `Locals +
  returnOff`); filter `IsGarbageClump(worldCtx) && IsChipPile(actorCls) && GetPropElementIdForActor(worldCtx)
  != invalid`; on match → `OnHostConvert(BoundEidOf(worldCtx), kToPile, ReturnValue)`. **ZERO proximity, same
  tick.** Process-lifetime, game-thread (BeginDeferred is GT-only), transparent forwarder, no unpatch.
- **GATE — an IDA pass on OUR binary** must pin `FFrame::Locals` (anchor: the `Locals = Frame` store inside
  the AOB-resolved `UObject::ProcessEvent`, cross-checked vs a native thunk's `Stack.Locals + offset` read) +
  `UFunction::Func` (anchor: the `mov rax,[rFn+FuncOff]; call rax` in ProcessEvent/Invoke; the resolved value
  must land in `.text`).
- **Validation — READ-ONLY first:** install the thunk as a pure forwarder that LOGS worldCtx/actor/result
  class names (no convert). Enable the convert ONLY once a re-pile prints `clump + chipPile`. Then DELETE the
  death-watch convert + `FindNearestUntrackedChipPile_` (RULE 2 — no parallel paths).
- **Fallback (ONLY if `FFrame::Locals` is genuinely unreachable, NOT merely an unusual offset):** the
  `prop_garbageClump_C` `BndEvt__…ComponentHit` delegate (ProcessEvent-VISIBLE) for the source clump +
  temporal correlation. Strictly worse (re-implements the convert gate + a correlation window); last resort.

When this lands, the GRAB direction can move to the same thunk too (worldCtx = a tracked chipPile, actorCls =
clump → kToClump), retiring the InpActEvt-PRE + held-edge adopt — a future tightening, not required now.

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
   RE-PILE has a hands-on no-dupe but the ~5s known-minor stands until the thunk hook.)
