# Hands-on runbook — purge-race VARIANT 1 (host-wire) + (a): close the join-window rubicon

**Date:** 2026-06-27 (variant-1 verify run)
**Deployed DLL:** SHA256 head `D54AB5B8` — hash-verified **MATCH** on HOST + CLIENT + CLIENT2 + DEV. Sidecar **v2** (eid->save-pos); wire/proto v90 unchanged. **BOTH peers must be on this DLL** (they are — deployed ×4).
**Build:** clean (Release). **Audit:** SHIP (no CRITICAL/HIGH; wire symmetric, version fail-safe, re-bind correct, lock order matches the existing bind, cold path). **push:** HELD (HEAD `54ee4b06`, 23 ahead).
**Commits:** lever (a) `bfe9182a`, (b) reverted `86bca8cb`, **variant 1 `54ee4b06`**. #1/#2/b2/b3 ride along.

> **This verifies the order/count/timing-independent fix** for the join-window ghost — after cursor-reset (b) was
> refuted (it MIS-BOUND the sparse GC-churn) and reverted. The mechanism (RE-confirmed): UE's incremental GC
> sporadically destroys + re-instantiates ~2 of 870 save-placed piles mid-join; they re-create at their save
> position UNBOUND = ghosts. Variant 1: the HOST ships each eid's save position (sidecar v2); the CLIENT, at
> quiescence, re-binds each churned native by an exact 1 cm position match. No cursor-reset, no flag-clear.

## CAPTURE LOGS IMMEDIATELY after the run
Save both, BEFORE launching anything else:
- `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log` (HOST)
- `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log` (CLIENT)
(or say "done, logs saved".)

## What to do — the SAME 09:54/11:32/12:29 slow-purge repro
1. Launch `mp_host_game.bat` (host) — your usual save (off-kerfurs + chipPiles).
2. Launch `mp_client_connect.bat` (client). **As EARLY as you can in the connect window:**
   - On the **host**: grab + move 2-3 chipPiles to far-apart spots, FIRST and FAST.
   - On the **host**: turn a kerfur **ON**.
3. Let the client finish joining + settle (~15-20 s).
4. On the **client**, **without grabbing**: look at the piles + kerfurs.
5. **Capture logs.** Then grab/throw to confirm interaction leaks to the host.

## Acceptance (PASS = all)
| # | Check | PASS = |
|---|-------|--------|
| 1 | **No mis-bind** | NO black / wrong-type / wrong-texture pile (the 12:29 symptom). Every pile looks like itself. *This is the regression the revert+variant-1 must clear.* |
| 2 | **No ghost piles** | every pile is interactable AND your grab leaks to the host. No visible-but-dead pile. |
| 3 | **Kerfur (#1)** | turning ONE kerfur on activates exactly ONE (no double-activate — the 12:29 kerfur symptom). The turned-on one is an active NPC on both; no stale lying-down dup (retire 1/1). |
| 4 | **Moved piles (b3)** | each EARLY-moved pile renders at its MOVED position on join, no grab needed. |
| 5 | Regression | clean-join intact; grab (E) + E-drop + LMB-throw work; #2 (no pile-move dup) + b2 hold. |

## The log reading (CLIENT — I'll confirm from the captured logs)
1. `save_identity_bind: BIND SUMMARY -- bound 874/874 ...` — the bulk cursor bind (first load), as always.
2. The mass-purge happens (`net_pump: mass-purge detected`), drains fast (lever (a)).
3. **The NEW marker** — for each GC-churned re-create:
   `save_identity_bind: RE-BIND by position -- chipPile native=... -> host eid=... @save-pos=(X,Y,Z) (GC-churned save native re-created unbound; authoritative host-wire position match)`
   then `position re-bind pass -- N sparse GC-churned native(s) re-bound (walked M unbound chip + K unbound kerfur)`.
   - **N matches the number of churned piles** (12:29 had ~2). If N>0 and each native re-binds to the eid whose
     `@save-pos` equals where it actually sits → the fix works (the right native got the right eid, by position,
     NOT by GC-order rank). **This is the contrast with (b)'s mis-bind.**
   - **N=0** (no churn this run) → fine; the GC didn't churn anything, no ghost to fix (verify no ghost visually).
4. `[PILE-B3] CLIENT pos-correction APPLIED eid=... drift=0.00cm` — b3 applies (the re-bind made the eid resolvable).
5. `kerfur_reconcile: sweep-retire -- 1 of 1` — #1 retire; and the turned-on kerfur is a single active NPC.
6. **NO** `chipPile keyless spawn beyond the mapped 870` overflow that stays a ghost — the re-bind pass catches them.

## FAIL signatures (tell me which + the eid)
- A black/wrong-type pile + a `RE-BIND by position` line binding a native to an eid whose `@save-pos` does NOT
  match where the native sits → a position mis-match (co-located? wrong savePos?). Capture the eid + the line.
- A pile ghost + NO `RE-BIND by position` line for it → the churned native wasn't walked (still bound-mirror? wrong
  class filter?) or its eid wasn't unbound. Capture the eid.
- `position re-bind pass` shows `walked M unbound` with M large (not ~2) → more churn than expected; tell me M.
- b3 still `armed` with no `APPLIED` → the re-bind didn't make the eid resolvable; capture the eid.
- Kerfur double-activate persists → NOT the (b) flag-clear (reverted); a separate kerfur issue — capture it.
- Any clean-join / grab / throw / #1 / #2 / b2 regression → capture logs.

## Honest notes
- This is the eyeball + logs verdict (autonomous smoke can't do host-mutates-while-joining + is rendering-blind).
- The fix rests on PROVEN invariants this time (RE'd before code, audit SHIP): the host's savePos is authoritative
  (no client-side derivation that bit cursor-reset/no-wire), the re-bind only touches unbound natives, the bulk
  survivors are untouched. The mis-bind class is structurally removed (position is authoritative + claim-tracked).
- The bug is small (~2 piles/join, GC-timed) — if the GC happens not to churn this run, N=0 and you'll see no
  ghost AND no re-bind line; that's a pass (nothing to fix), not a miss. A run WITH churn (N>0, re-bind lines, no
  mis-bind, no ghost) is the strong confirmation.
- On PASS this closes the join-window rubicon → **push #1/#2/b2/b3 + (a) + variant 1 together** = the verified sync
  etalon → then the sync-module consolidation refactor (step 4).
