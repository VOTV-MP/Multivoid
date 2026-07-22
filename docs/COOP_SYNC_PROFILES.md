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

1. **Every system carries a mandatory `remainder` row.** A facet nobody thought of is ABSENT, and an
   absent row reads as "nothing wrong there" exactly the way a green does. That is the same error
   asymmetry that killed the syntactic marker filter
   (`[[lesson-syntactic-marker-set-cannot-express-semantic-relevance]]`). The remainder row states,
   per system, that the list is open — and records how many facets were found only by RUNNING, which
   is the one measurable signal about how much the list is still missing.
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

---

## 1. Container (`Aprop_container_C` — 60 classes in the subtree)

Lane: `container_contents_sync.cpp` (853 LOC). Contents live in ONE global `saveSlot.GObjStack`,
addressed by `propInventory.index` (`[[lesson-container-contents-live-in-one-global-gobjstack]]`).

| # | facet | verdict | evidence | citation (symbol / section) |
|---|---|---|---|---|
| 1 | host -> client contents mirror (`addObject` receive) | **WORKS** | `hands-on` | `research/handson_runbook_2026-07-22_container_v125.md` — the user saw the burgers |
| 2 | client -> host extraction, upward (`takeObj`) | **WORKS** | `hands-on` | same runbook — shipped 2/1/0 matched applied 2/1/0 on both peer logs |
| 3 | simultaneous grab by two peers (the CAS) | **UNKNOWN** | `log` | `HostAcceptsClientWrite()` in `container_contents_sync.cpp`. `CONFLICT=0` on BOTH peers in the 07-22 take: the arbitration never executed once. Unexercised, not proven and not broken |
| 4 | the extracted item reaching the host's world | **BROKEN** | `inference` | the `freshBirth` guard in `prop_drop_intent.cpp` (`if (!parked && !freshBirth) continue;`). `[RD]` not `[V]`: the log cannot separate "dropped at the guard" from "never enqueued" — both are silence |
| 5 | nested container-in-container | **NOT BUILT** | `code` | the `BOUNDARY 2` comment block in `container_contents_sync.cpp` — a nested container's `ints[0][0]` is a sender-side slot index, meaningless on the receiver. Receiver-side bounds needed first |
| 6 | volume/mass re-derivation on extraction | **BROKEN** | `log` | `updateVolumesAndMass` never re-derives: records went 7 -> 6 while `currVol` stayed 28579.0 on BOTH peers, 07-22 take |
| 7 | slot 0 — the player's personal container | **NOT BUILT** | `code` | `IsWorldContainerInventory`, fail-closed today. A privacy/product fork and a BOUNDARY 1 redesign; gate:user |
| 8 | the INSERT direction (`putObjectIn_overlap`) | **UNKNOWN** | `none` | nobody has looked; no claim either way |
| — | **remainder — the list is open** | **UNKNOWN** | — | **3 of the 8 facets above (#3, #4, #6) were found by RUNNING, not by reading source.** That ratio is the only measurable signal about what a read-only sweep of this system still misses — and it says the majority of what is wrong here was invisible to code reading |

**Count, not percentage:** 8 facets — 2 `WORKS`, 2 `BROKEN`, 2 `NOT BUILT`, 2 `UNKNOWN`; by evidence,
2 `hands-on`, 2 `log`, 2 `code`, 1 `inference`, 1 `none`.

**Two rows earn the split axis immediately.** #6 is the system's highest-confidence bad news —
`BROKEN` on a measured cross-peer divergence — and the single-status draft had it filed under `OPEN`,
below an inferred one. #3 is the opposite shape: `UNKNOWN/log` means we ran it and the code path
never executed, so every code-derived view of this system reads the lane GREEN while the arbitration
it exists to perform has never once run. Neither row is expressible in a class, verb or lane profile.

---

## 2. Systems not yet profiled

Deliberately empty. Adding a system means authoring its facets by hand, with a citation each and a
remainder row — see §0. Do not seed rows from a grep; the prior pass measured that a syntactic
proxy for "is this synced" errs on both sides and only the false-negative side is measurable.
