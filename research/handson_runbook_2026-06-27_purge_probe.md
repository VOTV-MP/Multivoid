> **SUPERSEDED (2026-06-27).** This was the [BIND-PROBE] probe run that picked option 3 (cursor-reset) — which
> 12:29 then REFUTED (mis-bind) and which was REVERTED (`86bca8cb`). The [BIND-PROBE] no longer exists in the
> code. Kept only as the point-in-time record of the probe; do NOT run it. CURRENT runbook:
> `research/handson_runbook_2026-06-27_purge_v1_combined.md` (variant-1 host-wire). Saga:
> `research/findings/coop-purge-timing-reconcile-race-DESIGN-2026-06-27.md`.

# Hands-on runbook — purge-timing race: lever (a) + **[BIND-PROBE]** (decides the (b) fork)

**Date:** 2026-06-27
**Deployed DLL:** SHA256 head `258A8F0D` — hash-verified **MATCH** on HOST + CLIENT + CLIENT2 + DEV (proto **v90**, unchanged).
**Build:** clean (Release). **push:** HELD (HEAD `45991a99`).
**Commits this run:** lever (a) `bfe9182a`, (b)-RE doc `bf02254a`, **[BIND-PROBE]** `45991a99`. b3 `10284a8a` still in (innocent).
**`[dev]` flag (joiner):** `save_identity_bind=1` + `save_identity_map_log=1` — already set in HOST + CLIENT (`Game_0.9.0n_copy`). The probe is gated on `save_identity_bind=1`; join with the CLIENT that has it.

> **This is a PROBE run, not a fix-verify run.** It answers ONE question that decides how lever (b) is built:
> **during the same-world purge+re-create on join, does the game's Load-Primitives re-spawn fire our bind seam,
> and if so do the re-created piles hit the bind as fresh-but-exhausted, or as recycled-address false "survivors"?**
> The answer picks option 3 (cursor-reset, zero wire change) vs option 1 (position wire-extension). Lever (a)
> (faster purge drain) is bundled because it deploys anyway and the combined hands-on needs it later; it is NOT
> the thing under test here. **No (b) feature code is built yet** — that waits on this probe's verdict + your review.

## CHANGES IN THIS BUILD (both are safe to ship; neither is the (b) fix)
- **Lever (a) — reaper escalation** (`net_pump.cpp`): when a reaper scan hits the 256 evict-cap (backlog remains)
  or a purge episode is mid-drain, it cancels the 4 s throttle so the backlog drains at frame cadence
  (~256/frame) instead of 256/4 s. Effect: the 09:54 "37 s slow drain" should become near-instant, so the
  episode-end re-seed lands early (restores the 16:42 timing). Bounded per call (no hitch), self-terminating.
- **[BIND-PROBE] — read-only diagnostic** (`save_identity_bind.cpp`): logs every bind-seam fire that lands
  DURING a purge episode, with the per-fire breakdown (survivor vs exhausted-cursor). Mutates nothing.

## ⚠️ CAPTURE LOGS IMMEDIATELY after this run (the probe lines are the whole point)
Right after the test, BEFORE launching anything else, save both:
- `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (HOST)
- `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`  (CLIENT — the joiner, has the probe)
(or just say "done, logs saved" and I'll copy them.)

## What to do — reproduce the 09:54 slow-purge join (same steps as last time)
1. Launch `mp_host_game.bat` (host) — your usual save (with off-kerfurs + chipPiles).
2. Launch `mp_client_connect.bat` (client). **As EARLY as you can in the connect window:**
   - On the **host**: **grab and move two-or-three chipPiles** to clearly different, far-apart spots, FIRST and FAST.
   - On the **host**: turn a kerfur **ON** (wake a lying-down/off one).
3. Let the client finish joining + settle (~15–20 s). Watch the world.
4. On the **client**, look at the moved piles + the kerfurs **without grabbing** — note if any are **ghosts**
   (visible but interaction doesn't leak: you can't pick them up, or the host doesn't see your interaction).
5. **Capture the logs (see above).** Then you may grab/throw to confirm.

## The reading — the `[BIND-PROBE]` verdict (CLIENT log; this DECIDES the fork)
Grep the **client** log for `[BIND-PROBE]`. Each line (throttled to the first 8 + every 100th per family) reads:
`[BIND-PROBE] chipPile seam fire #N during purge episode: cursor=C/S armed=A survivor=V exhausted=E -- reset-would-rebind=R`

Also note the existing summary line near the end: `save_identity_bind: BIND SUMMARY -- bound .../... ; ... OVERFLOW -- X chipPile + Y kerfurOff`.

**Decision tree (I'll confirm from the captured log, but here's what each outcome means):**
- **Many `[BIND-PROBE]` fires with `survivor=1`** (re-created piles land at recycled addresses of the just-purged
  bound natives → mistaken for survivors → early-return → no rebind = the ghost) → **option 3 + clear-bound-flags**:
  on a purge episode, clear the bound-mirror flags for the purged natives AND reset the cursors, so the re-creates
  rebind via the proven ordinal. **Zero wire change.**
- **Many `[BIND-PROBE]` fires with `survivor=0 exhausted=1`** (fresh re-creates, cursor already consumed →
  overflow-guard → no bind) → **option 3 (plain cursor-reset)**: reset the per-family cursors at episode-start so
  the seam re-consumes the re-spawns in array order. **Zero wire change.** (If `reset-would-rebind=1`, that's the
  smoking gun a reset fixes it.)
- **~0 `[BIND-PROBE]` fires** but the world still shows ghost piles (and the re-seed log still says
  `world-change re-seed added NNNN`) → the re-create **bypasses the bind seam** → cursor-reset can't help →
  **fall back to option 1 (position wire-extension): +FVector on the sidecar `IdEntry`, host fills from the
  save-time positions, client matches save↔save (moved-pile-safe, b3 delivers the moved pos separately).**

## Also capture (context for the verdict)
- `net_pump: mass-purge detected (reaped 256 >= 64)` — the episode START (note its timestamp).
- `net_pump: world-change re-seed added NNNN live keyed prop(s)` + `NOT re-announcing` — the episode END.
  **With lever (a) the gap between these two should be SECONDS, not the ~37 s it was at 09:54.** Tell me that gap.
- `save_identity_bind: ARMED with 874-entry host eid map` — the bind arm (its timestamp vs the purge: was the
  purge before or after the arm?).
- Whether **ghosts actually appeared** this run. Lever (a)'s faster drain may itself land the re-seed early enough
  that the ghost does NOT reproduce — that would be informative (a helps), but the `[BIND-PROBE]` fire-count still
  tells us whether the seam fires on re-create (the option-3-vs-1 question), independent of drain speed.

## Honest notes
- **This run does not fix the ghost** — no (b) code exists yet. If ghosts still appear, that is EXPECTED; the probe
  is to measure the mechanism, not cure it. (Lever (a) alone *might* incidentally suppress it via timing — if so,
  good, but we still build (b) for the late-GC-purge case the design calls out.)
- The probe is read-only (RULE-2-exempt diagnostic); it cannot make anything worse.
- After you report the `[BIND-PROBE]` breakdown + the re-seed gap + whether ghosts appeared, I write the (b)
  design per the verdict (option 3 or option 1), you review it, then I build (b). Then the combined slow-purge
  hands-on verifies (a)+(b) (no ghost, b3 applies, kerfur retires), and we push the rubicon whole.
- If the probe shows the seam never fires AND lever (a) suppressed the ghost so you can't tell — say so and I'll
  add a finer probe (count the re-created natives that end up unbound) for a clean second read.
