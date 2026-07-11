# Hands-on runbook — 2026-07-11 (take 3) — DLL `B00459CC` (all 4 installs, hash-verified)

(BB1897D7 superseded minutes later by B00459CC: + the watchdog quiescence-by-ceiling fix, audit
MEDIUM — behavior identical on a healthy join; only the SnapshotBegin-lost flake path changed.)

HEAD = local main, NOT pushed. Build clean; deploy hash-verified. **NOT LAN-smoked** (user at PC).

## Take-2 result (on 7625828E) + the take-3 change
Take-2 user test: a rock placed from PRE-connect hotbar stock APPEARED (take-1 fix works), but a rock
the host picked up DURING the connect window and then placed stayed INVISIBLE on the client. Log-RCA:
the save was captured with that rock already in the hotbar -> its converge-bound mirror (eid=5384) was
churn-destroyed with NO save recreate -> dead row forever. Take-3 generalizes the defer into SPAWN
REVALIDATION: every in-episode wire expression's payload is captured; the quiescence drain re-expresses
ONLY the rows still dead/absent (survivors + RE-BOUND recreates skip O(1)). Plus positions in the
identity-critical logs (doom lines, RE-BIND, arm/apply) + the dead-row tripwire naming the residual.

## [USER-V] confirmed live on 8CC08A89 (take-1 items — CLOSED 2026-07-11 ~12:20 session)
- Swing motion continuous (v109 HandPose stream) — USER-V.
- Crowbar dupe gone (setKey base-resolve x2) — USER-V.
- Overlay hp-0.0 silence — USER-V.
- (From C9E50E92 live run: health/decal overlay USER-V; yellow actions / save-picker /
  chat-leak / email-gate = AS-BUILT, live-run silent.)

## NEW in BB1897D7 — the things to test
1. **The take-2 repro, exactly**: while the client is at the connection window, host picks a WORLD
   item up into the hotbar (hold-R) AND places it back somewhere else, all inside the window; also
   place an item that was in the hotbar since before the connect. EXPECT: after the client loads,
   BOTH items are visible at the host's placed positions; host E-grab moves them on the client.
   Log check (client): "dead keyed mirror row SURVIVED the re-bind pass -- eid=... key=..." (the
   tripwire naming the hotbar-moved item) followed by "[SPAWN-DEFER] re-expressing eid=... loc=..."
   and the drain summary "N in-episode expression(s) revalidated: X rows live, Y re-expressed".
2. **Same-spot multi-placement (identity-steal gate)**: host places 2-3 SAME-type items (e.g.
   crowbars/rocks) within ~30 cm of each other while the client watches (steady state is fine).
   EXPECT: the client shows the SAME COUNT as the host; host grabbing each one moves the right one.
   Log check (client): any "fuzzy match ... is already mirror-bound to eid=... never
   position-stolen; fresh-spawning instead" line = the gate working as designed (INFO, not a bug).
3. **Join regression sweep**: normal join must stay clean — no prop dups near spawn, mushrooms/
   garbage NOT doubled (the steal gate must not have broken the divergent-key dedup), piles intact.

## KNOWN-OPEN (do not re-report; lanes designed/queued)
- CLIENT melee damage = local-only (no cross-peer damage; silent crowbar door hits; door hit-open
  nudge unsynced). RE DONE: research/findings/votv-melee-damage-path-RE-2026-07-11.md — the
  damage-intent lane is the next build (design pass first; hook candidates ranked there).
- Container contents divergence (take-out ghosts / put-in losses / per-peer loot):
  docs/items/container.md §3 DESIGN ready — Inc-1 build queued.

## Log reads after the session
- Both peers: `[Error]`/`[Warn]` diff vs the known transient connect-burst set; any
  "setKey UFunction not found"; any "feed RESURRECT"; hand_item announce lines ("rel measured").
- Take 2: client "[SPAWN-DEFER]" arm/apply pairs (armed count == applied count); any
  "[SPAWN-DEFER] ... cap ... hit" (should never fire); "never position-stolen" INFO lines are
  the steal gate working, not an error.
