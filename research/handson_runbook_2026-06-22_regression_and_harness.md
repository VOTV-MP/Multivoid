# take-32 — take-31 Z REGRESSION fix + the autonomous test harness (no human in the loop)

**Deployed:** `votv-coop.dll` SHA **`CED7ABCF823D772E`** to all 4 copies (MATCH x4). Proto **v83** (no wire
change). Build CLEAN. **Verification mode CHANGED:** the user stepped out of the manual-tester loop, so this is
verified by the **autonomous log-truth harness**, not a human hands-on.

## The take-31 regression (found + root-caused, NOT the destroy)
take-31 (`01825BFA`) regressed: derived (thrown) piles **did not appear at all** on the client. Root, proven
from the HOST log (every `[TRASH-CH] HOST BROADCAST ToPile ... at (0.0,0.0,0.0)`): my Z fix read `loc` from
`newActor` (the pile) at the BeginDeferred POST — but the pile is **NOT positioned there** (FinishSpawningActor
runs *after* BeginDeferred returns), so `GetActorLocation(newActor)` = `(0,0,0)`. Every ToPile convert snapped
the client proxy to the world origin = "disappeared." (take-30's rotation "worked" only because an *identity*
rotation looks correct for a flat pile — masking the unset transform.) **The destroy was innocent** — the
harness confirms it (`dup-destroy-clean` PASS, `no-proxy-self-destroy` PASS); it only kills *natives*, never the
proxy. The user's "destroy eats its proxy" hypothesis was disproven in-harness.

## The fix (`CED7ABCF`)
Re-read the pile's REAL transform at the **land-settle COMMIT** (`TickCarry`, K ticks later = post-
FinishSpawning, the pile is positioned). `LandSettle` now stores the pile actor + its GUObjectArray index;
at commit it re-reads `GetActorLocation/Rotation/Scale3D(pileActor)` (IsLiveByIndex-guarded for the cross-tick
pointer), falling back to the thunk-captured clump transform only if the pile died (a committing settle always
has a live settled pile). The thunk now passes the **clump** transform as the fallback (a positioned actor),
not the unpositioned pile. This fixes Z **and** rotation **and** scale in one (all re-read from the settled
pile) — strictly more correct than reading the unpositioned newActor.

## The NEW autonomous test loop (the deliverable for this mode) — [[reference-pile-test-harness]]
- **`tools/pile-test-assert.ps1`** — reads the host+client `votv-coop.log`, checks pile invariants PASS/FAIL +
  exit code. The live regression unit is **`ToPile-loc-nonzero`** (no `BROADCAST ToPile at (0,0,0)`); it FAILed
  on the broken build and is the FIX's proof when it PASSes. Plus `land-commit-reread` (my fix logs
  "re-read from the settled pile" vs "FALLBACK"), sound/dup/arc/carry invariants.
- **`harness/autotest_chippile.cpp`** (env `VOTVCOOP_RUN_CHIPPILE_TEST=1`) — a REAL autonomous host scenario:
  grab (production `InpActEvt_use` + `playerGrabbed`) → 8s moving carry → re-pile. Mechanism is current.
- **Run it headless:** `VOTVCOOP_RUN_CHIPPILE_TEST=1 python tools/mp.py smoke --duration 200 --join-grace 90`
  → fresh logs → `pwsh tools/pile-test-assert.ps1`. No human.

## Status — VERIFIED [V] by the autonomous harness
- Regression root-caused (host log) + FIXED (`CED7ABCF`, built + deployed MATCH x4). Harness proved the bug on
  the broken-build log (`ToPile-loc-nonzero` FAIL, 18/18 at (0,0,0)) — the FAIL→FIX unit.
- **VERIFIED [V] (no human):** an autonomous chippile smoke (`VOTVCOOP_RUN_CHIPPILE_TEST=1 python tools/mp.py
  smoke --duration 200`) drove a real grab → 8s carry → re-pile; the host broadcast `BROADCAST ToPile ... at
  (2560.2,731.8,6098.8)` (a REAL loc) + `LAND COMMIT (transform re-read from the settled pile)`. The harness on
  those fresh logs: **VERDICT PASS, 11/11 invariants, 0 CRITICAL fail** (ToPile-loc-nonzero PASS,
  land-commit-reread PASS, plus dup-destroy 12 twins/0 SKIP, sound + carry + arc all PASS). The complete loop —
  edit→build→deploy→smoke→assert→PASS — ran with no human.
- LIMITS (honest): the smoke's re-pile is a vertical lift+drop, so the ARC is only lightly exercised (the throw
  arc itself was user-verified hands-on in take-29); the harness asserts the HOST broadcasts a sane loc + the
  re-read path, but does NOT yet assert the CLIENT renders the exact transform (needs client-loc instrumentation
  — next). Z/rotation magnitude vs the host is not yet auto-asserted.
- Push HELD (user directive, whole period).
- NEXT after green: refresh the autotest's stale morph prints + make the re-pile a directional throw; add
  client-loc instrumentation so the harness asserts client transform == host transform directly; then the
  deferred whoosh-cue (wire) / FPS re-seed / pile-dedup extraction.
