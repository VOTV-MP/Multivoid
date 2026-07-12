> **SUPERSEDED 2026-07-12.** The take-4 test on `3D250B83` FAILED the hotbar'd-in-window rock repro
> (the rock stayed gone) -> two more roots (5: phase drain inverted destroy->spawn wire order; 6: the
> take-3 re-key was INERT -- setKey resolution failed on the actor_save_C lineage). Current runbook:
> **`research/handson_runbook_2026-07-12.md` (TAKE 6, DLL `7030160D` — the JOIN BARRIER
> `bbf91f39`+`7847021e` superseded take-5's netting/revalidation machinery before it ran)**. RCA:
> `research/findings/join-identity/votv-join-window-placed-prop-RCA-2026-07-11.md` (SIX roots).

# Hands-on runbook — 2026-07-11 (take 4) — DLL `3D250B83` (all 4 installs, hash-verified)

HEAD = local main, NOT pushed. Build clean; deploy hash-verified. **NOT LAN-smoked** (user at PC).

## Take-3 result (on B00459CC) + the take-4 change
Take-3 user test: rocks no longer vanish (spawn revalidation works), BUT props ended up displaced
and the client sat at **2.5 fps**. Log-RCA found TWO roots:
- **ROOT A (game data)**: the host save ships DUPLICATE interactable Keys — 85 `trashBitsPile_C`
  across only FOUR keys (65+12+5+3 clone families; e.g. key `-GiVAop2Win4wvA1rllELw` on 65 distinct
  live piles). Our whole identity layer assumes Key uniqueness: a clone family funneled all 65 wire
  rows onto ONE actor; 63 rows died at loadObjects churn; the take-3 drain then honestly re-expressed
  them INTO already-occupied positions → ~75 interpenetrating AWAKE physics pairs → 285 ms/frame.
- **ROOT B (our order)**: at the quiescence edge the doom sweep ran BEFORE the spawn-revalidation
  drain — locals whose wire expression raced their loadObjects recreate (21 `g_paper_1` at spawn,
  ~150 others) were doomed as unclaimed, THEN re-expressed as fresh awake mirrors (232 doomed +
  230 re-expressed in one join).

Take-4 fixes (both root-level):
1. **HOST KEY-UNIQUENESS AUTHORITY**: at enroll (`MarkPropElement`, the one owner — covers the census
   walk AND the Init-POST late-load catch), a keyed actor whose Key is already carried by a DIFFERENT
   live actor is re-keyed with a fresh unique `rk_...` key via setKey. The game re-saves live keys, so
   the fix bakes into the host save + every transferred client world. Dead incumbent = churn recreate
   inheriting identity (spared structurally). Client/SP never re-key.
2. **ORDER FIX**: `quiescence_drain::RunReconcile` (incl. spawn revalidation) now runs at the fire
   edge BEFORE the doom sweep, while claim tracking is still armed — the re-runs converge-bind their
   recreates and CLAIM them, so the sweep spares them. Doom judges LAST.

## [USER-V] confirmed live earlier today (CLOSED)
- Swing motion continuous (v109 HandPose stream) — USER-V.
- Crowbar dupe gone (setKey base-resolve x2) — USER-V.
- Overlay hp-0.0 silence — USER-V.
- Take-3: placed rocks no longer permanently invisible (spawn revalidation) — USER-V (with the
  position/fps regression now fixed in take 4).

## NEW in 097AE46C — the things to test
1. **FPS + placement sanity (the take-3 regression)**: normal join on the same save. EXPECT: client
   fps normal after load (no 2.5 fps), props at their host positions (trash piles at the landfill /
   base, papers at spawn, your placed rocks where the host sees them).
   Log check (HOST): a burst of "KEY-UNIQUENESS -- second live actor carried Key ... re-keyed ->
   'rk_...'" lines at world load (~81 expected on the first load of this save; near-zero on the NEXT
   load — the fix persists via the save). Log check (CLIENT): the drain summary "N in-episode
   expression(s) revalidated: X rows live (skipped), Y re-expressed" with Y SMALL (single digits,
   only true no-recreate items), and few/no "dooming" lines for papers/trash.
2. **The take-2 repro, again (must still work)**: while the client is at the connection window, host
   hotbar-picks a WORLD item (hold-R) and places it elsewhere inside the window. EXPECT: visible on
   the client at the placed position; E-grab moves it. Client log: "[SPAWN-DEFER] re-expressing
   eid=..." for it (now BEFORE any dooming lines).
3. **Same-spot multi-placement (steal gate)**: host places 2-3 same-type items within ~30 cm.
   EXPECT: same count both peers; grabbing moves the right one.
4. **Join regression sweep**: mushrooms/garbage not doubled; piles intact; no dups near spawn.

## KNOWN-OPEN (do not re-report; lanes designed/queued)
- CLIENT melee damage = local-only (no cross-peer damage; silent crowbar door hits; door hit-open
  nudge unsynced). RE DONE: research/findings/player-puppet/votv-melee-damage-path-RE-2026-07-11.md — the
  damage-intent lane is the next build (design pass first; hook candidates ranked there).
- Container contents divergence (take-out ghosts / put-in losses / per-peer loot):
  docs/items/container.md §3 DESIGN ready — Inc-1 build queued.

## Log reads after the session
- HOST: count the "KEY-UNIQUENESS ... re-keyed" lines (expect ~81 first load, ~0 after); any
  "re-key FAILED" or "OFF the game thread" tripwire = report immediately.
- CLIENT: "revalidated: ... Y re-expressed" (Y should be near-zero on a healthy post-fix join);
  "SURVIVED the re-bind pass AND the pre-sweep spawn-revalidation drain" tripwire = a genuinely
  unhealable identity, name+key in the line; dooming lines should be few and legitimate.
- Both peers: `[Error]`/`[Warn]` diff vs the known transient connect-burst set; any
  "setKey UFunction not found".
