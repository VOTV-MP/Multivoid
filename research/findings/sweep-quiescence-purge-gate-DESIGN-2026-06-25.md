# Claim-sweep quiescence: purge-aware gate — DESIGN (2026-06-25, on-review, ZERO code)

**RULE-1 root fix for the 11:16 over-destroy RACE** (docs/piles/10, pinned from the 14:11 client log).
This is the TIMING layer; the per-class completeness floor (Phase 0, already built) stays as the NET.
Both layers ship; Phase 0 is VERIFIED only when both stand + the floor is proven on top of this fix.

---

## 1. The root this fixes (recap, one paragraph)

The claim sweep (`remote_prop_spawn.cpp` `RunDivergenceSweep_`) adjudicates the client's save-loaded world
against the host snapshot, dooming unclaimed local Prop Elements. It is released by a load-tail quiescence
gate (`CountLoadTailUnsettled_` + `TickClientReconcile`). That gate is **blind to the join-time world-reload
mass-purge**: it counts only allowlisted NPCs + keyless NON-pile props (chipPiles EXPLICITLY excluded,
`remote_prop_spawn.cpp:1329`) and never consults `InPurgeEpisode()` / the reaper backlog. So the sweep fires
at a RANDOM point relative to the native pile purge→reload cycle:
- fires AFTER the natives re-materialize LIVE + re-register as Prop Elements but BEFORE the host claims them
  → in-universe + unclaimed → **mass-doomed = 11:16 (953 unclaimed, 870 piles → 0)**;
- fires DURING the purge trough (natives dead/reaping, not yet re-registered) → ~88 survivors in-universe →
  nothing doomed → **14:11 / 13:21 "safe" — by luck, not design.**

The fix: make the gate wait until the registry reflects the FINAL reloaded world, so the sweep ALWAYS
adjudicates a complete world (then a genuine host under-express is the floor's job, deterministically).

## 2. The signals available (no new state needed)

All already owned by `coop::prop_element_tracker` (and written solely by `net_pump`'s reaper, RULE-2
one-owner):

| Signal | Meaning | Source |
|---|---|---|
| `InPurgeEpisode()` | a mass-purge (reaped ≥ `kReseedPurge`=64) is draining; the world-change re-seed is DEFERRED | set `net_pump.cpp:530`, cleared `:538` |
| `HasSeededOnce()` | the boot seed has run at least once (natives registered at least once) | `prop_element_tracker.cpp:776` |
| live chipPile actor count | the PHYSICAL pile population in the world right now (drops on purge, climbs on reload) | GUObjectArray walk (already done by the probe) |

**"Re-seed done" is NOT a separate signal to add.** The episode-end transition runs the re-seed
SYNCHRONOUSLY in the same `net_pump` Tick that clears the flag (`net_pump.cpp:538-539`:
`SetInPurgeEpisode(false); ReSeedKnownKeyedProps();`). The re-seed is one GUObjectArray walk + bulk
`MarkPropElement` — atomic from any other GT tick's view; there is no half-seeded state a later sweep probe
can observe. Therefore:

> **registry-world-coherent  ≡  `HasSeededOnce() && !InPurgeEpisode()`**

That single predicate is the "the registry reflects the current world (seeded, not mid-purge)" gate. (For
VOTV's single persistent `untitled_1` world `IsRegistrySeededForCurrentWorld()` is redundant here — it stays
TRUE through the trough, which is exactly why the take-3 gate never fired; do NOT rely on it. Listed only so
a future multi-world build knows where it would slot.)

## 3. The change — three edits, each located

### 3.1 `CountLoadTailUnsettled_` (`remote_prop_spawn.cpp:1308`) — count the chipPile population

Today the chipPile branch SKIPS (`:1329` `if (IsChipPile(obj)) continue;`). Change it to COUNT live,
non-CDO chipPile actors into `unsettled`. Effect: the purge (count drops 871→…) and the reload
(count climbs …→870) BOTH perturb the population, so the gate cannot quiesce through either half of the
reload — including the ≤4 s window where the purge has physically started but `InPurgeEpisode` is not yet
flagged (the reaper is on a 4 s throttle). This is the PHYSICAL-population half of the fix; it does not
depend on the flag's detection latency.

```
// (b') chipPile load tail — the pile field IS part of "the world finished loading".
//      Counting it here makes the purge-drop AND reload-climb perturb the population,
//      closing the pre-flag window. (Was excluded at :1329 because the probe used to
//      measure only the not-yet-keyed straggler tail; the sweep needs the full field.)
if (ue_wrap::prop::IsChipPile(obj)) {
    if (!R::IsLive(obj)) continue;
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
    ++unsettled;                 // <-- was `continue`
    continue;
}
```

Coupling to flag for the reviewer: `CountLoadTailUnsettled_` feeds BOTH the sweep and NPC adoption
(`HasLoadTailQuiesced`, probe doc `:1291`). Including piles makes NPC adoption ALSO wait for pile stability.
That is conservative + correct (piles and kerfur NPCs load in the same async pass), never a regression —
NPC adoption only fresh-spawns LATER. If we want to avoid touching NPC-adoption timing at all, the
alternative is a sweep-only pile sub-count in `TickClientReconcile` (second walk, ~costlier); recommended:
the shared count (one walk, benign coupling). Decision flagged for review.

### 3.2 `TickClientReconcile` (`remote_prop_spawn.cpp:1367`) — purge hard-gate + no-progress deadline

Insert the purge gate AFTER `deadlineHit` is computed (so the deadline can still fire on a stuck purge) and
BEFORE the population-stability check:

```
const bool deadlineHit = (now - g_sweepArmedAt) >= kSweepHardCapMs ||        // absolute ceiling (NEW)
                         (now - g_sweepLastProgressAt) >= kSweepDeadlineMs;  // no-progress (semantics change)
if (!deadlineHit) {
    // REGISTRY MID-PURGE -> the loaded world is incomplete; never adjudicate against it.
    if (!coop::prop_element_tracker::HasSeededOnce() ||
        coop::prop_element_tracker::InPurgeEpisode()) {
        g_sweepLastProgressAt = now;     // a draining purge IS progress -> hold off the no-progress deadline
        g_sweepLastUnsettledCount = -1;  // force a fresh stability run once the world re-seeds
        g_sweepStableScans = 0;
        return;                          // keep waiting (the absolute ceiling above still bounds it)
    }
    const int unsettled = CountLoadTailUnsettled_();
    if (unsettled != g_sweepLastUnsettledCount) {
        g_sweepLastUnsettledCount = unsettled;
        g_sweepLastProgressAt = now;     // population still moving = progress
        g_sweepStableScans = 0;
        return;
    }
    if (++g_sweepStableScans < kSweepQuiesceScans) return;
}
// gate satisfied (registry-coherent + population stable, OR a backstop deadline) -> fire the one real sweep
```

### 3.3 `ArmDivergenceSweep` (`remote_prop_spawn.cpp:1337`) — init the new progress clock

`g_sweepArmedAt = now;` already set (`:1358`). Add a sibling `g_sweepLastProgressAt = now;` so the
no-progress deadline measures from arm until the first progress/stall. No other arm-time capture needed —
the no-purge join (boot seed holds, population already stable) is allowed to quiesce immediately, so we must
NOT require a generation bump or a re-seed to have happened.

New constants near `:231`:
```
constexpr int kSweepDeadlineMs = 45000;   // unchanged VALUE, new SEMANTICS: NO-PROGRESS deadline
                                          // (resets on purge-active or population-change), not since-arm.
constexpr int kSweepHardCapMs  = 120000;  // NEW absolute ceiling from arm: fire no matter what (stuck-flag backstop)
```

## 4. Why the deadline MUST change (not just stay)

The current `kSweepDeadlineMs = 45000` is measured since-arm. The observed 14:11 drain ran arm(~14:11:21) →
re-seed-done(14:12:11) = **~50 s** — so a since-arm 45 s deadline would fire at ~14:12:06, INSIDE the
trough, before the re-seed. With the gate added but the deadline unchanged, the sweep would deadline-fire
mid-purge: harmless (nothing doomed) but it would NOT exercise the floor and Phase 0 could never be proven.
So the deadline becomes a **no-progress timer**: it resets whenever there is genuine progress (an active
purge episode, or a still-moving population). A 50 s legitimate drain keeps resetting it → no premature
fire → the sweep fires AFTER the re-seed when the population settles. A genuinely hung purge (flag stuck
true, no real progress) is bounded by the NEW **absolute ceiling** `kSweepHardCapMs = 120 s` from arm. The
backstop therefore STAYS (two-tier: no-progress 45 s + absolute 120 s); it is never removed — RULE-1 keeps
the worst-case bound, it just stops pre-empting a legitimate long drain.

## 5. Edge cases (the four the reviewer asked for)

1. **Purge NEVER completes (deadline).** The absolute ceiling `kSweepHardCapMs` (120 s from arm) fires the
   sweep unconditionally. If still mid-purge, `inClass` is small (only survivors), `doomed` is ~empty →
   harmless: the client keeps its loaded world; the host's PropDestroy stream still removes genuine
   deletions; the floor is dormant (nothing to keep) but costs nothing. Degrades to today's safe-trough
   outcome, now by DESIGN not luck.

2. **Partial re-seed (false-quiesce mid-way?).** Impossible to observe partial: the re-seed
   (`ReSeedKnownKeyedProps`) is one synchronous GUObjectArray walk + bulk `MarkPropElement` inside a single
   `net_pump` Tick; it completes before any other GT tick (incl. the sweep probe) runs. So
   `!InPurgeEpisode()` is a clean "registry re-seeded" edge — there is no half-registered window. The
   ASYNC actor-materialization tail (loadObjects still spawning natives AFTER the first re-seed) is covered
   by §3.1: the live chipPile count keeps climbing until the last native exists, holding the population
   unsettled; the steady 0.25 Hz re-seed (`net_pump.cpp:566`) registers those late natives, and the next
   sweep probe re-checks. The sweep fires only once BOTH the population is flat AND no episode is active.

3. **Multiple purge episodes back-to-back.** Each episode is detected/ended independently (the flag is an
   episode latch, not a cooldown — `net_pump.cpp:388`). While ANY episode is active the §3.2 hard-gate
   blocks + resets the no-progress clock. In the brief `!InPurgeEpisode()` gaps between episodes the §3.1
   chipPile population is STILL perturbed (each purge drops+climbs the count), so `g_sweepStableScans` never
   reaches `kSweepQuiesceScans` until the LAST episode finishes and the field stabilizes. The absolute
   ceiling bounds a pathological storm. No special multi-episode logic needed — the two gates compose.

4. **No purge at all (clean join).** `HasSeededOnce()` is true, `InPurgeEpisode()` is false, the boot-seeded
   natives stay live + the population is stable from the first probe → the sweep quiesces normally (10
   scans / ~2 s) and fires against the already-final world. The fix does NOT add latency to the common
   no-purge path. (If, in that clean join, the host genuinely under-expressed a class, the natives are
   live+unclaimed at this correct-timing sweep → the FLOOR keeps them. That is the floor's irreplaceable
   role, now reached deterministically.)

## 6. How this makes the floor deterministically provable (closes the PATH B gap)

With §3 in place the sweep ALWAYS fires against the fully-reloaded world (natives live + registered). Then
the existing dev flag `[dev] force_chippile_unclaim=1` (host skips all chipPile expression) leaves ~870
natives live + UNCLAIMED at sweep time → `doomed` carries them → the completeness floor MUST keep them
(`host census 870, claimed 0 < 870`) and log `completeness FLOOR kept K unclaimed 'actorChipPile_C'` with
the piles surviving. The "corrected injection" requested after 14:11 IS this timing fix — host-skip failed
before only because, without it, the sweep landed in the trough where host-skip is moot (no live natives to
leave unclaimed). Before/after: floor-disabled (`disable_completeness_floor=1`) over the SAME timing fix
wipes all 870 (proves the catastrophe is real + the floor is what prevents it).

## 7. Scope / non-goals / RULE check

- RULE 1: this is the ROOT (the sweep no longer adjudicates an incomplete world), not a filter/skip/suppress.
  The floor is the complementary NET for genuine under-express, not a crutch.
- RULE 2: no new flag/owner — reuses `InPurgeEpisode()`/`HasSeededOnce()` (net_pump-owned). The deadline
  SEMANTICS change replaces the since-arm timer (no parallel old+new timer).
- Modular: all edits are inside `remote_prop_spawn.cpp` (already 1466 LOC, > 800 soft cap). This change adds
  ~15 LOC; it does NOT justify the `claim_sweep.{h,cpp}` extraction on its own, but the extraction remains a
  queued separate refactor — flag at build time, do not bundle.
- Perf: no new walk (the chipPile branch rides the existing 5 Hz probe walk that runs ONLY while a sweep
  pends); two extra atomic reads (`InPurgeEpisode`/`HasSeededOnce`) per probe tick. COLD path.
- Files touched (build): `remote_prop_spawn.cpp` only. No header/API change (constants + internal gate).

## 8. Open decision for review

- §3.1 coupling: shared `CountLoadTailUnsettled_` (one walk, NPC adoption also waits for pile stability —
  recommended) vs sweep-only pile sub-count (second walk, isolates NPC-adoption timing). Recommend shared.
- §3.2/§4 deadline: two-tier (no-progress 45 s + absolute 120 s — recommended, adaptive) vs simply bumping
  the fixed since-arm deadline to ~90 s (simpler, brittle to bigger saves/slower disks). Recommend two-tier.
- Confirm `kSweepHardCapMs = 120 s` is acceptable as the absolute worst-case loading-screen-down-to-sweep
  bound (the curtain is already down at `Complete`; this only delays the invisible reconcile, not the
  player's control).
