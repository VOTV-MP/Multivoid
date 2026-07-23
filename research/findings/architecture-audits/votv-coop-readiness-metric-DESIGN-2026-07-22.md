# Coop-readiness metric — can a percentage be GENERATED? (2026-07-22)

**Status: SUPERSEDED IN PART, same day (2026-07-22 night).** This document records the DENOMINATOR
pass. Two of its statements were true when written and are no longer:

- "No code, no register built" — the register IS built: `tools/coverage.py` +
  `tools/verified_takes.tsv` (`ca73073f`, `0337d4c1`, `9f9aad5a`), and `README.md` carries generated
  numbers (`48f96238`).
- "§7 owes its own `/qf`" — that pass RAN the same night. Its outcome, and what it changed here, is
  recorded in §8 below. Read §8 BEFORE trusting any figure in §3-§5.

**Two figures in this document are WRONG and are corrected in §8:** the class total (2305) does not
reproduce, and the hands-on VERIFIED count (3) included a row that states a CONDITION, not a verdict.

**The ask (user, verbatim):** "можем ли мы для фазы 1 из roadmap и корня readme сделать процент
готовности типа как-то считать что мы уже сделали из votv систем coop friendly" — then the
correction that reframed it: "можно по всем системам пройтись, взять их на учёт и оттуда проценты
рисовать. **Всё задампить посчитать чётко**."

The correction matters: the denominator comes from the GAME (the CXX dump), not from our own docs.
That dissolves the objection raised against the first proposal (a doc-derived denominator is the
primary's judgment wearing a number).

---

## 1. Which "phase 1" — measured, previously assumed

The tree carries TWO phase numberings (`ROADMAP.md:47` says so explicitly). The percentage is for
the **arc** phase 1 (`ROADMAP.md:81` "1. **votv-coop**"), which `README.md:20` states as the current
phase. `ROADMAP.md:186` "Phase 1 — Engine archaeology" is the METHODOLOGY numbering and is closed —
a percentage on it would be meaningless. Previously I had not measured this and used the two
interchangeably.

## 2. What the dump gives with NO judgment (measured)

Source: `Game_0.9.0n_HOST/.../CXXHeaderDump`, 2645 `.hpp`.

| Fact | Value |
|---|---|
| class declarations total | 4890 |
| BP classes (`_C`) | **2305** |
| BP classes declaring ZERO own functions | **838** |
| direct `Aprop_C` subclasses | 456 |
| `ReceiveTick` / `getData` / `loadData` | 415 / 208 / 209 |
| dev-junk classes (`NewBlueprint*`, `Untitled*`, `Test*`) | 14 |

The **838 zero-own-function classes are a defensible structural floor**: with no own behaviour they
cannot diverge between peers by construction. This is the only readiness-adjacent number the dump
yields without a judgment call.

## 3. The marker-set filter — BUILT, CALIBRATED, REJECTED

I proposed a denominator of "behaviour-bearing classes" = union of `ReceiveTick` ∪
`{getData,loadData}` ∪ interaction verbs (`playerUsedOn`/`getActionOptions`/`actionOptionIndex`/
`playerHandUse_RMB`/`lookAt`), junk purged. That yields **686**.

**It does not survive calibration.** Of the 45 classes our OWN sync docs name as synced, the filter
throws **16 (36%) into "inert content"**:

| Miss | Own fns | Why the filter missed it |
|---|---|---|
| `UsaveSlot_C` | 32 | not an Actor — no `ReceiveTick`/`getData`; **this is where `GObjStack` lives** |
| `Aticker_base_C` | 19 | ticker family, never player-touched |
| `ApiramidSpawner_C` | 11 | spawner |
| `UpropInventory_C` | 11 | component, not an actor |
| `Aexplosion_C` | 10 | event-driven |

The marker set is ACTOR-shaped and PLAYER-INTERACTION-shaped. It structurally cannot see non-Actor
carriers (`USaveGame` / `UActorComponent` / `UUserWidget`) or spawner/ticker/event behaviour.

**And the false-POSITIVE side is uncalibratable** — there is no ground truth for "should have been
counted", so the over-inclusion can never be bounded. A syntactic proxy for a semantic property
("does this diverge between peers?") errs in both directions and only one direction is measurable.

**Verdict: 686 is not a denominator.** Both error directions are real and one is unbounded.

## 4. TWO instrument defects, both caught before publication

- **`chain()` to engine roots returned "100% covered".** Ancestor-walking reached `AActor`, which our
  source mentions somewhere, so every class resolved as covered. Caught by absurdity — a subtler
  wrong number would have shipped.
- **Exact-name grep vs runtime SUBSTRING matching.** Coverage was scored by exact class-name presence
  in `src/votv-coop/src`. Calibration against known positives killed it: our code carries `krampus`
  (`coop/creatures/npc_sync_install.cpp`) and `wisp_C` (`coop/creatures/npc_mirror.cpp`), yet
  `Anpc_krampus_C` and all 11 `Awisp_*_C` scored as NEVER TOUCHED, because the runtime matches by
  substring/prefix. The "170 untouched" figure is inflated by an unknown amount and is unusable.

Both are instances of `[[lesson-negative-grep-verify-against-known-positive]]`, applied BEFORE the
number was published rather than after.

## 5. The measurement that settled the status column

The critic asked: of the classes our docs call synced, how many are hands-on VERIFIED vs merely
AS-BUILT? **I did not know. Measuring it decided the design.**

Over `COOP_SYNC_MAP.md` + `signals/TRACKER.md` + `COOP_ENTITY_EXPRESSION_MAP.md` +
`COOP_RNG_AUTHORITY.md`, 44 named classes resolve into the dump:

| Rung | Count |
|---|---|
| hands-on **VERIFIED** | **3** (`AactorChipPile_C`, `Aeyer_C`, `UpropInventory_C`) |
| **AS-BUILT** | 11 |
| row carries **no status token** | 30 |

A boolean "coop-correct" column would have scored ~44 green, because the docs name them as synced.
**The truth on the rung that means "works for the player" is 3 — a 14x overstatement.** This is the
same DESIGN-vs-VERIFIED gap the discipline forbids collapsing, and it is why the status column must
be a ladder.

## 6. CONVERGED design (question pass)

- **Unit of account = an exhaustive REGISTER, not a filter.** The dump enumerates; it does not
  classify. Asking it to answer a semantic question is what §3 disproved.
- **The numerator is a three-rung LADDER, each rung measured by its own source:**
  1. **structural floor** — 838 zero-own-function classes, `[V]` from the dump, by construction;
  2. **AS-BUILT** — a lane exists in code, grep-derived from `COOP_SYNC_MAP`;
  3. **VERIFIED** — hands-on green on both peers.
- **No rung is filled by eye.** The output is a profile across rungs, not one number — a single
  percentage would hide exactly the AS-BUILT-vs-VERIFIED gap §5 measured.
- **Nothing hand-typed.** `README.md` was stale by three builds (b122 vs the real b125) — the manual
  path has already demonstrably failed; fixed in `22d3c79c` against `protocol.h:708`.

## 7. NOT converged — owes its own `/qf`

**The status schema.** Precisely: what is grepped to earn AS-BUILT (class name? lane name?
`ReliableKind`?), and what counts as hands-on VERIFIED (a runbook log? a tracker row? whose
verdict?). Running that in the same thread would harden a frame that just moved twice. It is a
different surface and gets a fresh brief.

**The trap that pass must aim at (marked 2026-07-22):** the schema will INHERIT the
AS-BUILT-vs-VERIFIED gap this session paid for. "AS-BUILT is grepped from `COOP_SYNC_MAP`" is only
honest if the rung cannot be READ as "works" — it means *a lane exists in code*, nothing more. This
session measured both failure modes directly: Q-STACK was green as a lane yet SEQUENTIAL-ONLY
(`CONFLICT=0`, the CAS never ran), and R11 counted as synced until the census killed it. So the pass
must settle not only *what earns AS-BUILT* but *what stops the AS-BUILT count from reading as
readiness* — otherwise the profile launders a DESIGN claim as a measurement one rung higher, which is
exactly the defect the ladder was adopted to kill. AS-BUILT must not become a soft green between the
structural floor and hands-on.

## 7b. Where to look FIRST next time

Before proposing ANY dump-derived metric: §3 of this doc. A syntactic marker set over the CXX dump
cannot express "coop-relevant", and its false-positive side is structurally uncalibratable. The dump's
honest contributions are the exhaustive enumeration (2291, corrected — see §8.1) and the
zero-behaviour floor (838).

[[lesson-negative-grep-verify-against-known-positive]]
[[lesson-string-presence-in-cooked-asset-is-not-a-structural-fact]]

---

## 8. What the STATUS-SCHEMA pass changed here (2026-07-22 night, same day)

The `/qf` §7 asked for ran. It did not refine this document's answers; it **corrected two of its
measurements and replaced its central recommendation's unit twice.**

### 8.1 Two figures in §3-§5 are wrong

| §3-§5 says | measured 2026-07-22 night | why the earlier figure was wrong |
|---|---|---|
| **2305** BP classes | **2291** | Does not reproduce. Two independently written patterns (a bare `class X_C` token scan and a header-only scan requiring `:` or `{`) BOTH now yield 2291, and the set of referenced-but-never-defined classes is EMPTY. The 2305 came from a script that no longer exists; it is not defended, it is retired. |
| **3** hands-on VERIFIED | **2** | `Aeyer_C` was counted by reading a token. `COOP_RNG_AUTHORITY.md:68` states *"the row goes VERIFIED only on a live `owner_entity: OWN 'eyer_C' spawned locally` from a NATURAL/night roll"* — a pre-registered CONDITION, not a verdict. The calibration TRUTH this document leans on was itself partly tail-derived. |

The second correction **strengthens** §4's argument rather than weakening it: tail-parsing scored 6
against a re-count of 2, so the divergence is threefold, not twofold.

### 8.2 The floor survived, for a reason worth recording

838 is confirmed — but the first re-implementation produced **1170**, and that was a bug, not a rival
rule. `re.split` on a bare `class X_C` cuts a class body at its own MEMBER declarations
(`class Aprop_fireExt_C* fireExt;` matches identically), re-attributing every following function to
the member's TYPE. `AfireExtHolder_C` reported 0 own functions; it has 4. Caught by reading four
bodies the rule called empty. See `[[lesson-class-member-declaration-looks-like-a-class-header]]`.

### 8.3 The unit moved twice more, and the second move is the durable finding

§6 recommended a per-class ladder. The schema pass tested finer units and both failed the same test:

- **verb** — genuinely finer, and its anchor is genuinely STRONGER (a `vm_dispatch` registration is
  functional where a class-name literal is referential). Verb surface is **20711** (class,verb)
  pairs, machine, no free parameter. But `FindFunction`-with-a-literal anchors the CALL, not the
  sync (`Concat_StrStr`, `GetGlobalTimeDilation` scored as anchored; `ReceiveBeginPlay` alone spans
  621 classes), interception is only ~29 verbs, and **a large share of our sync mirrors FIELDS**
  (pose, `DeskInput`, weather) with no verb shape at all. A verb denominator asserts the mechanism.
- **the test that killed it** — the container's known red facet (simultaneous grab, `CONFLICT=0`) is
  a race on the slot FIELD. Under a verb unit, `addObject` and `takeObj` both read VERIFIED and the
  container shows GREEN. See `[[lesson-a-unit-of-measure-must-express-the-known-red-case]]`.

**Settled: a verb is the right unit for DIAGNOSIS and the wrong unit for a PERCENTAGE.** The next
unit candidate is a FACET (verb OR field-invariant OR race), and it is NOT built — its enumeration is
not single-source (verbs from the dump, fields from the lane, races from hands-on reds), so it opens
the per-system-profile `/qf` rather than closing this one.

### 8.4 As-built, and one more instrument bug

`tools/coverage.py` + `tools/verified_takes.tsv`. Every column one machine source with a named limit;
base-walk coverage is reported PER COVERING ANCESTOR and never summed (`Aprop_C` alone accounts for
443, so an aggregate reads as reach the generic lane does not have).

Second scope bug, found while looking for the `vm_dispatch` API: the literal scan covered only
`src/votv-coop/src`, missing `include/`. **161 -> 175** over the can-diverge set. Both instrument
bugs shared a shape — the error was in what the tool considered its own territory, so the tool agreed
with itself and nothing internal could surface it.

`README.md` carries `1453 / 175 / 12 / 2` with percentages as a share of the line ABOVE, never of the
whole, and each limit stated in the README itself. (The written-status column was `14` while the
README claimed the numbers were generated; moving it INTO the tool made it disagree — a shell heredoc
had turned `\b` into a literal BACKSPACE in two regexes, and the column had been reading 0, which
looked entirely plausible as a finding about the docs. Fourth instrument bug, same shape as the other
three.)

---

## 9. The per-system PROFILE brief (2026-07-22, for its own `/qf`)

> **BUILT 2026-07-23** — this brief's `/qf` ran (11 rounds, converged) and produced the deliverable
> `docs/COOP_SYNC_PROFILES.md`: three filled profiles on a five-axis falsifiable status model. The
> "spine may be the LANE" position below was TESTED and REJECTED — the spine is the SYSTEM, hand-named,
> because a system's facets cross files. Read the profiles doc for the as-built; the brief below is
> the point-in-time reasoning that led into it.

The user proposed a chain: **granule -> domain -> synced? -> in work? -> table**, and asked the one
question that decides whether it is a measurement or a rubrication: **which arrows are machine and
which are judgment.** Two arrows had no named anchor. Both were measured before writing this brief,
because a chain is honest exactly as far as its weakest link.

### 9.1 Link 2 (domain): a super-anchor EXISTS, and it partitions the wrong thing

Machine and parameter-free — the domain is the subtree of a named ancestor, no threshold:

| anchor | subtree |
|---|---|
| `Aprop_container_C` | 60 |
| `Aprop_food_C` | 149 |
| `Aprop_corded_C` | 83 |
| `AkerfurOmega_C` | 23 |

So the container — the first system we would profile — HAS a machine domain. But the systems carrying
the hardest sync work do not:

| system | measured |
|---|---|
| weather | **1 class**, no parent, no subtree |
| meadow | **0 classes** — the domain lives entirely in a save struct |
| desk | 4 classes under THREE unrelated parents (`3dPrinterAnim_C`, `container_C`, `corded_C`) |
| laptop / console | under `Aactor_save_C`, a generic 180-class root, not a domain |

And there is no middle tier for most classes: **361 of `Aprop_C`'s 456 direct children are leaves**,
so "domain = subtree" degenerates either to the class itself or to `Aprop_C` (1236).

**Finding: the super-anchor partitions the GAME'S OBJECT TAXONOMY, not our SYSTEMS.** It is the same
shape as the verb result in §8.3 — the machine unit expresses object-ness, while a large share of our
work is field-shaped, save-shaped, or console-shaped and has no taxonomic existence at all.

### 9.2 Link 4 (in work): the registry is machine, but it is keyed on the FILE

`research/findings/votv-take4-arc-open-threads-2026-07-22.md` carries a real parseable vocabulary —
`gate:none` x10, `gate:user` x5, `gate:me` x3 — so "in work" need never be read from memory.

But across its 199 lines it cites **9 `.cpp` files and 9 class tokens**, several of the latter
incidental (`cockroachMaster_C`, `door_C`, from a probe). **A thread->class join does not exist; a
thread->file join does.** And file->domain is precisely the path heuristic that answered BACKWARDS on
known positives (§8.3's rejected candidate) — so routing link 4 through the class re-imports a
rejected instrument.

### 9.3 What the two measurements jointly say

The only two anchors that exist are keyed on things that are **not the class**: the registry on the
LANE (a file of our code), the taxonomy on the SUBTREE (a family of the game's objects). The class is
the intersection where neither lives whole. That the profile's spine may therefore be the LANE rather
than the class or the verb is a POSITION, not a measurement — it is the `/qf`'s opening question, not
a conclusion of this document.

### 9.4 The brief, link by link

| link | anchor | verdict |
|---|---|---|
| 1. granule | dump (class), bytecode (verb), lane (field), hands-on (race) | MACHINE for class+verb; races are hands-on-sourced, so enumeration is **not single-source** |
| 2. domain | ancestor subtree | MACHINE **where a taxonomy exists**; absent for weather/meadow/desk/laptop. Not universal -> cannot be a column for every row |
| 3. synced? | the 3-rung ladder (floor / literal-named / cited take) | MACHINE, limits already named in `tools/coverage.py` |
| 4. in work? | the thread registry's `gate:` token | MACHINE **only if keyed by FILE**; via class it needs the rejected path heuristic |
| 5. table | — | output, honest iff 1-4 are |

**Rules the `/qf` inherits, not re-litigates:** any link without a machine anchor ships as an explicit
`[judgment]` column and is never presented as measured; and the unit is tested against the KNOWN RED
before any denominator is fixed — the container's simultaneous grab (`CONFLICT=0`, the CAS never ran)
must occupy a row, and under a verb unit it does not
(`[[lesson-a-unit-of-measure-must-express-the-known-red-case]]`).

**Do NOT build the container profile before the unit settles.** Container is the right first system
precisely because its red facet is the one a class-or-verb profile cannot see.

### 9.5 The lane spine inherits the same blindness unless it EXPANDS — the `/qf`'s first question

§9.3 leaves "the spine may be the LANE" as the opening position. It carries a trap that must be
tested BEFORE it is adopted, by this document's own rule (unit against the known red, before any
denominator).

One lane, `container_contents_sync`, carries facets of **different** status:

| facet | status |
|---|---|
| `addObject` receive (burgers) | VERIFIED |
| `takeObj` upward, v125 client->host | VERIFIED hands-on |
| **simultaneous grab** | **RED — `CONFLICT=0`, the CAS never ran once** |
| nested container-in-container | deferred by design |

So "the lane is synced" is the same lie at a coarser granule that "the container is synced" and "44
classes are green" already were. **A lane spine is honest ONLY if a lane row EXPANDS into facets
(verb / field-invariant / race) — never if it carries one status.**

The `/qf` therefore opens on: **is the lane a UNIT OF ACCOUNT (one row, one status) or a CONTAINER OF
ROWS (lane -> its facets)?** If a unit, the simultaneous-grab red is invisible under a green lane and
the spine is wrong in exactly the way class and verb were. If a container, this IS the per-system
profile already named, with the lane as key instead of the class. Calibrate against
`container_contents_sync`'s `CONFLICT=0` before adopting either.
