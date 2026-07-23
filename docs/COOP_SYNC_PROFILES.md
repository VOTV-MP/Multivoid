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

**Beyond these four DETAILED worked examples, §6–§7 hold the FULL CATALOG** — a 2026-07-23 sweep of the
whole `coop/` tree (8 fact-gatherers + a gap-fill pass), ~64 systems and ~210 facets at a compact tier. That
catalog is looser: its `HO` means "observed in hands-on play" (weaker than the README's auditable
`verified_takes.tsv` "2"), and the large majority of mechanic facets sit at `U/code` — a lane exists,
never observed. The §6 master table is the scannable overview; the four profiles here remain the place
where each label's rationale is spelled out.

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
| **AUTHORITY** — who owns the write | `host-authored` (client mirrors) · `client-authored` (host validates) · `arbiter` (host CAS over contested writes) · `co-authored` (either peer authors via the wire, a CRDT merge converges, no owner and no arbiter) · `peer-owned` (each peer authors its OWN disjoint slice — its body, its entity, its mic — streamed to all, no contention and no arbiter) · `peer-private` (never shared) · `none` (convergent local, no owner) |

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
| **AUTHORITY** (host-authored/client-authored/arbiter/co-authored/peer-owned/peer-private/none) | who owns the write? |

Two of those values — `selftest` (EVIDENCE) and `co-authored` (AUTHORITY) — were added by the FOURTH
control (meadow, §4), each earned by a two-collapsed-rows falsification. A third — `peer-owned`
(AUTHORITY) — came from the full-catalog sweep (§6–§7). All three are new VALUES, not new axes: the axis
SET has stood at five across four worked shapes AND an exhaustive ~64-system sweep.

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

**Update after the FULL-CATALOG sweep (§6–§7, ~64 systems, ~210 facets): ONE new VALUE, still NO new
axis.** The whole-codebase sweep produced exactly one value the four worked examples never needed —
`peer-owned` on the AUTHORITY axis (each peer authors its OWN disjoint slice — body, entity, mic —
streamed to all, no contention, no arbiter; distinct from `peer-private`=never-shared and
`co-authored`=shared-contested, and it surfaced a real mislabel: streamed body pose had been filed
`peer-private`, which §0 defines as "never shared"). Every one of ~200 facets otherwise mapped onto the
five existing questions with a value already in the vocabulary. So across four deliberately-different
shapes AND an exhaustive sweep, the AXIS count has held at five while the VALUE set grew by three total
(`selftest`, `co-authored`, `peer-owned`) — exactly the "taxonomy grows at its margins, the questions do
not" pattern §5's test predicts for a converged frame. Completeness is STILL not claimed (the gate stays
open), but a full sweep adding no axis is the strongest evidence yet obtainable short of a proof.

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

## 6. Master summary table — every synced system, one row

Derived from the full facet catalog in §7 (2026-07-23 sweep: 8 read-only fact-gatherers over the whole
`coop/` tree, verdicts authored here). **This is a COUNT view, never a percentage** — the facet
denominator is unenumerable (§0). Read a row as "this system has N facets in these states", not "N% done".

**Legend.** Verdict `W`=WORKS · `B`=BROKEN · `U`=UNKNOWN (lane exists, behaviour unobserved) · `NB`=NOT
BUILT. Evidence `HO`=hands-on · `log` · `ST`=selftest/e2e · `code` · `inf` · `–`. Authority `HA`=host-authored ·
`CA`=client-authored · `ARB`=arbiter · `CO`=co-authored · `PO`=peer-owned (own disjoint slice, streamed) · `PP`=peer-private (never shared) · `∅`=none/convergent-local. `recv-local` in a catalog cite = a receiver-side derivation of a `PO` stream (authority `∅`, inherits its input row).

> **The evidence bar in this catalog is DELIBERATELY LOOSER than the README's `verified_takes.tsv` "2".**
> `HO` here means "observed working in hands-on play" — for the visible foundation (remote bodies, chat,
> voice, join) that is everyday-observed, and for a mechanic it is a dated runbook where one exists. The
> README's "2 verified" counts ONLY formally-logged takes and is the stricter, auditable number. When a
> take-4 (2026-07-21) hands-on showed a BUG and a fix shipped after but was NOT re-played, the verdict
> stays at the last MEASURED state (`B/HO`) with the fix noted — the same "last measured" rule as
> container #6. Most mechanic-specific facets have no observation at all and sit at `U/code` — that
> majority IS the honest state, and it is what a class-level percentage hides.

| System | shape | facets | verdict mix | best evid | authority | mid-join |
|---|---|---|---:|---|---|---|
| **Physics props** | prop pipeline | 11 | 6W · 5U | HO | HA/CA/CO/PP/ARB | snapshot |
| **Container** (§1) | verb-shaped | 8 | 2W · 2B · 2NB · 2U | HO | mixed | snapshot |
| **Chip-pile / clump** (grab-carry-throw) | prop+intent | 12 | 10W · 2U | HO | HA/ARB | snapshot + bind |
| **Trash-bits pile** (counter) | int-pair channel | 5 | 1W · 4U | code | CO/HA/∅ | snapshot |
| **Kerfur** (NPC⇄prop convert + skins) | host-sim + verb | 9 | 4W · 5U | HO | HA/PP | snapshot + adopt |
| **NPC host-sim** (generic creatures) | host-sim stream | 5 | 4W · 1U | HO | HA | snapshot |
| **Owner-entity** (eyer) | per-peer mirror | 3 | 3U | code | **PO** | keepalive |
| **Roach** | paged snapshot | 2 | 2U | code | HA/ARB | snapshot |
| **Wisp** (killer) | host-sim + event | 8 | 2W · 6U | HO | HA | none (transient) |
| **Pyramid** | host-sim + event | 4 | 3W · 1U | HO | HA | world-actor snap + replay |
| **Firefly** | cosmetic spawn | 1 | 1U | code | PO | none |
| **Drone** (delivery) | host singleton | 3 | 3U | code | HA | snapshot |
| **Desk-input / console** | field-delta + streams | 10 | 5B · 5U | HO | CO/HA/CA/PP | seed (adopt) |
| **Dish** | host theater + arm | 5 | 1B · 4U | HO | HA/CO/PP | snapshot |
| **Comp-processing** | single-sim | 3 | 1B · 2U | HO | CO/PP | adopt |
| **Device-occupancy** | claim table | 3 | 1W · 2U | HO | **ARB** | snapshot (table) |
| **Signal-catch** | intent → host replay | 4 | 4U | log | CO/HA | seed (kind=2) |
| **Playback-deck** | edge + gen-guard | 3 | 2W · 1U | ST | CO/PP | none |
| **Drive-chain** (drives) | slot + payload | 3 | 3U | code | HA/CO/CA | seed |
| **Drive-rack** | 16-slot CAS | 3 | 3U | code | **ARB**/HA | seed canonical |
| **Phys-mods** | array CAS | 3 | 3U | code | **ARB**/HA | seed canonical |
| **Tape-caddy** | slot + corrector | 3 | 3U | code | CO/HA | seed = save |
| **Laptop** (power/floppy/disc) | op-lane + blob | 5 | 5U | code | CO/HA | seed |
| **Laptop-buffer-quad** | edit-script CAS | 1 | 1W | ST | **ARB** | seed canonical |
| **Floppybox** | LIFO CAS | 2 | 2U | code | **ARB** | seed canonical |
| **Serverbox** | one-way mirror | 2 | 2U | code | HA/PP | snapshot |
| **Saved-signals store** | save-CRDT (no order) | 2 | 2U | code | CO | snapshot (save blob) |
| **World-actor mirror** | host mirror | 6 | 3W · 3U | HO | HA | replay / seed |
| **Meadow-DB** (§4) | save-CRDT | 5 | 2W · 3U | ST | CO/ARB/HA | seed |
| **Remote-player body/ragdoll** | pose stream | 8 | 4W · 4U | HO | **PO** | spawn = seed |
| **Nameplate + nick-color** | composite + pref | 3 | 1W · 2U | HO | ∅/CO | recompute / at-join |
| **Skins** | name-carried | 5 | 3W · 2U | HO | PO/CO | at-join |
| **Hand-item / item-activate** | identity + pose | 6 | 3W · 3U | HO | CO/PO/CA | replay |
| **Player-inventory** | per-peer persist | 3 | 1W · 2U | ST | **PP**/HA | seed pre-world |
| **Sleep** | tally arbiter | 4 | 4U | code | ARB/HA | join-awake |
| **Player-damage / hazards** | edge + vitals | 4 | 2W · 2U | HO | HA/PO | none |
| **Puppet** (orphan drive) | receiver drive | 4 | 2W · 2U | HO | ∅/HA | world-ready gate |
| **Sky** (day-night) | host mirror | 1 | 1U | code | HA | snapshot |
| **Time-of-day** | host clock | 3 | 3U | code | HA/CO | seed |
| **Alarm** (klaxon) | 1-bit channel | 1 | 1U | code | CO | snapshot |
| **Balance** (money) | host + delta | 2 | 2U | code | HA/ARB | connect-replay |
| **Daily-task** | host state | 1 | 1U | code | HA | baseline-first-sight |
| **Email** | append + delete | 2 | 2U | code | HA/CA | save-transfer + prime |
| **Doors / keypads / locks** | channel family | 7 | 4W · 3U | HO | HA/CO/ARB | snapshot |
| **Lights** | switch channel | 1 | 1U | code | CO | snapshot |
| **Turbine** | float channel | 1 | 1U | code | HA | snapshot |
| **Power panel** | mask channel | 1 | 1U | code | CO | snapshot |
| **Grime** | decrease-only min | 2 | 2U | code | CO | snapshot |
| **Window-cleaning** | decrease-only min | 1 | 1W | ST | CO | snapshot |
| **Garbage-chute** | client suppress | 2 | 2U | code | PP/HA | none |
| **ATV** | occupant pose | 4 | 4U | code | CA/HA | snapshot |
| **Shop-order** | client→host commit | 1 | 1U | code | **ARB** | watermark-prime |
| **Appliance** | 1-bit channel | 1 | 1U | code | CO | snapshot |
| **Weather** (§2) | field-state | 5 | 1W · 1B · 3U | log | HA | snapshot |
| **Lamp posts** (§3) | not-synced | 1 | 1W | code | ∅ | none (by design) |
| **World-events** | replay lanes | 3 | 3U | code | HA | replay / snapshot |
| **Chat** | line relay | 3 | 3W | HO | CA/HA/PP | none (ephemeral) |
| **Peer-action feed** | derived render | 2 | 1W · 1U | HO | ∅/PP | none |
| **Voice** | frame stream | 3 | 2W · 1U | HO | PO | replay (state) |
| **Save-transfer / join** | blob + snapshot | 8 | 6W · 2U | HO | HA | this IS the join |
| **Teleport-client** | host push | 2 | 1W · 1U | HO | HA | seed = join placement |
| **Session / join / roster** | handshake | 8 | 4W · 4U | HO | HA/PO/ARB/CA | world-ready barrier |
| **Moderation** (kick/ban) | host-local, **no wire** | 7 | 7U | code | HA | none (pre-slot reject) |
| **Save-suppression** | local lockout, **no wire** | 5 | 1W · 4U | ST | HA/PP | none |
| **Spawn-authority** | client suppress, **no wire** | 1 | 1U | code | HA | park at join window |

**What the table shows at a glance (counts, not a score):**
- **~64 systems, ~210 facets.** The single largest evidence bucket is `code` (a lane exists, never
  observed) — the honest majority §0 predicts a class-percentage would hide.
- **The verified core is the VISIBLE loop:** remote bodies, chat, voice, join/save-transfer, the
  grab-carry-throw prop economy, keypads, kerfur/NPC, wisp/pyramid — each carries `HO`. The workstation
  SIGNAL pipeline is the opposite: mostly `U/code` or `B/HO` (take-4 caught real bugs; fixes shipped
  unverified).
- **True arbiters are RARE and enumerable:** device-occupancy, drive-rack, phys-mods, floppybox,
  laptop-buffer-quad, shop-order, sleep-tally, container-CAS. Everywhere else authority is host-authored
  one-way or co-authored + host-relay — NOT the syncer-model arbiter (`docs/COOP_SYNCER_MODEL.md` is
  DESIGN, unbuilt). This table is the concrete input to "which facets a dedicated-server migration touches".
- **The sweep grew the AUTHORITY axis by ONE value — `peer-owned` — and added NO new axis.** Body pose,
  own skin, hand pose, voice frames, the eyer, fireflies: each peer authors its OWN disjoint slice and
  streams it, no contention and no arbiter. That is neither `peer-private` (§0 defines it "never shared",
  and these ARE shared) nor `co-authored` (no shared contested state — the slices are disjoint). It is the
  player/creature-mirror pattern the four §1–4 controls never hit, and it is a whole authority class the
  class register cannot see. `peer-private` proper stays for the genuinely-unshared: player-inventory,
  the garbage crash-fix, coord_isPing, the local sim-teardowns.
- **Some coop systems carry NO wire at all** — moderation (kick/ban via GNS-close + host-local files),
  save-suppression (client-local save lockout), spawn-authority (client spawner park). They enforce host
  authority "by construction", not by a packet. A wire-lane census is structurally blind to them; only a
  system-by-system read finds them (§8). This is the §0 spine claim — system, not lane — proving out, and
  it is exactly what the FIRST sweep missed until the self-audit.

---

## 7. The full facet catalog

One compact table per system (the master table's source). Citations are (file, SYMBOL); `NOT SYNCED`
lines carry the user's "что НЕ синхроним" half. Verdict discipline per §0: `code`-only ⇒ `U` (unobserved),
a take-4 bug that a later unverified fix addressed stays at its last-measured `B`.

### Physics props — `coop/props/`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | spawn host-born keyed | W | HO | HA | `GrabObserver_Aprop_Init_POST_Body` | snapshot (`prop_snapshot`) |
| 2 | spawn client-placed (drop) | U | code | CA | `OnPropDropIntent` | park-FIFO + episode gate |
| 3 | spawn client fresh-birth (reel/module/drive) | U | code | CA | `OnReelEjectIntent` | edge |
| 4 | grab / drag held-pose | W | HO | PP | `remote_prop::Tick` | mirror via snapshot |
| 5 | drop / throw release velocity | W | HO | PP | `remote_prop::OnRelease` | none (edge) |
| 6 | destroy | U | code | CO | `remote_prop::OnDestroy` **[trust gap: no arbiter]** | park / replay |
| 7 | stick-to-surface | U | code | CO | `OnStickState` | snapshot (physFlags) |
| 8 | convert pile⇄clump | W | HO | HA | `remote_prop::OnConvert` | PropSnapPos reconcile |
| 9 | chip-pile grab/throw request | W | HO | ARB | `trash_channel::OnGrabIntent` | staged |
| 10 | join-window position correction | W | log | HA | `event_dispatch_entity::PropSnapPos` (F1 probe) | THIS is the correction |
| 11 | identity stability (save/rejoin) | U | code | HA | `CreateOrAdoptPropMirror` / `MarkPropElement` | seed + sweep |

NOT SYNCED: local fall/free-sim physics (intentional SP-parity); pose while NOT held (divergence tolerated); a purely client-placed Aprop never picked up (`[ROCK-DROP]` host won't see); growing classes (mushroom7, host-auth); destroy anti-tamper (security A3/A4); mid-flight thrown prop for a joiner.

### Chip-pile / clump (grab-carry-throw) — `coop/props/trash_*`, `native_pile_mirror`, `pile_spawn_bind`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | host grab → clump | W | HO | HA | `trash_channel::NoteClumpBorn` | snapshot |
| 2 | client grab request | W | HO | ARB | `trash_channel::OnGrabIntent` | none |
| 3 | carry pose (hand-follow) | W | HO | HA | `puppet_carry_drive::Tick` | freeze-on-gap |
| 4 | client throw | W | HO | ARB | `trash_grab_intent::OnThrowIntent` | none |
| 5 | re-pile (land → chipPile) | W | HO | HA | `trash_channel::OnHostConvert` | key reconcile |
| 6 | carry termination (consumed/rest) | W | HO | HA | `trash_channel::TickCarry` | n/a |
| 7 | ctx ordering (stale-convert guard) | W | HO | HA | `trash_channel::AdoptInboundConvertCtx` | sentinel |
| 8 | client proxy render | W | HO | HA | `trash_proxy::SkinProxy` | on inbound spawn |
| 9 | save-loaded pile bind | W | HO | HA | `pile_spawn_bind::FindAndConsumeAdoptCandidate` | THIS is the reconcile |
| 10 | native pile materialize | U | code | HA | `native_pile_mirror::Materialize` | bind-or-materialize |
| 11 | client authority suppression | W | HO | HA | `trash_collect_sync::EnsureHeldItemBroadcast` | join-window gate |
| 12 | grab-holder disconnect | U | code | HA | `trash_grab_intent::OnGrabHolderLeft` | per-slot retire |

NOT SYNCED: `PileResyncRequest` reserved, NO handler (STAGED); a client-THROWN ground pile stays client-local (phase-2 gap, benign); proxy occlusion / walk-through (regression); an unbound-native ghost is made non-interactable; ambiguous 1 cm native cluster (`TryDestroyTwin` skips).

### Trash-bits pile (dispenser counter) — `coop/props/trash_pile_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | counter decrement | U | code | CO | `ApplyToLive` (MIN merge, no arbiter) | snapshot |
| 2 | host adopt-snapshot | U | code | HA | `OnReliable` (adopt gated slot 0) | snapshot |
| 3 | deferred apply (pile not streamed) | U | code | CO | `Tick` (pending 25 s TTL) | park |
| 4 | depletion → destroy | U | code | CO | `Tick` death-watch → `PropDestroy` | replay (`g_depletedKeys`) |
| 5 | pile index identity | W | code | ∅ | `RebuildIndex` (keysHash) | rebuild (convergent-local, correct) |

NOT SYNCED: the dispensed `Aprop` item (rides the generic prop pipeline); counter INCREASE / re-roll; far depletion (>800 cm, treated as stream-out).

### Kerfur — `coop/creatures/kerfur_*`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | NPC-form pose / lookAt / command / face | W | HO | HA | `npc_pose_host::TickPoseStream` | snapshot + adopt |
| 2 | convert request (client→host) | W | HO | HA | `kerfur_convert_host::OnConvertRequest` | replay |
| 3 | convert transition broadcast (sole signal) | W | HO | HA | `kerfur_convert_client::OnKerfurConvert` | anchor eids on EntitySpawn |
| 4 | convert reject (sentient/kill guard) | U | code | HA | `kerfur_entity::BroadcastConvertRejected` | none |
| 5 | stable identity (KerfurId) | U | code | HA | `kerfur_entity::AllocKerfurId` | seed (retireOffEid) |
| 6 | radial menu commands | U | code | HA | `kerfur_command::OnCommandRequest` | none |
| 7 | successor-B disambiguation | U | code | HA | `kerfur_form_assembler::ConsumeCapturedForm` | n/a (per-process) |
| 8 | client conversion-ghost custody | W | HO | PP | `kerfur_convert_client::ClaimConversionGhosts` | n/a |
| 9 | dropped floppy byproduct | U | code | HA | `kerfur_convert_host::ExpressConversionFloppies` | generic snapshot |

NOT SYNCED: kerfur HP as its own lane; carried contents beyond floppy; mirror AI timers (neutralized); the `kill` bool (host-checked, not streamed); pat/take/equip verbs (return Invalid). (`kerfur_reconcile` / `kerfur_prop_adoption` not fully read — off→active retire + prop-form save adoption are UNKNOWN.)

### NPC host-sim (generic creatures) — `coop/creatures/npc_*`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | NPC spawn (class + xform + save flags) | W | HO | HA | `npc_mirror::OnEntitySpawn` | snapshot (all live) |
| 2 | NPC pose (fair-share batch stream) | W | HO | HA | `npc_pose_host::TickPoseStream` | current-pose in spawn |
| 3 | NPC destroy | U | code | HA | `npc_mirror::OnEntityDestroy` | n/a |
| 4 | client ghost reconciliation | W | HO | HA | `npc_mirror::DestroyUntrackedClientNpcs` | snapshot-gated sweep |
| 5 | save-persisted NPC adoption | W | HO | HA | `npc_adoption::ResolvePending` | adopt (THIS is it) |

NOT SYNCED: per-limb HP; AnimBP montages/timers (mirror CMC parked); non-allowlisted classes; wisp keeps its own actor tick (cosmetic).

### Owner-entity (eyer, per-peer) — `coop/creatures/owner_entity_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | owner entity spawn | U | code | PO | `OnSpawnMsg` (per-sender cap) | 10 s keepalive |
| 2 | owner entity pose | U | code | PO | `OnPoseMsg` | keepalive |
| 3 | owner entity destroy / leaver teardown | U | code | PO | `OnDestroyMsg` (host speaks for leaver) | n/a |

NOT SYNCED: the eyer's own AI/anger/dash (mirror brain PARKED); state beyond pos+yaw+classId; only `eyer_C` in the class table.

### Roach — `coop/creatures/roach_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | infestation snapshot (paged 12/datagram) | U | code | HA | `SendSnapshot` | full paged snapshot |
| 2 | local consumption (eat/stomp) | U | code | ARB | `OnConsumedIntent` (nearest 200 cm) | replay |

NOT SYNCED: per-roach stable identity (ordinal only); roach AI; consumption attribution (host picks nearest).

### Wisp (killer) — `coop/creatures/wisp_*`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | existence + pose / landing | U | code | HA | `npc_pose_drive::ApplyToEngine` (rides NPC lane) | NPC snapshot |
| 2 | aggro target selection | U | code | HA | `wisp_attack_sync::Tick` (random+sticky) | n/a (host-local) |
| 3 | grab of a victim | W | HO | HA | `wisp_tear_mirror::OnWispGrab` | none (transient) |
| 4 | tear / fatality mirror | W | HO | HA | `wisp_tear_mirror::OnWispTear` | none |
| 5 | victim puppet socket-hold | U | code | HA | `wisp_grab_hold::Tick` | n/a |
| 6 | window lift + despawn | U | code | HA | `DischargePendingDestroys` | n/a |
| 7 | wisp-killed NPC death mirror | U | code | HA | `DischargeNpcKillWatch` | n/a |
| 8 | host false-grab protection | U | code | HA | `AddPlayerDamage_PreCancel` | n/a |

NOT SYNCED: wisp AI beyond pose (host owns Target locally); victim death is a scheduled local ragdoll (not HP-sync); a joiner mid-grab misses the transient.

### Pyramid — `coop/creatures/piramid_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | existence + pose + facing + head | W | HO | HA | `piramid_sync::ApplyMirrorHeadingYaw` (rides world_actor) | world-actor snapshot |
| 2 | client mirror brain suppression | U | code | HA | `BrainSuppress_Interceptor` | n/a |
| 3 | wisp gather COMMIT | W | HO | HA | `CheckIfReached_POST` / `OnPyramidGather` | replay in-flight |
| 4 | gathered wisp freeze/suck | W | HO | HA | `TryReplayPendingGather` | via gather re-send |

NOT SYNCED: pyramid RNG walk-target (host-only); montage timing (per-peer native branch); a gather whose wisp already retired at join (joiner misses the beam tail).

### Firefly — `coop/world/firefly_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | ambient firefly spawn position | U | code | PO | `OnFireflyTickPost` / `OnReliable` (each peer runs own spawner, cosmetic) | none (transient) |

NOT SYNCED: firefly lifetime/despawn; count/density (each peer runs its own spawner); no trust gate (cosmetic).

### Drone (delivery) — `coop/interactables/drone_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | body transform (singleton pose) | U | code | HA | `drone_sync::OnReliable` | snapshot (adopt=1) |
| 2 | FX (dust/cue/light/gates) | U | code | HA | `drone_sync::FillPayload` | via snapshot |
| 3 | cargo container repoint | U | code | HA | `drone_sync::RepointContainer` | prop snapshot |

NOT SYNCED: drone flight AI/pathing (client tick suppressed); the order economy (separate); singleton (no multi-drone identity).

### Desk-input / console — `coop/interactables/desk_input_sync`, `desk_cursor_sync`, `desk_sim_sync`, `desk_snd_fx`, `console_state_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | desk INPUT field deltas | B | HO | CO | `OnDeskInput` **[take-4 R2 poll-alias; v112 fix unverified]** | seed via DeskState |
| 2 | SHIFT quick-scan event | U | code | CO | `OnDeskScan` | none |
| 3 | coord_isPing run-flag | U | log | PP | `PatchScalar` (bookkeeping only) **[v115b]** | not adopted |
| 4 | desk log lines | U | code | CO | `ProduceLogLines` | none (joiner empty) |
| 5 | desk scalar ADOPT snapshot | U | code | HA | `OnDeskState` | THIS is the seed |
| 6 | desk unit-1 AUDIO fx | B | HO | CO | `OnDeskSndFx` (+[dev] `desk_snd_selftest`) **[take-4 R5; v115 fix unverified]** | loops re-asserted |
| 7 | download-SIM 7ch outputs | B | HO | HA | `desk_sim_sync::Tick` **[take-4 OPEN-0; v112 fix unverified]** | stream re-primes |
| 8 | live coords cursor | B | HO | CA | `desk_cursor_sync::Tick` **[take-4 OPEN-1 jerk, unresolved]** | stream re-primes |
| 9 | dish committed-aim locks | B | HO | CA | `console_state_sync::OnDishAim` **[take-4 R3 invisible on host]** | connect snapshot |
| 10 | sky-signal SET | U | code | HA | `OnSkySignalState` | point-to-point snapshot |

NOT SYNCED: CDOWN animated log family (filtered, regenerable); U2 gauge/detector sounds (BUG-3); SARV/spectrum remap (display-local); ping-FSM stage visuals on observers.

### Dish — `coop/interactables/dish_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | dish POSE (24 dishes) | B | HO | HA | `ApplyDishRow` **[take-4 R1 4 Hz hard-snap stepping]** | DishSnapshot |
| 2 | download ARM/DISARM | U | code | HA | `OnDishArm` (v113 AS-BUILT) | connect ARM row |
| 3 | dish calibration | U | code | CO | `OnDishCalib` (symmetric, host relay = total order) | in snapshot |
| 4 | dish full-state snapshot | U | code | HA | `OnDishSnapshot` | THIS is the seed |
| 5 | own-ping local slew kill | U | code | PP | `KillOwnPingSlews` | n/a |

NOT SYNCED: rest pose (per-peer RNG at BeginPlay, never saved); ambient ticker timing (client tickers PARKED).

### Comp-processing — `coop/interactables/comp_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | comp decode scalars | B | HO | CO | `OnState` **[take-4 R17 list lags 1 s poll]** | adopt=1 |
| 2 | comp_data_0 content (signal row) | U | code | CO | `ApplyData` | adopt blob |
| 3 | simulator-left teardown | U | code | PP | `OnPeerDisconnect` | n/a |

NOT SYNCED: comp RNG rate (local sim); level-up ID re-mint per-peer (OPEN-5).

### Device-occupancy — `coop/interactables/device_occupancy`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | device claim / release (8 screens) | W | HO | **ARB** | `OnReliable` (host busy-table CAS) | full table snapshot |
| 2 | busy-deny gate (E → no-op + chat line) | U | code | PP | `OnUseInputPre` (local of arbiter table) | n/a |
| 3 | desk FSM-hold (pinger keeps claim) | U | log | ARB | `ReconcileDeskFsmHold` **[v116 log]** | rides table |

NOT SYNCED: which specific unit within a shared widget (aim-seam memo, cosmetic).

### Signal-catch — `coop/interactables/signal_catch_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | signal CATCH (ping → coord identity) | U | log | CO | `OnReliable` **[07-17 lost-catch; v116 fix, effect unverified]** | seed kind=2 (feed-silent) |
| 2 | signal DELETE / cleared | U | code | CO | `ApplyReplay` (kind=1, not gated) | none |
| 3 | host dish-theater slew fan-out | U | code | HA | `ApplyReplay` (host replays StartMovingAll) | catch seed carries slew |
| 4 | catch-vs-snapshot race + feed | U | code | CO | `NoteIncomingSnapshot` (recent-TTL dedup) | recent-catch filter |

NOT SYNCED: kind=2 seed deliberately feed-SILENT; no holder/claim validation on receive (RULE-2 retired v116, transport-trusted).

### Playback-deck — `coop/interactables/deck_play_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | play edge (start track) | W | ST | CO | `OnPlayDeck` (+[dev] `deck_selftest`) | none (no in-progress snapshot) |
| 2 | stop edge (end/manual) | W | ST | CO | `OnPlayDeck` (gen-guard) | none |
| 3 | natural `fin()` end | U | code | PP | `OnFinPre` (each peer self-stops) | n/a |

NOT SYNCED: playback position/scrubbing; deck volume; a joiner mid-track sees no deck audio until the next edge.

### Drive-chain (drives) — `coop/interactables/drive_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | slot occupancy (insert/eject, 3 roles) | U | code | HA/CO | `OnSlotLine` (host canonical on conflict) | seed 3 slot lines |
| 2 | drive data row payload | U | code | CO | `ApplyPayloadBlob` (host reaps denied) | seed non-default rows |
| 3 | drive birth attribution | U | code | CA | `NoteLocalDriveBirth` | prime-only for save-loaded |

NOT SYNCED: drive world pose (generic prop lane); the signal image/spectrogram bytes (`adopt=false`, "sans image").

### Drive-rack — `coop/interactables/drive_rack_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | rack slot set (16 slots) | U | code | **ARB** | `HostApplyRackOp` (host CAS on `.has`) | seed one canonical/rack |
| 2 | take-race deny + reap | U | code | ARB | `HostApplyRackOp` (op2 deny) | n/a |
| 3 | set-race refund | U | code | HA | `HostApplyRackOp` (re-spawn drive) | n/a |

NOT SYNCED: rack world pose; per-row image bytes (same `adopt=false`).

### Phys-mods — `coop/interactables/physmods_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | module array (plug/unplug) | U | code | **ARB** | `OnPhysMods` (host sole canonical) | seed canonical |
| 2 | plug-dup race refund | U | code | HA | `OnPhysMods` (op0 dup → re-spawn) | n/a |
| 3 | unplug-race deny + ghost sweep | U | code | ARB | `ClientHandleDeny` / `HostShouldReapModuleBirth` | n/a |

NOT SYNCED: module actor ↔ byte mapping cosmetic/pose beyond `ClassForByte`.

### Tape-caddy — `coop/interactables/tape_caddy_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | reel slot insert/eject | U | code | CO | `OnReelSlot` (host wins occupied tiebreak) | seed = the save (prime-first-sight) |
| 2 | reel accrual / progress | U | code | HA | `Tick` corrector (host-only 1 Hz) | corrector-bounded |
| 3 | reel PROP birth on client eject | U | code | HA | `HostSpawnPlacedProp` | rides prop birth + savedScalar |

NOT SYNCED: wallunit power (separate `ApplianceState`); which tape content (rides the reel prop's savedScalar birth).

### Laptop (power/floppy/disc) — `coop/interactables/laptop_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | power lid open/close | U | code | CO | `ApplyPowerTarget` | seed op=3 |
| 2 | floppy slot insert (+content) | U | code | CO | `ApplyAssembledContent` (kind=0) | seed op=3 + content |
| 3 | floppy slot eject | U | code | CO | `OnLaptopState` (op=2) | n/a |
| 4 | post-eject disc content | U | code | HA | `DriveEjectContentWatch` (kind=1) | seed live content rows |
| 5 | portable-PC lid | U | code | CO | `LidSweep` (op=6, host re-fans) | seed current lid |

NOT SYNCED: portable-PC as remote terminal binding; disc physical pose (generic lane); screen UI state beyond the buffer quad.

### Laptop-buffer-quad — `coop/interactables/laptop_buffer_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | buffer quad (floppyData/buffer/uids/readWrites) | W | ST | **ARB** | `HostApplyBatch` (+[dev] `laptop_selftest` 0→1→0) | seed connect canonical |

NOT SYNCED: cursor/scroll position; per-row UID beyond `bufferUids`; no ORDER-move op (remove+re-append only).

### Floppybox — `coop/interactables/floppybox_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | box disc stack (LIFO) | U | code | **ARB** | `OnBoxChunk` (host CAS on pop content-hash) | seed one canonical/box |
| 2 | pop-race deny + held-disc reap | U | code | ARB | `ReapDeniedPop` | n/a |

NOT SYNCED: box world-prop pose; box open/closed lid.

### Serverbox — `coop/interactables/serverbox_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | server broken mask + efficiency | U | code | HA | `ApplyState` (one-directional; verbs EX_LocalVirtual → state mirror) | snapshot |
| 2 | client autonomous breaker neutralize | U | code | PP | `KillLocalBreaker` | re-kill after apply |

NOT SYNCED: per-server repair-minigame progress; break/fix attribution; the break/fix VERBS (invisible dispatch).

### Saved-signals store (2nd save-CRDT) — `coop/interactables/signal_sync`
The desk signal-library (`gamemode.savedSignals_0`), an append/delete CRDT — the meadow shape (content-hash multiset shadow + delete tombstones, 20 s TTL, host-relay) **but WITHOUT host-canonical order** (contrast meadow §4, which syncs order per the rule-1 decision).
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | row append (newly-saved signal) | U | code | CO | `signal_sync::CompleteAssembly` (either peer, host relays) | snapshot (rides save-transfer blob; shadow re-primes world-up) |
| 2 | row delete | U | code | CO | `signal_sync::ApplyDeleteByHash` (delete-wins tombstone) | snapshot (same blob) |

NOT SYNCED: in-place edit of a saved row (no field-delta lane — surfaces only as delete+append if the key changes); row ORDER within the array (append-only, positional order NOT reconciled — the meadow contrast); append-author attribution; a row unreadable at poll time is skipped.

### World-actor mirror (generic event actors) — `coop/world/world_actor_sync`, `world_actor_mirror`
Host-authoritative mirror of the ~14 non-Character event actors (saucers, mothership, ariral ships, UFO, jellyfish, firetank, pyramid). Identity = class leaf name vs `kWorldActorAllowlist`. Pyramid (§7 above) is a CONSUMER of this lane; this is the lane itself + its 3 pyramid aux fields.
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | spawn (class + transform + scale) | W | HO | HA | `world_actor_mirror::OnWorldActorSpawn` (`[WA-TRACE]`; take-4 pyramid) | replay/seed (live WAs re-sent) |
| 2 | destroy | W | log | HA | `world_actor_mirror::OnWorldActorDestroy` (`[WA-TRACE]`) | none-needed (only live re-sent) |
| 3 | pose stream (loc/rot) | W | log | HA | `world_actor_mirror::TickClientWorldActors` (`[WA-TRACE]` 4-hop; unreliable batch) | none (continuous) |
| 4 | pyramid auxYaw (arrow heading) | U | code | HA | `piramid_sync::ReadHostHeadingYaw` (rides pose) | none |
| 5 | pyramid auxVec (head relLook) | U | code | HA | `ApplyMirrorRelLook` (rides pose) | none |
| 6 | pyramid auxTargetEid (wisp target) | U | code | HA | `ApplyMirrorWispTarget` (rides pose) | none |

NOT SYNCED: any per-actor gameplay/health/BP internal beyond transform; non-pyramid actors carry ONLY transform (auxYaw defaults to actor yaw); the mirror's animation state is engine-local (actor tick parked); a client never authors spawns/destroys (defense-in-depth suppresses local allowlisted spawns).

### Remote-player body / locomotion / ragdoll — `coop/player/`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | body pose (loc/yaw/pitch/speed/state) | W | HO | PO | `RemotePlayer::SetTargetPose` | puppet spawns on first pose |
| 2 | puppet spawn / destroy lifecycle | W | HO | recv-local | `puppet_drive::DriveTick` | spawn IS the seed |
| 3 | locomotion drive (velocity/CMC/footsteps) | W | HO | PO | `RemotePlayer::ApplyToEngine` | rides pose |
| 4 | head-look / aim direction | W | HO | PO | `GetSyncedAimDirection` | rides pose |
| 5 | body-yaw presentation (turn-in-place) | U | code | ∅ | `RemotePlayer::Tick` | n/a |
| 6 | ragdoll DISPLAY flag | U | code | PO | `remote_player_ragdoll::OnWireBit` | pose bit current on first packet |
| 7 | ragdoll PHYSICS (pelvis velocity) | U | code | PO | `remote_player_ragdoll::SetPose` | stream resumes on edge |
| 8 | hurt-flash (nameplate + material) | U | code | ∅ | `SetVitals` | first-hit gate |

NOT SYNCED: crouch (Phase-2 wire bump); streamed ragdoll pelvis ROTATION (on-wire but unused).

### Nameplate + nick-color — `coop/player/nameplate`, `nick_color`, `session/player_handshake_prefs`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | nameplate render (nick/health/ping/voice) | W | HO | ∅ | `nameplate::Update` (composite of synced inputs) | recompute each tick |
| 2 | per-player plate visibility pref | U | code | CO | `HandleNameplateChange` | at-join via Join payload |
| 3 | nick color (packed RGB) | U | code | CO | `nick_color::StoreForSlot` | at-join via Join |

NOT SYNCED: health-bar/ping/voice-icon values are computed locally from their own sources; distance fade/occlusion are viewer-local.

### Skins — `coop/player/skin_registry`, `skin_effects`, `client_model`, `local_body`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | local skin selection / persistence | W | HO | PO | `local_body::RequestSkin` | ini reloaded at boot |
| 2 | skin change (mid-session) | U | code | CO | `HandleSkinChange` | at-join via Join field |
| 3 | skin → mesh/texture apply | W | HO | recv-local | `client_model::ApplySkinToBody` | at puppet spawn |
| 4 | skin EFFECT rig (RT face, step FX) | U | code | recv-local | `skin_effects::Apply` | rebuilt on apply |
| 5 | skin catalog (pak scan) | W | code | PP | `skin_registry::Entries` | starter roll |

NOT SYNCED: pak asset BYTES (each machine must have the pak; missing → local view degrades to kel).

### Hand-item / item-activate — `coop/player/hand_item`, `item_activate`, `flashlight_click_sound`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | hotbar hand-item identity (class+transform) | W | HO | CO | `HandleHandItem` | replay to joiner |
| 2 | hand live pose (swing) | U | code | PO | `hand_item::TickMirrors` | announce-rel covers pre-stream |
| 3 | hand mirror actor (display) | W | HO | recv-local | `hand_item::SpawnMirror` | on puppet+state |
| 4 | released-hand-actor → world prop | U | code | CA | `ExpressReleasedHandActor` | n/a (edge) |
| 5 | flashlight activation (on/mode/cone) | W | HO | PO | `item_activate::ApplyToPuppet` | replay (ON-only) |
| 6 | flashlight click sound | U | code | recv-local | `flashlight_click_sound::PlayIfStateChanged` | per-peer last-state |

NOT SYNCED: crank-lantern variant (deferred); non-flashlight item ACTIONS (only the display mirror + flashlight state).

### Player-inventory — `coop/items/player_inventory_sync`, `inventory_wire`, `inventory_pickup_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | client live inventory → host persistence | W | ST | **PP** | `ClientStreamTick` (+[dev] `inventory_selftest`) | host-terminal, never relayed |
| 2 | host → joiner per-player inventory | U | code | HA | `OnSaveObjectReady` | seed pre-world (first-join starter kit / else empty) |
| 3 | inventory-collect blip (2D cue) | U | code | PP | `inventory_pickup_sync::OnReliable` | none (edge) |

NOT SYNCED: the personal inventory is **peer-private by design** — never merged or relayed to other peers; only streamed to host for per-`<guid>.json` persistence.

### Sleep — `coop/player/sleep_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | in-bed report (peer→host) | U | code | **ARB** | `ReportLocalEdge` / `HostRetally` (host tally) | joiner arrives awake |
| 2 | tally / accelerate / end phase | U | code | HA | `HostRetally` / `ApplyAccelerateLocal` | phase re-derives on join |
| 3 | time-dilation / WAITING enforcement | U | code | HA | `Tick` (WAITING block) | edge-latched |
| 4 | nightmare (dreamProbability) policy | U | code | HA | `ApplyDreamProbPolicy` (single roller) | re-applied on connect |

NOT SYNCED: per-peer sleepCam view; individual dream CONTENT (only the roll authority is gated).

### Player-damage / hazards — `coop/player/player_damage`, `local_streams`, `dev/restore_vitals`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | enemy damage to a peer's player | U | code | HA | `player_damage::OnWireDamage` | none (edge) |
| 2 | vitals (health/food/sleep fractions) | W | HO | PO | `remote_player::SetVitals` (display-only, never saveWrite) | current on first pose |
| 3 | restore vitals (F3 dev refill) | U | code | HA | `restore_vitals::ApplyLocally` | none |
| 4 | killer-wisp fatal grab/tear | W | HO | HA | (wisp lane, cross-ref) | none |

NOT SYNCED: local hazard EFFECTS (screen shake, coffeePower); "stamina" is `sleep` (local-only).

### Puppet (unpossessed-orphan drive) — `coop/player/puppet_drive`, `puppet_carry_drive`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | puppet locomotion/pose drive | W | HO | recv-local | `puppet_drive::DriveTick` | world-ready gate |
| 2 | puppet carry of trash-clump | W | HO | HA | `puppet_carry_drive::Tick` | carry latch |
| 3 | puppet flashlight cone (lag_fl) | U | code | recv-local | `ApplyToEngine` (lag_fl block) | rides pose |
| 4 | puppet head-look bone | U | code | recv-local | `ApplyToEngine` (head block) | rides pose |

NOT SYNCED: puppet CMC integration (parked; owned by our drive); sit/stand posture (OPEN backlog).

### World / interactables — small systems (`coop/world/`, `coop/interactables/`, `coop/items/order_sync`)
| System | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| Sky | sky rot + moon phase | U | code | HA | `sky_sync::ApplySky` | snapshot |
| Time-of-day | clock steady-state | U | code | HA | `time_sync::ApplyClockSnapshot` | seed (connect-edge) |
| Time-of-day | clock connect-edge | U | code | HA | `time_sync::OnReliable` | THIS is the seed |
| Time-of-day | sleep-accelerate toggle | U | code | CO | `SetSleepAccelerate` | none (sleep lane) |
| Alarm | klaxon `active` on/off | U | code | CO | `alarm_sync::Apply` | snapshot incl active=0 |
| Balance | canonical Points total | U | code | HA | `balance_sync::ApplyFromHost` | connect-replay |
| Balance | credit delta request | U | code | ARB | `OnDeltaRequest` (only host applies) | none (transient) |
| Daily-task | taskNew (arrays + scalars) | U | code | HA | `OnTaskNewState` | baseline-first-sight (save transfer) |
| Email | row append (chunked) | U | code | HA | `email_sync::CompleteAssembly` | save transfer + shadow prime |
| Email | row delete (content-hash) | U | code | CA | `ApplyDeleteByHash` | episode gate |
| Doors | base door open/close (auto-revert) | U | code | HA | `interactable_sync::SmartApply` | snapshot |
| Doors | client door open intent | U | code | ARB | `OnDoorOpenRequest` (host runs guards) | none |
| Doors | garage door (no auto-revert) | W | HO | CO | `g_garageAdapter` **[take-4 R9 FName-id]** | snapshot (level-export FName) |
| Doors | locker/console door | U | code | CO | `g_doorBoxAdapter` | snapshot |
| Keypads | digit buffer (input replication) | W | HO | CO | `keypad_sync::ApplyState` **[06-12 echo-storm]** | snapshot |
| Keypads | active / door power + LED | W | HO | HA | `keypad_sync::ApplyState` **[06-17 keypads-dead]** | snapshot |
| Keypads | Accept/Deny submit event | W | HO | CO | `keypad_sync::ApplyIncoming` **[07-04 red-button]** | snapshot (None) |
| Lights | switch on/off | U | code | CO | `g_lightAdapter::CallUse` | snapshot |
| Turbine | 6 driver floats | U | code | HA | `turbine_sync::ApplyState` | snapshot + pending |
| Power panel | 5 breaker bools (mask) | U | code | CO | `power_sync::ApplyMask` | snapshot + pending |
| Grime | process wipe (min-wins) | U | code | CO | `grime_sync::ApplyResolved` | snapshot adopt=1 |
| Grime | one-shot destroy (super-sponge) | U | code | CO | `grime_sync::ApplyResolved` (value=0) | wiped-keys in snapshot |
| Window-cleaning | clean wipe (min-wins) | W | ST | CO | `window_sync::ApplyResolved` (+[dev] `window_synth`) | snapshot adopt=1 |
| Garbage-chute | tick/pickup AV suppress | U | code | PP | `garbage_sync::IsGarbageInstance` (crash-fix) | none |
| Garbage-chute | spawner suppression | U | code | HA | `InstallSpawnerSuppressors` (host rolls) | none |
| ATV | body pose (occupant) | U | code | CA | `atv_sync::SetTarget` | snapshot adopt=1 |
| ATV | authority-release / throw | U | code | CA | `OnAtvRelease` | none |
| ATV | purchased spawn | U | code | HA | `OnAtvSpawn` | snapshot (synth key) |
| ATV | purchased destroy | U | code | HA | `OnAtvDestroy` | n/a |
| Shop-order | new order forward | U | code | ARB | `order_sync::CommitOrder` (host commits) | watermark-prime |
| Appliance | on/off bool (6 classes) | U | code | CO | `g_applianceAdapter` | snapshot |

NOT SYNCED (world/misc): client clock never free-runs (TimeScale forced 0); balance HUD repaint is client-local; daily-task/email lean on save-transfer for JOIN state (no connect snapshot); unkeyed doors keep native behaviour; door swing is force-snap not animated when far; serverbox break/fix verbs are invisible (state+`check()` mirror); grime/window FAR vanishes ignored (stream-out); calm turbine world goes silent; idle save-ATVs stay per-peer physics until authored; client never mutates its own shop orders; sub-second event cues escape the 1 Hz poll.

### World-events — `coop/world/event_fire_sync`, `event_cue_sync`, `event_active_sync`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | scheduler row fire | U | code | HA | `event_fire_sync::ReplayVerdict` | replay (pre-world queued + dedupe) |
| 2 | in-flight event (join) | U | code | HA | `event_active_sync::SendJoinSnapshotForSlot` | snapshot (per-lane late-join) |
| 3 | cosmetic emitter cue (starRain) | U | code | HA | `event_cue_sync::Tick` | seed (already-broadcast cues) |

NOT SYNCED: `special`/`ariralPrank` RNG (replayed None); verdict 0/-1 rows (creature/prank/pyramid/arirShip/agrav — host-local); unmapped `kClassRowMap` classes (logged LOUD, not replayed); sub-second cues (1 Hz poll caveat).

### Chat — `coop/comms/chat_sync`, `chat_feed`, `chat_bubbles`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | text line | W | HO | CA | `chat_sync::OnReliable` (identity = transport slot) | none (TTL-ephemeral, no backlog) |
| 2 | nickname prefix / color | W | HO | HA | `chat_sync::OnReliable` (from handshake roster) | own handshake replay |
| 3 | overhead speech bubble | W | HO | PP | `chat_bubbles::OnChatLine` | none (expires in place) |

NOT SYNCED: chat history/scrollback (fading HUD); own line never received back (local echo only).

### Peer-action feed — `coop/comms/peer_action_feed`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | action line ("&lt;nick&gt; did X") | W | HO | ∅ | `peer_action_feed::Announce` (derived; driving lane owns authority) | none (each driving lane owns its join) |
| 2 | enable toggle (local pref) | U | code | PP | `SetEnabled` | none |

NOT SYNCED: the feed carries no packet (formatting layer); subject is ALWAYS a nickname, never "You".

### Voice — `coop/voice/voice_chat`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | Opus audio frames (3D positional) | W | HO | PO | `voice_chat::Tick` (own mic, host-relayed) | none (realtime, jitter-buffered) |
| 2 | mute / voice-disabled state | U | code | PO | `OnVoiceState` | replay (`ReplayPeerStatesToSlot`) |
| 3 | spatial listener/speaker position | W | HO | ∅ | `voice_chat::Tick` (derived from pose stream) | none (derived) |

NOT SYNCED: device config (mic/output/gain/PTT, local ini); talking indicator (derived locally).

### Save-transfer / join snapshot — `coop/save/save_transfer`, `session/subsystems`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | request world save | W | HO | CA→HA | `ClientNoteConnected` / `OnRequest` | THIS is the join mechanism |
| 2 | blob announce | W | HO | HA | `save_transfer::OnBegin` | net-thread co-located |
| 3 | blob chunks (CRC, paced) | W | HO | HA | `BulkSink_` | live-capture at request |
| 4 | identity sidecar (index→eid) | U | code | HA | `DeserializeSidecar` (dev-gated) | in the CRC'd stream |
| 5 | snapshot bracket (spawn burst) | W | HO | HA | `ConnectReplayForSlot` | THIS is the join snapshot |
| 6 | blob-vs-live divergence deletes | U | code | HA | `SendBlobDivergenceDeletes` | seed (removes before adds) |
| 7 | in-window save-pos corrections | W | log | HA | `FlushDivergedSavePositionsForSlot_` (F1 probe) | seed / late-arm |
| 8 | meadow-DB join seed | W | ST | HA | `CaptureJoinSnapshot` (+meadow selftest) | seed |

NOT SYNCED: host's canonical slot never sent (scratch only); `gameMode` hard-coded 0 (story only); no `reserve()` cap on the receive buffer (by design).

### Teleport-client — `coop/session/teleport_client`
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | teleport all clients to host | U | code | HA | `TeleportClientsToHost` (slot0 + NaN + AABB 1e6) | n/a (action) |
| 2 | teleport one slot to host | W | HO | HA | `TeleportSlotToHost` | seed = JOIN placement |

NOT SYNCED: no client→host teleport request path (host-only trigger).

### Session / join-handshake / roster — `coop/session/player_handshake*`, `coop/player/roster`, `players_registry`
The foundational join system: slot assignment, identity announce, cross-peer relay, the world-ready barrier, the b122 version gate.
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | peer slot assignment | U | code | HA | `player_handshake::HandleAssignPeerSlot` (host picks lowest free 1..3) | seed (once per connection) |
| 2 | own Join announce (eid+nick+guid+skin+prefs+color) | W | HO | PO | `player_handshake::HandleJoinMessage` (each peer authors own identity) | replay (latch cleared on disconnect) |
| 3 | cross-peer identity relay | W | HO | ARB | `player_handshake::BroadcastPlayerJoinedFromHost` (host relays validated) | seed (host tells joiner every mirrored client) |
| 4 | client world-ready barrier | W | log | CA | `event_feed` `ClientWorldReady` (`[PILE-1C] slot N world-ready`) | THIS is the load-quiescence barrier |
| 5 | version gate (b122 Paper pair) | W | log | ARB | `player_handshake_version::ValidateJoinVersionOrRefuse` (equality, 3-layer drills) | refused joiner = connected-never-Joined |
| 6 | roster / scoreboard snapshot | U | code | PP | `roster::Refresh` (each board renders what it truthfully knows) | none (derived view) |
| 7 | local Player Element identity | U | code | PO | `players_registry::EnsurePlayerElement_` (own eid in exclusive band) | seed (band set before alloc) |
| 8 | joined-the-game feed line | U | code | ∅ | `player_handshake::AnnounceJoinerOnce` | replay (re-announces on reconnect) |

NOT SYNCED: connection-type/link label + per-peer RTT (each peer's local GNS read); the master-server version-check line (informational only); the whole `session_manager` HTTP lobby layer (Host/Join/Refresh → master, not peer wire); host lobby-listing bookkeeping.

### Moderation — `coop/moderation/` — **NO ReliableKind (acts via GNS close + host-local files)**
Kick/ban/seen-players is a HOST-LOCAL control surface — no state is streamed to clients; a client only ever observes the GNS connection close.
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | kick slot | U | code | HA | `moderation::KickSlot` (GNS close, host-gated) | n/a (live action) |
| 2 | ban slot (online) | U | code | HA | `moderation::BanSlot` (IP+nick → `ban_list::Add` → Kick) | n/a |
| 3 | ban offline (by GUID) | U | code | HA | `moderation::BanOffline` (`seen_players::FindByGuid`) | n/a |
| 4 | ban enforcement (accept filter) | U | code | HA | `session_runtime::BanAcceptFilter` (fail-closed; **LanDirect only**) | rejected pre-slot, pre-handshake |
| 5 | ban-list persistence | U | code | HA | `ban_list::Add`/`WriteFileLocked` (host-local file) | loaded at session start |
| 6 | seen-players registry | U | code | HA | `seen_players::TouchOnJoin` (host-local file) | touch per join |
| 7 | teleport slot to host | U | code | HA | `moderation::TeleportSlotToMe` (delegates teleport) | n/a |

NOT SYNCED: the ENTIRE surface is host-local (no kick/ban/seen state streamed); P2P IP-ban deliberately absent (identity-ban is unbuilt Stage 6); no name/profanity moderation (only a char filter).

### Save-suppression — `coop/save/save_block`, `save_button_disable`, `save_guard`, `save_indicator_suppress` — **NO wire (local lockout)**
Coordination-by-construction: each client blocks its OWN world-save; the host is authoritative purely by being the only peer NOT running the block. No "host saves, others suppress" packet exists.
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | client disk-write block | U | code | HA | `save_block::SaveGameToSlotDetour` (blocks `saveSlot_C`, passes settings) | none (always active once installed) |
| 2 | client native save-cycle gate | U | code | HA | `save_block::Tick` (masked-bit `disableSave=true`) | none |
| 3 | client Save-button grey-out | W | ST | PP | `save_button_disable::ApplyGreyOut` (+[dev] savebtn selftest) | none (self-heal) |
| 4 | pre-session save backup | U | code | HA | `save_guard::BackupSaveOnSessionStart` (host-local, keeps 3) | n/a (before coop state) |
| 5 | SAVED-indicator detect (join window) | U | log | HA | `save_indicator_suppress::OnSaveAnim` (`[SAVED-DETECT]`; detect-only, misnamed) | window around each joiner's capture |

NOT SYNCED: nothing is streamed — all five are per-process local hooks/files; `save_indicator_suppress` performs NO suppression (read-only detector, still "PHASE 1 detect-log").

### Spawn-authority — `coop/world/spawn_authority` — **NO wire (client suppress/park)**
Not a sync system: a client-only layer that cancels/parks VOTV's ambient RNG spawners so the host is the sole roller of shared-world randomness; the products are mirrored by the NPC/roach/wisp lanes.
| # | facet | V | E | Auth | cite | mid-join |
|---|---|---|---|---|---|---|
| 1 | client spawner suppress + tick-park | U | code | HA | `spawn_authority::ParkWalk` / `g_cancelTargets` (RestoreAll repays on session end) | initial park on first client tick (join window) |

NOT SYNCED: everything — transfers no wire state; enforces "host rolls, client stops" locally. Player-anchored spawns (pinecone) are deliberately NOT parked (owner-effect, each peer rolls its own, made visible by `host_spawn_watcher`).

---

## 8. Systems still unprofiled / partial

**The FIRST §6–§7 sweep was NOT exhaustive — a self-audit caught it, and the gaps were then filled.**
A 2026-07-23 self-audit (cross-checking every wire lane and `*_sync.cpp` stem against the doc) found six
real systems the first 8 gatherers missed. **Five have since been profiled** (a second gap-fill pass,
now in §6–§7):

- ✅ **Saved-signals store** (`signal_sync`, `SavedSignal*`) — a SECOND save-CRDT (meadow shape, no order).
- ✅ **World-actor mirror** (`world_actor_sync`/`mirror`) — the generic lane the pyramid rides.
- ✅ **Session / join-handshake / roster** (`player_handshake*`, `roster`, `players_registry`) — the
  foundational join/identity/version-gate/roster system.
- ✅ **Moderation** (`moderation/`) — kick/ban/seen; NO wire (GNS close + host-local files).
- ✅ **Save-suppression** (`save/save_block` etc.) — client save lockout; NO wire (local hooks).
- ⏳ **Kerfur reconcile + prop-adoption** (`kerfur_reconcile`, `kerfur_prop_adoption`) — STILL PARTIAL:
  off→active retire arming + prop-form save-kerfur adoption were referenced but not fully read.

**A finding the gap-fill surfaced:** three of these (moderation, save-suppression, spawn-authority) carry
NO ReliableKind at all — they are coop systems enforced by GNS-close / host-local files / client-local
hooks, "coordination by construction". A wire-lane census (the first instinct) is structurally blind to
them; only a system-by-system read finds them. That is the §0 spine claim (system, not lane) proving out.

**Genuinely infra, not game-facet systems** (correctly excluded): `element/` (identity registry),
`net/` (transport), the `dispatch/` routers, `config/`, `dev/`, `session/net_pump` + `subsystems`
plumbing. These carry no game facet of their own.

**Adding a system:** author its facets by hand with a citation each and a two-field remainder row (§0).
Do NOT seed rows from a grep — a syntactic proxy for "is this synced" errs both ways and only the
false-negative side is measurable. (The self-audit's own `ReliableKind::\b<name>\b` check false-flagged
~100 lanes as MISSING — a broken word-boundary grep, the exact instrument-self-agreement trap §0 names;
the reliable signal was the file-stem cross-check, hand-verified.)
