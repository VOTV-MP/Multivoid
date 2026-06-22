> SUPERSEDED (2026-06-22) -- this take's work is FIXED + verified; see research/handson_runbook_2026-06-22_regression_and_harness.md + the canonical finding.

# Hands-on runbook — take-31 — pile-landing Z/height + LEVEL-PILE DUP destroy

**Deployed:** `votv-coop.dll` SHA **`01825BFA999F52D7`** to all 4 copies (host / copy / copy2 / dev) — verified
MATCH x4. Proto **v83** (UNCHANGED — receiver/host-stream/thunk-read only, no wire change). Build CLEAN
(Release, 0 errors). Adversarial audit PASS — it withdrew the wrong-actor-destroy concern (lineage filter
correct, `IsLiveByIndex` guards every deref, swap-pop sound, no double-consume) and its one CRITICAL was FOLDED
IN before this SHA: the destroy is now **gated on `g_claimTrackingActive`** (the join reconcile bracket), so it
never rebuilds the index or scans mid-gameplay — closing both the warm-path GUObjectArray-walk regression and a
derived-pile-near-a-level-native wrong-destroy window. **NOT autonomously smoked** — you are on the PC, this is
prepared ground and YOUR hands-on is the test ([[feedback-user-tests-claude-prepares-ground]]).

> Continues take-30 (`2026-06-22_throw_rotation_sound.md`): rotation FIXED [V], pickup-sound GONE [V]. This
> build fixes the two remaining items: the landed-pile **height (Z)** and the **level-pile DUP**.

## What this build changes (two independent fixes)

### #1 Z/HEIGHT — the thrown pile now sits at the host's height (sunk by design), not floating
**Root (reflection-proven, NOT a pivot mismatch):** the ToPile thunk read `loc` from `srcObj` (**the clump** —
a dirtball whose origin rests ~radius ABOVE the ground) while take-30 fixed only `rot` to read from `newActor`
(the pile). Applying the clump's elevated origin to the pile-proxy lifted it ~clump-radius, so it "climbed out
of the ground." `actorChipPile_C`'s StaticMeshComponent has NO relative-location offset (reflection-verified)
and the BP does no runtime `SetActorLocation`, so the "sink under ground" is purely the **mesh pivot** — which
the proxy SHARES (same `ResolvePileMesh` asset). **Fix:** read `loc` from `newActor` too (the pile's sphere-
trace ground anchor), symmetric to the rotation fix. The shared pivot then reproduces the sink exactly.
**Form affected: PILE only** (the ToPile thunk); clump-carry streams the clump's own loc → unaffected.

### #2 LEVEL-PILE DUP — destroy the coexisting native (probe-greenlit at 1cm=1)
**Root (probe-PROVEN, take-29: all 16 PILE-PROBE = `1cm=1`):** a level-placed chipPile gets a host eid + a
proxy, but the client's NATIVE level-loaded chipPile COEXISTS with the proxy = the visible dup (the divergence
sweep is blind to natives — they enter the Registry lazily). **Fix:** at a pile proxy-spawn, AFTER the proxy
spawns (no visible gap), destroy the co-located native via the existing build-once join index
(`EnsurePileBindIndex`): **EXACT ~1cm match** (the probe confirmed bit-exact load) + same chipType +
`IsLiveByIndex`. **Graceful on 0** (a DERIVED/gameplay pile has no native twin → skip). **Exact-or-skip on >1**
(a dense cluster with >1 native within 1cm is ambiguous → keep both, never destroy the wrong one). **NOT adopt**
(adopt reintroduces the BP self-morph/GC the proxy model avoids). The read-only PILE-PROBE is retired (RULE 2).

## The test — host grabs/throws near a cluster; client watches; also check level piles
1. **Z/HEIGHT (check #1).** Host throws a clump; it lands + morphs to a pile. CLIENT: the landed pile sits at
   the SAME height as the host (slightly sunk, by design) — NOT floating/standing-proud above the ground.
2. **LEVEL DUP (check #2).** Look at ORIGINAL (map-placed) piles on the CLIENT — there should now be exactly
   ONE pile per spot (the proxy), not two (native + proxy). Walk a dense pile cluster: no doubling.
3. **Derived piles (regression).** A pile you MADE by throwing should still appear once (no native twin to
   destroy → unaffected). No pile should VANISH that shouldn't.
4. **Carry/throw/rotation/sound (regression).** All of take-30 still holds: smooth carry, arc flies, correct
   rotation, silent throw, clean landing.

## Read the logs (CLIENT `Game_0.9.0n_copy/.../votv-coop.log`)
- **`Select-String "DESTROY native level-pile twin"`** — fires once per destroyed level-pile twin (throttled
  first-8 + every-200th). At join you should see a burst as level piles are de-duped.
- **`Select-String "pile-bind index built"`** — the one-time index build (count = the client's native chipPiles).
- **`Select-String "DESTROY SKIP"`** — a `>1 within 1cm` ambiguous cluster (kept both; a dup may persist there).
  If you see a dup that DIDN'T clear, grep this — it tells us the cluster was too tight for an exact match.
- HOST `Select-String "RE-PILE.thunk."` — the land convert (loc + rot now both from the pile).

## Acceptance
- **GREEN** = thrown pile sits at host height (sunk, not floating) + original/level piles show ONE pile each
  (no native+proxy doubling) + derived piles intact + take-30 regressions all hold.
- **Pile still floats** (FAILURE) = bring the host `RE-PILE(thunk)` lines; if Z is still off, the pile's
  Construction Script may re-place it post-spawn → we re-read at the land-settle commit.
- **Dup persists on some level piles** (FAILURE) = grep client `DESTROY SKIP` (ambiguous cluster) and
  `pile-bind index built` count vs the pile count; a late-loading native (built after the index) would also
  miss → tells us to rebuild/extend the index.
- **A pile WRONGLY vanished** (FAILURE, important) = a derived pile or a non-twin got destroyed → bring the
  `DESTROY native` lines near that spot; the 1cm+chipType gate should prevent it.

## Honest status
- Built CLEAN, deployed `01825BFA999F52D7` all 4 copies (MATCH x4), proto v83 (no wire change). Adversarial
  audit (wrong-actor-destroy + dangling-pointer focus) running at deploy. **NOT verified** — your hands-on is
  the test. `remote_prop_spawn.cpp` is at 1390 LOC (past the 800 soft cap, under the 1500 hard cap) — the
  pile-bind/destroy subsystem is flagged for extraction into its own file (follow-up).
- After your run: (1) thrown-pile height correct, (2) level dup gone, (3) no wrong vanish, (4) take-30 holds.
  Then NEXT: #2c whoosh-on-throw (wire) · #3 the ~4s FPS re-seed perf pass. Push to main on your go-ahead.
