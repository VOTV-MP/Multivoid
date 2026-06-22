# Hands-on runbook — trash-proxy PHASE 1 — take-25 (the CARRY is NOT fixed; NO carry hands-on pending — build option 2 first)

**Deployed:** `votv-coop.dll` SHA `8bc797ef` to all 4 copies (host / copy / copy2 / dev). Proto **v82**
(UNCHANGED). Code HEAD `70d28df4`. Build CLEAN (Release).

**STATUS — read this; it supersedes the take-24 "carry mechanism proven" runbook (that was WRONG).**
- **DUP FIX + VISIBILITY — hands-on VERIFIED [V]** (unchanged): no doubled piles; resting + landed piles
  mirror correctly and VISIBLY. This part is real and stands.
- **LIVE CLUMP CARRY — NOT fixed. The deployed `8bc797ef` STILL CHURNS.** The take-24 claim "the carry
  MIRRORS on a settled join / the earlier failure was the JOIN RACE" is **WITHDRAWN as FALSE.** A real
  **E-press** carries the clump in **`holding_actor`**, so the native re-pile gate (`@2927`, which checks
  `holdPlayer.grabbing_actor` = null in this slot) **never aborts** → the held clump **RE-PILES on cluster
  contact ~1/s** (`StaticMesh.OnComponentHit`) → the game auto-re-grabs → **CHURN**; each PropConvert teleports
  the client proxy (`ClearAnyDriveFor` + `SetActorLocation`) → the client's clump movement + morphs run
  **0.5–2 fps** + an old pile lingers. The user's earlier "old pile not removed / 2 fps" was this churn — REAL,
  not a join race.
- **Why the take-24 smoke lied:** `autotest_chippile.cpp` grabs by calling `playerGrabbed` directly → the
  clump lands in **`grabbing_actor`** (the PHC slot), so the native re-pile gate ABORTS → the autotest clump
  never re-piles while held → no churn → the clean `#1..#540 [proxy]` carry the smoke logged. That is the WRONG
  grab slot (a real E-press uses `holding_actor`). Render-blind smoke + wrong slot = the false "carry proven."
- **Host carries FINE** (native; the churn is seamless underneath); **OTHER physics props mirror FINE**
  (pure pose-stream); **ONLY the chipPile** (the lone convert-machine mirror) is broken **client-side**.

## There is NO carry hands-on to run right now — option 2 must be BUILT first
The carry is not in a testable state: the fix is designed but not built. **Do not test the carry on
`8bc797ef`** (it will churn — that is expected, not new data). The next code step:

- **Option 1 (`8bc797ef`, `SetNotifyRigidBodyCollision(false)` on the held clump) was BUILT + FAILED** — the
  live host BP re-arms hit-notify after our disable. REVERT it (RULE 2 — proven non-working).
- **Option 2 (DESIGN LOCKED, NOT BUILT) is the fix:** gate the convert **broadcast AND the ctx bump** in
  `OnHostConvert` by the clump's **`holdPlayer`** field (`@0x0240 AmainPlayer_C*`, CXXHeaderDump-confirmed) —
  `clump→pile` suppressed while `holdPlayer` non-null (the churn re-pile); `pile→clump` suppressed while a
  per-eid "carrying" flag is set (the churn re-grab); keep the local rebind. Removes the CLIENT lag; the HOST
  churn stays (transient + self-cleaning). KEEP fix #2 (the `OnConvert` `pile→clump` no-teleport re-skin) — it
  is part of option 2.
- **Full root + the fix design (the canonical doc):**
  `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`. Read it before building.

A fresh carry hands-on runbook will be written once option 2 is built + smoked.

## What CAN still be hands-on confirmed now (the dup-fix + visibility — already VERIFIED, re-confirm if desired)
1. **JOIN — dup + visibility.** Look at a host pile cluster on BOTH screens: each host pile = exactly ONE pile
   on the client, VISIBLE, in the right place. No doubles, no "won't disappear." (This is the [V] part.)
5. **Disconnect.** Dropping a peer cleanly retires the proxy (no floating ball, no crash).

(Steps 2–4 of the old runbook — GRAB+CARRY / THROW / RE-PILE — are intentionally removed: the carry churns on
`8bc797ef`, so there is nothing to confirm until option 2 lands.)

## Honest status
- **DUP FIX + VISIBILITY — hands-on VERIFIED [V]** (`69405445`, carried into `8bc797ef`).
- **CARRY — NOT fixed; deployed `8bc797ef` STILL CHURNS** (the contact-re-pile churn). The take-24
  "mechanism smoke-proven on a settled join" is WITHDRAWN (wrong grab slot + render-blind). Option 1 FAILED;
  option 2 (the `holdPlayer` convert/ctx gate) is designed-not-built. **No carry hands-on is pending** until
  option 2 is built. Root + fix: `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.
- **Open sub-question for after option 2:** the non-disappearing-pile symptom may be a SEPARATE proxy/orphan
  bug, not the churn — re-check it separately once the carry mirrors (do not assume one fix covers both).
