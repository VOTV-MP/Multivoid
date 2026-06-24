# COMBINED hands-on runbook (2026-06-24) -- instant-world visual + extract 3-instance re-verify + pile-move repro

**Deployed: MD5 `b0130c29` (instant-world) on HOST + CLIENT + CLIENT2 + DEV. Proto v88 (UNCHANGED -- both
peers run this DLL). HEAD `dcf56f3d` (5 ahead of origin/main `24ee5220`, push HELD).** One playthrough covers
everything below.

## What is ALREADY autonomously verified (no hands-on needed -- for context)
- **Extract L1 pile-dup regress = PASS** (L1 pile-drift smoke, build b0130c29): host drifted 5 destroyed + 3
  moved; client `[PILE-1C] sweep-reconcile -- 18 of 18 pending save-time twin(s) removed` -> `[PILE-CENSUS] 0
  live orphan native(s)`. The extract-migrated pile sweep WORKS. (research/smoke_L1drift_instantworld_2026-06-24.txt)
- **Instant-world lifecycle = WORKS** (both the L1 smoke + a joinchurn): `mirror_defer: ARMED` -> `lift-reveal
  125-126 confirmed shown, 870 held to quiescence` -> `quiescence-reveal 870 shown, window CLOSED`. The SEAM-3
  hasMatchPos partition held correctly; kerfur census `0 UNTRACKED + 0 UNCLAIMED` -> NO stuck-hidden, NO dup at
  rest. World-at-rest screenshot clean (research/joinchurn_shots/D_steady_client.png).
- **No extract / instant-world regress on the fuzzy/convert path** (kerfurtoggle): Gap-I-1 fuzzy de-duped a
  camera prop fine; instant-world was DISARMED post-join (inert for the toggle). The kerfurtoggle "FAIL" is the
  PRE-EXISTING client-off twin-in-air (docs/kerfur/08), not a regress.
- **git diff reconcile = 0** -- the reconcile BACKUP is byte-untouched; instant-world is a pure visual layer.

## What NEEDS YOUR EYES / HANDS (the harness cannot do these)

### TEST 1 -- INSTANT-WORLD visual (the headline; DYNAMIC, a screenshot can't catch it)
Just **connect the client to the host** and WATCH THE FIRST 1-2 SECONDS of the world appearing.
- **Expected (instant-world working):** a brief dark cover at connect that FADES OUT (~0.4s) to reveal an
  ALREADY-ASSEMBLED world -- NO visible "dance" (no dup props/kerfurs flickering in, no ghosts, no objects
  snapping from wrong positions). Piles/props that need reconcile appear settled, not popping/jumping.
- **Acceptable residual:** a host-only prop or two, or a moved-pile, may still settle a touch after the cover
  lifts (the short curtain lifts at SnapshotComplete, ~2s before full quiescence -- the honest trade for "no +2s
  blank screen"). The point is the CHAOS is gone, not necessarily zero late settle.
- **FAIL signatures:** the cover stays black/stuck (never fades) -> tell me; OR props are MISSING after the
  world shows + never appear (stuck-hidden) -> tell me (the autonomous census says this shouldn't happen, but
  your eyes are the real check); OR the dance is exactly as before (no improvement).
- Logs to confirm (I'll grep): client `mirror_defer: ARMED` -> `lift-reveal N confirmed / M held` ->
  `quiescence-reveal M shown`.

### TEST 2 -- KERFUR forward off->active (extract kerfur_reconcile re-verify; needs your in-WINDOW action)
The harness cannot turn a kerfur ON during the join window. Setup: a fresh save with >=1 kerfur OFF.
- Client connects; **during the client's join-load window, the HOST turns one OFF kerfur ON** (radial -> turn-on).
- **Expected:** client ends with the turned-on kerfur as an active NPC ONLY (no stale off-prop dup beside it);
  client log `kerfur_reconcile: ARMED off->active ... (from npc EntitySpawn)` -> `sweep-retire 1 of 1`. Under
  instant-world the active NPC is a fresh-spawn -> hidden until reveal, so you should NOT even see a flicker.
- This is the one extract-migrated path the autonomous smoke can't reach (validated at 18:30 pre-instant-world;
  re-confirm it still holds on b0130c29).

### TEST 3 -- PILE grabbed/moved IN the window (the 4th mirror-identity bug repro; needs your manipulation)
Setup: a pile near where you'll be. Client connects; **during the join window, GRAB a chip-pile, move it, drop
it** (it re-piles). Watch the client.
- **This is the OPEN bug (docs/piles/09, eid-0-at-grab gap), NOT yet fixed** -- expected to still DUP (a pile +
  an unreconciled proxy) on the client. I need to see whether instant-world's deferred-hide changes how it
  presents (the proxy may now be hidden until quiescence -> the dup may be less visible, or resolve under the
  cover). Tell me what you see; the fix (self-seed the eid at the grab edge) is designed but not built.

## After the run -- paste / tell me
TEST 1: did the cover fade to an assembled world with no dance? any stuck-black or missing props?
TEST 2: did the off-prop dup stay gone (active NPC only)? (I'll grep `kerfur_reconcile: sweep-retire`.)
TEST 3: did the moved-in-window pile still dup, and was it more/less visible under instant-world?
On TEST 1 + 2 clean (no regress, instant-world visibly better) -> the extract + instant-world are
hands-on-confirmed and I can push `dcf56f3d` on your OK. TEST 3 feeds the docs/piles/09 fix build.
