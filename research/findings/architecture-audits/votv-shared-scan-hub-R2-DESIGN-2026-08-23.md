# R-2: the shared object-scan hub — design of record (2026-08-23, 8-round /qf "that holds")

> STATUS: DESIGN converged 2026-08-23 (8 rounds, fresh critic each; "that holds" at R8 with no
> unmeasured assumption named). BUILD in progress the same day — the AS-BUILT box will replace
> this line when the arc lands.
>
> Read together with: `research/findings/votv-linux-fps-triage-2026-08-23.md` §3 (the R-2 row +
> field numbers), `memory/project_L5_fps_hitch_root_2026-06-23.md` (the June root + takes 1-3),
> `ue_wrap/core/settled_object_scan.h` (the component this arc RETIRES), and the /qf transcript
> (session scratchpad `qf_thread.md`, "R-2 shared-scan design pass").

## 1. The ask, in the user's terms

Violet (tester, Discord): her friend runs the game at ~9 fps on Linux when joining her session.
The triage's R-2 row is the part of that report our code owns in steady gameplay: a
once-per-1-2-second stutter — `sync:interactable` median 74 ms / p90 104 / max 305 on the
friend's machine, with `atv`/`keypad`/`power`/`turbine`/`grime` stacking ~11-14 ms each in the
same seconds, and ~500 ms/s of our walks inside the 20-s join load window. Fix at the class
level per RULE 1; autonomous verification only.

## 2. The measured fact base (all from this session; the [SCAN-DIAG] probe is committed)

- **The family:** 13 `SettledObjectScan` consumers — 6 interactable channels (door/light/
  container/garage/appliance/doorbox, one `scan_` member each) + 7 file-statics (keypad, power,
  window, turbine, grime, atv, trash_pile). Grep-censused; ZERO users of the scan components
  outside the family.
- **The cost is FULL walks; tails are free.** 90-s 2-peer smoke with `VOTVCOOP_SCAN_DIAG=1`:
  every consumer ran ~17-19 FULL walks of ~270k objects (settleScans=2 consumers: 4-7 fulls);
  tail ranges averaged ~0 (occasional 31-46k after churn). One full walk = 3-5.5 ms on the dev
  machine (~17 ns/object); ~11.5 ms on the friend's (~43 ns/object, from her own log's
  per-consumer `X:RebuildIndex` timers).
- **The amplifier:** `settleScans=15` at the (measured) 2-s call cadence means ANY completed-
  count change re-arms 15 consecutive full walks per consumer — ~220 full walks × 270k per
  session start family-wide; the friend's 20-s gameplay bursts of every-2s ~63-ms stacked events
  are re-settle windows; her quiet 30-s stretches are settled phases.
- **grime never settles under sustained play** (count steps ~every 30 s: 975→978→979 in her
  log; keys = quantized PosKey), which is why it runs settleScans=2.
- **Indexes are KEYED maps** (`byKey_`/`g_index`) — pass-internal double-count is impossible
  (map insert collapses); smeared-pass count noise reduces to real key churn.
- **The dead-world exposure is PRE-EXISTING:** today's index entries are raw `Ref{actor,idx}` —
  world-BLIND (R-1/4j: dying-world actors stay un-kill-flagged 44+ s; `IsLiveByIndex` +
  `SlotSerial` cannot see world death). A deferred apply can `CallFunction` a dead-world actor
  inside that window today. This arc closes it family-wide (see §3.3).
- **Join world-swap = exactly TWO adjacent generation flips in ~1 s** (old→null→new,
  seedgreen_client.log 16:11:45-46), then the world pointer is stable through the ~11-s
  streaming tail. Pass-restart-on-gen-flip cannot starve.
- **Use-time re-verification is already universal:** every consumption site of all 13 re-checks
  `IsLiveByIndex` (channel poll :157, connect-snapshot :386, RestoreAutonomy :450, prune :507,
  ResolveFast :547; statics 5-7 sites each). The index is a resolution hint, never a liveness
  proof.
- **None of the 13 treats absence-from-index as death evidence** (censused; the
  destroy-on-absence walkers — registry_reaper, the npc ghost-sweep — are NOT consumers).
  Pending-apply expiry margin: `kPendingTTL` 25 s vs 3.6 s worst-case sliced discovery.
- **MTA precedent (opened):** `CClientStreamer` — the shared iteration service — lives in
  `mods/deathmatch/logic` (the MOD layer); elements register into it; `game_sa` exposes raw
  primitives only. Hence the hub lives in `coop/element/`, and `ue_wrap` keeps only the raw
  walk primitives it already exposes.

## 3. The design

**`coop::element::ObjectScanHub`** (GT-only). Consumers register
`{name, base-class getter, per-object callback, settleScans}`.

1. **One shared pass** per `max(2 s, pass duration + ε)` cadence, FULL while ANY consumer is
   unsettled (each consumer's `settleScans` is a term of the shared demand function — grime@2
   stops demanding fulls 2 passes after its churn stops). Per object: ONE `ObjectAt` + ONE
   `ClassOf` + a pass-scoped `UClass* → consumer-bitmask` memo entry `{cls, InternalIndex,
   SlotSerial}` re-verified per hit (one `ItemAt` read) — `IsDescendantOfAny × 13` runs once per
   DISTINCT UClass per pass, not once per object per consumer. Priced (arithmetic, falsified by
   the numeric gate): ~40-60 ns/object on the weak machine, replacing today's ~300-650.
2. **Passes are SLICED**, time-boxed ~1 ms/frame (clock checked every 4k objects). Settle
   counts COMPLETED passes; computed settle wall-clock = 30 s — identical to today. The one
   honest regression (stated in-round): worst-case discovery latency grows by pass duration
   (≤2 s → ≤~3.6 s on the 9-fps machine while unsettled).
3. **World-stamped passes / gen-checked indexes (folds the R-1 class for all 13):** the pass
   records `world_identity` generation at start and aborts on a flip; the swap stamps the gen
   into the consumer index; every consumer read-path first compares `index.gen == current gen`
   (ONE int compare per Tick) and treats a stale-gen index as EMPTY (demanding full passes).
   The 18:41 menu→save-world recovery becomes gen-driven instead of waiting for purge-time
   prune.
4. **All-settled** → shared tail pass (appended objects only) + ONE staggered 60-s backstop
   full pass (replacing 13). Index swap per consumer once at pass completion under its existing
   mutex; consumer scratch = `Ref{actor, idx}` (today's found-list shape). Channel `Tick`s stop
   rebuilding; deferred-apply retries stay on their own 2-s throttle.
5. **Runtime `Register()` during an active pass queues to the next pass start** (asserted +
   logged; the memo is pass-scoped so consumer-set changes need no invalidation machinery).
6. **Sustained-churn steady state** (grime never settling while driving): permanent 2-s-cadence
   full passes sliced to ≤1 ms/frame (~6.75 ms/s spread) — replacing today's ATOMIC 12.8-ms
   single-frame spike every 2 s in the same scenario. Accepted and documented.
7. **RULE 2 retirements in this arc:** the 13 per-consumer scans and their full-walk loops; the
   per-consumer stagger machinery (its purpose — de-correlating 13 backstops — dissolves with
   ONE backstop); `SettledObjectScan` and `IncrementalObjectScan` themselves (zero non-family
   users). Per-consumer `settleScans` tunings SURVIVE — under the hub they still purchase cost
   behavior (the demand function).

## 4. Migration (per-consumer atomic; nothing ships mid-arc)

C1: hub + the parity probe drill land with ZERO consumers (a new component alone is not a dual
path). C2: the 7 file-static consumers swap (each old scan deleted in the same commit). C3: the
6 channels swap via the one shared template path. C4 (or folded into C3): both scan components +
stagger deleted; docs. Every commit builds + smokes; the whole arc is unpushed until done.

## 5. Acceptance (pre-registered)

(i) **Parity drill**, per-commit for the migrated subset, both modes INSIDE ONE GT task (GC
runs on the GT — a same-task pair is atomic): **mode A** `ForceSyncFullPass()` (dev-only,
un-sliced) + the old-shape independent probe walk (`ObjectAt` + per-consumer
`IsDescendantOfAny` + `GetKey` → count + keysHash) — certifies the CLASSIFIER;
**mode B** probe vs the last SLICED-BUILT index at the settled anchor (at settle, staleness is
provably nil) — certifies the shipping accumulation path. MUTATE control: `VOTVCOOP_HUB_SKIP=
<class>` must turn parity RED for exactly that consumer once before any green counts.
(ii) **Final numeric gate:** ≥10× fewer full-walk object-visits ([SCAN-DIAG] totals), no
hub-attributable [WALK-TIME] over the frame budget, [HITCH-SRC] count before/after, per-pass
timing under budget (the arithmetic price's falsifier).
(iii) The **18:41 reload case** re-driven (host menu→save world; every index rebuilds through
it, now gen-driven).
(iv) **Pending applies drain to zero** in the smoke.
(v) **Settle-bound assertion** (~32 s computed) for every consumer, both peers, in the parked
scenario; never-settle-under-driving is the measured, accepted steady state covered by (ii).

## 6. Filed alongside, NOT in this arc (evidence in the triage doc §5b)

- `reseed:KnownKeyedProps` (registry_reaper.cpp:382): 120 ms avg / **1,880 ms max** on the
  HOST field log — same census class, but its cost may be dominated by per-prop ToString + the
  second world walk + minting, not the iteration. Own measured dig first; hub-consumer
  conversion is the expected but unproven outcome.
- `sync:npc_client` 42 ms avg ×25 (mirror interp/apply, not an array walk).

## 7. /qf round map (fresh critic each; full transcript in the session scratchpad)

R1 phase-split demanded → [SCAN-DIAG] built, fulls dominate, amplifier found; slicing promoted
to load-bearing. R2 pass-abort-on-purge → per-entry serial validation (TOCTOU); control made
old-vs-new; slice budget time-boxed on measured ns/object; retry staleness bound named. R3
keyed-map noise analysis + settle-bound assertion; memo priced as REPLACING 13 chains;
same-GT-task parity + mutate control; stagger dies, settleScans kept as demand terms;
components retire (zero outside users). R4 per-consumer atomic swaps; MTA CClientStreamer
opened → coop/element; settle bound computed (30 s, unchanged); discovery-latency regression
named; family-wide guarantee grounded in the shared component + measured cadence. R5 (real
find) the dead-world index exposure — folded: world-stamped passes + gen-checked indexes;
ForceSync parity (zero staleness); grime never-settle measured + accepted; price labeled
arithmetic with the numeric gate as falsifier. R6 absence-as-death censused clean (TTL margin
25 s vs 3.6 s); memo pass-scoped + queued registration; per-commit subset parity vs final-only
numeric gate; join gen-flips MEASURED (2 adjacent, then stable). R7 parity mode B (certify the
sliced accumulation, not only the classifier); use-time re-verification censused at every
consumption site of all 13; smeared counts add no new failure mode. R8 "that holds".
