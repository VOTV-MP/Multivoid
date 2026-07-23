# Per-system sync profiles — what we sync, what we don't, facet by facet

**What this is.** For one game SYSTEM, the list of its sync FACETS with a status each. It answers the
question the class register cannot: *inside this thing, what works and what doesn't.*

**What this is NOT.** Not a percentage, and not derivable. Read §0 before adding a system — the
limits are structural, and a profile read as a measurement is worse than no profile.

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
| **EVIDENCE** — how we know, independent of the verdict | `hands-on` (artifact path, a human played it) · `log` (a matching real log) · `code` (read from source) · `inference` (reasoned, `[RD]`) · `none` |

A verdict is only as good as the evidence beside it, and the pairing is the point: `BROKEN/inference`
and `BROKEN/hands-on` are not the same claim, and `UNKNOWN/log` (we ran it and the path never
executed) is not the same as `UNKNOWN/none` (nobody looked). **No autonomous smoke, e2e selftest or
harness run ever earns `hands-on`** — that is the rule `tools/verified_takes.tsv` already enforces.

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
| **AUTHORITY** — who owns the write | `host-authored` (client mirrors) · `client-authored` (host validates) · `arbiter` (host CAS over contested writes) · `peer-private` (never shared) · `none` (convergent local, no owner) |

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
| 6 | volume/mass re-derivation on extraction | **BROKEN** | `log` | `host-authored` (intended) | `updateVolumesAndMass` never re-derives: records went 7 -> 6 while `currVol` stayed 28579.0 on BOTH peers, 07-22 take |
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

**Meadow probe (Q4, cheap read, not a full profile):** "facet" DOES resolve for a 0-class save-CRDT.
`meadow_db_sync.cpp` (884 LOC) exists; the "0 classes" was the CXX-dump CLASS count, but the system
lives as a save-struct CRDT and its facets are OPERATIONS (append / delete / order), not classes.
That a 0-class system still yields facets further confirms rows = facets, spine = system — the facet
is a behaviour, and a behaviour needs no class to exist.

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
and I do not yet have one. See §4.

---

## 4. Convergence state — the AXES are settled; VALUES stay open (that is correct)

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
| **EVIDENCE** (hands-on/log/code/inference/none) | how do we know? |
| **REMAINDER** (found-by-running × ever-run) | is the facet list whole? |
| **SYNC-LANE** (present / none-by-design / none-but-owed) | should it carry a lane at all? |
| **AUTHORITY** (host-authored/client-authored/arbiter/peer-private/none) | who owns the write? |

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

**Why this is a ship-able deliverable, not an open loop:** the SETTLED CORE never moved across any
round — spine = the SYSTEM; rows = FACETS, hand-named, set not machine-enumerable; citations =
(file, symbol); no percentage at this granule. The AXES are a growing-but-falsifiable vocabulary with
a named test, not a guess. The user asked to NAME what we sync and don't, per system — three profiles
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

## 5. Systems not yet profiled

Adding a system means authoring its facets by hand, with a citation each and a remainder row — see
§0. Do not seed rows from a grep; the prior pass measured that a syntactic proxy for "is this synced"
errs on both sides and only the false-negative side is measurable. Next candidates by contrast value:
a save-CRDT system (meadow) — 0 classes, the domain is entirely a save struct, so it stresses the
"facet" definition hardest.
