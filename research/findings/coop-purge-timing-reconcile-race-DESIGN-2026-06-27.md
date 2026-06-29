# Purge-timing reconcile race (the 09:54 ghost) — root + fix design (2026-06-27)

> **RESOLVED + PUSHED 2026-06-28 (origin/main `63aa4c01`).** The 09:54 re-seed-orphan ghost is FIXED (`ddaec1fa`)
> and PROVEN by a deterministic in-process self-test (`c6192d06`, VERDICT=PASS both peers — NOT a clean-join smoke).
> FINAL ROOT (refined from the design below): the reaper Take()s a purged bound save-native's Element but DEFERS
> ~Element -> the registry actor->eid reverse is STALE in that window -> the post-purge re-seed orphans the
> GC-churned native. FIX at net_pump's purge-drain re-seed edge: (a) ElementDeleter::Flush() before
> ReSeedKnownKeyedProps + (b) BindUnboundReCreatesByPosition() right after (re-bind by position immediately). See
> the PLAN doc's "09:54 RE-SEED-ORPHAN GHOST" section + [[project-sync-module-refactor-2026-06-27]]. The design below
> is the point-in-time 2026-06-27 thinking that led here.

**Status (2026-06-27, latest at top):**
- **Lever (a) reaper escalation — BUILT + VERIFIED** (commit `bfe9182a`; 11:32 real log: mass-purge->re-seed gap
  collapsed 37s->1s). KEEP.
- **(b) cursor-reset (option 3) — BUILT (`685c9662`) -> REFUTED at 12:29 hands-on (MIS-BIND) -> REVERTED
  (`86bca8cb`).** The re-create is a SPARSE engine-GC subset (~2 of 870), NOT a full array-order batch, so
  resetting the cursor bound the lone re-create to entry[0] = a foreign eid (the black pile). DEAD; §2.5/2.6 are
  the post-mortem (kept for the lesson, NOT the plan).
- **VARIANT 1 host-wire — BUILT (`54ee4b06`), then ABSORBED INTO THE SYNC-CONSOLIDATION REFACTOR (2026-06-27).**
  Host ships each eid's save-pos; client re-binds the sparse GC-churned natives by a 1cm position match. ORIGINALLY
  it ran ONLY in the join-window one-shot sweep, which (a) the >50% valve ABORT skipped on an incomplete snapshot
  and (b) fired DURING the purge (before re-creates land) -> N=0, never exercised. The refactor FIXED both: the
  identity reconcile now runs on the valve-abort path too AND has a post-purge steady trigger
  (`coop/sync/sync_reconcile`, commits `47384057`/`432183ce`/`8b85cb2e`). 15:44 hands-on: reconcile-on-valve-abort
  VERIFIED (variant-1 ran, log `position re-bind pass`); world ended CLEAN (870 piles, 0 orphans); variant-1 N=0 =
  no native left unbound = a pass. **CURRENT HOME: `research/findings/sync-consolidation-refactor-PLAN-2026-06-27.md`
  (step 4b+/7) + [[project-sync-module-refactor-2026-06-27]].** The `force_save_churn` synthetic probe is MOOT (the
  real purge pre-empts it) -- see the superseded runbook banner.
- **b3 is INNOCENT** (proven below); never rolled back.

This is a pre-existing fragility ([[feedback-snapshot-before-state-ready]] / [[feedback-recurring-bug-is-architectural]] /
docs/piles/10). Logs: `research/joinwindow_09-54_{host,client}_2026-06-27.log` (09:54 ghost) ·
`research/purgefix_12-29_{host,client}_2026-06-27.log` (12:29 cursor-reset mis-bind, the variant-3 refutation) ·
`research/purgeprobe_11-32_{host,client}_2026-06-27.log` (lever (a) confirmed).
**Read order: §2.7 (TRUE mechanism + no-wire refuted) -> §2.8 (variant-1 AS-BUILT). §1-§2.6 are the saga/post-mortem.**

## 1. The proven root (corrected — accurate this time)

A **same-world mass-purge + re-seed during the join** reaps the bound save-authoritative natives (chipPiles +
off-kerfurs) and re-creates NEW ones — and the same-world re-seed **deliberately does NOT re-announce / re-bind**
(the anti-dupe suppression, net_pump.cpp:518-526). The save-identity **bind is event-driven** per native load
(`save_identity_bind::OnKeylessLoadSpawn`, via trash_collect_sync.cpp:100); re-loaded placed props fire **no
catchable Init POST** (the reason the re-seed's MarkPropElement tracking exists at all), and the re-seed only
**tracks** the new natives — it never re-runs the **bind**. So after the re-seed the re-created natives are
**tracked but UNBOUND = GHOSTS** (no host mirror → interaction never leaks to the host). The divergence sweep
then sees ~870 unclaimed natives and its **>50% valve correctly aborts** the destroy ([[feedback-join-reconcile-sweep-safety]]),
leaving the ghosts standing.

### Why 16:42 worked and 09:54 didn't — pure ORDERING (engine-GC + drain-speed)
Both runs were same-world re-seed (both logged "NOT re-announcing"). The difference is **when the purge+re-seed
landed relative to the bind/reconcile arm**:

| | 16:42 (worked) | 09:54 (ghost) |
|---|---|---|
| world-ready / bind begins | 16:42:09 | 09:54:57 |
| mass-purge **drain** | **`cap 4096/call`, 2315 in ONE shot (~2s)** via `StaleIndexSelfHeal` (it overlapped the snapshot burst → triggered) | **`cap 256/call`, 256 every ~4s for 37s** (steady reaper only; purge hit AFTER the snapshot, so the fast self-heal never triggered) |
| re-seed completes | 16:42:15 | 09:55:43 |
| divergence sweep ARMED | 16:42:17 (**after** re-seed) | 09:54:59 (**before** purge) |
| sweep FIRES (on quiescence) | 16:42:19 → kerfur retire **1/1** | 09:55:45 → kerfur retire **0/1**, b3 corrections never APPLIED |

**The sweep fired CORRECTLY on quiescence in BOTH** (not the hard deadline — my first read said "45s hard
deadline"; that was wrong, the reason logged is "load tail quiesced"). At 16:42 the fast drain made the re-seed
land early, so the bind/reconcile settled on the **post-re-seed** world → everything matched. At 09:54 the slow
drain pushed the re-seed 37s late, **after** the bind/reconcile had already keyed on the pre-purge world → the
re-seed orphaned the bound natives → ghosts; the user waited 45s, saw a half-reconciled world, and closed at 48s.

### b3 is innocent (code + log)
b3 touches no reaper/GC/kerfur/bind code. Its only log actions: HOST sent 2 correct corrections (eids 4438/5273,
right positions — host-detect works), CLIENT armed both (client-arm works). It spawned/retired/bound nothing.
b3's apply never ran usefully because its target eids were purged+re-seeded under it (same root). Rolling back b3
would NOT fix the ghost (the purge orphans the bind with or without b3; 16:42 only worked by lucky fast-drain
timing). b3 is a **victim**, not a cause — it unblocks the moment the bind survives the re-seed.

## 2. Fix design — two levers (recommend BOTH; (b) is the correctness root, (a) makes it fast + shrinks the race)

### (a) Reaper escalation — drain a mass-purge backlog FAST (net_pump.cpp:360-461)
The steady reaper runs once per **4 s** (sNextReap throttle), reaping `kReapEvictCap = 256`/call; the episode-end
re-seed only runs when a scan reaps `< kReseedPurge (64)` (backlog drained). So a ~2300 backlog drains over
~9 scans × 4 s = **37 s**. The fast path (`StaleIndexSelfHeal`, prop_element_tracker.cpp:557-561, a
`4096`-cap × 8-pass loop) only triggers from the snapshot de-dupe on a stale-fallback — which a post-snapshot
purge misses.
- **Change:** when the reap **hit the cap** (`reaped >= kReapEvictCap` → backlog remains) OR `InPurgeEpisode()`
  is active, **skip the 4 s throttle** — set `sNextReap = reapNow` so the next tick reaps again immediately. The
  backlog then drains at frame cadence (256/frame ≈ 9 frames ≈ ~0.15 s) instead of 256/4 s. Per-call cost stays
  bounded at 256 (no big single-frame hitch); it runs under the join cover (hitches hidden). Episode-end re-seed
  then fires promptly → restores the 16:42 timing. Self-terminating (drains to < cap → throttle resumes).
- **Effect:** the user never waits 45 s; the re-seed lands early; the race window (re-seed-after-arm) shrinks to
  near-zero in the common case. But (a) alone is NOT sufficient — the engine GC fires the purge whenever it wants
  (09:55:06 here, after the bind armed), so a purge can still land after the bind. Hence (b).

### (b) Re-bind after a same-world re-seed — the correctness fix (net_pump.cpp:534-545)
When the episode-end re-seed re-creates the save-authoritative natives, the bind must be **re-run** against the
new natives, WITHOUT a full re-announce/re-snapshot (the anti-dupe suppression must stay).
- **Change:** at the same-world episode-end re-seed (net_pump.cpp:538-545, where `ReSeedKnownKeyedProps()` runs
  and `maybeReAnnounce()` decides NOT to re-announce), additionally invoke a **lightweight re-bind**: re-run
  `save_identity_bind` over the freshly-tracked keyless natives (chipPiles + off-kerfurs) using the **cached
  received ordinal map** (`SetReceivedMap` already holds it), re-mapping per-family ordinal → host eid (Build 3).
  Then **re-arm** the divergence sweep + re-arm the kerfur retire + re-apply the pending b3 corrections, so all
  reconcile adjudicates the post-re-seed world. (b3's `ApplyPendingPosCorrections` is already idempotent + retried
  — re-running it after the re-bind resolves the eids.)
- **Open design points (settle on review / during build):** (i) the exact re-bind entry — does
  `OnKeylessLoadSpawn` re-fire for re-seeded natives, or do we add a `save_identity_bind::RebindAllTracked()`
  that re-enumerates per-family ordinal? (ii) idempotency vs the already-bound survivors (natives whose actor
  survived the purge must NOT double-bind); (iii) ordering — re-bind must precede the divergence sweep's
  unclaimed-destroy so the re-seeded natives are claimed, not doomed.

### (b) COMPLICATION surfaced by the bind RE (2026-06-27, save_identity_bind.cpp read) — the re-bind KEY
`save_identity_bind` (Build 3) keys the bind on **SPAWN ORDER**: the k-th keyless spawn of a family binds to
that family's k-th map entry == saveSlot array index k (RE-proven: within-array spawn order == array index by
construction). `OnKeylessLoadSpawn` advances a per-family **cursor** (`g_chipCursor`/`g_kerfurCursor`); after the
initial bind the cursors sit at `fam.size()` (all consumed), so naively re-firing `OnKeylessLoadSpawn` overflows
(the `cursor >= fam.size()` guard → "NOT binding"). **After a re-seed the spawn order is GONE** — the re-seeded
natives exist as a SET, but a fresh `GUObjectArray` walk is in object-index order, NOT saveSlot/array-index order
(recycled slots), so the ordinal→native correspondence the bind relies on can't be re-derived from a walk. So a
`RebindAllTracked()` cannot simply re-walk + re-assign ordinals.
**Idempotency is fine** (`OnKeylessLoadSpawn` early-returns on `IsBoundMirrorNative(newActor)`; survivors stay
bound, only the new re-seeded natives need binding) — the hard part is purely the KEY for the new ones.
**Re-bind key options (decide on build):**
1. **Position-keyed re-bind** — both peers loaded the SAME save → a re-seeded native sits at its save position;
   the host eid's save-time position is known (`save_transfer` map / the host map). Bind the re-seeded native to
   the eid whose save position it matches (1cm). This is the pre-Build-3 position-adopt approach; its known
   weakness is co-located piles (>1 within 1cm → ambiguous). docs/piles: "piles deterministic → position is a
   key" — acceptable with the ambiguous-skip + the leftover handled by the divergence sweep.
2. **Re-seed preserves spawn order** — make `ReSeedKnownKeyedProps` (or a parallel pass) enumerate the re-loaded
   natives in load order and feed `OnKeylessLoadSpawn` (reset cursors first), IF the BP re-load loop is
   synchronous/deterministic like the initial load (it is — same `loadObjects` path). Needs verifying the
   re-seed sees them in array-index order (the re-load loop order), which the initial-load RE supports but the
   GUObjectArray walk does not preserve — so this likely needs a load-order capture, not a walk.
3. **Capture the ordinal at first bind** — store, per host eid at the initial bind, a STABLE re-find key (the
   save position is the only cross-peer-stable one for a keyless pile), then on re-seed re-bind by that key.
   (Converges with option 1.)
### (b) RE update 2026-06-27 (3 parallel RE traces) — a THIRD option, and option 1's hidden cost
Three read-only RE traces refined the fork. Summary:

- **Option 2 (ordinal re-derivation via the re-seed walk) is DEAD.** `ReSeedKnownKeyedProps` ->
  `SeedWalk_` (prop_element_tracker.cpp:606-619) enumerates in **GUObjectArray object-index order**, NOT saveSlot
  array order; for purge-recycled slots the two are unrelated. The ordinal was never stored — it was a transient
  consumed by the per-family cursor during the BP load loop. A walk cannot recover it. Proven, not reachable.

- **Option 1 (position-keyed re-bind) is moved-pile-SAFE but NOT free — it needs a WIRE EXTENSION.** The
  save<->save match IS the right identity shape and the user's moved-pile worry does NOT apply: the re-bind only
  assigns eid identity; the host's moved/current position is delivered SEPARATELY by eid via b3
  `ApplyPendingPosCorrections` AFTER the bind (pile_reconcile.cpp:398-409). BUT the **client has no per-eid
  save-time position to match against**: the sidecar `IdEntry` carries `{index, eid, family}` only
  (save_identity_map.h:37-41); `g_blobPileXforms` is **host-side, keyed by host eid, never transmitted**
  (save_transfer.cpp:116-117). So option 1 requires **adding an FVector to `IdEntry` + bumping the sidecar
  version**, host-populated from the byLoc data `BuildHostMap` already computes. Co-located piles (>1 within 1cm)
  -> ambiguous -> **skip** (safe: chipPile is doom-exempt from the divergence sweep, remote_prop_spawn.cpp:1491,
  and the >50% valve protects), leaving that one pile a ghost — the bounded residual.

- **Option 3 (cursor-reset — re-fire the PROVEN per-spawn bind) is NEW and, if a probe confirms it, STRICTLY
  BEST (zero wire change, reuses Build 3 ordinal, no position ambiguity).** The crux RE: the natives are
  re-created during the episode by the **game's own `loadObjects`/Load-Primitives BP loop deferred-spawning each
  pile** (docs/piles/README.md:81; the same-world "Load Primitives re-run" destroys+respawns the natives) — the
  **exact same deferred-spawn path the original bind catches** at `OnBeginDeferredSpawnObserve`
  (trash_collect_sync.cpp:79-105, a `BeginDeferredActorSpawnFromClass` Func-patch; save_identity_bind.h:7-9
  "1A-proved to catch every keyless load-spawn"). That seam has **no `InPurgeEpisode` gate** — so it fires for
  each re-created native. The 09:54 ghost is then precisely explained: in a LATE purge the per-family cursors are
  already at `fam.size()` (consumed by the first load's bind), so the seam's `OnKeylessLoadSpawn` re-fires but
  hits the overflow guard (`cursor >= size` -> "NOT binding") -> the re-spawns don't bind -> ghosts. **Fix:
  RESET the per-family cursors (g_chipCursor/g_kerfurCursor = 0, map NOT cleared) at purge-episode START so the
  existing seam re-consumes the re-spawned natives in array order** — the identical Build-3 ordinal bind, no
  wire change, no position match. `IsBoundMirrorNative` (save_identity_bind.cpp:182) keeps purge-surviving
  bound natives from double-binding.

  **The ONE unproven assumption (the probe target, per [[feedback-probe-dont-guess-rule]]):** does the
  cursor-reset land BEFORE the first re-spawn fires the seam? The reaper detects the episode on a TICK *after*
  the purge began; if Load Primitives destroys+respawns within the same BP burst, some re-spawns could fire the
  seam before our reset. **Read-only probe:** a `[BIND-PROBE]` log in `OnBeginDeferredSpawnObserve`
  (trash_collect_sync.cpp ~:94) gated on `InPurgeEpisode()`, logging fam + the live cursor value per fire. Run
  the 09:54 repro. **If `[BIND-PROBE]` lines appear during the episode with cursors that a reset would re-consume
  -> option 3 confirmed (cursor-reset, zero wire change). If zero fires (or re-spawns precede any possible reset)
  -> fall back to option 1 (position wire-extension).** The probe is read-only diagnosis (RULE-2-exempt) and
  decides the whole fork; bundle it with lever (a)'s deploy so the next hands-on settles it.

**Recommendation: PROBE option 3 before committing to a design** — it is the most-native, lowest-cost fix IF the
ordering holds, and only a runtime measurement can confirm the ordering (the head-freeze clamp was REFUTED by
exactly this kind of probe; do not guess). Option 1 is the proven fallback if the probe refutes option 3.
**Lever (a) is unaffected, built + committed (`bfe9182a`), and independently useful** — its fast drain also
shrinks the window in which a re-spawn could precede the reset, helping option 3.

### Why both
(b) is the root: the reconcile must bind the FINAL (post-re-seed) world, regardless of purge timing — this fixes
the ghost deterministically. (a) makes the purge fast so the user never sees a 45 s broken world AND shrinks the
window where a late purge can orphan the bind. (a) without (b) = fast but still races a late GC purge; (b) without
(a) = correct but the user still waits up to ~45 s for the slow drain. Together = fast + correct, matching 16:42.

## 2.5 PROBE RESULT (11:32 hands-on, 2026-06-27) — VERDICT: OPTION 3, with one design refinement

Logs: `research/purgeprobe_11-32_{host,client}_2026-06-27.log` (deployed SHA 258A8F0D = lever (a) + [BIND-PROBE]).
Client timeline: bind ARMED 11:31:49 (874 map, cursors->870/4) · mass-purge detected 11:32:01 · **re-seed added 2999
the SAME SECOND** · kerfurOff `keyless spawn beyond` 11:32:04 (+ kerfur retire 1/1, census 0 unclaimed) · chipPile
`keyless spawn beyond` 11:32:19 · divergence sweep fired 11:32:04 · b3 armed eid 4436 but NEVER applied.

**(a) CONFIRMED working:** the mass-purge -> re-seed gap collapsed from **37 s (09:54) to <1 s (11:32)**. The
reaper escalation drains the backlog at frame cadence as designed. KEEP it.

**(a) does NOT fix the pile ghost (as designed):** the chipPile `keyless spawn beyond the mapped 870` at 11:32:19
proves the re-created piles still hit the exhausted cursor and DON'T bind = ghosts. (a) is timing; (b) is the
correctness fix — confirmed.

**[BIND-PROBE] logged ZERO — but NOT because the seam is silent.** Lever (a) collapsed the `InPurgeEpisode`
window to <1 s (re-seed completes the same second the purge is detected), while the re-created natives fire the
bind seam over the NEXT ~18 s (kerfur :04, chip :19 — the Load-Primitives respawn TRICKLES in, it is not
instantaneous). So the `InPurgeEpisode`-gated probe never overlapped the re-spawns. **The probe and the fix it
shipped with interfered.** Honest miss in the probe gate; salvaged below.

**The question is ANSWERED by the pre-existing overflow instrumentation:** the `keyless spawn beyond the mapped`
lines (save_identity_bind.cpp:196) ARE `OnKeylessLoadSpawn` firing for re-created natives. That branch sits
**after** the `IsBoundMirrorNative` survivor early-return (line 182), so a native reaching it is a **fresh
actor with the cursor exhausted** — i.e. `survivor=0 exhausted=1`. **This is option 3's signature: the re-create
DOES go through the bind seam, fresh, with the cursor consumed by the first load.** => **VERDICT: OPTION 3
(cursor-reset). Option 1 (wire-extension) is NOT needed. The seam fires; no position channel required.**

### Option-3 design (refined by the 11:32 evidence) — all ZERO wire change
1. **At mass-purge episode-START** (net_pump.cpp:530, `SetInPurgeEpisode(true)`), call a new
   `save_identity_bind::ResetForReseed()` that (i) resets the per-family cursors (`g_chipCursor=g_kerfurCursor=0`,
   map + entries untouched) AND (ii) clears the bound-mirror flags of the purged natives. The cursor-reset lets
   the fresh exhausted-cursor re-creates (proven at :04/:19) rebind via the proven Build-3 ordinal. The
   flag-clear covers the un-ruled-out **silent** case: a re-create landing at a RECYCLED address of a
   just-destroyed bound native would hit `IsBoundMirrorNative==true` and early-return (NO log — invisible in the
   11:32 run), so clearing the flags ensures those rebind too. The 11:32 log proves the fresh half directly and
   cannot rule out the recycled half, so the robust fix does BOTH (cheap, safe: at a mass purge ALL bound natives
   are in the purge, so clearing all bound flags orphans nothing).
2. **Re-arm AFTER the re-create TRICKLE finishes, not at the early quiescence.** 11:32 shows the divergence sweep
   fired at :04 while chip re-creates were still spawning until :19, and b3 (eid 4436) armed but never applied
   because its target was mid-re-create. So the re-bind + divergence sweep + kerfur-retire + b3
   `ApplyPendingPosCorrections` must adjudicate the world AFTER the Load-Primitives respawn settles (the
   quiescence gate must not declare "load tail quiesced" while keyless re-spawns are still arriving — gate it on
   the per-family spawn count reaching the mapped count, or on a no-new-keyless-spawn dwell). This is the
   `[[feedback-snapshot-before-state-ready]]` class again: the sweep snapshotted before the re-create was ready.
3. **Array-order correctness:** the re-create is the same Load-Primitives loop (objectsData then primitivesData),
   so within each family the k-th re-spawn == array index k — the cursor-reset rebind maps correctly (the same
   invariant Build 3 relies on). The :04-kerfur-before-:19-chip ordering matches objectsData-before-primitivesData.

**Open points for the (b) design doc (settle on review):** the exact re-arm trigger (spawn-count-complete vs
dwell), whether `ResetForReseed` clears the bind tallies (cosmetic) too, and whether the [BIND-PROBE] gate should
widen (drop `InPurgeEpisode`, gate on `armed && cursor>=size`) for a clean confirmation run — likely unnecessary
since the overflow lines already prove it.

## 2.6 12:29 HANDS-ON: VARIANT 3 REFUTED (mis-bind) -> fall back to VARIANT 1 (position-key)

Logs: `research/purgefix_12-29_{host,client}_2026-06-27.log` (deployed SHA 2663BD72 = lever (a) + (b) cursor-reset).
Symptoms: (1) client turns ONE kerfur on -> TWO activate (kerfurs sharing identity); (2) client grabs a LOCAL pile
-> it turns BLACK / wrong type (bound to a FOREIGN eid). Both = mis-bind.

**Proven root -- the re-create is a SPARSE SUBSET, not a full array-order batch:**
- 12:30:05 first load bound IN ORDER: k=0->eid 4435, k=1->4434, k=2->4433 ... (cursors 870/4, 874 bound).
- 12:30:09 mass-purge -> RESET-FOR-RESEED (cursors->0, ALL 874 bound-mirror flags cleared) + sweep re-arm.
- 12:30:14 **exactly ONE** chipPile re-fired the seam: `[BIND-PROBE] re-create #1 cursor=0/870` -> `BOUND k=0 ->
  eid 4435 [case(i) fresh mirror]`. The lone re-create bound to **entry[0]=4435** because the cursor was reset to 0.
- 12:30:25 `SETTLED chip 1/870 kerfur 0/4 (no-progress-dwell)`; `BIND SUMMARY bound 875/874, cursors chip=1 kerfur=0`.
- `re-seed found 871 keyless chipPile, added 0 NEW` = the 869 other piles + 4 kerfurs **SURVIVED** (re-tracked,
  not re-spawned); they kept their correct first-load binds.

So **Part 3 (k-th re-spawn == array index k) is FALSE for the re-create**: only a SPARSE subset re-creates (1 of
870 here; 2 at 09:54/11:32 -- the `overflow=2` I misread as "bulk fires"), so resetting the cursor to 0 binds the
k-th re-spawn to entry[k] -- but the k-th re-spawn is an arbitrary array index, so it gets the WRONG eid (the lone
re-create -> 4435 -> a foreign identity = the black pile). The whole of variant 3 stood on this unproven invariant
(the probe couldn't confirm it -- lever (a) broke the gate); 12:29 disproves it. **Variant 3 REFUTED.**

**Bonus damage:** ResetForReseed cleared ALL 874 bound-mirror flags though ~1 native re-created -- the 869+4
survivors lost flags needlessly; the kerfur cursor stayed 0 (no kerfur re-bind), so the two-kerfurs-one-input is
the flag-clear side effect, not a cursor mis-bind. Reverting (b) removes it.

**VERDICT: revert (b)'s cursor-reset + flag-clear + reseed gate + sweep re-arm (parts 1+2); KEEP lever (a)
(independent, drain 37s->1s, confirmed 11:32); design VARIANT 1 (position-key), which is ORDER- and
COUNT-independent so a sparse subset re-creating is fine.**

### Variant 1 direction (design on review, NOT built)
The bulk (869/870) SURVIVE with correct binds; only the sparse re-creates (the OLD overflow-drop path = the
09:54/11:32 ghosts) need re-binding. So: on a **past-cursor (re-create) seam fire** -- the existing
`cursor >= fam.size()` overflow branch in OnKeylessLoadSpawn -- instead of DROPPING it, **position-match the
re-create to its correct eid and bind** (no cursor reset, no flag clear, surviving binds untouched). Each
re-create matches its OWN save position -> its OWN eid, regardless of order/count.
- **Enabler (eid -> save position):** capturable **client-side at first-load bind** (store
  `g_eidSavePos[eid] = GetActorLocation(native)` in BindLocalNativeToHostEid_, since the first-load native sits
  at its save position) -- so likely **NO wire-extension needed** (better than the originally-scoped +FVector on
  IdEntry; settle on review). A moved-in-window pile's re-create is at SAVE pos on the client (it doesn't know the
  host moved it) -> matches save-pos -> correct eid; b3 delivers the moved position separately.
- **Co-located ambiguity** (>1 pile within ~1cm of the same save pos): ambiguous -> skip (the rare residual stays
  a ghost; the divergence sweep's chipPile-exemption keeps it from being wrongly destroyed). This is the same
  weakness Build 3 left position-adopt for -- acceptable for the sparse re-create minority.
- Open design points (settle on review): exact match tolerance, whether to ALSO handle the kerfur family by
  position (only 4, co-location unlikely), and whether the b3 PendingPosCorrection apply still works (it resolves
  by eid via the registry -- unaffected, since the re-create now binds to the correct eid).

## 2.7 RE before variant-1 design (2 parallel traces) -- TRUE mechanism + the no-wire enabler REFUTED

Per "don't build on an unverified invariant" (we were burned twice), two read-only RE traces ran before designing
variant 1. Both confirmed by the 12:29 logs.

### TRUE mechanism (the "purge re-creates all 870" model is FALSE)
- The 870 save chipPile natives are **NOT mass-destroyed**. The keyless-chipPile element count holds **flat**
  (870->869->868 across the episode, client:24658/24770/25423); the 868 survivors keep their first-load addresses
  + correct binds. The mass-purge reaped **dead tracker elements + dying/menu-shadow objects** (the "reaped 256 x9
  + 3219 dying"), not the live save piles.
- Only a **SPARSE handful** (here exactly 2: eid 4434 + 4435) are **client-local engine-GC'd and re-instantiated
  at their save position** by UE's incremental GC under the join load. They re-spawn via
  `BeginDeferredActorSpawnFromClass` -> fire the bind seam fresh (`case(i) E free`, the prior mirror GC'd). Gated
  to a subset purely because the seam fires only on a REAL re-spawn.
- **Move-UNCORRELATED + client-only:** the host had NO purge (host:11628). The b3 host-moved piles (4436/5271) are
  handled by `trash_proxy` SPAWN (AStaticMeshActor proxies) + the convert channel, and **never native-re-create**;
  the sparse native churn (4434/4435) is a separate, GC-timed, client-local phenomenon. So the re-creates are
  NOT the moved piles.
- Why (b) cursor-reset mis-bound (mechanism-level): the ~2 re-spawns consume the reset cursor in **GC order**,
  whose rank != saveSlot array index -> the lone re-create took `g_chipEntries[0]`=eid 4435 = a foreign identity
  (the black pile). Confirmed.

### The no-wire enabler is REFUTED (critical -- kills my proposed shortcut)
The variant-1 "capture `eid->save-pos` client-side at first-load bind" idea (doc 2.6) is **FALSE at that point**:
`BindLocalNativeToHostEid_` runs INSIDE the BeginDeferred POST-hook (trash_collect_sync.cpp:79->100->
save_identity_bind.cpp:252), which is **PRE-FinishSpawningActor -- the transform is not set**, so
`GetActorLocation(native)` there is **(0,0,0)** (code-pinned trash_collect_sync.cpp:112-121, the take-31 finding
b3 already relies on). Capturing at bind captures garbage.
- Earliest reliable client-side position is **quiescence** (pile_reconcile already reads native positions there:
  EnsureIndex:110, SweepReconcileSaveTimeTwins:337) -- but the **churned eids have no survivor at quiescence** to
  capture from, and a pre-churn periodic capture is GC-timing-fragile (the churn at 12:30:09 precedes the first
  quiescence at 12:30:25). So a robust client-only eid->save-pos source does NOT exist.
- The client has **no** blob-position store (g_blobPileXforms is host-only, save_transfer.cpp:116, host
  OnRequest:445). pile_reconcile's twin machinery (ArmPendingSaveTimeTwin/SweepReconcileSaveTimeTwins) does
  position-twin **DESTROY** (1cm exact + ambiguous-skip + >50% valve), NOT a position->eid BIND -- so it can't be
  reused as-is.

### Variant-1 design (REVISED to robust HOST-WIRE; on review, NOT built)
**Revert (b)** (cursor-reset + flag-clear + reseed gate + sweep re-arm). **Keep lever (a).** Then:
- **Host sends `eid -> save-position`** alongside the identity map (extend the sidecar `IdEntry` with an FVector,
  or a parallel array; ~+12B x 870 ~= 10 KB, trivial). The host has the authoritative positions
  (`g_blobPileXforms`). This is the originally-scoped variant 1 -- ROBUST: order-, count-, AND timing-independent
  (no GC-timing dependency, no transform-unset problem), authoritative from the save.
- **Client, at quiescence:** for each UNBOUND chipPile native (a sparse re-create), match its (now valid) position
  against the received `eid->save-pos` (1cm exact, the proven bit-exact save round-trip) -> bind it to that host
  eid. Co-located (>1 within 1cm) -> ambiguous-skip (the rare residual stays a ghost; chipPile doom-exemption
  prevents wrong-destroy). The bulk survivors are untouched (already bound). b3 PendingPosCorrection still resolves
  by eid post-bind. A moved-in-window pile that churned would re-create at its SAVE pos on the client -> matches
  save-pos -> correct eid; b3 delivers the moved pos separately.
- Why host-wire over the no-wire capture: the no-wire capture is REFUTED at bind (transform unset) and
  timing-fragile elsewhere (churn precedes quiescence); after being burned twice on timing/order assumptions, the
  authoritative host-wire removes the entire class of fragility. The wire cost is trivial.
- Open points (settle on build): exact sidecar layout (FVector in IdEntry vs parallel array + version bump); the
  quiescence match step (new code, or graft onto SweepReconcileSaveTimeTwins's read+match but BIND instead of
  destroy); kerfur family (only 4 -- position or leave to its own retire path); the 1cm tolerance + co-located
  policy (reuse pile_reconcile's kExactMatchR2Cm + ambiguous-skip).

(The "bind at BeginDeferred = (0,0,0)" fact is already load-bearing in the codebase (b3 design); a 1-line
read-only `GetActorLocation` probe at the top of BindLocalNativeToHostEid_ would make it probe-certain if desired,
but it is well-established -- not a new unverified invariant.)

## 2.8 VARIANT 1 host-wire -- AS-BUILT (`54ee4b06`, deployed `D54AB5B8`, audit SHIP, HANDS-ON PENDING)
(Clean base = `86bca8cb` ((b) reverted, (a) kept). The design below is what was built; integration points were
code-verified before writing, not assumed. Kerfur INCLUDED (off-prop is a save-placed actor that can GC-churn).)

**Goal:** re-bind the sparse (~2/join) client-local engine-GC-churned chipPile natives -- which re-create at their
save position, UNBOUND (= the 09:54/11:32 ghosts) -- to their correct host eid, ORDER/COUNT/TIMING-independent.
The cursor bind stays for the bulk first load (proven 874/874); this ADDS a quiescence safety-net for the
re-creates the cursor missed. No cursor-reset, no flag-clear (reverted).

### Why host-wire (recap): no robust client-only `eid->save-pos` exists
The bind seam is pre-FinishSpawning -> GetActorLocation=(0,0,0); the churned eids have no survivor at quiescence
to capture from; a pre-churn capture is GC-timing-fragile. So the host -- which holds the authoritative save
positions -- sends them. Order/count/timing-independent, no transform-unset problem.

### HOST side -- send `eid -> save-position` in the existing sidecar (verified integration points)
1. **`IdEntry`** (save_identity_map.h:37, currently `{uint32 index; uint32 eid; uint8 family}`): add `float
   savePosX, savePosY, savePosZ` (the save-time world location).
2. **`BuildHostMap`** (save_identity_map.cpp:80) already builds the location->eid byLoc bridge from
   `CollectTrackedPileTransforms` (:99) -- the save-time location for each eid is ALREADY in hand at emit time.
   Populate `IdEntry.savePos` from it (zero extra work; same value the host-local join already uses).
3. **`SerializeSidecar`/`DeserializeSidecar`** (save_identity_map.cpp:215/229): bump `kSidecarVersion` 1->2;
   append/read the 3 floats per entry. The deser already rejects a version mismatch (:237) -> an
   old-version peer fails the parse gracefully (bind stays in its current cursor-only mode = today's behavior,
   no crash). Both peers on the new mod version -> full fix. (Mod-version handshake already gates compatibility.)
   Wire cost: +12 B x ~870 ~= 10 KB once per join -- negligible.

### CLIENT side -- position-match the UNBOUND re-creates at quiescence
1. **`SetReceivedMap`** (save_identity_bind.cpp): the map now carries `savePos` per entry -> build an
   `eid -> savePos` lookup (and a `savePos -> eid` acceleration structure, e.g. the same quantized-location
   bucket BuildHostMap uses, for the match).
2. **At quiescence -- graft onto `SweepReconcileSaveTimeTwins`** (pile_reconcile.cpp:321), which already does a
   FRESH GC-robust GUObjectArray walk of live chipPile natives + a 1cm exact match + ambiguous-skip + a >50%
   valve. ADD a step: for each live chipPile native that is **UNBOUND** (`!IsBoundMirrorNative` AND not already an
   eid mirror -- i.e. a sparse re-create / orphan), read its (now valid, post-FinishSpawning) position, match it
   against the received `savePos->eid` (1cm exact = `kExactMatchR2Cm`), and on a UNIQUE match **BIND it** to that
   host eid via the existing terminal op (`save_identity_bind::BindLocalNativeToHostEid_` / `RegisterPropMirror`).
   - `matchCount > 1` (co-located within 1cm) -> **ambiguous-skip** (the rare residual stays a ghost; chipPile
     doom-exemption prevents wrong-destroy). Same policy the twin sweep already uses.
   - This is a position-match -> **BIND** (vs the twin sweep's position-match -> destroy); it's a sibling action
     on the same walk, not a replacement.

### Why this is correct + robust (the invariants, each checked)
- **Bulk survivors untouched:** they are `IsBoundMirrorNative==true` -> the unbound-only filter skips them; their
  cursor binds stand (proven 874/874). The position-match only ever touches the sparse unbound re-creates.
- **No cursor conflict:** after the first load the cursor is exhausted, so re-creates never cursor-bind (they
  overflowed = were dropped); only the position-match handles them. A native can't be both cursor-bound and
  position-matched (the filter excludes bound mirrors).
- **Moved-in-window pile that churned:** the client re-creates it at its SAVE position (it never knew the host
  moved it) -> matches the host-sent SAVE position -> correct eid; b3 `ApplyPendingPosCorrections` (by eid)
  delivers the moved position separately at the same quiescence. (At 12:29 the churned piles were NOT moved --
  this just covers the case.)
- **Timing-independent:** the host sends ALL eid->savePos at join, before any churn; the client matches whenever
  the re-create appears (at quiescence, position valid). No dependency on capturing before the GC churn, no
  transform-unset window. This removes the entire fragility class that bit cursor-reset + the no-wire capture.
- **Kerfur family (4 off-kerfurs):** OPEN -- include in the position-match for symmetry, OR leave to the existing
  kerfur retire/adopt path (the 12:29 kerfur-double was the (b) flag-clear, now reverted, so kerfurs may already
  be fine). Decide on build; positions are sent for kerfurs too (CollectTrackedKerfurTransforms exists), so
  including them is cheap.

### Scope / proportionality
The bug is small (~2 ghost piles/join, GC-timed) -- but it's the sync ETALON, and host-wire is the proportional
robust fix: a trivial sidecar field + a graft onto an existing quiescence walk, no new subsystem. The alternative
(a per-re-create host request/response) is lighter on the wire (~2 lookups vs +10 KB) but adds a round-trip
protocol + async wait -- more complexity for less robustness; the bulk-send is simpler and one-shot. Recommend
bulk-send; flag request/response as the lighter-wire alternative if the +10 KB/join is ever a concern.

### Build order (after review)
(1) host IdEntry+savePos + sidecar v2 (serialize/deser); (2) client SetReceivedMap savePos lookup; (3) the
quiescence unbound-native position-match->bind in SweepReconcileSaveTimeTwins; (4) decide kerfur inclusion;
(5) audit (hot-path: the match is one extra read per unbound native on an existing cold quiescence walk -- O(few));
(6) hands-on (no ghost, no mis-bind, b3 applies, kerfur ok, #1/#2/b2/clean-join/grab/throw unregressed).

## 3. b3 stays (unblocked by the fix)
b3's host-detect + client-arm are log-verified correct; only its apply is unverified (blocked by the purge). With
(b) the bind survives the re-seed → the sweep adjudicates a stable world → b3 corrections + kerfur retire + mirror
reveal all apply. Do NOT roll back b3.

## 4. Verify gate (after build)
Reproduce a slow/late purge join (like 09:54 — move piles EARLY + turn a kerfur on, join). Expect: the purge
drains fast (no 37 s 256/4 s crawl — a `[reaper]` fast-drain marker), the re-seed re-binds (a re-bind log line,
bound N/N on the post-re-seed natives), the sweep fires on quiescence in ~10 s, NO ghost piles/kerfur (interaction
leaks to host), b3 corrections APPLIED (drift log), kerfur retire matches. #1/#2/b2 unregressed; clean-join
unaffected.

## Cited source map
net_pump.cpp:360-461 (steady reaper, 256-cap, 4 s throttle) · :528-545 (purge-episode detect + episode-end
re-seed + same-world re-announce suppression) · prop_element_tracker.cpp:557-561 (StaleIndexSelfHeal 4096-loop) ·
:998 (256-cap reap) · :604 g_inPurgeEpisode (SetInPurgeEpisode/InPurgeEpisode) · save_identity_bind.cpp:152
(SetReceivedMap — the cached ordinal map) · trash_collect_sync.cpp:100 (OnKeylessLoadSpawn — the per-native bind
seam) · remote_prop_spawn.cpp:1412-1477 (TickClientReconcile — sweep gate, fires on quiescence; 1429-1431 the
deadline logic, NOT the cause here) · :1380 ArmDivergenceSweep.
