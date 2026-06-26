# Hands-on runbook — join-window #1 (kerfur turn-on dup) + #2 (pile-move dup) + b2 (pile position), ONE run

**Date:** 2026-06-26 (b2 re-run)
**Deployed DLL:** SHA256 head `1155147789AA` — hash-verified MATCH on HOST + CLIENT + DEV (was `FE87964A893A`; b2 rebuilt).
**Build:** clean (Release). **Audit:** SHIP (b2; mirrors the already-audited re-skin snap). **push:** HELD.
**Commits:** #1 `39a381b0` (kerfur_reconcile), #2 `acc416eb` (save_identity_bind), **b2 `2829ce6d` (remote_prop OnConvert snap+drift)**. Grab/throw chain already pushed (`eb85ddfb`).

> **#1 + #2 are already hands-on VERIFIED (15:42 run): no dup, identity correct.** This re-run is to confirm the
> ADDED **b2** — the window-MOVED pile should now render **at its moved position** (not the old spot) — and to
> read the new **drift** log line that tells us WHERE the proxy actually landed. #1/#2 must stay PASS (no regression).

## ⚠️ CAPTURE LOGS IMMEDIATELY after this run
The 12:02 raw logs were overwritten by later runs. **Right after you finish this test, BEFORE launching
anything else,** copy both logs so I can confirm the refinements:
- `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (HOST)
- `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (CLIENT)
(or just tell me "done, logs saved" and I'll copy them.)

## What these fix (both = save-identity bind colliding with live host mutation DURING the join window)
- **#1:** host turns a kerfur ON in the connect window → the convert reliable-send fails mid-join (by design) →
  the retire fallback used to skip the stale off-prop because the bind marked it a host mirror. Now the sweep
  reconciles bound mirrors → the stale lying-down kerfur is destroyed.
- **#2:** host grabs+moves a pile in the connect window → the client's proxy correctly tracks the move, but the
  bind used to destroy it and bind the native at the OLD position. Now: if a convert touched the eid in-window,
  the proxy wins and the redundant stale native is retired → no dup.

## What to do (ONE scenario — exactly the 12:02 case)
1. Launch `mp_host_game.bat` (host) — your usual save (with off-kerfurs + chipPiles).
2. Launch `mp_client_connect.bat` (client). **While the client is still joining (the connect window, before it
   fully spawns in):**
   - On the **host**: turn a kerfur **ON** (interact with a lying-down/off kerfur to wake it). [tests #1]
   - On the **host**: also optionally turn one **OFF** (so there's a legit off-kerfur too — control).
   - On the **host**: **grab and move two chipPiles** to new spots (grab, carry, drop). [tests #2]
3. Let the client finish joining + settle (~10s).
4. On the **client**, look at the kerfurs and the moved piles.
5. **Then capture the logs (see top).**

## Acceptance
| # | Check | PASS = |
|---|-------|--------|
| 1 | Kerfur count | client shows the **same kerfurs as the host** — NO extra lying-down (stale) kerfur. The turned-on one is an active NPC on both; the turned-off one is a lying-down off-prop on both. |
| 2 | Moved piles (dup) | the two moved piles appear **once each** on the client — NO duplicate / NO ghost (this is #2, already verified; must hold). |
| 2b | **Moved piles (POSITION — b2)** | each moved pile renders **at its MOVED position** on the client (where the host dropped it), NOT back at its old/original spot. *This is the new b2 check — the whole point of the re-run.* |
| 3 | Regression (clean) | everything else looks normal: other piles/kerfurs in place, no missing/extra entities, world intact. |
| 4 | Regression (grab/throw) | after join, client can still grab a native pile (E), E-drop it, and LMB-throw it (the verified chain) — unaffected. |

PASS = #1 no extra kerfur + #2 no pile dup + **2b moved piles at the moved position** + #3/#4 nothing else broke.

### The b2 drift reading (decides whether b2 is the fix, or we need b2.1)
Whatever your eyeball says, the captured CLIENT log now has a new line per moved-pile LAND-on-convert:
`[PILE] CLIENT ToPile SNAP(spawn-on-convert) eid=... applied=(...) host=(...) drift=X.XXcm`.
- **Pile looks right (at moved) AND drift > 0** → the spawn transform had NOT taken; **b2's snap fixed it** → b2 VERIFIED.
- **Pile looks right AND drift ≈ 0** → the proxy was already at the moved position; b2 confirms placement is correct
  (any earlier "old spot" sighting was transient settle/perception). b2 still a net win (closes the observability hole).
- **Pile STILL at the old spot AND drift ≈ 0** → the proxy genuinely sits at `host=(moved)` yet renders old → the
  divergence is downstream (physics-settle / host hasn't streamed the settled rest pose) → **b2.1** (host streams the
  settled position post-quiescence), a separate follow-up. Tell me the eid + the drift line and I'll pin it.

## Log markers (I'll confirm from the captured logs)
- #1 success: CLIENT `kerfur_reconcile: retired bound-mirror stale off-prop ... (mirror-aware teardown)` and a
  `sweep-retire -- 1 of 1` (not `0 of 1`).
- #2 success: CLIENT `save_identity_bind: PROXY-WINS ... case(ii)-converted: pile grabbed/moved in-window` for the moved pile(s); no overflow-driven extra pile.
- **b2 reading: CLIENT `[PILE] CLIENT ToPile SNAP(spawn-on-convert) eid=... drift=X.XXcm`** for each moved pile (see the drift table above for what each value means).
- Regression: bind summary still `bound 874/874` (or the run's total); no new `[Warn]/[Error]` from our DLL.

## FAIL signatures
- #1 still an extra lying-down kerfur + log `sweep-retire 0 of 1` → the bound-mirror still excluded / teardown didn't fire (tell me the eid).
- #2 still two piles for a moved one → CtxForEid didn't gate (tell me the host eid + whether you saw `PROXY-WINS`).
- Anything in clean-join/grab/throw broken → a regression (the isolation should prevent this; capture logs).

## Honest note
Autonomous smoke can't do "host mutates while client joins" (it's a timing-sensitive hands-on), and it's
rendering-blind. The fixes are RE-pinned + audited SHIP, but the dup-gone / position-correct / no-regression
verdict is your eyeball + the captured logs. **b2 is an attempt + observability, NOT a guaranteed fix** — the
drift line decides (snap fixed it, vs the divergence is downstream → b2.1). On PASS (dup gone + piles at moved
position + no regression) this closes the #2 rubicon → push #1/#2/b2 together; then optionally (c) host-gate
generalization. If 2b fails with drift≈0, b2.1 is the next step (not a push-blocker for #1/#2 themselves, which
are already verified — but per your call we hold the #2 rubicon push whole until the position is right too).
