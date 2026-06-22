> SUPERSEDED (2026-06-22) -- this take's work is FIXED + verified; see research/handson_runbook_2026-06-22_regression_and_harness.md + the canonical finding.

# Hands-on runbook — take-29 — throw ARC (carry/flight stream-continuity) + level-pile PILE-PROBE

**Deployed:** `votv-coop.dll` SHA **`c2a5f49cc98add31`** to all 4 copies (host / copy / copy2 / dev) — verified
MATCH x4. Proto **v83** (UNCHANGED — receiver/host-stream only, no wire change). Build CLEAN (Release, 0
errors). Audit PASS (zero CRITICAL — additive flight branch + read-only probe). **NOT autonomously smoked** —
you are on the PC, so this is prepared ground and YOUR hands-on is the test
([[feedback-user-tests-claude-prepares-ground]]).

> Continues take-28 (`2026-06-22_v83_scale_interp.md`): carry JANK fixed [V] (fixed-delay interp), derived-pile
> dup gone [V]. This build adds the **throw ARC** and a **read-only level-pile dup PROBE**.

## What this build changes (two independent things)

### 1. Throw ARC — carry/flight stream-CONTINUITY (the verb hunt is dead)
The release-VERB approach failed empirically: BOTH `simulateDrop` (`SIM-DROP=0`) AND its RE-pinned successor
`dropGrabObject` (`DROP-GRAB=0`) fired ZERO times for the chipPile clump release (the same `UFunction::Func`
facility's BeginDeferred thunk fired all run — so it's not the facility; the clump's release path uses neither
verb). The throws confirmed `|v|=0.0` (the land-settle closes the latch only AFTER the clump re-piled/died).

**Fix (`136ed779`): the arc needs NO verb, NO velocity, NO flip.** The host's thrown clump really flies
(physics) until it re-piles, so the carry pose-stream just CONTINUES through the release. In `local_streams.cpp`
the release-edge `!carrying`-SKIP branch now streams `g_lastHeldProp`'s pose under the SAME eid E while it is a
LIVE `garbageClump` (the post-release flight); the client's fixed-delay interp shows the REAL arc; it ends when
the clump re-piles (the re-pile thunk's ToPile re-skins + snaps the proxy to the landed spot). The
churn/flight discriminator is **`IsLive`** — a churn re-pile DESTROYS the clump (skip the gap), a real release
leaves it ALIVE + flying (stream the arc). The carry main branch (`heldActor`-keyed) is byte-identical
(additive — zero regression risk, audit-confirmed).

### 2. Level-pile dup PILE-PROBE (read-only — destroys NOTHING this build)
Root CONFIRMED (agent trace): the dup that remains is on ORIGINAL (level-placed) piles — the client's NATIVE
level-loaded chipPile COEXISTS with the proxy the host expresses on top (~871 native + 870 proxy); the
grab-convert re-skins the proxy, the native sits untouched = the visible dup. Derived (gameplay-born) piles
have no native twin → no dup. The probe (`remote_prop_spawn.cpp:355`, sampled first-12 + every-200th) logs, at
a pile proxy-spawn, the count of native `actorChipPile_C` near the host-expressed pos. **It only LOGS** — the
DESTROY fix is the NEXT build, gated on this run's numbers.

## The test — grab a pile near a cluster, carry, THROW (host grabs; client watches)
1. **Carry (regression check).** Host grabs + carries a clump ~10 s. On the CLIENT: still SMOOTH like a normal
   held object (the main branch is untouched — it must not regress).
2. **THROW (the MAIN check).** Host THROWS the clump. On the CLIENT: it should now **FLY AN ARC** mirroring the
   host's physics flight (via the interp), then morph to a pile on landing — **NOT** freeze + teleport.
3. **Landing.** The clump lands and morphs to a pile at the authoritative spot (ToPile re-skin + snap), no
   hang, no double-pile at the end of the arc.
4. **Original vs derived pile grab (the dup + the probe).** Grab an ORIGINAL (map-loaded) pile and a DERIVED
   (one you made by throwing) pile. The original may still DUP (expected this build — the destroy isn't built);
   the derived should not.

## Read the logs
- HOST `Select-String "carry/flight CONTINUE"` — fires during the flight (the arc is being streamed). Throttled.
- CLIENT `Select-String "drive #"` — the proxy drive should continue through the throw (the arc), not stop at
  release.
- CLIENT **`Select-String "PILE-PROBE"`** — `[PILE-PROBE] before SpawnProxy eid=.. within 1cm=N 10cm=M`.
  **Expect `1cm=1` for level piles** (the coexisting native = the dup) and **`0` for derived**. If `1cm=0` but
  `10cm=1` → the load drifted and the destroy-fix needs a looser tolerance; if `1cm=1` → bit-exact, the tight
  match is cluster-safe. **Bring these numbers.**

## Acceptance
- **GREEN throw** = the clump flies a visible arc on the client (mirrors the host) + lands/morphs cleanly +
  carry stayed smooth.
- **Not-flying** (FAILURE) = the clump still freezes + teleports → bring the host `carry/flight CONTINUE` lines
  (absent? then the flight branch didn't fire) + the client `drive #` cadence.
- **Carry regressed** (FAILURE, shouldn't happen) = the main branch is byte-identical → bring the carry `drive
  #` cadence.

## Honest status
- Built CLEAN (Release, 0 errors), deployed `c2a5f49cc98add31` all 4 copies (MATCH x4), proto v83, audit
  zero-CRITICAL. **NOT verified** — your hands-on is the test. Root + design:
  `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.
- After your run, tell me: (1) does the throw fly an arc (MAIN), (2) carry still smooth, (3) clean landing, (4)
  the `[PILE-PROBE]` 1cm/10cm numbers. Then: throw verified → retire the dead `dropGrabObject` thunk + build
  the level-pile DESTROY fix with the probe data.
