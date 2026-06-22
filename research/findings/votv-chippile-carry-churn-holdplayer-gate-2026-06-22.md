# chipPile carry — the CONTACT-RE-PILE CHURN root + the CLOSE-B carry-latch fix

**Date:** 2026-06-22. **Status:** root **RE-confirmed + hands-on-confirmed**; option 1 (hit-notify) **BUILT +
FAILED**; option 2 (holdPlayer convert/ctx gate) **DISPROVEN by the bytecode** (holdPlayer is never cleared —
see that section); **THE FIX = CLOSE-B** (host-side carry latch + land-settle on the convert stream) —
**DESIGN LOCKED, building**. Supersedes the "carry MIRRORS on a settled join / the failure was the JOIN
RACE" conclusion in `votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md` (that was FALSE — see "Why
the smoke lied" below). The filename keeps `holdplayer-gate` for link stability; the gate it names is the
DISPROVEN option 2, not the fix.

This is the canonical doc for *why the host-carry of a chipPile clump does not mirror cleanly on the client*
and the fix that is queued. Read it before touching trash carry / the re-pile thunk / `OnHostConvert`.

---

## The symptom (user hands-on, build `8bc797ef`, take-24+)

- **Host:** carries the clump perfectly — native, clean, piles vanish, all good. (User: "all interactions
  on host are local so they're all good and native.")
- **Other physics props:** grab + carry + throw mirror **perfectly** on the client (pure pose-stream + lerp).
- **ONLY the chipPile/clump mirror is broken on the client:** the carried clump's movement + morphs run at
  ~**0.5–2 fps** (NOT the client's frame rate — the game runs 120 fps; the *interaction mirror* is choppy),
  and an **old pile does not disappear** at the rest spot. (Two possibly-distinct client symptoms; the
  lag is the churn, the non-disappearing pile is the proxy/orphan — keep them separate until proven common.)

So the host simulation is correct; the bug is **entirely in the host→client mirror path, and only for the
clump**, because the clump is the **only** entity mirrored through the **convert machine** (PropConvert
pile↔clump) instead of the plain pose-stream every other prop rides.

## The root — a CONTACT-RE-PILE CHURN (RE-confirmed bytecode + host log)

The held clump **re-piles on contact with the pile cluster ~once a second**, and the game **auto-re-grabs**
it, producing a churn the client faithfully renders as a teleport per convert.

**The re-pile trigger (votv-clump-ball-to-pile-conversion-RE-2026-06-08.md, bytecode):** the clump converts
to a pile on its **`StaticMesh.OnComponentHit`** delegate → `ExecuteUbergraph_prop_garbageClump(@2702)`. The
convert gate (@2874–@3573):
```
@2884  IsValid(holdPlayer)?               no → CONVERT
@2927  IsValid(holdPlayer.grabbing_actor)? no → CONVERT  : yes → ABORT (still held)
@3242  Dot(HitNormal, Up) > 0.75 ?        no → abort   (slope: near-flat only)
@3462  IsSimulatingPhysics(HitComp)?      yes → abort   (static surface only)
@3536  StaticMesh.SetNotifyRigidBodyCollision(false) ; @3573 Delay(0) → SPAWN pile
```
**The "still held" abort (@2927) gates on `holdPlayer.grabbing_actor`.** But a real **E-press** grab carries
the clump in **`holding_actor`** (the chipPile morph slot), so `grabbing_actor` is **null** → the gate
**never fires** → **the clump re-piles on every flat-static contact while you carry it.** In a pile cluster
the held clump touches neighbours/ground constantly → re-pile ~1/s. This is **stock VOTV behaviour** (SP does
it too; the user's "SP is fine" was open-space, no contact). The game then auto-re-grabs the re-piled pile
(grab-toggle still on) → a NEW clump → churn. Host log (`8bc797ef`, 09:45): `GRAB → RE-PILE(thunk) → LAND →
GRAB → RE-PILE → LAND …`, a *new* clump actor + a *new* chipPile actor each cycle (real engine pointers).

**Why the host looks flawless but the log churns:** the re-pile + auto-re-grab is instant, so the user sees a
solid clump; underneath it churns. Both "host flawless" and "re-pile every second" are true simultaneously.

**Why the client lags:** each PropConvert (`OnHostConvert`) on the client does `ClearAnyDriveFor(proxy)` +
`SetActorLocation(proxy, …)` — a **hard teleport that kills the lerp** (`remote_prop.cpp:1148`). A normal prop
**never converts** (pure pose-stream → snap-per-pose at ~60 Hz = smooth). The clump converts ~2×/s and
teleports each time → the 0.5–2 fps "morphs."

## Why the smoke LIED ("carry proven on a settled join" was FALSE)

`autotest_chippile.cpp` grabs by calling **`playerGrabbed` directly** → the clump lands in **`grabbing_actor`**
(the PHC path). With `grabbing_actor` valid, the native re-pile gate **aborts** (@2927) → the autotest clump
**never re-piles while held** → no churn → the smoke's `drive #1..#540` clean carry. A **real E-press** uses
**`holding_actor`** → churns. So the two clean smokes (`b97z33gyh`/`b7oxr23uy`) proved a grab path the user
never takes. The render-blind smoke + the wrong grab slot = the false "carry MIRRORS on a settled join."
**Lesson:** an autotest that grabs via `playerGrabbed` is NOT representative of a real E-press grab — the
carry slot (grabbing_actor vs holding_actor) differs, and that slot decides whether the clump re-piles.

## Option 1 — suppress the re-pile on the host clump — BUILT + FAILED

Built (`8bc797ef`, uncommitted in `local_streams.cpp`): on the grab edge,
`engine::SetActorRootNotifyRigidBodyCollision(heldClump, false)` (the game's own "stop further hits"
mechanism, @3536); re-enable on release. **Hands-on FAILED — the churn persisted** (host log still shows
`RE-PILE(thunk)` ~1/s). **Why:** the **host clump is LIVE** — its own BP re-arms hit-notify after our disable
(our *mirror* clump stays inert only because its BP never runs). Disabling the live actor's hit-notify
**fights the live graph and loses.** Making the native gate "see held" by writing the player's
`grabbing_actor` is risky (it's the PHC slot) and would change stock host behaviour. **Option 1 is the wrong
layer.** REVERT it.

## Option 2 (`holdPlayer` convert/ctx gate) — DISPROVEN by the bytecode (2026-06-22)

The queued `holdPlayer`-keyed gate is DEAD. `prop_garbageClump_C::holdPlayer` (`@0x0240`) is written
**once**, on grab, by the PILE: `actorChipPile` `turnToPile`/`playerGrabbed` does
`SetObjectPropertyByName(newClump, "holdPlayer", grabbingPlayer)` right AFTER
`BeginDeferredActorSpawnFromClass` (`bp_reflection/actorChipPile.json` @8492). It is **NEVER cleared** in
any blueprint — grep across `prop_garbageClump` / `actorChipPile` / `mainPlayer`: the clump only READS it in
the re-pile gate (@2884 `IsValid(holdPlayer)`, @2927 `IsValid(holdPlayer.grabbing_actor)`); `mainPlayer`
never references it. So on a REAL drop `holdPlayer` stays non-null until the clump self-destructs — the
native gate distinguishes "still actively held" via `holdPlayer.grabbing_actor` (the PHC slot, **null during
an E-press carry** — the churn root), NOT via `holdPlayer` itself. A convert gate keyed on
`holdPlayer != null` would therefore SUPPRESS the real land (holdPlayer still set) → the clump stuck a clump
forever client-side. The user predicted this exact hole ("is holdPlayer cleared before the land thunk?");
the bytecode says no. **Disproven — do not build it.** (Also disproven en route: `holding_actor` is the
weapon-attached equipment PUPPET that `updateHold` destroys+respawns — `updateHold[32/33/52/53]`,
collision-off — and whether the chipPile clump even uses it is unresolvable from BP; so no `holding_actor`-
timing gate either. `holdObjectChanged_pre/post` ARE broadcast, in `updateHold` — but they fire in the churn
too, so not churn-silent.)

## THE FIX — CLOSE-B: a host-side carry latch + land-settle on the convert stream (DESIGN LOCKED, 2026-06-22)

Resolved by proof, not the murky `holding_actor` timing: gate the convert STREAM the host already observes,
with a per-eid latch closed by a settle. Foundations all bytecode-proven: the re-pile thunk
(`OnBeginDeferredSpawnObserve`, clump→pile ONLY — `trash_collect_sync.cpp:76`) + the per-eid ctx map.

**The churn as the host's plumbing sees it (the refinement that explains BOTH client symptoms):** the thunk
catches ONLY the re-pile (kToPile); the churn RE-GRAB (pile→clump) is caught by neither the thunk (it filters
`srcObj=clump`) nor the held-edge (no pending grab). So pre-fix the host broadcasts a stream of **kToPile
only** → the client re-skins to PILE every churn cycle and never back → the single eid-E proxy sticks as a
pile at the cluster, teleported + drive-cleared each cycle (`remote_prop.cpp:1148-1152`). That IS both the
0.5–2 fps AND the "old pile doesn't disappear" — ONE root, not two bugs.

**CLOSE-B (host-side, in `trash_channel`):**
- **OPEN** — `OnHostConvert(kToClump)` while not carrying (the real grab, via `AdoptPendingGrabClump`):
  broadcast the one ToClump, `carrying[E]=true`.
- **Suppress churn re-pile** — `OnHostConvert(kToPile)` while carrying: do NOT broadcast, do NOT bump ctx;
  `RebindE(E, pile)` locally (track the live actor); start/refresh a land-settle (capture loc/rot/chipType/
  class, countdown K).
- **Rebind churn re-grab** — `OnHostRegrab(E, newClump)` from the held-edge (a new held clump during carry,
  no pending grab): `RebindE(E, clump)` + CANCEL the settle (a re-grab proves the preceding re-pile was
  churn). Keeps the carry pose-stream alive across the churn (today the binding is LOST here → the freeze).
- **CLOSE** — the settle reaches 0 with no re-grab: broadcast the held ToPile (the ONE real land), bump ctx,
  `carrying[E]=false`.
- **SAFETY** — v1: `OnDisconnect` clears all (`ForgetEid` also exists for an explicit retire). A stuck latch
  on a dead, never-reused eid is benign (no future convert comes for it; the client's phantom clump is cleaned
  by the existing PropDestroy when E's actor dies). The on-DESTROY `ForgetEid` hook is **v2** — it must NOT
  hang off the generic `K2_DestroyActor` PRE (that fires on every churn re-pile clump-destroy → would
  false-close mid-churn); it needs the guard that a churn re-pile resolves `eid=0` there (E rebinds to the
  pile first), confirmed from the smoke. A quiet-carry-SAFE watchdog is also v2 (a tick "no-activity" timeout
  would false-close a long km carry far from any cluster — converts≠liveness).

K starts at **6 ticks** (> a synchronous re-grab); the first smoke logs the real ToPile→ToClump gap
(`[TRASH-CH] … CANCEL eid=… gap=N`) to tune it. **Graceful (K non-critical, not a race):** K too small → a
churn ToPile commits early, the re-grab re-opens → brief self-correcting `clump→pile→clump` flicker; K too
large → a few frames' land-morph lag. NEITHER strands E. The settle is a host-side hold-then-commit on an
idempotent, ctx-ordered convert — it races nothing.

## AS-BUILT (2026-06-22, hands-on-iterated)
- **`65AD883A`** — CLOSE-B v1: the carry latch + land-settle + `OnHostRegrab` + `TickCarry` + `ForgetEid`
  (`trash_channel.{h,cpp}`) + the held-edge re-grab rebind (`local_streams.cpp`) + `TickCarry(session)`
  (`subsystems.cpp:363`). **Hands-on:** opened the latch + suppressed the churn (no stuck pile ✓), drop→pile
  fast ✓, BUT the carry was **frozen between E-events** (the release-flicker, below). The dup (old-pile +
  new-clump on grab) showed once = a separate intermittent eid-mismatch (`fwdEid` vs the client's mirror eid),
  NOT the churn — dormant, not fixed.
- **`C9F28176`** — the release-flicker gate: the held-prop poll's release edge (`local_streams.cpp:379`) had
  NO carry gate, so the churn's 1-frame `holding_actor` flicker fired it → cleared `g_lastHeldEid` + a spurious
  PropRelease → the carry binding died until the next E-press. FIX: suppress the release edge ONLY on the churn
  flicker — `carrying && HasPendingSettle(E)` (a re-pile just started a settle, the flicker's cause) — and fire
  on a real drop/throw (no pending settle, because a churn re-grab cancels the settle before any throw → a real
  release never coincides with one) → PropRelease velocity ships + `ForgetEid` closes the latch. **No
  `R::IsLive`** (UAF on the just-destroyed clump), no input-event RE (`InpActEvt_drop` is UI-gated, `throwPath`
  is path-aiming). Hands-on-PENDING (take-26).

## NEXT (resume here)
1. Hands-on `C9F28176` (take-26): the carry should now flow **continuously** (not frozen between E). If GREEN,
   commit-verified. Tune K from the `[TRASH-CH] … CANCEL … gap=N` lines.
2. **The dup (eid-mismatch)** if it recurs: pin from `E-PRESS … localEid=X mirrorEid=Y` — the grab broadcasts
   `ToClump{fwdEid}`; if the client mirrors that pile under a different eid, `OnConvert` spawns fresh + leaves
   the old pile. Separate track from the churn.
3. v3: the quiet-carry-safe host watchdog + the on-destroy `ForgetEid` hook (guarded so a churn re-pile's
   `eid=0` destroy doesn't false-close); then the grab-via-thunk tightening (retire the InpActEvt-PRE adopt — RULE 2).

## Credit / method note
This root was found by the **user driving the diagnosis** — rejecting three premature builds (a perf scare, a
held-gate that would miss the churn, a 150 ms debounce race), forcing the bytecode read that exposed the
`holding_actor`-vs-`grabbing_actor` gate, and naming `holdPlayer` as the deterministic signal. The lesson:
when a fix fails, re-derive the trigger from the bytecode before patching the wrapper —
[[feedback-recurring-bug-is-architectural]].
