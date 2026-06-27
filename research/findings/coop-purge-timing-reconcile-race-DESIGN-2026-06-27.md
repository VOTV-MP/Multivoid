# Purge-timing reconcile race (the 09:54 ghost) — root + fix design (2026-06-27)

**Status: DESIGN (on review, NOT built).** Root-cause of the 09:54 hands-on regression (ghost piles + ghost
kerfur, interaction doesn't leak to host). **b3 is INNOCENT** (proven below); do NOT roll it back. This is a
pre-existing fragility ([[feedback-snapshot-before-state-ready]] / [[feedback-recurring-bug-is-architectural]] /
docs/piles/10) bigger than b3. Logs: `research/joinwindow_09-54_{host,client}_2026-06-27.log` (09:54 ghost) vs
`research/joinwindow_16-42_{host,client}_2026-06-26.log` (worked).

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
**Leaning option 1 (position-keyed re-bind), bounded by ambiguous-skip + the existing sweep**, because it needs
no load-order plumbing and reuses the deterministic-position fact — but this is exactly the call to make on build,
and it foreshadows the sync-module refactor (the bind/re-bind/position-key/ordinal all want to live behind one
coherent identity API). **Lever (a) is unaffected and ready** (a 1-2 line throttle-skip, fully understood).

### Why both
(b) is the root: the reconcile must bind the FINAL (post-re-seed) world, regardless of purge timing — this fixes
the ghost deterministically. (a) makes the purge fast so the user never sees a 45 s broken world AND shrinks the
window where a late purge can orphan the bind. (a) without (b) = fast but still races a late GC purge; (b) without
(a) = correct but the user still waits up to ~45 s for the slow drain. Together = fast + correct, matching 16:42.

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
