# Hands-on runbook — v112 desk INPUT + v113 L4 DISHES (batched), take 1

DEPLOYED: `votv-coop.dll A1C13DB9108775DC` x4 (HOST + CLIENT_1/2/3), hash-verified.
kProtocolVersion **113** (a 112-or-older peer HARD-CLOSEs at the gate). HEAD `71e6d7b2`
(committed; unpushed). Autonomous LAN smoke PASS (~35 s steady: client parked both tickers,
snapshot 24/24 applied, zero lane WARN/ERROR, RSS stable). Perf audit 0 CRITICAL; correctness
audit F1 fixed pre-deploy. **NOTHING below is hands-on verified yet.** This take BATCHES the
never-taken v112 verdict with v113 — the log lines are per-lane attributable (`desk_input:` /
`desk_sim:` vs `dish_sync:` / `[dish]`); if anything desk-input-ish regresses, it is v112
territory (runbook `handson_runbook_2026-07-16_desk_v112.md`, now folded into this one).

## What changed in v113 (L4, all BUILT — design `votv-dish-L4-impl-DESIGN-2026-07-16.md`)
1. CLIENT dishes no longer self-simulate: both dish tickers parked; the client's own ping slews
   are killed right after the catch relays; ALL client dish motion is the HOST's pose stream.
2. The download ARM (and its POLARITY — previously a per-peer random roll = the pol=1-vs-0
   divergence) is host-authored; the client gets the full native display tail (camera aim +
   photo-screen render) at the host's arm moment.
3. Dish calibration syncs both ways (terminal/toolgun on any peer, host decay, virus scramble).
4. The joiner seeds all 24 dish poses + calibration + an armed download from connect rows
   (the old pending-adopt is gone).

## STEPS
### v112 half (desk input — unchanged from the v112 runbook)
1. HOST fresh save / New Game, create session; CLIENT joins.
2. CLIENT (standing, no interface) presses freq/pol +1/+5/+15, filter toggles, polarity-dir:
   knobs MOVE + numbers match on BOTH peers (~0.5 s).
3. Tuning sounds + unit hums/lamps flip on BOTH peers.
4. CLIENT coords panel 1/2/3: cooldown charges + refuses re-press natively; CDOWN on both.
5. CLIENT SHIFT scan: arrows + long beep on the HOST too.
6. ENTER ping on one peer: APPROXIMATION:/ANALYSIS: lines appear on the OTHER peer's coordLog.

### v113 half (dishes)
7. **HOST pings + catches a signal**: the CLIENT's dishes must visibly slew (mirrored motion,
   with motor audio) and settle where the host's settle; while they move, the CLIENT's console
   ping must refuse ("Satellites are active") like the host's.
8. When the host's dishes finish: the CLIENT's download must arm by itself — same signal, SAME
   POLARITY on both peers (the old bug showed pol=1 vs pol=0), photo screen renders on both.
9. **CLIENT pings + catches**: the client's own dishes start then hand off (brief motion, then
   mirror); the HOST's dishes run the real theater; the arm lands on both with the host's
   polarity. No stuck "Satellites are active" on either peer afterwards; no looping dish motor
   audio left behind (listen near a dish).
10. CLIENT calibrates a dish at the ui_console terminal: the percent rises on the HOST's list
    too (1 Hz steps are expected); toolgun set syncs likewise.
11. JOIN mid-slew (CLIENT_2 joins while host dishes move): its dishes appear mid-motion, not at
    random rest poses; if a download is armed, its desk arms on arrival.
12. Delete the signal (desk button): the download machine clears + the rendered signal object
    disappears on BOTH peers.

## WHAT TO READ IN THE LOG
- `dish_sync: client ticker_disher parked` + `ticker_dishUncalib parked` — once per client boot.
- `[dish] N '<techName>' host slew START/STOP` (host) and `mirror slew START/STOP` (client) —
  per-dish edges; identical dish sets on both.
- `dish_sync: host ARM broadcast (decoded=.. polarity=N)` then client `ARM applied (...same N)` —
  THE polarity take.
- `dish_sync: connect snapshot -> slot K (24 dishes)` + client `snapshot applied (24 dishes)`.
- WATCH-FORS (each = a bug, stop and grep): a repeating `cue reconciler: leaked cue deactivated`
  line (the one inferred risk — cue mirroring); `ARM pre-clear wiped N mirrored mover(s)` WARN
  (gate-bypass arm); any 1 Hz-repeating `[dish]`/`desk_input` line at idle; `DishArm dropped --
  ... not yet resolved` WARN on a joiner.
- v112 watch-fors unchanged: zero `console_state: desk button edge broadcast`; no 1 Hz
  desk_input spam at idle.

## Honest status
BUILT + smoke-passed only. The v112 lanes additionally carry two prior smoke PASSes. Neither is
VERIFIED until this take.
