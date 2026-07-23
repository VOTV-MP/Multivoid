# Per-system sync profiles — what we sync, what we don't, facet by facet

**What this is.** For one game SYSTEM, the list of its sync FACETS with a status each. It answers the
question the class register cannot: *inside this thing, what works and what doesn't.*

**What this is NOT.** Not a percentage, and not derivable. Read §0 before adding a system — the
limits are structural, and a profile read as a measurement is worse than no profile.

**The one boundary that must not be lost: the FORM is converged; the CELL VALUES are not all
verified.** There are **19 facet rows** across FOUR profiles (8 container + 5 weather + 1 lamp + 5
meadow; the doc's "table rows" figure is higher — it counts the remainder/lane rows too). After the
LABOR pass, their evidence stands:

- **2 hands-on** — container #1, #2: the cited runbook, a human played it.
- **3 real-log** — container #6, weather #1, weather #5: each cites a real matching log line.
- **2 selftest** — meadow #1, #2: an autonomous e2e digest (0->1->0 cross-peer) passed; below `log`,
  above `code` (the new evidence value the meadow profile earned — §4).
- **11 code-read** — the owning code was read and the label matches: container #3
  (`HostAcceptsClientWrite`, arbiter), #4 (the `freshBirth` guard, `prop_drop_intent.cpp:278`), #5
  (`BOUNDARY 2`), #7 (`IsWorldContainerInventory`, fail-closed), weather #2/#3
  (`ReliableKind::RedSky` / `LightningStrike` send+receive), #4 (`weather_fog.cpp` exists), lamp #1
  (verified by ABSENCE — zero `AlampPost` refs in our tree, exactly the "we sync zero facets" claim),
  meadow #3/#4/#5 (order/join-seed/concurrent-merge — the whole lane was read this pass).
- **1 unlooked** — container #8 (`putObjectIn_overlap`): `evidence=none` is honest; no owning code found.

So **only container #8 has no evidence beside it**; the other 18 are code-read, selftest, real-log, or
hands-on. TWO caveats stand: (1) code-verifying a label confirms it *matches the code* — for a
`UNKNOWN/code` row that means "the lane exists, behaviour unobserved", NOT that it behaves; the VERDICT
axis still needs a run. (2) The two ORIGINAL verified cells (container #6, weather #5) were both
*wrong-label catches*; the LABOR pass found no wrong label, only a citation line-rot (currVol, fixed to
a symbol anchor). Read a filled profile as "the form holds and these labels now match the code they
cite", NEVER as "the behaviour is proven".

---

## 0. The rules this document is built on (measured 2026-07-22/23, do not re-litigate)

**The row set is JUDGMENT, and it cannot be otherwise.** There is no machine enumerator of facets:

| facet source | machine-enumerable? | why |
|---|---|---|
| verbs | partly | `RegisterVirtualVerb` is a real registration, but verbs COLLAPSE — in `container_contents_sync.cpp` both `addObject` and `takeObj` register with the single id `kVerbDirty` onto the one callback `OnVerbEntry`, which carries no arguments — and a large share of our sync mirrors FIELDS with no verb shape at all |
| field invariants | only where a codec exists | the codec names the fields it packs; a field nobody packs is invisible |
| **races** | **no, in principle** | a race is a property of INTERLEAVINGS, not of source text. `CONFLICT=0` was found by RUNNING; nothing in the tree enumerates the concurrent orderings that have never been tried |

**And a system's facets cross files**, so no file-keyed enumerator would find them even if facets were
enumerable: the `freshBirth` guard in `prop_drop_intent.cpp` (`if (!parked && !freshBirth) continue;`)
drops the item a client extracted from a container — a CONTAINER facet living in a drop-intent file.

**Three consequences, each load-bearing:**

1. **Every system carries a mandatory `remainder` row — with TWO fields, never one.** A facet nobody
   thought of is ABSENT, and an absent row reads as "nothing wrong there" exactly the way a green
   does — the same error asymmetry that killed the syntactic marker filter
   (`[[lesson-syntactic-marker-set-cannot-express-semantic-relevance]]`). The naive remainder
   ("N facets found by RUNNING") has that asymmetry inside IT: a remainder of 0 means either "we ran
   interleavings and found nothing more" or "nobody ever ran one", and reading the second as the
   first is the filter's exact false-negative. So the remainder carries **(a) how many facets were
   found by RUNNING, AND (b) whether this system was EVER exercised under concurrency / a live probe
   at all.** A `0-found / never-run` remainder is `UNKNOWN completeness`, NOT `complete` — the
   incompleteness signal is only meaningful once (b) is yes.
2. **Citations anchor to a SYMBOL or a SECTION, never a bare line** — and a checker that only asks
   "does the citation resolve?" catches a DELETED line while missing a REWRITTEN one, silently
   preserving a VERIFIED over changed semantics
   (`[[lesson-cite-sections-not-lines-in-files-you-also-edit]]`). A row whose cited region changed
   goes **STALE**, not green: it fails into unknown, never into pass.
   **And the anchor is a (file, symbol) PAIR, never a bare symbol** — measured while self-checking
   this very document: `OnVerbEntry` resolves first into `kerfur_form_assembler.h` and `freshBirth`
   into `drive_sync.h`, neither of which is the cited lane. A bare-symbol checker would have reported
   both rows green against the wrong file, which is the same shape as every instrument bug the
   readiness pass hit: the error sits in what the tool considers its own input, so the tool agrees
   with itself.
3. **No percentage at this granule.** Counts are honest (this system has N facets, K verified);
   a percentage is not, because the denominator is the unenumerable set. The README's percentages
   stay where they have a denominator — the class register.

**TWO axes, never one column.** The first draft of this document used a single status and it broke on
its own first instance: facet #4 was `RED` on an INFERENCE while facet #6 sat in `OPEN` on the only
actually MEASURED divergence in the system. A single column was encoding verdict and evidence at once
and ranking them against each other (`[[lesson-split-fused-options-before-comparing-architectures]]`).

| axis | values |
|---|---|
| **VERDICT** — what is true of the behaviour | `WORKS` · `BROKEN` · `NOT BUILT` · `UNKNOWN` |
| **EVIDENCE** — how we know, independent of the verdict | `hands-on` (artifact path, a human played it) · `log` (a matching real log) · `selftest` (an autonomous e2e / digest run passed — below `log`, above `code`; earns neither hands-on nor log per the rule) · `code` (read from source) · `inference` (reasoned, `[RD]`) · `none` |

A verdict is only as good as the evidence beside it, and the pairing is the point: `BROKEN/inference`
and `BROKEN/hands-on` are not the same claim, and `UNKNOWN/log` (we ran it and the path never
executed) is not the same as `UNKNOWN/none` (nobody looked). **No autonomous smoke, e2e selftest or
harness run ever earns `hands-on`** — that is the rule `tools/verified_takes.tsv` already enforces. A
passed autonomous run is not thrown away, though: it lands on the `selftest` value (added §4, the
meadow profile) — strictly more than reading source, strictly less than a real log, and never
`WORKS/hands-on`.

**A third per-facet axis: AUTHORITY — who owns the write.** Surfaced round 8 by the same
falsification the other axes earned: container facets #1 (host→client mirror) and #2 (client→host
extraction) are IDENTICAL on verdict, evidence and sync-lane (`WORKS/hands-on/present`) yet are not
the same facet — #1 is host-authored and the client mirrors, #2 is client-authored and the host
validates. Nothing in verdict/evidence/lane distinguishes them; only WHO authored the write does. And
authority is the central architectural property of the whole project (`docs/COOP_SYNCER_MODEL.md`: the
host runs the arbiter today, a per-element syncer later), so a profile that cannot sort facets by it
cannot answer "which facets does a dedicated-server migration touch?"

| axis | values |
|---|---|
| **AUTHORITY** — who owns the write | `host-authored` (client mirrors) · `client-authored` (host validates) · `arbiter` (host CAS over contested writes) · `co-authored` (either peer authors via the wire, a CRDT merge converges, no owner and no arbiter) · `peer-private` (never shared) · `none` (convergent local, no owner) |

`none` does NOT mean "independent / safe": a convergent-local facet is only correct while its INPUT is
identical on both peers. That input can be a deterministic shared value (the lamp post's day/night
clock — identical by construction) OR a wire-synced facet from THIS same table (container #6 derives
its volume from the host-authored content #1). So a `none` facet INHERITS a dependency on the facet it
derives from — readable as a relationship between two rows, not a split of the `none` value (round 10
checked: two `none` facets, lamp #1 `WORKS/code` and container #6 `BROKEN/log`, differ on verdict and
evidence, so the falsification test yields no sub-axis). Read `none` + its input row together.

---

## 1. Container (`Aprop_container_C` — 60 classes in the subtree)

Lane: `container_contents_sync.cpp` (853 LOC). Contents live in ONE global `saveSlot.GObjStack`,
addressed by `propInventory.index` (`[[lesson-container-contents-live-in-one-global-gobjstack]]`).

| # | facet | verdict | evidence | authority | citation (symbol / section) |
|---|---|---|---|---|---|
| 1 | host -> client contents mirror (`addObject` receive) | **WORKS** | `hands-on` | `host-authored` | `research/handson_runbook_2026-07-22_container_v125.md` — the user saw the burgers |
| 2 | client -> host extraction, upward (`takeObj`) | **WORKS** | `hands-on` | `client-authored` | same runbook — shipped 2/1/0 matched applied 2/1/0 on both peer logs |
| 3 | simultaneous grab by two peers (the CAS) | **UNKNOWN** | `log` | `arbiter` | `HostAcceptsClientWrite()` in `container_contents_sync.cpp`. `CONFLICT=0` on BOTH peers in the 07-22 take: the arbitration never executed once. Unexercised, not proven and not broken |
| 4 | the extracted item reaching the host's world | **BROKEN** | `inference` | `client-authored` | the `freshBirth` guard in `prop_drop_intent.cpp` (`if (!parked && !freshBirth) continue;`). `[RD]` not `[V]`: the log cannot separate "dropped at the guard" from "never enqueued" — both are silence |
| 5 | nested container-in-container | **NOT BUILT** | `code` | `arbiter` (intended) | the `BOUNDARY 2` comment block in `container_contents_sync.cpp` — a nested container's `ints[0][0]` is a sender-side slot index, meaningless on the receiver. Receiver-side bounds needed first |
| 6 | volume/mass re-derivation on extraction | **BROKEN** | `log` | `none` | **NOT host-authored** — `container_contents_sync.cpp` `ResolveRederiveFns` (the "we never raw-write currVol / Mass" comment, ~:430) states each peer RE-DERIVES the volume LOCALLY from the already-synced content via the engine verb `updateVolumesAndMass`. Convergent-local like the lamp post, no owner. The BROKEN root is that the local re-derive did not RUN (the verb resolved null off the instance class — `FindFunction` doesn't walk the superclass chain, `reflection.cpp:427`), NOT that a host stream was missing. A declaring-class fix exists (`411743af`); its effect on this facet is UNVERIFIED against the 07-22 stale-`currVol` take, so the verdict stays at the last MEASURED state |
| 7 | slot 0 — the player's personal container | **NOT BUILT** | `code` | `peer-private` | `IsWorldContainerInventory`, fail-closed today. A privacy/product fork and a BOUNDARY 1 redesign; gate:user |
| 8 | the INSERT direction (`putObjectIn_overlap`) | **UNKNOWN** | `none` | `client-authored` (intended) | nobody has looked; no claim either way |
| — | **remainder — the list is open** | **UNKNOWN** | — | — | (a) **3 of 8 facets (#3, #4, #6) were found by RUNNING**, not by reading source — so the majority of what is wrong here was invisible to code reading. (b) **Concurrency WAS exercised** (the 07-22 take produced `CONFLICT=0`), so this system's incompleteness signal is meaningful — but the ONE interleaving that ran did not trigger the CAS, so even here (b) is "run once, not run adversarially" |

**Count, not percentage:** 8 facets — 2 `WORKS`, 2 `BROKEN`, 2 `NOT BUILT`, 2 `UNKNOWN`; by evidence,
2 `hands-on`, 2 `log`, 2 `code`, 1 `inference`, 1 `none`.

**Two rows earn the split axis immediately.** #6 is the system's highest-confidence bad news —
`BROKEN` on a measured cross-peer divergence — and the single-status draft had it filed under `OPEN`,
below an inferred one. #3 is the opposite shape: `UNKNOWN/log` means we ran it and the code path
never executed, so every code-derived view of this system reads the lane GREEN while the arbitration
it exists to perform has never once run. Neither row is expressible in a class, verb or lane profile.

---

## 2. Weather — the FORM-AGAINST-A-VERB-LESS-SYSTEM control

Chosen as the second control precisely because it is the OPPOSITE shape from the container: almost no
verb interception, sync is field-state + reliable messages, and the system spans **five files**
(`weather_sync.cpp` orchestrator + `weather_rain` / `weather_fog` / `weather_lightning` /
`weather_redsky`). If the split axis fills here, the form is not container-shaped; if a verb-less
system would not fill at all, the form names its own limit. It filled — and the EVIDENCE axis
immediately did work: it separated a MAP `[V]` from a player `hands-on`, which a single status column
would have flattened to one green.

| # | facet | verdict | evidence | authority | citation (symbol / section) |
|---|---|---|---|---|---|
| 1 | rain / snow scalar mirror | **WORKS** | `log` | `host-authored` | client apply line `weather: applied flags 0x1D -> 0x1C ... rain-tx=1 scalars-changed=1` in `docs/piles/test-evidence/handson-s31-doom-CLIENT.log`. This is a REAL matching log, NOT the map `[V]` — see the correction note below |
| 2 | red sky | **UNKNOWN** | `code` | `host-authored` | `ReliableKind::RedSky` in `weather_redsky.cpp` — the lane exists. NO apply/receive line in any test-evidence CLIENT log (grepped). `COOP_SYNC_MAP.md`'s `[V]` is a MAP verdict, and the readiness pass discredited doc-status parsing (6 vs 2, both directions) — it is not admissible as evidence here |
| 3 | lightning strike | **UNKNOWN** | `code` | `host-authored` | `ReliableKind::LightningStrike` in `weather_lightning.cpp` — lane exists, host broadcasts strike loc. NO client receive line in the logs (grepped). Same map-`[V]`-inadmissible note |
| 4 | fog (host-authoritative) | **UNKNOWN** | `code` | `host-authored` | `weather_fog.cpp` — host-clear heartbeat (MTA `CBlendedWeather::DoPulse` precedent), client backstop destroys stray rolling-fog. Built s25, **smoke only** — and per §0 a smoke earns neither `hands-on` NOR `log`; the lane exists, its behaviour is unobserved |
| 5 | wind | **BROKEN** | `log` | `host-authored` | `changeWindOrigin` PRE-interceptor client-suppresses the gust roll, host streams `windTarget`; `COOP_SYNC_MAP.md` records "wind desync under live probe — INSTRUMENTED, not diagnosed". The verdict is BROKEN from the live probe; the ROOT is undiagnosed |
| — | **remainder — the list is open** | **UNKNOWN** | — | — | (a) **0 facets found by RUNNING** — but (b) **weather was NEVER exercised under concurrency**; the wind bug came from a live SINGLE-flow probe, not an interleaving. So this 0 is `UNKNOWN completeness`, NOT "nothing missed" — reading it as complete would be the marker-filter's false-negative. weather's RNG knob jitter (`COOP_RNG_AUTHORITY.md:157`) is a MECHANIC input neutralized by the host stream, deliberately not a row |

**Count:** 5 facets — **1 `WORKS`, 1 `BROKEN`, 3 `UNKNOWN`**; by evidence, **0 `hands-on`, 2 `log`,
3 `code`**.

**THE CORRECTION IS THE FINDING (round 4).** The first draft of this table read "4 `WORKS`, 1
`BROKEN`" — I had assigned `WORKS/log` to facets 1-4 from `COOP_SYNC_MAP.md`'s `[V]` markers, which
are MAP verdicts, exactly the doc-status source the readiness pass measured as unreliable in both
directions. Applying the EVIDENCE axis STRICTLY — demand a real matching log line, not a map marker —
collapsed THREE false greens: only facet 1 has an actual client apply line; redsky/lightning/fog have
a lane in code and no observed behaviour. This is the two-axis form doing precisely the job it exists
for, on its author: **a map `[V]` is `code`-tier at best, never `log`, and never `WORKS` on its own.**
The spine being the SYSTEM (not the file) is re-confirmed: no file-keyed row could name "wind" as one
facet, since its logic is split across the interceptor and the orchestrator's stream.

**Meadow probe (Q4) — now PROMOTED to the full profile in §4.** The probe established that "facet"
resolves for a 0-class save-CRDT (the "0 classes" was the CXX-dump CLASS count; the system lives as a
save-struct CRDT and its facets are OPERATIONS — append / delete / order — not classes). §4 completes
it into a five-facet profile and, in doing so, earned the `selftest` and `co-authored` values. That a
0-class system yields facets confirms rows = facets, spine = system — a behaviour needs no class to
exist.

---

## 3. Lamp posts — the WE-SYNC-ZERO control (the other half of the ask)

The user asked for "что синхроним И что НЕ синхроним". The first two controls are systems we DO sync.
This third is the untested half: a VOTV system we sync **zero** facets of, chosen because a
fully-unsynced system exercises two things neither prior control did — (a) does the form MOVE on a
third, different-shaped control (round-5 Q1: convergence is a control that changes nothing), and
(b) can the form even EXPRESS "we sync nothing here, and that is correct"?

`AlampPost_C` — outdoor lamp posts, `NOT synced` per `COOP_SCOPE.md` (§ Out of scope): the game's
day/night cycle (`mainGamemode` tracks `allLampPosts`) runs identically on both peers, so lamps
toggle in lockstep with no wire traffic. RE-confirmed in the Phase 5D doors+lights pass 2026-05-25.

| # | facet | verdict | evidence | authority | citation (symbol / section) |
|---|---|---|---|---|---|
| 1 | day/night lamp toggle | **WORKS** | `code` | `none` | `AlampPost_C` driven by `mainGamemode`'s `allLampPosts` day/night cycle; RE pass 2026-05-25. Convergent local computation on both peers — correct WITHOUT a wire lane. Authority `none`: no peer owns the write, both compute it |
| — | **sync lane** | **NONE — and correct** | — | — | zero facets carry a wire lane, BY DESIGN, not by omission. The reason is convergent local compute, not a deferred gap — see the form note below |
| — | **remainder — the list is open** | **UNKNOWN** | — | — | (a) 0 facets found by running; (b) never exercised under concurrency. But here the completeness question is narrower: the only risk is that the two local cycles DESYNC (a clock drift), which no run has checked |

**THE FORM MOVED — a third time, and this is the round-6 finding.** The lamp post exposed that the
verdict `NOT BUILT` was fusing two states the way the single status column fused verdict and evidence:

- `NOT BUILT / gap` — we should sync it and have not (container facets 5, 7: nested, slot 0).
- `NOT BUILT / needs none` — we will NEVER sync it because it is correct by construction (lamp posts).

A reader seeing `NOT BUILT` on both cannot tell an owed lane from a lane that must never exist.
So the **SYNC-LANE** state is split: `present` / `none-by-design (correct)` / `none-but-owed (gap)`.
This is a genuine form change, not a data fix — which means the running rate is now **3 controls, 3
form changes** (split axis / two-field remainder / sync-lane trichotomy). **The form is NOT
converged**: every control profiled so far has moved it. Convergence is a control that moves nothing,
and I do not yet have one. See §5 — but §4 (meadow) is the closest yet: it moved only VALUES, not
axes, which is the first control to leave the axis set alone.

---

## 4. Meadow — the SAVE-CRDT control (the hardest test of "facet", and the first that moved no axis)

Chosen as the fourth control because it is a fourth distinct SHAPE: not verb-driven like the container,
not field-state like weather, not a convergent-local clock like the lamp — a **save-struct CRDT** with
ZERO game classes of its own. The DB lives entirely in `saveSlot.savedSignals_0`; its facets are
OPERATIONS on that struct, not classes. If the five-axis form survives a system with no class and no
verb-shaped facet, the spine claim (system, not class/verb) holds under its hardest case. It survived —
and it grew the vocabulary by TWO new VALUES with NO new axis, the "converging" signal §5 defines.

Lane: `meadow_db_sync.cpp` (884 LOC). Three reliable kinds — `MeadowAppend` / `MeadowDelete` /
`MeadowOrder`; convergence is a broadcast-acknowledged multiset shadow (`g_shadow`, ContentHash→count)
plus tombstones (`g_tombs`) for the delete-vs-append race.

| # | facet | verdict | evidence | authority | citation (symbol / section) |
|---|---|---|---|---|---|
| 1 | append a signal line | **WORKS** | `selftest` | `co-authored` | `AuthorLine` → `ApplyAppendBlob` + `MeadowAppend`. The [dev] `SelftestTick` injects a synthetic row and `LogDigest` asserts 0→1 cross-peer. Either peer authors; the multiset merges with no host validation — see the new-value note |
| 2 | delete a line | **WORKS** | `selftest` | `co-authored` | `ApplyDeleteByHash` / `OnDelete` + `MeadowDelete`; `g_tombs` covers a delete arriving before its append. The selftest's 1→0 remove closes the same e2e |
| 3 | reorder (`sortSignal`) | **UNKNOWN** | `code` | `arbiter` | `ApplyOrderBlob` + `MeadowOrder`, host-canonical LWW (a client order line is dropped unless `senderSlot==0`; the host applies any peer's move and re-broadcasts ITS canonical). **The selftest cannot reach this facet:** `LogDigest`'s sum is `h*count`, order-INDEPENDENT by construction (~:226) — a permutation leaves the digest identical, so the e2e that proves append/delete is BLIND to order. Lane exists, behaviour unobserved |
| 4 | join-seed into a populated DB | **UNKNOWN** | `code` | `host-authored` | `CaptureJoinSnapshot` / `QueueConnectBroadcastForSlot`, `seedDelta = cur − snap − unmaskedPendingNet`. The mid-join answer (principle 8). The selftest injects AFTER both peers connect, so it exercises live-append-cross-peer, never a join INTO a pre-populated store — this lane is code-only |
| 5 | concurrent append/delete merge | **UNKNOWN** | `code` | `co-authored` | the multiset shadow + `g_tombs` in `ApplyAppendBlob`. This is the CRDT's whole reason to exist and — exactly like the container CAS (#3) — has NEVER run: the selftest is a single sequential host inject/remove |
| — | **remainder — the list is open** | **UNKNOWN** | — | — | (a) **2 of 5 facets (append, delete) were exercised by RUNNING** (the autonomous e2e); the other 3 only read. (b) **never exercised under CONCURRENCY** — the selftest is one sequential host flow, so the merge (#5), the order LWW contest (#3), and a real join-seed (#4) are all un-run. A 3-peer RELAY question is also open: a CLIENT append reaches the host (`ApplyAppendBlob` does not re-broadcast), so whether a client's line reaches OTHER clients before a join-seed is unverified |

**Count:** 5 facets — 2 `WORKS`, 3 `UNKNOWN`; by evidence, 2 `selftest`, 3 `code`. All 5 labels are
code-verified this pass (the whole lane was read).

**TWO new VALUES, ZERO new axes — the convergence signal.** Meadow is the first control whose facets
did not fit the existing vocabulary yet needed no new QUESTION asked of a facet:

- **`selftest` (EVIDENCE).** The prior three profiles never needed a value between `code` and `log` —
  their facets were hands-on, real-log, or unobserved. Meadow's append/delete are proven by an
  autonomous e2e digest, which §0's rule earns NEITHER hands-on NOR log, yet is strictly MORE than
  reading source. Under the old vocabulary append (e2e-proven) and order (never run) both read `code`,
  collapsing "an e2e proved it" with "nobody ran it" — the two-collapsed-rows falsification that earns
  the value. Ordering of the tier: `hands-on` > `log` > **`selftest`** > `code`.
- **`co-authored` (AUTHORITY).** Meadow append is authored by EITHER peer, wire-synced, and converges
  via the multiset+tombstone merge with NO host validation or rejection — not `host-authored` (a client
  authors too), not `client-authored` (the host authors too), not `arbiter` (nothing CAS-gates an
  append; contrast the meadow ORDER facet, which IS `arbiter` — host-canonical LWW), not `none` (it has
  a wire lane, unlike the convergent-local lamp). Append (`co-authored`) vs order (`arbiter`) are two
  facets of ONE system that collapse without the value — the falsification that earns it.

Both are new VALUES on existing axes, not new axes. Per §5's gate that is normal taxonomy growth, and
on a system as differently-shaped as a class-less CRDT it is the strongest evidence yet that the FIVE
axes are the right frame: a fourth control moved the vocabulary at its margins without adding a
question. It does not PROVE completeness (the standing gate forbids that) — but it is the first control
to add zero axes, which is precisely the signal §5 was waiting for.

---

## 5. Convergence state — the AXES are settled; VALUES stay open (that is correct)

The first read of the 3/3 rate was "not converged, keep profiling until a control changes nothing."
That test is WRONG, and measurement shows why: a control that changes NOTHING will never exist for a
qualitative taxonomy — the vocabulary meets reality at the margins forever, so demanding it stop is
demanding it stop meeting reality. The right convergence test is narrower:

> **Does a new control add a new VALUE to an existing axis, or a whole new AXIS?**
> New values are normal and expected (a taxonomy grows). A new AXIS means the model was incomplete.

Measured against that test, the changes were each a new **axis**, not a value. The axis set now stands
at FIVE, mapping onto five distinct questions a sync facet raises:

| axis | the question it answers |
|---|---|
| **VERDICT** (WORKS/BROKEN/NOT BUILT/UNKNOWN) | does it work? |
| **EVIDENCE** (hands-on/log/selftest/code/inference/none) | how do we know? |
| **REMAINDER** (found-by-running × ever-run) | is the facet list whole? |
| **SYNC-LANE** (present / none-by-design / none-but-owed) | should it carry a lane at all? |
| **AUTHORITY** (host-authored/client-authored/arbiter/co-authored/peer-private/none) | who owns the write? |

Two of those values — `selftest` (EVIDENCE) and `co-authored` (AUTHORITY) — were added by the FOURTH
control (meadow, §4), each earned by a two-collapsed-rows falsification. They are new VALUES, not new
axes: meadow is the first control to leave the axis SET at five.

The completeness check the project requires
(`[[lesson-a-unit-of-measure-must-express-the-known-red-case]]`): every known measured-red this pass
hit maps into the vocabulary with no leftover — `currVol` and wind = `BROKEN/log`; the never-run CAS =
`UNKNOWN/log`; the lost extracted item = `BROKEN/inference`; the correctly-unsynced lamp post =
`WORKS/code` + `lane=none-by-design`.

**Convergence call — deliberately WEAKER than round 7's.** Round 7 declared "four axes, complete, no
fifth question apparent." Round 8's falsification produced a fifth (authority) IMMEDIATELY, using the
project's own two-collapsed-rows method — so the completeness claim has now FAILED ONCE, and asserting
"five is complete" from the same reasoning that just failed at four would be laundering an inference
the evidence contradicts. **So the call is: FIVE axes is the CURRENT model, and its completeness is
NOT claimed.** The falsification test is the standing gate — any future system that produces two facets
identical on all five axes but distinct in kind reveals a sixth. That the axes went 4→5 under one
round's scrutiny is the reason to ship the model as OPEN, not to keep hunting a closure the taxonomy
may not have.

**Update after the LABOR pass (meadow, §4): the first control that added no axis.** The three prior
profiles each ADDED an axis (the 3/3 rate §3 recorded). Meadow — a fourth, deliberately different shape
(a class-less save-CRDT) — added two new VALUES (`selftest`, `co-authored`) and NO axis: every meadow
facet mapped onto one of the five existing questions. Under the correct convergence test (new value vs
new axis) that is the FIRST positive signal for five-axis completeness. It still does not license the
completeness CLAIM — one non-adding control is not proof, and the two-collapsed-rows gate stays open for
a sixth — but the axis set surviving its hardest structural case (no class, no verb) is the strongest
evidence the frame is right.

**Why this is a ship-able deliverable, not an open loop:** the SETTLED CORE never moved across any
round — spine = the SYSTEM; rows = FACETS, hand-named, set not machine-enumerable; citations =
(file, symbol); no percentage at this granule. The AXES are a growing-but-falsifiable vocabulary with
a named test, not a guess. The user asked to NAME what we sync and don't, per system — four profiles
now do that, each internally complete on all five axes (the authority column was back-filled the same
round it was found, not deferred: a doc declaring five axes while showing four would be the very
"unnamed boundary reads as green" trap this document codifies in §0). The remaining work is LABOR
(more systems) and the class-register verdict/evidence split (named in the README). Neither is
blocked; both are hand-off-able.

**No sixth axis is proposed.** A timing/rate axis was considered and DROPPED: the standing gate
requires a falsification instance (two facets identical on all five axes, distinct only in the
candidate dimension) and none exists for timing. Proposing an axis without it would be the
closure-hunt round 7 named.

---

## 6. Systems not yet profiled

Adding a system means authoring its facets by hand, with a citation each and a remainder row — see
§0. Do not seed rows from a grep; the prior pass measured that a syntactic proxy for "is this synced"
errs on both sides and only the false-negative side is measurable. Four shapes are now covered —
verb-driven (container), field-state (weather), convergent-local (lamp), and a class-less CRDT
(meadow). Next candidates by CONTRAST value (each should stress a different axis, not repeat a shape):

- **A held/per-player-inventory system** — to stress the `peer-private` authority value with a real
  profile rather than a single container facet (#7). `[[lesson-held-collected-prop-is-per-player-inventory-not-a-world-actor]]`.
- **A host-simulated NPC** (kerfur pose stream) — a continuous host-authored stream, to test whether a
  rate/timing axis finally earns its falsification instance (the one §5 dropped for lack of a case).
- **An event lane** (`docs/events/`) — the late-join answer table is per-lane; a profile would test
  whether "mid-join answer" is a sixth axis or a facet of every system's remainder.
