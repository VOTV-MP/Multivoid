# chipPile carry — the CONTACT-RE-PILE CHURN root + the holdPlayer convert/ctx gate (option 2)

**Date:** 2026-06-22. **Status:** root **RE-confirmed + hands-on-confirmed**; option 1 **BUILT + FAILED**;
option 2 **DESIGN LOCKED, NOT BUILT**. Supersedes the "carry MIRRORS on a settled join / the failure was
the JOIN RACE" conclusion in `votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md` (that conclusion
was FALSE — see "Why the smoke lied" below).

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

## Option 2 — gate the CONVERT (not the re-pile) by `holdPlayer` — DESIGN LOCKED, NOT BUILT

Don't fight the host's stock churn; **stop telling the client about it.** The carry state is already a
deterministic game field — the gate just reads the wrong one. **`prop_garbageClump_C` has
`AmainPlayer_C* holdPlayer; // 0x0240`** (CXXHeaderDump-confirmed; readable via `FindPropertyOffset("holdPlayer")`,
recook-safe). The clump knows its holder; no player iteration. And `holdPlayer` is **valid at the re-pile
thunk** (the gate @2884 reads it; nothing clears it before `BeginDeferred` @1722) — unlike the player-side
`holding_actor`, which is cleared *before* the thunk (the `held->released` edge fires first).

**The gate (in `OnHostConvert` — both the wire send AND the ctx bump):**
- **`clump→pile` (re-pile):** read `srcObj.holdPlayer`. **Non-null ⇒ carried ⇒ SUPPRESS** (churn re-pile;
  no ctx bump, no broadcast). **Null ⇒ released ⇒ BROADCAST** the single land convert (real throw/drop).
- **`pile→clump` (grab):** a per-eid **"carrying" flag**. **Not carrying ⇒ real grab ⇒ BROADCAST** + set the
  flag. **Carrying ⇒ churn re-grab ⇒ SUPPRESS** (no ctx bump, no broadcast). The flag **sets on the real
  grab, clears on the real land** (the `holdPlayer==null` re-pile).
- **Keep the local rebind** so the pose-stream keeps tracking the live held clump (the stream reads the
  engine held actor + the cached eid, not the Element's actor).

**CRITICAL — the ctx bump must be gated too, not just the wire send.** `OnHostConvert` bumps the per-eid
sync-time-context, and the carry poses are stamped with it (`CtxForEid`). If the host keeps bumping ctx
through the churn while the client's `known` stays frozen (no broadcasts), `IsInboundStreamCtxFresh` will
**hold** every carry pose → the carry stalls. So a suppressed (churn) convert must bump **nothing**. Net:
ONE `pile→clump` (real grab, ctx=1) → all carry poses ctx=1 → ONE `clump→pile` (real land, ctx=2). The churn
between sends nothing and bumps nothing.

**What option 2 does and does NOT do:** removes the **client** lag (the client sees one clump, pose-streamed,
then one land). Does **NOT** remove the **host** churn — the host still spawns+destroys real pile/clump actors
each cycle. That churn is **transient + self-cleaning** (the game destroys them natively; piles vanish on the
host) = wasted spawns/s, NOT permanent garbage. Acceptable for now; suppressing the re-pile itself is option 1
(fragile). If the host wasted-work ever shows as a perf cost, that's a separate lower-priority pass.

**Open sub-question (the non-disappearing pile):** with option 2 the client's proxy stays a clump (carried)
and lands once at the real spot — so the rest spot should be empty. If a pile still lingers there, it is a
SEPARATE proxy/orphan bug, not the churn. Re-check after option 2 lands; do not assume one fix covers both
client symptoms.

## NEXT (resume here)
1. **User to BLESS the option-2 design** (esp. the ctx-bump-must-also-be-gated point). Then BUILD:
   `OnHostConvert` gate by `srcObj.holdPlayer` (ToPile) + a per-eid carrying flag (ToClump), suppressing the
   ctx bump + the broadcast for churn converts; keep the local rebind. Add `prop::GetClumpHoldPlayer(clump)`
   (or an inline `FindPropertyOffset("holdPlayer")` read) in `ue_wrap`.
2. **REVERT option 1** (the `SetActorRootNotifyRigidBodyCollision` grab/release edges in `local_streams.cpp`)
   — proven non-working (RULE 2).
3. **KEEP fix #2** (`remote_prop.cpp` `OnConvert`: `pile→clump` re-skins WITHOUT teleport — `if (proxy &&
   !wantClump)` reposition) — it's part of option 2 (the grab convert lets the pose-stream drive the position).
4. Hands-on verify the carry mirrors cleanly (one re-skin in, smooth follow, one re-skin out), THEN re-check
   the non-disappearing-pile symptom separately.

## Credit / method note
This root was found by the **user driving the diagnosis** — rejecting three premature builds (a perf scare, a
held-gate that would miss the churn, a 150 ms debounce race), forcing the bytecode read that exposed the
`holding_actor`-vs-`grabbing_actor` gate, and naming `holdPlayer` as the deterministic signal. The lesson:
when a fix fails, re-derive the trigger from the bytecode before patching the wrapper —
[[feedback-recurring-bug-is-architectural]].
