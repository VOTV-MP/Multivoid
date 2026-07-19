# weather_rain + weather_probe extraction — weather_sync under the cap (2026-07-19, AS-BUILT)

The s24b-queue "go next" item: `coop/world/weather_sync.cpp` measured **1154 LOC** (soft cap 800).
7-round /qf converged ("that holds" round 7); two-commit plan per the s24b precedent.

## Axis (settled round 1-2 by measurement, not preference)

The FAMILY axis: this same file already shed weather_fog (322), weather_lightning (220),
weather_redsky (326) as sub-lane modules in `coop/world/` — each owns its sub-lane's
resolve+apply+debug+disconnect, takes the cycle AS A PARAMETER, latches its OWN substrate.
weather_fog.h:44 states the shape explicitly: "P7: this module owns the fog engine substrate
(offsets + UFunction thunks); weather_sync (gameplay/network) drives it." A competing
`ue_wrap/world/daynight_cycle` wrapper axis was REJECTED: it would fragment the cycle's
WEATHER substrate across coop modules + a ue_wrap file, diverging from the shipped family x3.

**Post-cut correction (documentize 2026-07-19):** the /qf pass argued the rejection as if no
ue_wrap cycle wrapper existed — WRONG as a negative existence claim: `ue_wrap/world/
daynightcycle.{h,cpp}` EXISTS and wraps the cycle's CLOCK concept (totalTime/Day/TimeScale/
MaxTime/timeZ; consumers time_sync + dev/set_clock). Neither the primary nor 7 critic rounds
grepped for it. The DECISION stands — the cycle's substrate is deliberately split BY CONCEPT
(clock half = ue_wrap::daynightcycle per its own header note; weather half = the coop
weather-module family), and weather_rain conforms to the weather half's shipped shape — but
the axis argument should have CITED the existing wrapper, not asserted its absence. Lesson:
[[lesson-axis-decision-census-existing-wrappers]]. (Cache census post-cut: THREE independent
IsLiveByIndex-validated cycle caches — daynightcycle::Cycle throttled, weather_sync::
ResolveCycle, weather_rain::ResolveCycle — all the correct recycled-slot-safe shape.)

## Commit 1 — coop/world/weather_rain.{h,cpp} (the rain+snow cycle-side sub-lane)

- Owns: its OWN 5 mutator UFunction resolves (causeRain, setRainProperties, setWindParameters,
  intComs_triggerSnow, setRainParticles) + own latch; the causeRain echo-suppress flag + PRE
  interceptor + its registration (in-module `Install()`, both roles, once-latch); its own
  small cycle cache (IsLiveByIndex shape) for the external-entry surfaces.
- `ReadState(cycle, &payload)` — the rain/snow flag bits + 5 scalars read (verbatim block,
  incl. the enable_fog/enable_superfog CONFIG bits whose actor-driven APPLY stays in
  weather_fog); `ApplyFromHost(cycle, payload, cur, &outcome)` — the cycle-side delta apply
  (verbatim steps A-E + snow); `DebugForceRain/DebugForceSnow/ReadLocalIsRaining` verbatim;
  `SetSession` for the Debug* role checks; `OnDisconnect`.
- **No cross-module fn-ptr handoff** (/qf R2): the orchestrator resolves its 3 POST-observer
  targets (causeRain/setRainProperties/intComs_triggerSnow) INDEPENDENTLY in its own table
  (5 sched + 3 mut rows). UFunction ptrs are UClass-stable (the existing OnDisconnect comment)
  — no re-latch machinery anywhere. Cost: 3 duplicate FindFunction calls once per session.
- **The :1144 fused "applied" log line is NOT reshaped** (/qf R3): the orchestrator reads the
  composite `cur` BEFORE delegating (today's position), rain returns
  `{rainTx, snowTx, scalarsChanged}`, the orchestrator emits the byte-identical format string
  in the same position (after fog+wind applies).
- **All log lines keep the `weather:` prefix** (/qf R4 measurement: redsky keeps 24 such
  lines, lightning 9; `tools/lan-test.ps1:440-449` greps the literals
  `weather: DebugForceRain` / `weather: host broadcast flags` / `weather: applied flags` —
  grep-stability is load-bearing). New latch line: `weather: rain module resolved`.
- **RULE-2 rider**: the 3 thin forwards (DebugForceRedSky / ApplyRedSky / ApplyLightningStrike)
  RETIRED; measured 16-row caller census migrated: event_dispatch_world → weather_redsky::Apply
  / weather_lightning::Apply direct; autotest → weather_rain:: + weather_redsky::DebugForce;
  force_weather → weather_rain::DebugForceSnow. autotest.h doc comments re-anchored.
- Wire format UNCHANGED; no proto bump.

## Commit 2 — coop/dev/weather_probe.{h,cpp} (the ini-gated diagnostics)

The `[probe weather]` + `[probe wind]` block from TickConnect + `ReadComponentIsActive`
(measured single-caller: the probe) move to the dev tree (force_weather.cpp precedent).
Wind-roll counters stay with their writer (the interceptor in weather_sync); read via a tiny
accessor pair. The cycle comes from `weather_rain::Cycle()` under the probe's own ini+throttle
gates (no third cache, no hot-path change when the ini flag is off).

## Equivalence evidence (per commit)

- `rain_body_diff.py` (scratchpad): 315 moved-region lines from the BASELINE weather_sync.cpp
  (13 spans) set-diffed against weather_rain.cpp, with ENUMERATED drops (1 comment line whose
  "below" became wrong) + enumerated new-scaffolding list + a NEGATIVE check that the composed
  weather_sync.cpp is clean of the moved bodies (with a by-design ALLOWED_SHARED list: the 3
  independent observer-target resolves, the orchestrator's own cycle cache, generic guard
  idioms). **Mutate control**: a swapped ParamFrame field name injected into the moved set →
  FAIL on both directions; real run → PASS.
- **Acceptance gate** (`accept.sh`): lan-test.ps1's literals + thresholds imported VERBATIM
  (>=2 / >=2 / >=2), probe floors (>=30 `[probe weather]` per peer — the dead-gate lesson),
  4/4 host settle lines with expected values, final cross-peer parity (isRaining +
  rainStrength from the last probe lines), rain-tx presence, fused-line format identity,
  zero weather-lane WARN/ERROR (the WARN gate itself proven by a live injection
  known-positive). **The BASELINE run passed the full gate first** (4/6/75, probes 101/88,
  parity r=0 s=1.03) — the instrument cannot pass vacuously.
- Deterministic start both runs: identical restored `s_1234.sav` bytes (snapshot in
  scratchpad), identical peer inis (`weather_probe=1` both runs, reverted after), identical
  shell env (`VOTVCOOP_RUN_WEATHER_TEST=1` — propagates through mp.py's os.environ.copy);
  baseline ran FIRST on pre-cut HEAD bytes.

## Landings (live wc -l, not estimates)

- weather_sync.cpp: 1154 → 850 (commit 1, `828844b2`) → **784 UNDER CAP** (commit 2).
- weather_rain.cpp/h: 432 + 82 (incl. the commit-2 `Cycle()` public).
- weather_probe.cpp/h: 110 + 23. All under caps.
- The /qf R4 estimate said ~820 после commit 1 — measured 850; ~745 after commit 2 —
  measured 784. Estimates recorded as estimates; the live numbers are the record.

## Commits + audit

- Commit 1 `828844b2` (weather_rain + RULE-2 rider), commit 2 `cd59ad13` (weather_probe).
  DLL `c2b33b2a4b1e5d3f` x4 hash-verified, proto 121.
- Commit-2 smoke note: the FIRST commit-2 run failed ONE floor (client probe lines 29<30)
  with every discriminating check PASS — the gameplay window was ~90s vs 215s because the
  audit agent ran in parallel (the s_1234 lesson's quiet-machine clause). The quiet-machine
  re-run matched baseline exactly (101/89 probes, 4/6/75, 4/4 settles, parity r=0 s=1.41).
  The floor did its job: it forced a re-run instead of a hand-wave.
- Audit (code-reviewer agent): 6/6 PASS, 0 findings >=80 confidence. Informational (~40):
  one g_session atomic load per tick moved out of the ini gate (one mov; below the bar).
  Residue flag: autotest.cpp 1003 LOC (pre-existing >800, touched by the rename only) —
  queued: split the per-feature autonomous test routines. *(Resolved 2026-07-19 s26:
  dissolved into 6 one-feature TUs + autotest_grab.cpp 393 —
  votv-autotest-dissolve-DESIGN-2026-07-19.md.)*
- Ini revert CHECKED (known-positive 1 -> 0, byte-identical to pre-session backups);
  s_1234.sav restored to the pre-session snapshot bytes.

## Honest status

Behavior preservation is literal-diff + gated-smoke + audit proven; NOT hands-on
(weather surfaces additionally ride the standing take-4 runbook; the in-smoke
weather_test exercised the forced rain path end-to-end cross-peer).
