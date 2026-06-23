# L5 persistent ~2s FPS hitch — ROOT PROVEN (data): per-channel full GUObjectArray walk every 2s — 2026-06-23

**Status: ROOT PROVEN by data (autonomous perf run). Fix ATTEMPTED TWICE, BOTH FAIL the N-match gate (door
detection regresses) -- NOT verified, NOT committed (working-tree only). Deployed `FD9D2DC...` (take-2).
A design fork is pending the user's call. See "## FIX ATTEMPTS (take-1 regressed, take-2 partial)" below.**
This is the L5 the user reported for weeks (a periodic FPS stutter all-game, both peers). FOUR prior blind
walk-fixes missed it because they targeted the WRONG walks (reseed 20s + census 4s); this is a different,
never-instrumented set of walks. **Second finding this session:** interactable is only HALF the steady
hitch -- with it silenced, `device_occupancy` (count 80, avg 30ms) is the co-dominant bucket cost (its Tick
early-returns, so 30ms is suspicious -- its own pass). So fixing interactable alone HALVES the hitch
([HITCH] 38->16 post-settle), not eliminates it.

## How it was found (data-led, no blind fix — the methodology the user mandated)
1. **`[HITCH]`/`[HITCH-SRC]` (always-on probe).** Steady-state, BOTH peers: a `[HITCH] frame=46-67ms` every
   ~2s, and EVERY one carries a `[HITCH-SRC] net_pump::Tick=37-57ms`. The probe's own discriminator (a bare
   `[HITCH]` with NO `[HITCH-SRC]` = engine-side GC) **never** fired in steady state. -> NOT an independent
   engine GC. **GC hypothesis refuted (1).**
2. **`perf_probe` buckets.** The ~45-100ms is FULLY accounted by ONE bucket: `interactable` tracks
   `netPumpTick` at ~95% (22:22:59: netPumpTick=101.9/s, interactable=99.6/s; off-seconds both ~2-3/s). A GC
   inside Tick would leave the time UNACCOUNTED. It's accounted -> **our CPU, not GC. GC refuted (2).** But
   `interactable` is a CATCH-ALL bucket for ~30 `*_sync::Tick()` calls -> named the BLOCK, not the member.
3. **Per-sync `ScopedWalkTimer`** (the L5-purpose-built `[WALK-TIME]` profiler), wrapped on each of the ~30
   syncs. One run named the culprit (cheap no-op syncs stay silent):

   | sync | host (count/avg/max) | client (count/avg/max) |
   |---|---|---|
   | `sync:interactable` | 153 / 24ms / 62ms | 58 / 38ms / 76ms |
   | `sync:device_occupancy` | 80 / 20ms / 57ms | 55 / 18ms / 56ms |
   | `sync:npc_client` | — | 15 / 28ms / **110ms** |

   One-shot entries (console_state 46ms×1, sleep/keypad/window/power ×1) = join/world-load, not the steady hitch.

## ROOT (proven)
**`interactable_sync::Tick()` fans out to 6 channels (door/light/container/garage/appliance/doorbox). Each
channel's `RebuildIndex()` walks the FULL GUObjectArray (`R::NumObjects()` ~237k, interactable_channel.h:469)
to find its interactables, on a throttle of `kRetryRebuildThrottle = std::chrono::seconds(2)`
(interactable_channel.h:53,401).** When the 2s throttle fires, all 6 channels re-walk -> ~15-21ms steady
(up to ~100ms during load) every 2s = the felt hitch. The per-tick `PollAndBroadcast()` over the small built
index is cheap (<1ms, never logs) -- the throttled FULL walk is the entire cost.

- **The 2s period = `kRetryRebuildThrottle` exactly.** Matches the observed ~2s `[HITCH]` cadence.
- **device_occupancy** is edge-driven and early-returns in steady state (device_occupancy.cpp:209); its
  18-20ms `[WALK-TIME]` entries are almost certainly preemption noise during the same heavy frame, NOT its
  own walk. **npc_mirror** (client NPC pose apply, max 110ms) is a secondary contributor worth its own pass.
- **ARCHITECTURAL pattern (the real class):** it is NOT one sync -- a DOZEN `*_sync` subsystems each
  independently walk the full ~237k GUObjectArray on their own throttle (the 6 interactable channels +
  keypad/power/window/turbine/grime/... each have their own `RebuildIndex`). O(N_subsystems x NumObjects)
  periodic. This is why it is persistent and why per-walk gating never fixed it. ([[feedback-recurring-bug-is-architectural]])

## Growth mechanic + plateau-vs-unbounded (the user's degradation question)
- The bucket grew 43->60->101 ms/s over the first minute. `RebuildIndex` cost = O(NumObjects); NumObjects
  RAMPS as the world streams in after join. **It PLATEAUS:** private-commit went 3248->5098 MB during load
  then FLAT at ~4800 MB for the rest of the run (even drifted down); `sync:interactable` settled to ~15-21ms
  per 2s in steady state. **So in this ~90s+ run it is a STEADY hitch (~1 dropped frame / 2s), NOT a
  degrade-over-hours leak.** Caveat: a much longer session that accumulates UObjects (uncollected particles /
  un-reaped mirrors) would re-grow NumObjects and worsen the walk -- watch with the existing `[perf][mem]`
  line; not observed here.

## THE FIX MECHANISM — `coop/util/incremental_object_scan.h`
Authored 2026-06-23 FOR THIS ("a dozen *_sync subsystems each rebuilt their index by walking ALL ~237k
UObjects every 2s ... the periodic FPS stutter. The ROOT fix is to stop full-walking: scan only the NEW
objects (the array TAIL) each call -- O(new) ~microseconds -- with a RARE full re-scan (~5 min) as the
slot-reuse backstop"). The interactable channels' `RebuildIndex()` was never migrated -- migrating it is
the fix shape, BUT the slot-reuse caveat bit harder than the helper's "static actors tolerate it"
suggested (see below).

## FIX ATTEMPTS (take-1 regressed, take-2 partial) -- both FAIL the N-match gate, NOT verified
The verify gate (user-mandated, decisive): the per-channel `index rebuilt -- N live` count POST-fix must
MATCH the pre-fix full-walk count (door 57 / light 42 / container 56 / garage 1 / appliance 62 / doorbox
20). A dropped N = the new scan MISSED actors = broke detection, even if the walk went silent.

- **take-1 = pure tail-scan + prune + 5min backstop** (MD5 `148CA304`). Walk silenced (153->6) BUT
  **door 57->19, light 42->15 = REGRESSION.** Root: `Channel::RebuildIndex` full-REBUILT byKey_ every 2s
  (clear+repopulate from the live set), so removal was implicit; a pure tail-scan only sees NEW tail
  indices. Doors/lights **STREAM IN PROGRESSIVELY** ("26 at connect -> 57 later" -- the channel's own
  comment) into GC-recycled slots BELOW the tail during the join load -> tail-scan missed the 38 late
  doors; door's 300s backstop didn't fire in the 173s run. **My Safety-1 analysis ("static level actors
  load once -> first full-scan catches all") was WRONG -- static-by-origin but progressively-streamed.**
  The N-match gate caught it (numbers > "should work" logic; the user and I BOTH missed it in reasoning).

- **take-2 = stream-settle gate** (MD5 `FD9D2DC...`, current deploy): FULL-walk WHILE the channel's live
  count is still changing (full walk can't miss a recycled-slot actor -- correct by construction during
  streaming), switch to the cheap tail-scan ONLY after the count holds steady for `kStreamSettleScans`
  (=15 -> ~30s). Guarded `liveCount>0` so a pre-load 0 can't settle. Result: walk SILENCED in the steady
  tail (0 `[WALK-TIME] sync:interactable` after settle) AND 5/6 N MATCH -- **but door 50 != 57.** door
  held 50 for 30s -> settled -> 7 more doors streamed in >30s later (a streaming GAP > the settle window)
  -> tail-scan missed them. **A fixed settle window can't guarantee completeness when streaming has gaps.**
  NOT verified (criterion #1: ALL six must match).

**Lesson:** doors stream UNPREDICTABLY (pause-then-resume), so settle-then-PURE-tail cannot guarantee
completeness. Open design fork (user's call -- latency vs hitch trade-off):
- (A) Longer settle (~90s): catches slow *initial-load* streaming; fragile to any gap > window; no steady
  backstop hitch. (Needs: do VOTV doors stream once at load, or ongoing by proximity? unknown.)
- (B) stream-settle + a *frequent* backstop (30-60s full re-scan): robust (missed door recovers within the
  backstop), residual ~28ms full-walk every 30-60s/channel (staggered). Bounded latency, small periodic hitch.
- (C) interactable + device_occupancy together -- since interactable alone only HALVES the hitch.
Recommendation: (B) 60s backstop + do device_occupancy next. Door-open live-propagation = user hands-on
(joinchurn doesn't open doors); the N-match is the autonomous structural gate.

## Diagnostic instrumentation LIVE (uncommitted; keep until the user's door hands-on)
- subsystems.cpp: each Interactable-block `*_sync::Tick()` wrapped in `ue_wrap::ScopedWalkTimer{"sync:<name>"}`
  (per-sync `[WALK-TIME]` attribution). UNCOMMITTED working-tree.
- interactable_channel.h: the take-2 stream-settle migration. UNCOMMITTED, NOT verified (door 50!=57).
- inis (gitignored, deployed only): `perf_probe=1` on host + client. Revert for normal play.
Remove instrumentation + finalize the migration only AFTER the user's door-open hands-on confirms (or the
fork is chosen + re-verified). Deployed `FD9D2DC...` carries take-2 (door 50) -> the user's door hands-on
will show the 7-door gap live (validates the N-finding).
