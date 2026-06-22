> SUPERSEDED (2026-06-22) -- this take's work is FIXED + verified; see research/handson_runbook_2026-06-22_regression_and_harness.md + the canonical finding.

# Hands-on runbook — take-30 — pile-landing ROTATION + throw SOUND (kill the wrong pickup)

**Deployed:** `votv-coop.dll` SHA **`778BC35F706A16B7`** to all 4 copies (host / copy / copy2 / dev) — verified
MATCH x4. Proto **v83** (UNCHANGED — host-stream + thunk-read only, no wire change). Build CLEAN (Release, 0
errors). Adversarial audit PASS (zero CRITICAL) — its one real finding (a cross-tick UAF window on the cached
`g_lastHeldProp` pointer in the flight branch) was FOLDED IN before this SHA: it now validates the cached clump
via `R::IsLiveByIndex` + a cached GUObjectArray index (the established cross-tick-pointer pattern). **NOT
autonomously smoked** — you are on the PC, so this is prepared
ground and YOUR hands-on is the test ([[feedback-user-tests-claude-prepares-ground]]).

> Continues take-29 (`2026-06-22_throw_arc_probe.md`). **take-29 results (all confirmed in the real v83 logs
> `Game_0.9.0n*/.../votv-coop.log`):** ① the throw ARC **FLIES** [V] (host: 27 `flight CONTINUE`). ④ the
> level-pile PROBE read **`1cm=1` on all 16 samples** → the native-coexistence dup root is PROVEN and a tight
> ~1cm DESTROY match is safe (next build). This take-30 build fixes the two REMAINING throw bugs you reported:
> the **landed pile's rotation** and the **wrong sound on throw**.

## What this build changes (two independent fixes, both root-caused in the logs)

### ④ ROTATION — the landed pile now lands at the host's orientation, not a random one
**Root (code-proven):** the re-pile thunk (`trash_collect_sync.cpp`) read the convert ROTATION from `srcObj`
(the **clump** — a live physics actor at its tumbled FLIGHT orientation), while it read scale from `newActor`
(the **pile**). So the client pile inherited the clump's random tumble instead of the pile's deterministic
resting rotation. Bytecode confirms the pile's spawn transform is a fresh sphere-trace `MakeTransform` (a
deterministic resting orientation), applied to `newActor` immediately at the thunk POST — which is exactly why
scale-from-`newActor` already worked. **Fix:** read `rot` from `newActor` (the pile), symmetric to scale. `loc`
stays from `srcObj` (you confirmed position is correct).

### ② SOUND — the throw no longer replays the pickup `use click` (two contributors, both removed)
**Root (log-proven, client had 58 `use click` + 0 whoosh + 29 spurious `proxy throw`):**
- **(a)** the carry/flight CONTINUE branch streamed `key=''` while the carry main branch streamed `key='None'`.
  The receiver's drive-continuity gate (`remote_prop.cpp` `KeyMatchesCache`) saw the key change mid-stream →
  implicit-release + re-`StartDrive` → replayed `PlayUseClick`. **Fix:** the flight branch now streams the SAME
  key as carry → one continuous drive, one grab sound.
- **(b)** 29 `FIRE(release)` edges fired post-land (`carrying=0`) → spurious `PropRelease |v|=0` → the client
  turned each into a `proxy throw` `ClearAnyDriveFor` (drive churn → more `use click`). **Fix:** a TRASH eid's
  throw sends NO `PropRelease` (the flight-stream + the ToPile convert own the throw end; the client drives no
  clump physics, and the ToPile convert clears the drive). A non-trash Aprop_C keeps its normal velocity release.
- The now-dead `OnHostRelease` (the throw-edge ctx bump) was removed (RULE 2).

> **Whoosh-on-throw is NOT in this build.** With (a)+(b) the throw is now SILENT (the *wrong* sound is gone). The
> *right* whoosh needs a throw-cue signal to the client (a new reliable-message kind = wire work) — it is the
> next item, sequenced after you confirm the pickup sound is gone.

## The test — host grabs a pile, carries, THROWS (client watches), near a cluster
1. **Carry (regression check).** Carry a clump ~10 s. CLIENT: still SMOOTH (the carry main branch + the flight
   stream are key-consistent now; must not regress).
2. **ROTATION (check #1).** Throw the clump; it lands and morphs to a pile. CLIENT: the landed pile should sit at
   the SAME orientation as on the host (deterministic), NOT a random/"however" rotation.
3. **SOUND (check #2).** On the THROW: you should hear NO pickup `use click` (the throw is silent this build);
   the GRAB still plays its one pickup click. Listen on the CLIENT especially (that's where it doubled).
4. **Landing.** Clean morph to a pile at the authoritative spot, no double-pile, no hang.

## Read the logs (CLIENT `Game_0.9.0n_copy/.../votv-coop.log`)
- **`Select-String "use click"`** — should now be **~1 per grab cycle** (was 2). Count it across a few throws.
- **`Select-String "proxy throw"`** — should be **0** (was 29). The spurious release is gone.
- **`Select-String "GRAB-IN"`** — should be ~1 per grab (was 2: a `key='None'` then a `key=''`).
- HOST `Select-String "PropRelease SUPPRESSED"` — fires once per trash throw (confirms (b)).
- HOST `Select-String "RE-PILE.thunk."` — the land convert (rotation now from the pile).

## Acceptance
- **GREEN** = landed pile orientation matches the host + the throw is silent (no doubled pickup) + carry smooth +
  clean landing.
- **Rotation still wrong** (FAILURE) = the pile still lands rotated oddly → bring the host `RE-PILE(thunk)` lines
  and note whether it's yaw-only or fully random (tells us if `newActor`'s rotation at the thunk POST is
  pre-FinishSpawning garbage → then we re-read at the land-settle commit instead).
- **Pickup sound still on throw** (FAILURE) = bring the client `use click` + `GRAB-IN` + `proxy throw` counts
  across a few throws.

## Honest status
- Built CLEAN (Release, 0 errors), deployed `778BC35F706A16B7` all 4 copies (MATCH x4), proto v83 (no wire
  change). Adversarial audit was running at deploy. **NOT verified** — your hands-on is the test. Roots:
  `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.
- After your run, tell me: (1) pile rotation correct, (2) throw silent (no doubled pickup), (3) carry smooth,
  (4) clean landing. Then NEXT: throw whoosh cue (wire) · the ~4s FPS re-seed perf pass · the level-pile DESTROY
  fix (probe-greenlit at `1cm=1`).
