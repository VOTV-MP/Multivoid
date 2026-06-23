# chipPile JOIN-WINDOW DUP — the two-channel race (DECISIVE, hands-on) — 2026-06-23

**Status: ROOT CONFIRMED (hands-on). Fix BUILT in THREE layers (Path 1c key + the load-tail sweep retry +
the OnRequest pre-capture re-seed) + audit GO-WITH-NITS (0 critical); HANDS-ON take-3 PENDING. Deployed
`06104FED9F72ED7B7DEE19F7336E2F73` (MD5), proto v86, push HELD.** See "## AS-BUILT (2026-06-23 take 3)"
below for the DECISIVE break — take-2 STILL duped because the host captured **`0 pile save-time xforms`**
(the eid-keyed map was EMPTY, so neither the key nor the sweep retry had anything to act on); root = the
host's world-change re-seed is DEFERRED at the connect instant, so the live chipPiles have no eid yet. The
take-2 section ("the REAL break was TIMING") is the SECOND layer and remains correct ONCE a key exists, but
it was downstream of an empty map; the (a)/(b) fix-options sections lower down stay SUPERSEDED. This whole
finding supersedes the "L1 orphan / absence-removal at the join" line — that chased a bug that **does not
exist** (see "What is now FALSE").

## The decisive hands-on (user, 2026-06-23)
Setup: 6 items on a path — **2 kerfur + 4 chipPile** — host saves. Then, **in the join window** (the
client has received the scratch save but is NOT 100% loaded yet), the host drops/moves the items onto
the asphalt. The client then renders:
- **2 kerfur @ path** (old/save position) — NO dup.
- **4 chipPile @ path** (old, from the save) **AND 4 chipPile @ asphalt** (new, from the broadcast) =
  **8 piles instead of 4 — a DUP.**

## Root cause: chipPile has TWO identity channels; kerfur has ONE
- **chipPile travels by TWO channels:** (1) the **scratch save** — the client loads the host's save, so
  the pile exists as a CLIENT-LOCAL NATIVE `actorChipPile_C` at the SAVE position; (2) the **broadcast
  convert** — the host's live grab/move broadcasts a `PropConvert` (ToClump/ToPile) that materializes a
  host-eid **PROXY** at the NEW position. In the join window these two channels DIVERGE (the save is the
  old position, the broadcast is the new one).
- **kerfur travels by ONE channel** (the save only — a plain prop in the save, no separate convert-
  broadcast double path). So a host kerfur move in the window does not produce a second kerfur. **Kerfur
  is the clean CONTROL that proves the two-channel pile path is the cause.**

## Why the existing 1cm dedup is blind to it
The join reconcile de-dups a client native against a host proxy by **POSITION (1cm)** — `kDestroyR2Cm`
in `remote_prop_spawn.cpp` (the destroy loop, ~line 405-425). For an UNMOVED pile the save-native and the
proxy are co-located → matched → the native destroyed → one mirror. But for a pile MOVED IN THE WINDOW,
the save-native (old pos) and the broadcast-proxy (new pos) are **far apart (>1cm)** → the position dedup
**cannot match them** → both survive → **two distinct objects = a persistent dup.** This is the SAME
">1cm → dedup blind" failure mode the session kept circling, but **created by the timing window, not by
host-drift** (host-drift can't create it — see below).

## The dup is PERSISTENT, NOT self-resolving (correction)
- **kerfur self-heals:** a kerfur with a wrong position is ONE object; a client interaction re-syncs its
  position (host teleports it to the right spot) → converges.
- **a pile dup does NOT self-heal:** it is **TWO DISTINCT objects** on the client (the scratch-save
  native + the broadcast proxy). Interacting with ONE (e.g. grabbing the proxy) does not touch the OTHER
  (the native stays). The earlier-session belief that "the pile dup self-corrects" was WRONG — grabbing a
  dup pile only *removed the one you grabbed*; the other stayed. **Severity is NOT reduced — it is a
  persistent extra world object, not a cosmetic phantom. The fix is required.**

## The dup is CLIENT-LOCAL — the host state is CORRECT (lowers risk + fixes the locus)
The dup does NOT leak to the host. The host sees the correct SINGLE state (one pile at the new position).
The extra object exists ONLY on the client, born from its TWO inbound channels (scratch-save native +
broadcast proxy) that it assembles independently. Consequences: (1) host authority is intact — no
host-side corruption, lower risk; (2) **the fix is purely CLIENT-side channel reconciliation** (the client
must collapse its two inbound representations of one pile), NOT anything host-side. This rules out fix
variants that touch host state and points squarely at the client's apply path (the OnConvert / save-load /
dedup ordering on the client).

## What is now FALSE (RULE 2 — retire it)
- **"Pre-connect host-drift creates orphans" is FALSE.** The join is a SAVE-TRANSFER join: the host takes
  a FRESH scratch save AT connect (`save_transfer.cpp` stamps a captured blob at connect; the user
  OBSERVED the save fire at connect). A fresh save captures the host's CURRENT state, so ANYTHING the host
  did BEFORE the save is IN the save → the client loads it → host==client → **no pre-connect divergence
  exists as a class.** Divergence is ONLY possible from a host change AFTER the save snapshot.
- **Therefore the "[PILE-CENSUS] orphan census + absence-removal AT THE JOIN" line is the WRONG fix and is
  CANCELLED.** It searched for join-time orphans that the save-transfer makes impossible — which is why
  every census reported `0 live orphans (totalLiveNatives=870 proxyMatched<=1cm=870)` on every clean run.
  That 0 was always CORRECT (nothing to find), not a metric bug (though 3 real census bugs WERE found +
  fixed along the way — silent-on-empty `a54fbd22`, stale-index→fresh-walk `ff5cdac6`, the totalLive
  diagnostic `ac13394d`; the census + `coop::util::IncrementalObjectScan` + the host-drift scenario remain
  as diagnostic infra but the orphan-at-join premise is dead).
- The autonomous **same-machine identical-save drift smoke cannot reproduce the real bug** — the bug needs
  the JOIN-LOAD WINDOW + a host move in it, which the drift scenario (pre-connect) structurally can't make.

## The fix — DESIGNED, 2 options [SUPERSEDED by the AS-BUILT section above]
> **SUPERSEDED 2026-06-23: the gate checks below resolved BOTH (a) and (b) as DEAD, and the shipped fix is
> a THIRD path — match by SAVE-TIME position, reconciled at the post-quiescence sweep (see "## AS-BUILT").**
> Gate (a) hold-broadcast: MISTARGETED — the live convert is ALREADY dropped by the pre-world gate; the
> @new proxy arrives via the POST-load replay snapshot, so holding the convert changes nothing. Gate (b)
> match-by-eid: IMPOSSIBLE — the save-native carries a client-minted peer-range eid, the proxy a host-range
> eid; disjoint, no bridge, chipPile keyless. Kept below for the audit trail of why the shipped fix differs.

The dup = one pile expressed as TWO objects (scratch-native@old + broadcast-proxy@new) that the
position-dedup can't reconcile because they diverged in the window. The clean fixes:
- **(a) HOLD the broadcast convert until the client signals load-100%** (queue host pile-converts during
  the join-load window; flush in-order after load-complete). Then the client loads the save FIRST, then
  applies the convert IN ORDER → no divergence → the existing path re-skins the existing mirror. Cleanest
  — kills the race at the source (MTA push-in-order). **GATE: does the broadcast convert actually leave
  the host BEFORE the client is load-complete, and is there a load-complete signal to queue against?**
- **(b) dedup MATCH-BY-EID instead of 1cm position** — if the scratch-save native and the broadcast proxy
  of one pile share an eid, reconcile by eid (distance-independent). **GATE: do the client's save-loaded
  native and the host-eid proxy share an eid?** (Likely NOT — the save-native is keyless/lazily-eid'd with
  a CLIENT-minted eid, the proxy carries the HOST eid; if so, (a) is the path, or (c) a post-load
  reconcile that links them by the pile's stable level-identity, not distance.)

## AS-BUILT (2026-06-23 take 2) — the REAL break was TIMING, not the key; fixed at the sweep
The first build (Path 1c: stamp the join snapshot with the pile's save-time position, match the client
native against it) shipped + was hands-on tested and STILL DUPED. The log chain RE'd the break precisely
and it was **NOT the key**: the map built (870 xforms), the wire stamped the save-time key (`[PILE-DELTA]
matchPos = @old`, not loc@new), and the matchKey was **bit-for-bit correct** (`matchKey eid 2278 =
(1681.2,-265.5,6124.6)` == the census orphan @old to 0.1cm). The break is a **LOAD-TAIL TIMING RACE**: the
pile-bind index is built ONCE at world-ready when the moved piles' proxies drain FIRST, but the client's
async native-pile load-tail is STILL draining — the native@old has not loaded/indexed yet (`nearestNative
856cm` at index build), so the twin-destroy can't match it. By the post-quiescence divergence sweep (~10s
later) the native@old IS present (the orphan census sees all 4 @old). **world-ready ≠ native loaded** — the
edge we had NOT considered (the float/re-seed edges we closed earlier were closed correctly; the key is
exact). Why only the 4 MOVED piles dup (not all 871): the race hits every pile whose proxy drains before
its native loads, but an UNMOVED pile's late native sits UNDER its co-located proxy (invisible overlap,
census `proxyMatched`), while a MOVED pile's native@old and proxy@new are SEPARATED → visible dup (census
`gt30`). The run: `proxyMatched=866, orphans=4 (gt30)`.

**FIX (commit `124fbc9d`):** retry the save-time twin-destroy at the **post-quiescence sweep**, where the
late natives are loaded. `TryDestroyTwin` records a save-time-key MISS in `g_pendingSaveTimeTwin`;
`pile_reconcile::SweepReconcileSaveTimeTwins()` (called from `RunDivergenceSweep_` before `LogCensus`, which
runs ONLY post-`kSweepQuiesceScans`-quiescence) fresh-walks the now-loaded natives and destroys the
native within 1cm + same chipType per pending key. Absence-removal Phase-2 **keyed by save-time** (not blind
proximity → no dense-cluster hazard) + its own >50%-of-live-natives abort-valve. This also closes a hole
the Registry doom STRUCTURALLY misses (a level-native chipPile enters the Prop Registry lazily, so
`SnapshotActorsByType(Prop)` never enumerates it → only this keyed-by-save-time retry removes it). Audit
verdict GO (0 CRITICAL/HIGH). **Hands-on PENDING** (deployed `F9B6589E1F62955F`, proto v86).

### Known caveat (audit MEDIUM, follow-up — NOT yet fixed)
The pending set is bracket-scoped (cleared in `pile_reconcile::Reset()` at `BeginClaimTracking`) but its
CONSUMER fires a bracket LATER (the deferred sweep). A host RE-BRACKET between the world-ready burst and the
deferred sweep (level travel / a racing re-seed bracket during the multi-second quiesce window) calls
`Reset()` → wipes `g_pendingSaveTimeTwin` → the in-flight save-time twin-destroys silently no-op → dup
persists. It **fails SAFE** (never an over-destroy) and is largely self-healing (the new bracket re-expresses
+ re-reconciles), and it does NOT affect the single-join in-window-drop repro. Fix-if-needed: decouple the
pending set from the bracket Reset (survive a re-bracket), or re-arm it on the new bracket.

## AS-BUILT (2026-06-23 take 3) — the DECISIVE break: the host captured an EMPTY save-time map
Take-2 (Path 1c key + the load-tail sweep retry, deployed `F9B6589E1F62955F`) shipped + was hands-on tested
and STILL DUPED (client saw 8). Grepping the chain step-by-step found the FIRST break is EARLIER than either
prior theory and is on the **HOST**: at the connect instant the host log printed

    save_transfer: slot 1 -- captured 191 keyed-prop keys + 0 pile save-time xforms at blob instant

**`0` pile save-time xforms.** The eid-keyed save-time map (`g_blobPileXforms`) was EMPTY — so there was no
key on the wire at all, and BOTH the take-1 match and the take-2 sweep retry were operating on nothing
(they only ever mattered once a key existed). Everything downstream was correct but starved.

**Root cause — the capture races AHEAD of the world-change re-seed.** Host timeline (take-2 log):
- `19:03:52` boot seed: `2434 live actors (... 34 keyless chipPile element(s) ...)` — the menu/transient world.
- `19:03:56` `net_pump: mass-purge detected (reaped 256 >= 64) -- world-change re-seed deferred to drain-complete`
  (the host's own menu→game world swap; net_pump parks the re-seed to avoid opening a bracket against a
  majority-dead registry, `net_pump.cpp:528`).
- `19:04:24` client connects → `save_transfer::OnRequest`: takes the live scratch save AND captures the
  baselines — but the registry is still pre-re-seed, so the NEW world's keyless chipPiles have **no host eid**
  (`GetPropElementIdForActor == 0`). `CollectTrackedPileTransforms` skips every pile with `eid == 0`
  (`prop_element_tracker.cpp:713`) → **0 captured.** (The keyed side was starved too: only 191 keys, vs the
  ~3000 the world really has.)
- `19:04:32` the deferred re-seed finally runs: `re-seed found 3241 live keyed props, added 2993 NEW
  (869 keyless chipPile element(s))` — the eids the capture needed, bound **8 s too late.**

So Path 1c's premise (an eid-keyed save-time map) was never satisfied when a client connected DURING the
host's own load. The runbook's "Honest scope" had even predicted this ("an UNSEEDED-at-save pile … skipped
… rare; flag if seen") — but it is NOT rare: a join right after the host loads in (the autonomous-test
timing AND the user's take-2) leaves EVERY pile unseeded.

**FIX (this commit):** force the world-change re-seed at `OnRequest` BEFORE the baseline captures, gated on
`!prop_element_tracker::IsRegistrySeededForCurrentWorld()` (so it fires only when the registry is stale for
the live world — a no-op, byte-identical, on a host that was up a while, which is why take-1 captured 870):

    if (!IsRegistrySeededForCurrentWorld()) { added = ReSeedKnownKeyedProps(); UE_LOGI("forced pre-capture re-seed ... -> %zu tracked", added); }

`SeedWalk_` is purge-safe (double IsLive filter + idempotent `MarkPropElement`) and the eid it mints is the
SAME one the connect drain later broadcasts (idempotent → net_pump's deferred re-seed adds 0, eids
unchanged), so the client's save-time-position key now resolves. It opens NO snapshot bracket (the slot-1
drain is `ConnectReplayForSlot` at world-ready, ~8 s out), and the joining slot is NOT starved — its snapshot
comes from `ConnectReplayForSlot → TriggerForSlot` independent of net_pump's `retriggerReadySlots`. Bonus: the
keyed R2 baseline (`g_blobKeys`) is now complete too. Audit verdict **GO-WITH-NITS** (0 critical; the only
nits are the carry-forward `prop_element_tracker.cpp` 927-LOC extraction + a comment wording note).
**Hands-on take-3 PENDING** (deployed MD5 `06104FED9F72ED7B7DEE19F7336E2F73`, proto v86). Acceptance gate:
the host log must show **`captured K keyed-prop keys + P pile save-time xforms`** with **P non-zero**
(hundreds — the whole world's piles), and (if the host connected mid-load) the `forced pre-capture re-seed`
line; THEN the take-2 sweep retry resolves the visible dup to 4 within ~10-15 s.

**Lesson:** a connect-edge baseline that reads our engine-overlay identity (eid/key) must guarantee the
overlay is SEEDED for the live world at capture time — net_pump's mass-purge re-seed deferral means a
client connecting during the host's own world-load lands on an unseeded registry, and any eid-keyed capture
silently records nothing. Force-seed (idempotent, gated on stale-world) at the capture, don't assume the
periodic seed already ran.

## NEXT (build, after the 2 gate checks)
1. Trace the save-transfer + join-load timeline: when is the scratch save taken, when does the client
   signal load-complete, when does a host grab/move broadcast its convert relative to that — confirm the
   broadcast lands during the window (`save_transfer.cpp`, `event_feed` SnapshotBegin/Complete,
   `remote_prop::OnConvert` spawn-fresh-for-unknown-eid path).
2. Confirm the eid (non-)link between the save-native and the proxy (`prop_element_tracker` lazy eid mint
   vs the host eid on the proxy).
3. Build (a) if queueable (preferred), else (b)/(c). Then re-run the user's controlled repro (drop in the
   window) — dup must become 0.

## Controlled re-test protocol the user proposed (if the trigger needs further isolation)
A/B/C/D over {1st vs 2nd connection} x {move IN-window vs move AFTER load-complete}, to split the
timing-window trigger from any reconnect-state factor. The decisive hands-on already PROVED the window is
necessary (move-in-window → dup) and two-channel pile is the cause (kerfur control clean); the A/B/C/D
grid remains only if a reconnect-number co-factor is later suspected. Requires a visible "client
load-100%" marker so the user can reliably tell in-window from after-window (NOT YET added — a candidate
next-build item: log/marker on the client's load-complete).
