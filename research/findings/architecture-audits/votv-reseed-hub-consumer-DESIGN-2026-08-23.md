# The steady prop re-seed becomes scan-hub consumer #14 — DESIGN (2026-08-23, 11-round /qf "that holds")

> ## AS-BUILT (2026-08-23 day 3 eve; BUILT + acceptance run same evening, NOT hands-on)
>
> Everything in §1 shipped as specified, with TWO acceptance-run corrections the drills caught
> (both are the RED-calibration discipline paying off — neither would have been visible without
> running the gates):
> 1. **The first run violated the drain budget exactly where the spec said "Deliver counted
>    inside the budget" and the code had it outside**: the end-of-tick chunk Deliver put ~800
>    adoptions' payload+send on ONE tick = 140/254 ms [WALK-TIME] reseed:drain. Fixed: express
>    PER ITEM inside the loop (1-element vector keeps DeliverLateRegisteredProps the one
>    kerfur-vs-generic routing owner), QPC budget checked EVERY item (a per-8 check let 8
>    back-to-back adoption+express items overshoot 2.4x).
> 2. **The first bump-mute RED run showed bumps=19 with the generation frozen** — the summary
>    counter counted bump ATTEMPTS at the call site. Moved inside BumpSeedGeneration_ past the
>    mute; re-run: `bumps=0` under mute = RED shown, gate calibrated.
>
> **Acceptance evidence (this box, autonomous smokes; DLL `875142419E8E45D1` x4, proto 134):**
> - Steady 120 s smoke PASS x3 (+90 s x2): old label `reseed:KnownKeyedProps` = 0 occurrences;
>   **steady-state drain max = 1.004 ms** (was 13-18 ms single-frame every ~20 s); load-window
>   (pre-join world settling, 20:52:06-12) worst 13.3 ms, rest 2-4 ms (cold-actor page-in
>   inflating single items; was 453 ms local / 1,880 ms field on the same event). Hub steady
>   tails 120-165 us; summary `queues=4-6 bumps=4-6 /60 s` steady = the ~20 s cadence parity.
> - Mass-adopt via the drain: host 3,101 adoptions in 4 ticks (run 1, pre-fix walltime), client
>   3,100 in 3-4 ticks (client Mark is index-only ~1 us/item — the 875/s throughput estimate was
>   for the HOST's express path only, client is ~1000x cheaper).
> - **Interleave drill GREEN**: forced synchronous ReSeed mid-drain (queue n=2,273 head=1,738 on
>   host; n=3,156 head=1,102 on client) -> ZERO duplicate-eid expresses across 2,639 total
>   (insert().second single-authority held). The sync walk's 1 adopted prop rode the later
>   bracket (its production callers pair with retrigger/re-bracket; the drill calls bare).
> - **scanparity DONE fail=0** (13/13 consumers OK with #14 registered — R-2 unregressed).
> - **joinchurn PASS** (the delivery + sweep evidence): 937 incremental PropSpawns through the
>   NEW drain reached the client; divergence sweep ran ONCE, no steady re-arm; no-local-match
>   flood 0; kerfur mirrored 1x, 0 convert flip-flops; 0 duplicate-eid expresses.
> - **Bump-mute RED calibrated** (`bumps=0` under VOTVCOOP_RESEED_MUTE_BUMP=1).
> - RESIDUAL (honest): the dup-bracket gate's RED (VOTVCOOP_SNAPSHOT_DEDUP_BYPASS=1) was armed
>   through joinchurn but the same-slot-same-gen mid-drain re-trigger never arose organically
>   (1 bracket, 0 queued re-triggers) — the lever exists, its red demonstration needs a scenario
>   with an episode-add during an active bracket drain. Not achieved this session.
> - File sizes: prop_census.cpp 557, registry_reaper.cpp 342 (shrunk), all touched files under
>   the 800 cap (imgui_overlay.cpp 815 pre-flagged separately in `9e41892f`).
>
> The /qf thread (11 rounds, 4 superseded fragments purged, 2 primary self-retractions) lives at
> the session scratchpad `qf_reseed_brief.md`; round map §6. Post-ship audit: see the commit
> message + the audit fold note appended below when it lands.

## §0 The ask and the measured defect

Violet's (Discord field report, the Linux-9-FPS thread) HOST log carries `[WALK-TIME]
reseed:KnownKeyedProps` n=17: **avg 120 ms, max 1,880 ms single-frame stalls** — the largest
single [WALK-TIME] anywhere in either field log (triage §5b). Measured attribution (this /qf):

- The label exists ONLY at the STEADY branch (registry_reaper.cpp:382); Violet's log has ZERO
  episode/world-change reseed lines → **all 17 stalls are the steady branch**; the 1,880 ms is
  the FIRST steady census after session start, which ADOPTED 1,826 props (local twin: 453 ms,
  3,103 adopted). The first census is the de-facto PRIMARY enrollment of the save-loaded world
  (Init-POST sees only dispatched inits; distant/streamed props are census-only).
- Post-R-2 local cost TODAY: 13-18 ms single-frame every ~20 s host, ~4-8 s cadence client.
- The [WALK-TIME] bracket covers walk + phase-2 adjudication; adoption costs ~140 us/prop
  (mint + registry + express), an already-known item ~1-3 us (insert-fail + idempotent Mark).
- **The periodic SAFETY census catches REAL recycled-slot spawns in the field** (NumObjects
  flat): Violet 2x (distinct keys/eids `z87AD…`/3964, `azU0k…`/3965 — genuine, not oscillation),
  the friend 3x consecutive (50/17/12 per 20 s window). The v106 "hand-edge express is primary"
  comment does NOT hold at field scale → **~20 s worst-case detection latency must be preserved**.

## §1 The conversion (one arc, one commit family)

**Hub consumer #14 "prop_reseed"** (the census domain owns it — `prop_census.cpp`):

1. `IsInstance` = `ue_wrap::prop::IsKeyedInteractable` (CLASS-PURE: prop.cpp:145-157, memoizable).
   `EnsureResolved` = the Aprop base resolved (extras resolve lazily as today).
2. `settleScans = 0` = **demand-exempt**: 0<0 is false at all three hub sites
   (object_scan_hub.cpp:85/160/289; AbortPass:148 harmless) — the consumer never forces full
   passes and never blocks all-settled tails. Contract comment added to the hub header.
3. `OnMatch`: push `{obj, InternalIndexOf(obj), SlotSerial(idx)}` to pass scratch. NO filters at
   match time (hand-axis membership churns mid-pass — per-slice filtering would re-open the
   13:44:00 phantom-express class).
4. `OnPassComplete(isFull, worldGen)`: compute `grew = (NumObjects != s_lastSeenNum)` (update it);
   gate on the predicates code-identical to today's steady `else if`
   (inGameplayWorld && HasSeededOnce && IsRegistrySeededForCurrentWorld && !InPurgeEpisode) —
   gate fails → drop scratch (diag). Gate passes →
   - merge scratch into THE QUEUE: different worldGen → REPLACE; same gen: TAIL batch APPENDS,
     FULL batch REPLACES (a full scratch is a superset: an undrained still-live candidate is
     re-matched — the census is idempotent; an undrained dead one is correctly dropped);
   - **bump SeedGeneration when (isFull || grew)** — grew-based parity with today's
     unconditional-every-walk bump (today's walk runs on grew or periodic); an object-append
     tail with zero matches still bumps (scanned >= 1 object = grew), a flat-count pass does not.
     The bump is NOT gated on queue-empty (R9-C3: a bracket built from a partially-drained
     registry self-heals — §3). Return value = raw enqueued-candidate count (pre-adjudication;
     sole consumer DebugConsumerCount, documented as such).
5. **Budget drain** `DrainReseedQueue()` from net_pump, ~1 ms/tick (QPC check every 8 items),
   `ScopedWalkTimer("reseed:drain")`:
   - per drain TICK: re-check worldGen==current && the four predicates; mismatch → DROP the whole
     queue (diag `reseed: queue dropped (n=.. reason=episode|gen-flip)`); the episode paths own
     post-transition re-derivation (DrainChunk:525's shape).
   - per ITEM: verify `IsLiveByIndex(obj, idx) && SlotSerial(idx)==serial` (D1 pattern, never
     bare IsLive) + `WorldOf(obj)==CurrentWorld()` (reject diag `reseed: rejected dying-world
     candidate`, counted) + hand-axis (collected once per tick) + IsChildActor + `Default__`;
     then TODAY'S newness block relocated verbatim (under g_knownKeyedPropsMutex: insert;
     .second → churn guard → freshness → newness) — `insert().second` is the SOLE newness
     authority, which makes every interleave single-express BY CONSTRUCTION (episode walk between
     drain ticks inserts first → drain insert-fails; tail-dup of an undrained full item → first
     wins); then MarkPropElement / keyless-pile mint (idempotent refresh, client index-only per
     v122) OUTSIDE the mutex, today's ordering (lock is a GT leaf: 4 sites, all game-thread).
   - per drained chunk: `DeliverLateRegisteredProps(chunkNew)` (kerfur OFF-prop routing preserved
     — same function per chunk); queue-empty logs `reseed: queue drained (n= new= rejects=
     ticks=)`.
   - **Convergence rests on the REGISTRY/set, not slice order**: a drained item is an insert-fail
     wherever it reappears; each full-replace cycle re-does only the undrained remainder →
     monotone. 9-fps worst case: mass-adopt 3,103 ≈ 434 ms of adoption work ≈ ~3 cycles ≈ 60 s
     (printed as a diag, measured not gated); steady queue (~4k items, mostly ~1-3 us) empties
     within the inter-pass gap on any box.
6. `kBackstopEvery` 30 → 10 (~20 s sliced FULL cadence — the field-proven recycled-slot latency;
   tails still catch appends at ~2 s at delta cost). Per-frame cost is capped by the hub's slice
   budget by construction; duty cycle rises (~3x fulls).
7. **RULE 2 deletions**: the steady `else if` branch + the NumObjects high-water guard
   (registry_reaper.cpp:323-396). The episode-end / small-travel / save_identity_bind /
   prop_key_index / director callers KEEP the synchronous `ReSeedKnownKeyedProps` (different
   trigger class: rare transition events needing a settled synchronous result).
8. **Labels** (blind-instrument lesson): `ScopedWalkTimer("reseed:sync-walk")` moves INSIDE
   `SeedKnownKeyedProps`/`ReSeedKnownKeyedProps` so every kept synchronous caller is visible
   (measured: episode walks do NOT run in steady play — 0 lines in Violet's log AND local steady
   smokes; kReseedPurge=64 threshold); the old steady-branch label dies with the branch (old→new
   label map: `reseed:KnownKeyedProps` → pass visibility via `[SCAN-DIAG]`/`sync:scan_hub`,
   adjudication via `reseed:drain`, synchronous callers via `reseed:sync-walk`).

## §2 What was DESIGNED AND DROPPED (do not re-derive)

- **CensusSettledForCurrentWorld (R2-C4 → dropped R3-C2)**: census completion is
  engine-resolved, not lane-settled (the boot walk "completed" while the world streamed +1,826
  over 3 s), and its setter is the same walk as the existing stamp → it defers NO join the
  existing three-signal gate passes. The enrollment window is delivery-safe end-to-end instead
  (§3).
- **Fixed 256/tick drain (R2 → replaced R6-C1)**: 256 x ~140 us = ~36 ms/tick = a comb of stalls.
  Budget-based (~1 ms) instead.
- **The R2-C2 dedupe rationale for bump semantics (retracted R7-C1)**: TriggerForSlot:471's
  dedupe guards re-triggers of the DRAINING slot, issued only by retriggerReadySlots (episode
  paths, cadence untouched); today's 4-8 s grew bumps already occur mid-drain. Parity is the
  basis, not the dedupe.
- **"Non-empty-tail items" bump trigger (R3-C1 → replaced R4-C2/R9-C1)**: grew-based (scanned
  >= 1 object), independent of matches.
- **Queue-empty gating of the bump (R4-C2 → dropped R9-C3)**: its only basis ("bump implies
  adoptions visible") is unnecessary — see §3; and at 9 fps it delayed the deferred-slot rescue
  by up to ~60 s (fast-box artifact caught before it shipped as a gate).

## §3 The join-window safety chain (all MEASURED this /qf)

- `SendPropSpawn` → `SendReliable` per-slot `IsSlotWorldReady` gate (session.cpp:246): a bracket
  drain starts AFTER the slot's world-ready edge → a mid-bracket drained adoption IS sent.
- `PropSpawn` and `SnapshotBegin` share `Lane::Bulk` (session_lanes.h:42/74) → in-lane GNS order
  guarantees SnapshotBegin (arms + clears the claim set) precedes any express sent after
  bracket-open — no express-before-arm race.
- The client apply CLAIMS the actor (`RecordClaimIfTracking`, remote_prop_spawn.cpp:334/484/593;
  join_membership_sweep.h:17 "each PropSpawn ... survives the sweep") → the SnapshotComplete
  sweep spares a mid-bracket-expressed prop. **F-R3-1 = CLOSED BY READING + drilled** (§4).
- A bracket re-send of an already-expressed prop is idempotent: the apply dedups by exact key
  (ResolveLiveActorByKey :314) or eid (Registry :305-312) → SAME local actor, claim + converge,
  no second actor. The express-then-bracket-resend sequence already exists today.
- The WorldOf terms are a NAMED directional behavior delta: today's census can adopt a
  dying-world actor in the 44-s two-live-worlds window (bare IsLive passes) and express a
  phantom; the terms close that R-1-class hole. An A/B over transition windows may legitimately
  show FEWER adoptions — audited via `old - new == rejects + drops` (the two diag counters).
- The stamp during a sub-64 travel: today's steady re-stamp re-selects the OLD world (first
  'ntitled' match at the lower index) = literal no-op; branch 2 (small-travel) is the real
  refresher when the purge kills the stamp — in BOTH designs. No stamp consumer is dropped.

## §4 Acceptance (numeric, scoped, RED-calibrated)

Machine "steady state" = lines INSIDE the smoke's MONITORING window, per-peer log. Gates:
1. Hub walk cost <= 2 ms/frame (R-2 bound, same instrument); `reseed:drain` max <= ~2 ms/tick;
   no our-label [WALK-TIME] > 10 ms inside a STEADY window; ANY `reseed:sync-walk` line inside a
   STEADY window = RED (join scenarios: REPORT-ONLY — bind/key-index callers legitimately run).
2. [SCAN-DIAG] full-pass cadence ~3x the R-2 baseline (20 s vs 60 s), not higher.
3. Delivery drill (spawnmenutest): the client applies EXACTLY ONE PropSpawn; kerfur OFF-prop
   case greps the ExpressIncrementalKerfurOffProp routing line.
4. Interleave drill (env-gated): a forced synchronous ReSeed BETWEEN drain ticks with a charged
   queue → the assert is DUP-SIDE ONLY: ZERO duplicate-eid expresses across the run (audit
   IMPORTANT-2 corrected the original "not 2, not 0" wording — a prop adopted BY the bare
   drill walk legitimately expresses zero times; its production callers pair with re-bracket,
   the drill call does not).
5. Mid-bracket drill (joinchurn variant): a host spawn DURING a client's bracket drain → the
   client's prop survives Complete (no destroy of its key) + one apply.
6. A/B frozen digests, STEADY scenarios only: adopted totals, incremental PropSpawn counts,
   client apply counts EQUAL old-DLL vs new-DLL (same save/scenario). Transition scenarios:
   directional (new <= old, delta == rejects + drops diags).
7. RED calibration BEFORE green counts: dup-bracket grep shown RED by an env mutate (bypass the
   :471 dedupe / force retrigger), bump-cadence grep shown RED by an env bump-mute.
8. Steady queue empties within the inter-pass gap (diag "ticks="); mass-adopt convergence cycles
   printed, measured not gated.

## §5 FILED (own hooks, outside this arc)

- The first-census mass-adopt (1,826/3,103) reveals Init-POST enrollment misses the streamed
  world tail; the census remains the reconciler by design — no action, recorded as the mechanism.
- `sync:npc_client` 42 ms x25 on the friend (triage §5b third-row candidate if it recurs).

## §6 /qf round map (11 rounds, fresh critic each)

R1 field-attribution + SAFETY-net falsification of the 60 s backstop widening; R2 queue
worldGen/drop + bump parity + oscillation ruled out (keys) + the enrollment-window timeline;
R3 queue merge rules + PREDICATE DROPPED + return semantics; R4 stamp no-op via lower-index
old-world + grew-based bump fix + delivery acceptance layers + WorldOf delta named; R5 insert
block as sole authority + lock scope measured + purge-drop ordering + labels; R6 budget drain
(256/tick comb falsified by own arithmetic) + F-R3-1 closed by reading + numeric bounds + shrink
parity; R7 dedupe rationale RETRACTED + sync-walk label + queue-empty arithmetic + lane-order
claim-window proof; R8 consolidated-spec rule + machine steady-state + RED calibration; R9 fourth
fragment purged + equality-gate scoping + queue-empty bump gate DROPPED (9-fps artifact) +
sync-walk rule scoped; R10 drop diag + registry-based convergence stated + re-send idempotence
measured; R11 "that holds".
