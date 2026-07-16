# Hands-on runbook — v112 desk INPUT lane (the BUGS-v111 fix), take 1

DEPLOYED: `votv-coop.dll 43E662E19057216F` x4 (HOST + CLIENT_1/2/3), hash-verified.
kProtocolVersion **112** (a 111 peer HARD-CLOSEs at the gate — that itself is a take point).
HEAD `7d57478f` (committed; 3 commits unpushed). Autonomous LAN smoke PASS x2 (45 s + 40 s
steady, RSS ~3.2 GB stable, logs clean, retired lanes silent). Perf audit PASS, 0 CRITICAL.
**NOTHING below is hands-on verified yet.**

## What changed (v112, all BUILT)
The five BUGS-v111 fixes (design: `votv-desk-input-lane-DESIGN-2026-07-16.md`):
1. Claim-free field-granular `DeskInput` deltas for ALL desk input (knob speeds, filter
   toggles, polarity dir, volume, select, maxLevel, unit power, coordIsPing) — the world-space
   buttons no longer need the intComs claim.
2. Cooldown = charge events + native per-peer decay (the stream no longer erases a client's
   charge; the native press-gate works on every peer).
3. Sounds: speeds/toggles now mirror -> the gauge loops wake; unit hums/lights apply via the
   native setter effects; the host's knobs are no longer stomped (the flap lane is deleted).
4. Detector: per-channel exact-snap interp -> the host's bitwise 1.0 lands -> ONE finish beep.
5. SHIFT scan mirrors (spawnDirs arrows + beep on the other peer, <=250 ms late); the coordLog
   CR:/APPROXIMATION:/ANALYSIS:/AREA SCAN: families now ship (only CDOWN stays regenerated).

## STEPS (host + client)
1. HOST fresh save / New Game, create session; CLIENT joins.
2. **CLIENT (standing, no interface)** presses the download unit's +1/+5/+15 freq/pol buttons,
   the two filter toggles, the polarity-dir button: the knobs must MOVE and the numbers must
   match on BOTH peers (host screen follows within ~0.5 s).
3. Both peers near the desk while tuning: the polarity click-loop + freq radio-tune SOUNDS play
   on BOTH peers; toggling a unit's power flips its hum + lamp on BOTH.
4. CLIENT at the coords panel: press 1/2/3 — the cooldown must charge and REFUSE re-press
   natively (the v111 bug erased it); the CDOWN bar counts down on both peers.
5. CLIENT presses SHIFT (cooldown empty): arrows + long beep appear on the HOST too.
6. Catch + download a signal to 100% detector: the finish beep plays ONCE (not a loop);
   SAVE SIGNAL works on the peer that watched.
7. ENTER ping on one peer: APPROXIMATION:/ANALYSIS: lines appear in the OTHER peer's coordLog.

## WHAT TO READ IN THE LOG
- `desk_input:` lines — every charge logs its verdict (`verdict=SCAN|DOT`); deltas are silent
  by design at idle (any 1 Hz-repeating desk_input line = a detector bug, stop and grep).
- ZERO `console_state: desk button edge broadcast` lines (the retired lane must stay silent).
- On a leaver mid-ping: `desk_input: cleared dangling coordIsPing`.

## Known residuals (documented, not bugs): sub-RTT same-control cross (display-only on the
loser); a delta arriving during a level transition drops (stale till next press); <=2 extra
detector pulses near the crossing; scan mirror latency <=250 ms; joiner misses in-flight deck
playback. Full list in the design doc.
