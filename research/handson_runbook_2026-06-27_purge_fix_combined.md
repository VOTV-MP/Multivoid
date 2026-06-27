> **SUPERSEDED (2026-06-27).** This verified the (a)+(b) cursor-reset, which 12:29 REFUTED (mis-bind) and which
> was REVERTED (`86bca8cb`). The RESET-FOR-RESEED / reseed-rebind machinery this runbook reads no longer exists.
> Do NOT run it. CURRENT runbook: `research/handson_runbook_2026-06-27_purge_v1_combined.md` (variant-1 host-wire).
> Saga: `research/findings/coop-purge-timing-reconcile-race-DESIGN-2026-06-27.md`.

# Hands-on runbook — purge-timing fix COMBINED (a)+(b): kill the join-window ghost

**Date:** 2026-06-27 (combined verify run)
**Deployed DLL:** SHA256 head `2663BD72` — hash-verified **MATCH** on HOST + CLIENT + CLIENT2 + DEV (proto **v90**, unchanged).
**Build:** clean (Release). **Audit:** SHIP (no CRITICAL/HIGH/MEDIUM; hang provably bounded, lock order consistent, doom exposure closed). **push:** HELD (HEAD `685c9662`, 17 ahead).
**Commits:** (a) `bfe9182a`, (b) `685c9662`. b3 `10284a8a` rides along (its apply was blocked by the ghost — should now unblock).
**`[dev]` flags (joiner):** `save_identity_bind=1` + `save_identity_map_log=1` (HOST + both CLIENT inis).

> **This IS the fix-verify run** (the 09:54 + 11:32 runs were diagnosis). The purge-timing race: a same-world
> Load-Primitives re-create reaps + re-spawns the save-authoritative chipPiles/off-kerfurs during the join; the
> per-family bind cursors were already consumed by the first load, so the re-spawns hit the exhausted cursor and
> DON'T bind = **ghost piles** (visible but interaction doesn't leak to the host). **(a)** drains the purge fast
> (37s -> <1s, already confirmed 11:32). **(b)** resets the cursors + clears the bound-mirror flags at the
> purge so the re-creates **re-bind** via the proven ordinal, and **holds the divergence sweep until the re-bind
> trickle settles** (so the sweep + b3 + kerfur-retire adjudicate the RE-bound world, not the half-rebound one
> the sweep fired on at :04 in 11:32).

## ⚠️ CAPTURE LOGS IMMEDIATELY after the run
Right after, BEFORE launching anything else, save both:
- `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (HOST)
- `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (CLIENT — the joiner)
(or say "done, logs saved" and I'll copy them.)

## What to do — the SAME 09:54/11:32 slow-purge repro
1. Launch `mp_host_game.bat` (host) — your usual save (off-kerfurs + chipPiles).
2. Launch `mp_client_connect.bat` (client). **As EARLY as you can in the connect window:**
   - On the **host**: **grab + move 2-3 chipPiles** to clearly different, far-apart spots, FIRST and FAST.
   - On the **host**: turn a kerfur **ON** (wake a lying-down/off one).
3. Let the client finish joining + settle (~20-30 s — the sweep now waits for the re-create trickle, so it fires
   LATER than before; give it room).
4. On the **client**, **without grabbing**: are the piles all interactable (no ghosts)? Are the moved piles at
   their MOVED spots (b3)? Is the kerfur correct (active NPC, no stale lying-down dup)?
5. **Capture the logs.** Then grab/throw a pile to confirm the interaction path + that grabs leak to the host.

## Acceptance (PASS = all)
| # | Check | PASS = |
|---|-------|--------|
| 1 | **No ghost piles** | every pile on the client is interactable AND your grab/move leaks to the host (host sees it). NO pile that's visible-but-dead. *This is the whole fix.* |
| 2 | **Moved piles (b3)** | each EARLY-moved pile renders at its MOVED position on join, no grab needed (b3 now applies post-rebind). |
| 3 | Kerfur (#1) | the turned-on kerfur is an active NPC on both; NO extra stale lying-down dup (retire 1/1). |
| 4 | Regression | clean-join intact; grab (E) + E-drop + LMB-throw still work; #2 (no pile-move dup) + b2 (delivered-convert snap) hold. |

## The log reading (CLIENT — I'll confirm from the captured logs)
The new fix markers, in order:
1. `net_pump: mass-purge detected (reaped 256 >= 64)` — episode start. **Immediately followed by**
   `save_identity_bind: RESET-FOR-RESEED -- per-family cursors->0 + bound-mirror flags cleared` (Part 1 fired).
2. `[BIND-PROBE] chipPile re-create #N in reseed-rebind window: cursor=C/870 ... will-rebind=1` — the re-creates
   re-binding from cursor 0 (cursor climbing toward 870). **This is the fix working** (vs 11:32 where they hit
   the exhausted cursor). survivor/exhausted should read 0.
3. `save_identity_bind: reseed-rebind SETTLED (chip 870/870 kerfur 4/4; reason=all-rebound)` — the trickle
   finished binding. (reason=no-progress-dwell or hard-cap = the fallback fired because the re-create count
   didn't reach the mapped count — tell me if so, it means the purge re-created fewer than mapped.)
4. `remote_prop_spawn: divergence sweep FIRING (load tail quiesced; ...)` — now AFTER the SETTLED line (later
   than 11:32's :04). 
5. `[PILE-B3] CLIENT pos-correction APPLIED eid=... drift=X.XXcm` — b3 now APPLIES (vs 11:32 armed-but-never).
6. `kerfur_reconcile: sweep-retire -- 1 of 1` — #1 retire (post-rebind).
7. **NO** `save_identity_bind: chipPile keyless spawn beyond the mapped 870` overflow line — the ghost signature
   should be GONE (the cursors were reset, so the re-creates bind instead of overflowing). **If this line still
   appears, the fix didn't take** — tell me (it means re-creates fired the seam before the reset, an ordering
   miss).

## FAIL signatures (tell me which + the eid)
- Ghost piles persist + a `chipPile keyless spawn beyond 870` line in the log -> re-spawns fired the seam BEFORE
  ResetForReseed (ordering); the fix would move the reset earlier (e.g. arm on the first dead-detect, not the
  episode threshold).
- `reseed-rebind SETTLED` never logs + the sweep fires on the 120s ceiling -> the rebind hung (shouldn't — the
  60s hard-cap should settle it); capture the cursor counts.
- b3 still `armed` with no `APPLIED` -> the sweep fired before SETTLED (re-arm/gate miss) — capture the sweep
  FIRING timestamp vs the SETTLED timestamp.
- Any clean-join / grab / throw / #1 / #2 / b2 regression -> capture logs; the change is client-join-only +
  gated, so a regression would be unexpected.

## Honest notes
- Autonomous smoke can't do "host mutates while client joins" + is rendering-blind, so this is your eyeball + the
  captured logs. The fix is RE-pinned (the 11:32 overflow lines proved the seam fires on re-create), audited SHIP,
  and the hang is provably bounded (3 backstops) — but the no-ghost / b3-applies / no-regression verdict is the
  hands-on.
- The sweep now fires LATER (it waits for the re-create trickle ~15-20s), so the world settles a few seconds
  later than before — that's intended (the 11:32 bug was firing too early). If it feels slow, that's the gate
  doing its job; the log's `SETTLED` -> `FIRING` ordering confirms it.
- On PASS this closes the join-window rubicon -> **push #1/#2/b2/b3 + purge-fix (a)+(b) together** as the
  verified sync etalon, then the sync-module consolidation refactor (step 4).
- If a FAIL signature shows, capture the eid + the named log lines and I'll pin the exact gap (ordering vs
  re-arm vs hang) — no guessing.
