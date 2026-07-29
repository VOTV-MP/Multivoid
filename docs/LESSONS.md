# LESSONS — what VOTV_MP has learned the hard way

The single browsable ledger of durable lessons + **DIG-RULE** lessons. Each row is a takeaway, a
"look here FIRST next time" pointer where one applies, and a link to the full `memory/` file (the
authoritative detail). This complements — does not replace — `MEMORY.md` (the terse auto-memory index
loaded each session): `MEMORY.md` is the machine index; this is the human-readable, categorized digest.

**Maintained by `/documentize`** (Step 3.5): every sweep ADDS the session's new lessons and RECONCILES
existing rows for staleness (a row whose cited symbol/path moved is fixed or archived — a lesson pointing
at a dead symbol sends the next session on a worse dig than no lesson at all). A lesson earns a row only
if it saves a FUTURE dig.

> Links use the `memory/` slug: `memory/<slug>.md`.

---

## 0. The DIG-RULE (why this file exists)

**When a dig produces a hard-won measured fact, record it as a durable lesson so a future session reads it
instead of re-excavating the same hole.** Born because the project dug the same place twice (rock F2,
2026-07-08/09) and the user made it a rule. Two faces:

- **MAP-ALL-WIRE-EVENTS** — when a synced entity does not APPEAR / STICK / flickers / resets on a peer,
  MAP the full set of wire events ONE action emits (both peers, tick order) BEFORE building any fix; a
  later same-key event can silently UNDO an earlier one. `memory/feedback_map_all_wire_events_before_fixing_missing_sync.md`
- **PROBE-DON'T-GUESS** — MEASURE the root with a read-only probe before building a fix; a design stays
  provisional until the probe discriminates it. `memory/feedback_probe_dont_guess_rule.md`
- **VERIFY, don't re-derive** — recalled memory reflects what was true WHEN WRITTEN; verify a cited
  file/function/offset/flag still exists before recommending it (this is why staleness reconciliation is
  part of every sweep).

---

## 1. How to work (process / working agreements)

- **Converging a QUALITATIVE STATUS TAXONOMY: the test is "new VALUE vs new AXIS", never "a control
  that changes nothing" — and a converged FORM is not verified DATA.** Building the per-system sync
  profiles (2026-07-23, 11 `/qf` rounds), every control moved the status model (verdict×evidence /
  two-field remainder / sync-lane trichotomy / authority), and "keep profiling until one changes
  nothing" was declared as the convergence bar — WRONG: a qualitative vocabulary meets new facet shapes
  forever, so a change-nothing control is unachievable and the bar is unfalsifiable. The right bar:
  does a new instance add a new VALUE to an existing axis (normal, expected) or a whole new AXIS (the
  model was incomplete)? Each axis must be EARNED by a falsification instance — two rows that collapse
  (become indistinguishable) without it — and the vocabulary is complete only when every KNOWN measured-
  red maps in with no leftover; completeness is asserted, never proven, so it ships OPEN with the
  falsification test as the standing gate. SEPARATELY: surviving N critique rounds proves the FORM is
  coherent, NOT that the DATA is right — two cell VALUES were wrong (authority mislabeled) and both were
  caught by READING THE CODE, not by the form holding; the doc must SAY so or the next reader takes
  convergence for verification. Confirmed at scale 2026-07-23: a full ~67-system / ~200-facet sweep
  added a THIRD value (`peer-owned`) and still ZERO new axes, and the honest ceiling is WIRE-complete
  (provable: 113 kinds + 13 streams all accounted) but FACET-complete NEVER. *Look FIRST:*
  `docs/COOP_SYNC_PROFILES.md` §3+§8 (the axis set + the convergence call) and
  `[[lesson-converge-a-taxonomy-by-new-axis-not-a-null-control]]`.
  `memory/lesson_converge_a_taxonomy_by_new_axis_not_a_null_control.md`
- **A WIRE-LANE / enum census is BLIND to no-wire systems; completeness is WIRE-provable but
  FACET-never.** Filling the sync profiles to the whole tree (2026-07-23), a sweep keyed on wire lanes
  produced ~58 systems and MISSED six — three of which carry NO ReliableKind at all (moderation =
  GNS-close + host files; save-suppression = client-local hooks; spawn-authority = client-local park):
  "coordination by construction" the host wins by being the only peer not running the local suppressor,
  invisible to any lane census. Only a system-by-system source read finds them. The honest completeness
  ceiling splits in two: WIRE-complete IS provable (enumerate all 113 ReliableKinds → each has a router
  case → each maps to a system; all 13 unreliable MsgType streams cross-checked to a facet — no lane
  outside the catalog), FACET-complete is NEVER (a race is not a lane; a HUD/sound/guard has nothing to
  census). Also: the self-audit's `ReliableKind::\b` grep agreed with itself (word-boundary matched
  nothing → ~66 false MISSING); the reliable signal was the file-stem cross-check, hand-triaged.
  *Look FIRST:* `docs/COOP_SYNC_PROFILES.md` §9 (residual + dig) and
  `[[lesson-wire-census-blind-facet-completeness-ceiling]]`.
  `memory/lesson_wire_census_blind_facet_completeness_ceiling.md`

- **A CAUSING probe must prove its stimulus LANDED before its verdict means anything** (sharpens
  "a probe must COUNT, not confirm"). When the probe has to trigger the event it measures, an absent
  result line is ambiguous: dead lane, or dead trigger? Measured 2026-07-22: the R11b instrument fired
  `addLoot` on the first two world containers by registry order, both EMPTY, changed nothing — and only
  avoided reporting a false RED because it also measured its own effect and said
  `records 0 -> 0 ... the TRIGGER is inert, so an absent 'callback ENTERED' line says nothing about the
  lane`. Pick the target by the property that makes the stimulus VALID (a container with contents), not
  by convenience. Once fixed, the same instrument caught two real bugs in the lane it was testing.
  **Sharpened 2026-07-22: a positive control must name the SAME CHANNEL the test reads.** A runbook's
  host-side control grepped `PROP-DROP|SPAWN broadcast`; both are the wrong channel for the host
  (`PROP-DROP` is client-only, `SPAWN broadcast` is the `takeObj` POST observer that has never fired),
  so it returned 0 on a healthy run and the take read as VOID when it had passed — the host's real line
  is `host_spawn_watcher: spawn-seam adopted`. A mis-named control is WORSE than none: it manufactures
  a false negative and discards a real measurement. *Look FIRST:* grep the source for the exact log
  string, and confirm its emitter runs in the ROLE the step is performed as.
  **A SHAPE is not a COUNT of the disputed thing (2026-07-24)** — a readout was extended to print each
  inventory record's payload SHAPE (elements per value group) to test "a taken item lands empty". It
  FALSIFIED that (the taken record printed `{b5,f3,nm2}`) and still could not settle the weaker
  question, because the shapes differ BY CLASS (crowbar `{b5,f1,nm2}` / food `{b6,f4,i1,nm2}` / drive
  `+sig1`): the group slots are a **class fingerprint** present regardless of the VALUES in them, so a
  spawn-default record and a restored one of the same class print IDENTICALLY. *The general tell:* if
  the output would look the SAME under both hypotheses, it is a confirmation instrument, not a
  measurement — even when it just falsified something. Ask which VALUE differs between the two worlds
  (here: a `sig1` a fresh spawn cannot have) and whether your output contains it.
  `memory/feedback_probe_must_count_not_confirm.md`
- **A cross-source SUM instrument needs a positive control PER SOURCE, not one direction.** When a
  verifier's verdict is a SUM across multiple stores/peers, the positive control must be run so EACH
  source is, in at least one run, the known-positive that holds the target — else the un-exercised
  source's read is unproven and a real defect there reads as ABSENT. Measured 2026-07-23: the
  container-race no-dup verifier passed a `taker=host` control (`host 1 + client 0 = 1`), which proved
  the client sees the CONTAINER but NOT that the client's OWN personal-store walk finds X — exactly where
  a losing client's optimistic dup lives; a blind client-personal walk would make a real race dup read
  `sum==1` = false "no dup". Fix: the MIRROR control (`taker=client`, host idle) → `0 + 1 = 1` with the
  client copy at its own `idx=0`. A sum hides which source contributed (`1+0` vs `0+1` are the same
  total), so print WHERE each match was found (per-source slice id). *Look FIRST:* enumerate the sources
  of any aggregate-count instrument; run the control once per source; keep the matches decomposable.
  Pairs with `[[feedback-probe-must-count-not-confirm]]`.
  `memory/lesson_multi_source_count_needs_per_source_positive_control.md`

- **String presence in a cooked asset is NOT a structural fact** — a grep hit inside a `.uasset`/`.uexp`
  proves only that the string is in the package NameMap (UE bakes a shared string pool: a parent's member
  names and imported type names land in any asset that references them). Measured 2026-07-22:
  `grep -la propInventory_GEN_VARIABLE objects/*.uasset` → **168** classes incl. `candle`, `rug`, `poster`,
  `wisp_*`; the structural check (an **export** whose `ClassIndex` resolves to `propInventory`) → ~10, all
  real containers. The bad instrument manufactured a killer counterexample (`prop_toolbox`) that nearly
  forced a whole SCS-walk/CDO-probe predicate for an invariant that was correct all along — **it invented
  work**. Third instance of one family in a single session ("census" label; "append-order" from one
  `Array_Add` without reading the guard; this). *Look FIRST:* parse with `kismet-analyzer to-json` and read
  `Exports[].ClassIndex` / `.SuperIndex` / `LoadedProperties` through the `Imports` table; use `grep` to
  LOCATE candidates, never to CONCLUDE. `memory/lesson_string_presence_in_cooked_asset_is_not_a_structural_fact.md`
- **SAVE-EXCLUDED is not RUNTIME-ABSENT, and our logs cannot prove absence.** `prop_dronesack_C::
  ignoreSave -> EX_True` was read as "does not exist at runtime"; it means the SAVE SYSTEM skips it.
  `Aprop_dronesack_C : Aprop_C` is a real actor with its own `container@0x0380`. The corroborating
  `grep -ci sack <log>` = 1 was equally worthless: an actor with no save Key is never enrolled as an
  Element, so it can never print — **the log was silent about a thing it is structurally incapable of
  reporting.** The false conclusion then RETRACTED a whole line of investigation for a session and a
  half. Same family as the string-presence row with the sign flipped: there PRESENCE was read as
  structure, here ABSENCE was read as non-existence; both are "the instrument does not measure what I
  claimed". *Look FIRST:* ask what would have to be true for our logs to mention it (enrolment? save
  key? class filter?) — if any of those excludes it, enumerate live objects
  (`FindObjectsByClass`), do not grep. Read `ignoreSave`/`Transient`/`bNetLoadOnClient` as a
  SUBSYSTEM's treatment, never as existence. `memory/lesson_save_excluded_is_not_runtime_absent.md`
- **Run `/qf` (up to 15 rounds) BEFORE any non-trivial implementation + when planning new changes** —
  default to it; the adversarial pass is where crutches/wrong-layer/un-measured-assumption get caught
  before cementing. `memory/feedback_qf_before_implementation.md`
- **The /qf critic has a SELECTIVE-TRUST blind spot** — it interrogates claims individually + accepts the
  primary's answers as settled, so "trust a source for X, distrust it for Y" slips a whole pass. Skill
  patched 2026-07-13 (source-consistency / cross-answer / undone-measurement angles); still check manually.
  `memory/feedback_qf_selective_trust_blindspot.md`
- **The /qf critic escalates WITHIN THE FRAME it is handed** — so a MIGRATION design (repoint/rebind/re-key)
  that migrates the ONE identity map the brief names, while a PARALLEL map keyed on the same entity finalizes
  late, slips a whole multi-round pass (11 rounds + a "that holds" missed the host-only KerfurId table). When
  a design migrates identity, ENUMERATE every map that keys on the entity + prove the op updates or gates ALL
  of them. Skill patched 2026-07-13 nite (IDENTITY-MAP-COMPLETENESS angle + brief-enumeration + convergence
  bar). `memory/feedback_qf_enumerate_identity_maps_on_migration.md`
- **The /qf critic inherits the primary's BRIEF as ground truth (blind-spot #3: CARRIED-FRAMING)** — the
  fresh-per-round critic sees only what the primary wrote, and the primary writes its own brief, so a
  load-bearing NOUN it introduced as an inference and hardened by repetition ("the existing two-phase arm
  record" — actually FOUR distinct converge mechanisms) launders into an apparent fact every fresh critic
  reads blind. Worst in `/qf N` auto-loop (N self-summarized briefs, no external check). Fix: tag facts by
  PROVENANCE (measured-artifact vs carried-framing) + code-verify the 1-2 nouns the design hangs on before
  convergence + SURFACE to the user after any material REFRAME instead of auto-continuing (the user, holding
  the real history + raw artifacts, is the only party who catches framing drift, retroactive-foundation-
  invalidation, and cross-artifact synthesis). Skill patched 2026-07-14 (FRAMING-PROVENANCE angle + brief
  provenance-tag + reframe-surface + carried-primitive convergence bar).
  `memory/feedback_qf_challenge_carried_framing_not_just_the_frame.md`
- **A NEGATIVE grep is only evidence if the pattern can match a KNOWN-POSITIVE line** — before concluding
  "0 matches -> never happens / mechanism dead / gate clean", prove the pattern CAN match the positive case
  (grep one real hit; check the log line even CONTAINS the field you filter on). A query structurally blind
  to its target returns 0 and the null reads as PROOF. Worst kind: not a case that never arose, but one that
  arose EVERY time and was invisible to the query. Cost 2026-07-14: `grep 'grab_hook\[destroy-seam\].*kerfur'`
  =0 "proved" the destroy-seam never fires for kerfur (the line prints actor/key/eid, NO class) -> declared
  `TryCaptureKerfurPropDestroy` dead -> nearly RULE-2-deleted the guard sitting on bug1's actual relay.
  Corollary: when ONE negative-grep turns out blind, RE-RUN the audit on every other "0 fires" in the
  inventory. 2nd instance 2026-07-16: asserted "the master server isn't in the repo" from a `find -type d
  -iname '*master*'` — blind, because it's a FILE `tools/coop_master_server.py` (679 LOC stdlib) a dir
  search can't match. Search by the artifact's real shape (`glob **/*.py`, a signature string like
  `/v1/host`), not a guessed folder. **3rd + 4th instances 2026-07-24, both in ONE session and both the
  "convenient" way:** (a) wrong LEVEL — `loadObjects` showed 0 name-refs to `inventoryData`, read as "the
  load path doesn't touch it"; resolving CALLS shows `EX_LocalVirtualFunction loadData` x3 (it dispatches,
  like `loadTriggers`), and calls live as `StackNode` INDICES not names; (b) wrong SCOPE — grepped
  `save_block.cpp`, found no `saveObjects`, asserted "an undocumented second consequence" into a MEASURED
  doc; the contract is in `save_block.h` Part 3, which names `saveObjects` explicitly under a user mandate
  3 weeks older. **The tell both share: a zero was accepted because it made the story better.** A negative
  that FLATTERS the hypothesis needs the known-positive check MORE, not less. Also: `save_block: BLOCKED`
  = 0 across every log EVER (no known-positive anywhere — the detour has never fired), so its silence
  proved nothing. **5th instance 2026-07-24, in the very turn that wrote the near-twin lesson: wrong
  CASE.** Searched `"Player"` (the SDK header's spelling, `propInventory.hpp: bool Player`) for who sets
  the personal-inventory flag, got 0 everywhere, and wrote into a MEASURED doc that the setter was "not
  visible in any bytecode — native or a defaults blob, not our toolchain". The serialized BP property is
  **`player`**, lowercase, sitting in that same asset's component template
  (`propInventory_GEN_VARIABLE`: `index=0 player=True customVolume=50000`) — which answered the question
  outright and explained why the container's `loadData` override is an empty stub (the slot is baked at
  construction). Two spellings of ONE field. *The free tell:* 0 in EVERY package, including ones that
  must use it, is a blindness signal, not a finding — grep both the reflected and serialized spellings,
  or case-insensitively. **6th + 7th instances 2026-07-24 (the ini `/qf`):** (6) to prove a config token
  had "never been documented" I grepped `docs/` + `README.md` — and missed `release/votv-coop.ini`, a
  user-facing example ini in the repo that documents the token AND seeds the exact layout under design;
  the search space for "never documented" is *every artifact a user could receive*. (7) NEW SUB-SPECIES —
  **the CORPUS was blind, not the pattern**: a differential old-vs-new run over the 4 real inis reported
  ZERO verdict flips, but those files have no duplicate keys, no `yes|on|true` flag values, and the key
  filter excluded the one live phantom key, so the sample could not exhibit ANY change class. A clean
  corpus measures the corpus. Fixture with INJECTED positives, shared by every instrument, and a run that
  reports zero must first prove it can report non-zero.
  `memory/lesson_negative_grep_verify_against_known_positive.md`
- **A near-twin name (`X` vs `X2`) lets a DEAD function impersonate the live one — the discriminator is the
  CALLER COUNT, not the body** (2026-07-24). `mainGamemode::putObjectInventory` writes
  `saveSlot.inventoryData` x6, calls `getData`/`noRespawn`/`K2_DestroyActor`, plays `inventory_Cue` — it
  reads end-to-end like THE pickup path, and it has **zero callers game-wide**. All 24 apparent references
  are substring hits on `mainPlayer::putObjectInventory2`, a different function on a different class writing
  the OTHER store (`GObjStack`). Both grep polarities fail in opposite directions and neither is flagged: a
  substring grep says "24 callers" (all false); an exact grep says "0 calls here" — literally TRUE and
  substantively misleading, because the behaviour IS present via the `2` variant. Cost: `inventoryData`
  looked like it had a live pickup writer, when its only live writer is `saveObjects`' projection copy and
  gameplay never reads it back. **It had already bitten twice:** `COOP_DISPATCH_VISIBILITY.md` glossed
  `putObjectInventory` as "=R-pickup", and `votv-inventory-drop-spawn-RE-2026-05-24.md` listed it as a live
  helper with no note that nothing calls it — the stale row is the likely reason the 2026-07-24 pass started
  out treating it as live. Both corrected. *Look FIRST:* before building on "X does Y", grep **who calls X**
  with the BARE name and again as a substring, and compare the counts — a difference means a near-twin
  exists. In UE4 BPs a `2`/`_new`/`_old` suffix is the usual shape of a refactor that left the original
  compiled in. **And give the count its own known-positive** — "zero callers" is itself a negative grep:
  here the bare-name query still returned `mainGamemode` (the definition) and the substring query
  returned 24, proving the method reaches the corpus. Strip those and "0 callers" is indistinguishable
  from a blind pattern. `memory/lesson_near_twin_function_name_hides_a_dead_original.md`
- **A failure branch that shares a resolver with the success path is UNREACHABLE — and that dissolves the
  ambiguity without a run** (2026-07-24). `dup_verifier`'s `player=0` looked like it fused "read failed"
  with "found nothing", and two rounds were spent hedging + designing a control to force the failure.
  Structural answer, in the same file: `CountItemInstances` returns -1 at its own `!save` guard BEFORE any
  COUNT prints, and `INV::ReadAll` has exactly TWO returns (censused) — the SAME `ResolveSaveSlot()`, and
  `return true` (its offsets are compile-time constants, no post-resolve failure path), both in ONE GT
  task. So every historical `player=0` beside a scan summary already meant "read OK, found zero". *Look
  FIRST:* when a line looks fused, census the reader's return paths and check whether caller and reader
  gate on the SAME guard — reachability is a read, not a run. And when a control fails, read WHICH guard
  it hit: a control tripping a different guard than the one under test is a finding about guard ORDER.
  `memory/lesson_fused_failure_branch_sharing_a_resolver_is_unreachable.md`
- **A SYNTACTIC marker set over the class dump CANNOT express a SEMANTIC property** ("is this coop-relevant
  / can it diverge between peers"), and the reason is an ERROR ASYMMETRY, not a tuning problem: the
  false-NEGATIVE side is measurable, the false-POSITIVE side is uncalibratable in principle (there is no
  ground truth for "should have been counted"), so over-inclusion can never be bounded. Measured 2026-07-22
  while trying to GENERATE a coop-readiness % with the denominator taken from the game: a filter
  (`ReceiveTick` | `getData`+`loadData` | interaction verbs) over 2291 BP classes yielded 686 and threw 16
  of 45 already-synced classes (**36%**) into "inert content" — including `UsaveSlot_C` (32 own fns), where
  `GObjStack` lives. The set is Actor-shaped + player-interaction-shaped, structurally blind to non-Actor
  carriers (`USaveGame`/`UActorComponent`/`UUserWidget`) and to spawner/ticker/event behaviour. Two
  instrument defects on the way, both plausible-looking: ancestor-walking to engine roots returned "100%
  covered" (every class reaches `AActor`), and exact-name grep scored `Anpc_krampus_C` + all 11 `Awisp_*_C`
  as never-touched while our code matches them by SUBSTRING at runtime. **The dump ENUMERATES (2291 classes;
  838 with zero own functions are a structural floor that cannot diverge by construction) — it does not
  CLASSIFY.** Corollary that decided the status column, measured the same day: of 44 doc-named classes only
  **3** are hands-on VERIFIED, 11 AS-BUILT, 30 carry no status token at all — a boolean "coop-correct"
  column filled from doc claims would have scored ~44 green, a **22x** overstatement (originally recorded as 14x on a count of 3; `Aeyer_C` was a CONDITION, not a verdict -- corrected 2026-07-22 night) on the only rung that
  means "works for the player". Hence a LADDER (structural floor / AS-BUILT grepped / VERIFIED hands-on),
  each rung measured by its own source, reported as a profile not one number — and the AS-BUILT rung must
  never read as "works" (Q-STACK was green as a lane yet sequential-only, `CONFLICT=0`; R11 counted as
  synced until the census killed it). Look FIRST:
  `research/findings/architecture-audits/votv-coop-readiness-metric-DESIGN-2026-07-22.md` §3.
  `memory/lesson_syntactic_marker_set_cannot_express_semantic_relevance.md`
- **A class MEMBER declaration is indistinguishable from a class HEADER by a bare regex** — in the CXX
  dump `class Aprop_fireExt_C* fireExt;` matches `class X_C` exactly as a header does, so splitting on
  the bare form cuts each body at its own member declarations and credits the functions that follow to
  the member's TYPE. Measured 2026-07-22: `AfireExtHolder_C` reported 0 own functions while its four
  went to `Aprop_fireExt_C`, putting the zero-behaviour floor at 1170 where it is 838 — a 332-class
  error sitting under every ratio, and entirely self-consistent from inside the instrument. Caught only
  by READING four bodies the rule called empty. Require the inheritance colon or the opening brace.
  Corollary: "which of these two rules is right" was the wrong question — there was one rule and one
  broken one; ask whether each counts what it claims before comparing outputs. Look FIRST: the
  `CLASS_RE` comment in `tools/coverage.py`.
  `memory/lesson_class_member_declaration_looks_like_a_class_header.md`
- **Anchor a coverage/status claim to a REGISTRATION, not to a MENTION** — a registration (an entry in a
  dispatch/handler table) either reaches a callback or it does not; a mention (a name in a string
  literal, a grep hit) can serve a UI list or an enumeration. Measured 2026-07-22: the class-level
  literal anchor is false-positive on enumeration literals, and a file-path heuristic to separate them
  answered BACKWARDS on known positives (narrowed out the real lanes `ApiramidSpawner_C` /
  `Aticker_base_C`, kept `AATV_C` / `Abed_C`). The verb-level anchor (registration in `vm_dispatch`) has
  no such failure mode — so the FINER granularity carries the STRONGER anchor, inverting the usual
  expectation: granularity and anchor strength are independent axes. State the new anchor's own limit
  before building on it (registration cannot see PE-seam or field-poll lanes; size unmeasured).
  `memory/lesson_a_registration_is_a_functional_fact_a_mention_is_not.md`
- **A unit of measure must be able to EXPRESS the case you already know is red** — before adopting a
  unit for any coverage/readiness metric, take a failure you have already SEEN and ask what row it
  occupies. If it has none, the metric reports GREEN over it with the authority of a generated number.
  Measured 2026-07-22: the unit changed three times in one session and the same test killed each —
  class hid that the container's simultaneous grab is unexercised; VERB would have scored
  `addObject`/`takeObj` VERIFIED and shown the container green, because the red facet is a race on the
  slot FIELD and has no verb shape at all. The trap is that each new unit looked strictly better
  (finer + a stronger anchor), and "can it represent the known failure" is not a question about
  precision, so it never gets asked. Corollary: a unit is a claim about the MECHANISM, not just
  granularity — "verb" asserts behaviour is intercepted, but much of our sync mirrors FIELDS (pose,
  DeskInput, weather) and is invisible to a verb denominator on both sides. Look FIRST: run the
  known-red test BEFORE fixing the denominator, which is where a unit gets locked in.
  `memory/lesson_a_unit_of_measure_must_express_the_known_red_case.md`
- **Before changing a FUNCTION's behavior, enumerate ALL its call sites + state what each expects; before
  SUBTRACTING an output at a seam, enumerate every other producer/consumer at that seam** — acting on an
  incomplete map of what you're touching is ONE recurring root with many faces (a "mechanism" that is N
  mechanisms; a converge fn with an unenumerated 3rd/4th caller; a "suppressor" that is 3 coordinating
  broadcasters; a proxy criterion that only correlates with the real fact). A subtraction breaks unenumerated
  consumers SILENTLY (no error). Prefer the DIRECT fact over a PROXY. Cost 2026-07-14: captured-B wired into
  `ConvergeAfterConversion` without mapping its 4 callers (the POLL death-watch was the one that duped); fixed
  by enumerating the seam's 3 PropSpawn broadcasters FIRST -> "track-but-don't-broadcast" (remove the output,
  keep the tracked-flag contract the others coordinate on). `memory/feedback_enumerate_call_sites_before_changing_behavior.md`
- **A RULE-2 retire census = the NAME vocabulary + the ALIAS/DATAFLOW vocabulary** — s27's
  netloopback name-grep was blind to the `displayOffsetX` chain (net_pump::Tick param →
  puppet_drive shift, "loopback mirror") that only the dying scenario ever fed nonzero; and the
  first closing negative grep matched nothing INCLUDING its own known-positive because the pattern
  was narrower than the vocabulary. Walk the retired code's outgoing call expressions per-argument
  ("who else passes non-default here?") + grep prose synonyms, then gate on a control that must hit.
  `memory/lesson_retire_census_alias_vocabulary.md`
- **"per rule 1" = full green light** for the root-cause fix in its complete form (incl. hard
  architectural change). Don't scope down, don't ask "is this too big". `memory/feedback_no_crutch_questions_act_autonomously.md`
- **No design/architect AGENTS** — design yourself from code + docs + MTA; search + audit agents OK. `memory/feedback_no_design_architect_agents.md`
- **Claude OWNS every mechanical chore** (ini flags, grep/log-read, build, deploy) — never hand them to the
  user; the user does only in-game actions. `memory/feedback_claude_owns_all_mechanical_chores.md`
- **User ON the PC = USER tests**; Claude launches only user-AWAY + green-lit. `memory/feedback_user_tests_claude_prepares_ground.md`
- **Ask in PLAIN TEXT, never the AskUserQuestion UI.** `memory/feedback_ask_in_text_not_question_ui.md`
- **Never assert a VOTV game-domain fact from assumption** — verify vs SDK/bp_reflect/wiki FIRST. `memory/feedback_verify_game_domain_facts.md`
- **RE all related blueprints STATICALLY before any runtime probe.** `memory/feedback_re_blueprints_before_probes.md`
- **Read the cooked umap + BP bytecode before concluding a fact "needs a live probe."** `kismet-analyzer`
  (`research/pak_re/tools/ka/`) `to-json` on a cooked `.umap`/`.uasset` reads a PLACED actor's baked
  export name + its sublevel (cross-peer stable by construction — both peers load the identical file) AND
  a BP function's real dispatch (Kismet bytecode: makeKeys/BeginPlay are ubergraph stubs; `loadObjects`
  shows `GetAllActorsWithInterface`). `bp_reflection/*.json` are SIGNATURE-ONLY; the bytecode is in the
  extracted `.uexp` under `research/pak_re/extracted/`. Often a sibling subsystem in the two logs is an
  empirical control (door_box FName keysHash host==client through the reload that broke garage Key). take-4
  R9: I twice wrongly said "needs a live probe"; the user pushed back and the static tools closed it.
  `memory/lesson_read_cooked_umap_and_bytecode_before_concluding_live_probe.md`
- **Commit autonomously at verified checkpoints; still ASK before PUSH.** `memory/feedback_commit_autonomously.md`
- **Never retire a load-bearing fix on an unverified theory.** `memory/feedback_verify_before_retiring_a_fix.md`
- **SAME bug after 2+ targeted fixes = the patch LEVEL is wrong; the root is architectural** — stop patching, re-root. `memory/feedback_recurring_bug_is_architectural.md`
- **A cross-cutting axis has ONE owner** — handlers CAPTURE, never apply (anti-smear). Sharpened 2026-07-25 (CI /qf R6 reframe): the axis question applies to AUTOMATION too — a release workflow auto-committing a version bump made CI a second writer of main; demoting the robot to VERIFIER (refuse-to-publish preconditions) dissolved three rounds of machinery. Ask "who else writes this axis?" before designing anything that commits/pushes. `memory/feedback_one_owner_order_axis.md`
- **Fix a mirror-identity race WORKING first, generalize only after N>=3.** `memory/feedback_fix_then_generalize_mirror_identity.md`
- **Every source FOLDER = ONE domain concept; no catch-all names.** `memory/feedback_folder_per_domain_concept_rule.md`
- **RULE 2 does NOT apply to probes/diagnostics/tools** (they may stay) — but the exemption protects WORKING diagnostics, NOT stubs whose documented capability was already removed (s27 netloopback: doc said loopback verifier, code said stub-since-PR-2 → retired `e6f8576e`). Read the code, not the doc row. `memory/feedback_rule2_exempts_probes_diagnostics_tools.md`
- **Test/probe flags live in `multivoid.ini [dev]`, NOT bats/env.** `memory/feedback_test_flags_in_ini_not_bats_or_env.md`
- **`docs/piles/` is the LIVING pile KB** — mark DESIGN vs AS-BUILT vs VERIFIED. `memory/feedback_docs_piles_living_knowledge_base.md`
- **A diagnostic probe's built-in comparability/quiescent tag is only as good as its DERIVED inputs** —
  validate EACH gate input against the codebase's MEASURED field-behavior before trusting the tag; a wrong
  input silently mislabels samples (a clean diff on mislabeled data is worse than none). Two inputs broke
  the desk_diag `q=Y` tag (game-jittered knobs → always-N; unchecked active-filter integration → q=Y
  mid-ramp). Prefer clean DISCRETE state over float-delta heuristics; first check the log = do `q=Y`
  samples cluster at the real pauses. `memory/lesson_comparability_tag_inputs_need_measured_validation.md`
- **A handed-down measurement / build / on-disk noun is a CLAIM, not a fact** — verify it with grep
  (SDK+reflection+src) and `git status` BEFORE building on it, whoever asserts it (user, critic,
  prior-turn summary) and however confidently/repeatedly; **re-assertion AFTER a grep-refutation is a
  STRONGER red flag, not weaker.** Born 2026-07-15: four consecutive fabricated nouns
  (`serverStorageComp`/`ELEMENT`/`getAll`/`getServerStorage`, all 0 hits) + a "ship it" for a build
  `git status` showed never existed — caught every time by grep+git, never by reasoning.
  **SHARPENED 2026-07-22: verifying the CITED FACT is not verifying the CONCLUSION.** An agent's
  facts were all TRUE by grep ("`droneContainer` occurs in one asset", "`getObjectFromKey` is an
  exact `Array_Find`") but its LEAP ("therefore the drone spawns its own container") was never
  tested; it became the central result of a 733-line RE doc and a counting probe killed it on the
  first sample. Two levels: are the facts real (grep), AND does the conclusion follow / what would
  falsify it? "X can never happen, therefore Y" is a RUNTIME prediction — tag `[RD]`, settle with a
  count before building.
  *Look FIRST:* `memory/feedback_verify_handed_down_measurement_before_building.md`
- **A probe must COUNT, not CONFIRM — and must never resolve through the mechanism under suspicion.**
  A probe written to confirm "the drone spawns its own container" would have looked the container up
  BY KEY — the very operation suspected of being broken — and agreed. Written instead to enumerate
  `FindObjectsByClass` and print every row, it answered `containers=1` (the saved one, and it IS
  `drone.container`) on sample #1 and killed the conclusion. Second instance in the same probe: a
  recorded "mystery" (contents 2->0 in 8 s, a sack transfer theorised) dissolved once EVERY
  `GObjStack` slot was read instead of one — `[1] 2->1` paired with `[0] 3->4` is just a player
  taking an item. Watching one row invents mysteries the neighbouring rows explain.
  *Look FIRST:* `memory/feedback_probe_must_count_not_confirm.md`
- **Cite SECTIONS, not line ranges, in a file you are also editing.** Writing a new design doc I cited
  `ROADMAP.md:62-66` and `COOP_SYNCER_MODEL.md:324-326`, then edited both files later the same session
  to add supersession notes — the first citation became **circular** (it now pointed at my own
  supersession note instead of the claim being superseded) and the second landed on a bare `## 10.`
  header. Line numbers are for source you are NOT touching; a superseded doc gets a section anchor plus
  a quoted fragment. Re-read every line citation you wrote at the end of a doc sweep — your own edits
  are the likeliest thing to have broken them. Second instance in two days (the prior sweep found two
  lessons pointing at lines a fix had moved). **Same failure at tree scale:** renumbering the project
  phase arc shifted **24 "Phase 7+" forward-references across 6 docs** by one — and the tree carries
  TWO unrelated phase numberings (the project arc 1-8 vs `COOP_METHODOLOGY`'s work phases 0-5, which
  are also the `## Phase N` headings inside ROADMAP). A number is a citation into an ordered list, and
  ordered lists renumber: **name the target** ("the public-server phase") and it cannot rot.
  *Look FIRST:* `memory/lesson_cite_sections_not_lines_in_files_you_also_edit.md`
- **A source cannot confirm a belief it planted.** A `/qf` round caught me tagging a conclusion
  `inferred` while `ROADMAP.md` phase 6 already fixed that same conclusion as an "architectural
  commitment decided up front" — I had inherited the framing and then cited it back as independent
  corroboration. Also check the cited entry for FUSION: that one answered *who arbitrates* and *who
  simulates* at once, and only one half was still true. *Look FIRST:*
  `memory/feedback_qf_selective_trust_blindspot.md` (2026-07-20 section)
- **Your own tool can manufacture a false outage.** Python reported `certificate has expired` against
  the production master; the SERVER cert had 86 days left and `curl` verified the chain fine — the stale
  CA bundle was local. A ready-made causal story (snapshot cert + a documented renewal hazard) made the
  error read as confirmation rather than data. **Reproduce with a second, independently-trusted client
  before escalating**; for TLS read the served cert's own dates, which are a fact about the server, not
  the verify verdict, which is a fact about you. *Look FIRST:*
  `memory/lesson_your_own_tool_can_be_the_false_outage.md`
- **Classify a repeated literal by the QUESTION each site answers, not its syntactic role.** The
  `"Player"` nick literal sits at 7 sites that look like one group and are two — and the axis is **MY
  NAME vs SOMEONE ELSE'S**, not default-vs-fallback. `SanitizeNickname`'s empty fallback
  (`player_handshake.cpp:253` — re-cited 2026-07-28; the row read `:224`, and `:219` before that,
  which is three drifts and a standing argument for citing the SYMBOL over the line) reads as a
  fallback but decides *my* displayed name; changing too few ships two different defaults, changing
  too many labels a nameless remote peer with your name. A 2026-07-27 re-census found
  `peer_action_feed.cpp:53` printing a NAMELESS REMOTE PEER with the MY-NAME literal — the trap, live.
  *Look FIRST:* `memory/lesson_nick_default_axis_is_mine_vs_theirs.md`
- **A destructive UI action correlates by CONTENT, never by a snapshot-time index.** A
  persistent-until-dismissed report ages while it sits on screen; any unrelated write shifts line
  numbers/row ids, and a stale index deletes the WRONG target — or BOTH copies of a duplicate
  identity key (`player_guid` → orphaned inventory), with `removed==0` guards blind to
  "matched-but-wrong". Carry the VALUE the user clicked, re-validate it exists at act time, refuse +
  re-sweep on a vanish (arc-2 audit CRIT-2, fixed `7f1765ea`; drill G). *Look FIRST:*
  `config_ini_write.cpp RemoveDuplicateKeyLinesAt` — the pattern; grep destructive ops taking
  index/lineNo params. `memory/lesson_correlate_destructive_ui_actions_by_content_not_index.md`
- **Public claim surfaces (the website, the README) carry the same verdict-axis discipline as status
  docs (2026-07-26, the 13-round site /qf).** Every marketing VERB is a status claim ("is played
  together" over a smoke-only chain = false-PROVEN; "Kerfur works for everyone" survived on its HO
  row), every NUMBER needs a code anchor ("up to 4" → `kMaxPeers=4`), every image CAPTION must match
  the pixels (3 of 6 first drafts were false), retracted phrasings resurrect from the copy-SOURCE
  unless annotated at the phrase, and the surfaces must be diffed against EACH OTHER (site↔README:
  install path, player count). *Look FIRST:*
  `memory/lesson_public_claim_surfaces_carry_verdict_discipline.md` + docs/COOP_SYNC_PROFILES.md
- **Site dev-loop instruments lie in THREE measured ways (2026-07-26, +1 on 2026-07-27):**
  `zola serve`'s Windows watcher silently misses external edits (serves STALE memory bytes;
  `public/` is not refreshed by serve at all, and a `zola build` run BESIDE a live serve does not
  change what serve returns — only a restart does) → curl the served asset vs disk, restart zola,
  always fresh `zola build` before deploy; headless Chrome (legacy AND new) clamps its layout to a
  ~500px minimum window → a 360px screenshot fakes mobile overflow that no real phone shows; and
  **zola MINIFIES the built HTML**, so `grep 'id="qa"' public/index.html` false-negatives on
  `id=qa` and, since the page is ONE line, `grep -c` caps at 1 — a 2026-07-27 false negative was
  stated to the user as "the site has no Q&A section" when it had five. Grep the TEMPLATE, or use
  an attribute-agnostic pattern plus `grep -o | wc -l`. *Look FIRST:*
  `memory/lesson_site_dev_instruments_stale_serve_and_chrome_clamp.md`
- **Ask before changing the user's system/network settings (USER CORRECTION 2026-07-26).** A correct
  diagnosis (stale upstream DNS cache) is not a license to reconfigure the machine: the elevated
  DNS-to-1.1.1.1 change got an immediate "верни как было" — the router does local DNS the bypass
  would break. Read-only probes are free; any write outside the project tree (DNS, services,
  firewall profiles, registry) = name the change + ask, offer the in-place alternative (here:
  restarting the router's Unbound, which is what worked). *Look FIRST:*
  `memory/feedback_ask_before_changing_user_system_settings.md`
- **A census of ONE operation KIND reads as a complete census of the path.** Enumerating every
  *widening* conversion on the nickname path was exhaustive — for widening — and therefore FELT
  complete, while four raw *truncations* on the same path stayed invisible for fifteen `/qf` rounds
  (`config.cpp:508` `resize(255)`; `player_handshake.cpp:302` and `:573` `resize(200)` AFTER `ToUtf8`;
  `SanitizeNickname:212` capping in UTF-16 units — **all four are RETIRED as of `9ae83454`**: the caps
  are `coop::text::CapUtf8Bytes` / `CapCodepoints` and `tools/text/nick_gate.ps1` polices the verb). Two of them would have manufactured the ill-formed
  UTF-8 that the same design's new fail-closed receive boundary rejects — the feature would have broken
  its own sender's nick and looked like a wire bug. Grep per VERB (`resize`, `substr`, `memcpy`,
  `snprintf`, `WideCharToMultiByte`), not per concept; and prefer a TYPE owning capacity + truncation
  over a corrected list of sites. **SECOND INSTANCE 2026-07-27 (arc A):** the design's census of
  `peerConns_` write sites listed FIVE; there are SEVEN — it grepped `.store(` and both
  `.exchange(0)` clears (`Session::Stop`, `Session::Kick`) were invisible. Kick's was load-bearing:
  without its generation clear a kicked slot keeps a live occupancy token and the ledger never
  empties the row. The repair was to mechanise the census by OPERATION KIND —
  `tools/net/peerconn_gate.ps1` requires a `GEN: mint|clear|none -- <why>` annotation at every
  mutating verb, FAILS on a zero-site census (a gate that finds nothing has gone blind, and green is
  not evidence), and carries six fixture must-fire controls. It immediately caught the author's OWN
  eighth site (`KickWithToken`'s compare-exchange) and refused to build. *Look FIRST:*
  `memory/lesson_census_the_operation_kind_not_only_the_sites.md`
- **An EDGE detector on state a peer cannot observe is silently DEAD, not wrong.**
  `subsystems::DisconnectSlot` — ~20 per-slot person-state teardowns — hung off a falling edge of
  `IsSlotReady`. On a CLIENT that latch never RISES for slots 1-3 (a client only fills
  `peerConns_[0]`), so the whole fan-out never executed there for the life of the project: a
  departed third peer's voice channel, prop/owner-entity mirrors, trash proxies, flashlight cache and
  Player Element survived to session end, frozen puppet still standing. A dead handler leaves NO
  evidence — no wrong value, no warning — and it works perfectly on the host, which is where anyone
  debugging looks. Ask, PER ROLE, whether the predicate is structurally reachable; transport-derived
  predicates are asymmetric by construction. The repair shape is always the same: replace the edge
  with a comparison against a VALUE both peers hold. *Look FIRST:*
  `memory/lesson_an_edge_that_never_rises_never_fires.md`
- **Baseline an instrument on something the system does not reset under it.** Three consecutive
  harness bugs while building the arc-A departure drill, each of which made it test NOTHING while
  looking like progress: `multivoid.log` is APPENDED across runs (so the previous run's roster
  matched and the drill fired before any peer existed), then it turned out to be ROTATED at boot
  (so the line-count baseline fixing that could never be exceeded and the wait hung), and `smoke4`'s
  default monitor window expired and killed the peers mid-settle. Also `Get-Process VotV` finds
  nothing — the image is `VotV-Win64-Shipping`, `VotV (Client)` is only the window TITLE. Cue on what
  you CONTROL (the process set), grade on what you MEASURE (the log); never both on one artifact. A
  drill that cannot tell "nothing to test" from "passed" is worse than no drill. *Look FIRST:*
  `memory/lesson_baseline_an_instrument_on_something_the_system_does_not_reset.md`

- **MID-ACTIVITY JOIN is ALWAYS handled, per RULE 1 — it is architectural principle 8** (USER RULE
  2026-07-19). A peer joining mid-event / mid-decode / mid-download / mid-ping / mid-playback /
  mid-drive / mid-ANYTHING is never an unsupported edge case. Every sync lane MUST define and
  implement its late-join answer (snapshot / seed / park / replay / unlatch) AT THE ROOT; "don't join
  during X", a suppressive filter, or an undefined window is a crutch. A new lane is not DONE until
  its mid-join row exists. *Look FIRST:* the per-lane answer table in `docs/COOP_EVENT_JOIN.md` — it
  is the pattern, and it extends to every activity lane, not just events.
  `memory/feedback_mid_activity_join_per_rule1.md`

- **AUTO-RUNBOOKS: where a check needs EYES, SCREENSHOT it — don't hand the check back** (USER RULE
  2026-07-27: *"Where you need eyes you should just screenshot and point to me those screens, thats
  how we are going to run auto handbooks from now on."*). Writing "whether it LOOKS right is
  irreducibly human" into a runbook is the wrong output. Drive the scenario autonomously, capture at
  the DECISION MOMENT, and put the picture in front of the user — their job is the verdict on the
  picture, not operating the game to produce it. Only what a picture cannot carry (a felt hitch, a
  between-frames flicker, subjective smoothness over minutes) stays manual, and must be NAMED, never
  used as a catch-all for "I didn't build the capture". Photograph the peer where the BUG lived, not
  the convenient one — `mp.py scoreshot` captures the HOST roster, which was never broken.
  Corollary (same user, same day): **"an autonomous run can't press X" is almost never true — check
  before adding a dev bypass.** We already drive bots that act in the world; the overlay owns a
  WndProc hook, so `PostMessage(hwnd, WM_KEYDOWN/KEYUP, vk, 0)` per window drives the real binding
  with no focus stealing across four peers (`SendInput` cannot). The `VOTVCOOP_SCOREBOARD_OPEN=1`
  bypass had been hiding two facts a real keypress exposes immediately: the player list is
  **tilde (`VK_OEM_3`), not TAB** (a runbook handed to the user said TAB throughout), and it is
  **hold-to-peek on a client, toggle on the host**. *Look FIRST:* `tools/net/roster_shot.ps1`.
  `memory/feedback_show_screens.md`

- **A readiness ANNOUNCEMENT is not evidence of the VISIBLE state it precedes.** Measured 2026-07-27:
  BOTH obvious "peer is ready" markers fire while the peer is still on a loading screen —
  `net_pump: ClientWorldReady announced` means the SYNC layer is satisfied and precedes the level
  `open` completing; `harness: ==== PLAY READY ====` fires earlier still. A screenshot gated on either
  caught CLIENT_3 behind the unclickable OMEGA content-warning screen, photographing "PLAYERS
  offline" — and a COUNT-based drill would have passed it, since the log had all four rows. Gate on an
  EFFECT that requires the state you need (another peer receiving this peer's pose stream ⇒ a live
  simulating pawn), preferably logged by a DIFFERENT component than the one you are waiting on. Also:
  size a hold in FRAMES not milliseconds — 600 ms was ~2 frames on a peer at 4.2 FPS.
  **SECOND INSTANCE 2026-07-28, now with a NAMED mechanism:** `smoke_i18n` gated its chat typing on the
  peer's own `Joined X's game` line; client 3 logged it at 22:13:28 and still would not accept a
  keystroke ~16 s later, while demonstrably RECEIVING the other peers' messages. `CaptureActive()`
  (`ui/imgui_overlay.cpp:136`) counts `LoadingOpen()`, so the loading cover OWNS INPUT and swallows the
  `T` bind whole — a marker can be true about the SESSION and false about the INPUT PATH at the same
  instant. Same run, same shape: `VOTVCOOP_SCOREBOARD_OPEN=1` also lands in `CaptureActive()` via
  `ScoreOpen() && LocalIsHost()`, so **a fixture that opens a UI surface for a screenshot disabled the
  bind the test was about.** Sharpened rule: gate on an effect ON THE PATH YOU ARE ABOUT TO USE — if
  you are about to type, require the artifact (`chat: sent`) and FAIL rather than retry silently.
  *Look FIRST:* `tools/net/roster_shot.ps1` preconditions; `coop/dev/menu_proceed.cpp` for why OMEGA
  cannot be clicked. `memory/lesson_readiness_announcements_precede_visible_state.md`

- **ONE COLUMN, TWO AXES — a fused display axis stays invisible until a SECOND viewpoint renders it.**
  Measured 2026-07-27 from the first 4-peer screenshot of the player list: `Link` fuses the peer's
  TRANSPORT to the session (LAN/P2P/relay) with MY ROUTE to that peer (direct vs host-relayed →
  "VIA HOST"). On the HOST's board the two coincide, so it looked coherent for as long as only the
  host listed peers; arc A made a CLIENT list them and the column then showed transport on two rows
  and routing on two others, on an all-LAN session. The client cannot fix it locally —
  `LinkLabelForSlot` reads `peerConns_[slot]`, which a client owns only for slot 0. Ask of every
  per-peer column: **is this a property OF that peer, or of MY relationship to them?** "VIA HOST" read
  as information, which is why it survived; a blank would have been caught sooner.
  **CORRECTED 2026-07-28 when it was fixed (v131):** the doc's own claim *"a client cannot fix it
  locally"* was FALSE — `session_status.cpp:491-494` bailed on `hConn == 0` **above** the
  `cfg_.topology` branch that returns `"LAN"` without needing any connection, so **an ownership
  bail-out placed above a config-derived answer is what made a shared fact look role-exclusive.**
  The publish rule still fires, but for the PING (per-connection in every topology, so no client can
  ever derive another client's) — visible as `--` in the same screenshot. And the `LAN` verdict was
  never a measurement either: a port-forwarded WAN peer printed `LAN`. **The rule the episode reduces
  to: publish or drop, never synthesise** — a coarser same-axis value shown everywhere is a scope
  decision; a different-axis value invented locally is a lie, and so is a plausible value nobody
  measured (a published host ping of `0` renders as `<1ms`). *Look FIRST:* `coop/net/link_kind.h`,
  `Session::LinkKindForSlot`, `ui/link_format.{h,cpp}`.
  `memory/lesson_one_column_two_axes_transport_vs_route.md`

- **AN INSTRUMENT BLIND TO THE PHENOMENON ALWAYS REPORTS "NOT PRESENT".** Measured 2026-07-28
  chasing the no-cursor bug: `tools/capture-window.ps1` does no `GetCursorInfo`/`DrawIconEx`
  compositing, so **no screenshot this repo has ever taken can show an OS cursor** — a capture is
  identical whether the bug exists or not. Stacked on top: ImGui's Win32 backend only updates
  `io.MousePos` when the window is **FOREGROUND**, so an unattended capture renders no software
  cursor *by construction* (the first probe run hit exactly this and looked like a clean repro).
  A negative is evidence **only if the instrument could have produced a positive**. *Look FIRST:*
  ask what the capture path physically records; pair every visual probe with a known-positive
  frame; for cursor-shaped questions prefer a state read (`GetCursorInfo` gives `flags` +
  `ptScreenPos`, which separates hidden from recentered — a photo cannot); and any focus-dependent
  measurement must `SetForegroundWindow` and ASSERT it took.
  `memory/lesson_an_instrument_blind_to_the_phenomenon_always_passes.md`

- **VERIFY ROLE-EXCLUSIVITY BEFORE INVOKING THE PUBLISH RULE — read the accessor top to bottom.**
  Measured 2026-07-28: a 15-round design pass was founded on "only the host can measure a peer's
  link". False. The accessor returned `"LAN"` from a session-wide CONFIG value that every peer holds;
  it merely returned early on `hConn == 0` first. The guard was about OWNERSHIP, the answer was about
  CONFIG, and the ordering made one look like the other. Cost: a wire field justified on the wrong
  grounds for four rounds. **The tell: a function that "can only be answered by role X" but whose
  answer body never touches anything role-specific.** *Look FIRST:* read the whole accessor before
  writing "only X can know this" in a design; and state which measured line makes it exclusive.
  `memory/lesson_verify_role_exclusivity_before_publishing.md`

- **A failing selftest is a claim about TWO things — check the EXPECTATION before the code.** Measured
  2026-07-28: the nickname-arbiter selftest failed 13/14 asserting a name the arbiter can never emit (a
  19-char stem + `"10"` is 21 characters, over the 20-cap) — the code was right, the test was wrong. The
  same day the codec selftest failed on `CountCodepoints(...) == 2` and that one was REAL but two layers
  down: MSVC without `/utf-8` decodes source literals with the SYSTEM codepage, so every non-ASCII
  literal in the tree was locale-dependent. Note which assertion caught it: every ROUND-TRIP case passed,
  because a round trip is self-consistent under a shared corruption. **Include at least one ABSOLUTE
  assertion per selftest**, and treat `/utf-8` as mandatory on MSVC. *Look FIRST:* re-derive the expected
  value by hand, constructing it the way the code numbers rather than the way it reads.
  `memory/lesson_a_selftest_expectation_can_be_the_bug.md`
- **A capability can be silently STRIPPED where you expected an assert — and no compiler control can see
  it.** Measured 2026-07-28 pricing ImGui 1.92: `imgui_impl_dx12.cpp:984` does
  `io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures` when the legacy `Init` signature is used. It
  does not assert; it degrades. The upgrade would have paid its whole cost (a submodule bump, a semantic
  sweep of four kept redirects) for none of its benefit — 16-64 MB and a 139-416 ms atlas rebuild instead
  of 0.25 MB — **DX12-only, clean compile, no warning**, on the RHI least likely to be tested. Worse, the
  headless probe SET THAT FLAG ON ITSELF, so it measured a configuration the product never reproduces
  (`grep RendererHasTextures src/` = zero). *Look FIRST:* when an upgrade's VALUE rests on a capability
  flag, grep the vendored backend for `&= ~<FLAG>` before believing any benefit number, and ship a boot
  assertion that the flag survived on EVERY backend.
  `memory/lesson_a_capability_can_be_silently_stripped_not_asserted.md`

- **A drill whose inputs stay UNDER the threshold proves the half that was never broken.** MEASURED
  2026-07-28: arc D1 shipped "Cyrillic nicknames work", photographed with `Пельмень` — 8 characters,
  **16 UTF-8 bytes**, comfortably under the **23-byte** buffer cliff that BLANKED the roster row at 12
  Cyrillic characters (`WideCharToMultiByte` returns 0 on overflow; it does not truncate). Every drill
  name passed by being short. And the photograph framed the **TAB board** — while the request had
  literally named the **floating nameplate**, which was still rendering `????????` and had never once
  appeared in a drill shot. *Look FIRST:* derive the drill input from the CODE's boundary
  (`cap/bytes_per_unit + 1`), never from a plausible-looking example; and require one frame per SURFACE
  the change touches — a shot of a different surface is not weak evidence for this one, it is none.
  State the input's MAGNITUDE in the evidence line, not just its script.
  `memory/lesson_a_drill_that_stays_under_the_threshold_proves_the_wrong_half.md`

- **A gate that polices ONE verb makes the whole path look policed.** MEASURED 2026-07-28:
  `tools/text/nick_gate.ps1` — fail-closed, positive-controlled, and whose header **quotes** "grep the
  VERB, not the concept" — policed only `resize|substr` and sat GREEN over four shipped NARROW defects
  on the same lane (a `WideCharToMultiByte` blanking two surfaces, an ASCII squash on the nameplate, a
  filter dropping a name from a record), plus a fifth latent owner it could not see at all because a
  literal `char nick[24]` is a *declaration*, not an operation. Citing a lesson is not applying it: one
  verb reads as "the verb" to whoever writes it. *Look FIRST:* enumerate the lane's verbs exhaustively
  before the first pattern (truncate / narrow / widen / copy-into-fixed / declare-a-width), state in the
  header which are NOT covered, and **injection-prove every detector against the exact code you retired**
  — a gate that has only ever been green is an assertion, not a measurement.
  **SECOND OCCURRENCE 2026-07-28 (same gate, new dimension):** the widened gate still keys on the
  RECEIVER'S SPELLING (`\bnick[A-Za-z0-9_]*\.`), so `nickname_arbiter.cpp:37`'s `stem.substr(0, keep)`
  — a UTF-16-unit cut that can split a surrogate pair — is invisible and the gate is green and blind
  again. **A gate must key on the operation's SUBJECT (what the value IS), never on what the variable
  is CALLED**; a naming convention is not an invariant. Tell: a domain noun used as an identifier
  prefix inside a regex that is meant to prove a property.
  `memory/lesson_a_gate_on_one_verb_reads_as_a_gate_on_the_path.md`
- **A question handed to you to DECIDE can be MALFORMED — check its premise before answering.**
  Measured 2026-07-28: a design carried an "open product boundary" for days ("which hanzi set counts as
  common **also decides which NAMES are accepted**"), it survived a 19-round pass, and the user
  delegated it explicitly. The premise was false against HEAD — `SanitizeNickname` had become a
  DENYLIST the same day, so every hanzi and emoji was already accepted and the font set decided
  nothing. Answering it would have produced a converged answer to a question that had stopped
  mattering. *Look FIRST:* an open boundary inherited across passes is `carried-framing`, not a fact —
  **re-derive its PREMISE against the code before treating the question as the work.** The tell is a
  premise phrased as a consequence ("X *also decides* Y"): that clause is a claim about code and it is
  one grep away. `memory/lesson_a_delegated_question_can_be_malformed.md`
- **Oscillation on an axis means the axis is not what decided it.** Measured 2026-07-28: a `/qf` pass
  flipped FOUR times on demand-vs-eager font baking (R8 delete, R9 restore, R10 kill, R11 restore),
  every flip backed by a real fresh measurement and none of them converging. R12 found the actual
  contradiction, which was not on that axis: the thing demand existed to afford (CJK) had been rejected
  on **NEED**, and every later round re-argued **MECHANISM** — so re-admitting it on *affordability* was
  smuggling. *Look FIRST:* treat the **second** reversal on one axis as a stop signal, then stop arguing
  the axis and write side by side (a) the ground the dependent decision was originally rejected on and
  (b) the axis you are now arguing. Different words = you cannot converge, only smuggle. State the
  original ground in the next brief so the critic can test it. And **disclose the oscillation in the
  write-up** — the record of what was deleted and why is what stops the next session re-deriving all
  four positions. **SECOND OCCURRENCE 2026-07-29, and the stop signal was missed by FOUR reversals:** a
  chat-history `/qf` argued "where a retained line's nick colour is captured, and from which live table"
  across rounds 13-18, and *every round's fix replaced the previous one* (bake-at-push -> bake-at-
  retirement -> push-inversion -> palette merge -> neutral colour -> user says "no visual distinction").
  Six rounds, four reversals, **net diff ZERO**. Two sharpenings: (a) **every round being individually
  correct is not progress** — only the reversal COUNT is evidence; (b) **if four engineering fixes have
  failed on one axis, ask whether the question was ever engineering's to answer** — round 17's
  "dissolution" was a look-and-feel call, and it collapsed in one message once it went to the user.
  `memory/lesson_oscillation_means_the_axis_is_not_what_decided_it.md`
- **A red verdict can be the INSTRUMENT's defect, not the feature's.** Measured 2026-07-29:
  `smoke_i18n` reported six failures of the form "HOST: never saw 'привет всем' -- it was blanked,
  squashed or truncated somewhere on the way", against a log holding every line byte-intact and a chat
  lane working end-to-end on four scripts. Root: `tools/mp.py:1345` `_log_count` called
  `read_text(errors="replace")` with **no `encoding=`**, so a RU Windows box decoded the UTF-8 log as
  cp1251 and every non-ASCII needle compared against mojibake (0 hits -> 2/9 after the fix, `781245b1`).
  Every other `read_text` in that file passed the encoding; this was the single escapee, and the one the
  whole i18n verdict rests on. *Look FIRST:* the blind-instrument lesson trains suspicion of GREEN —
  **red is unaudited**, and here the failure text plausibly named a defect class the project HAD shipped
  the day before. Reproduce the assertion against the raw artifact with an explicit encoding before
  believing it; and in a file where most call sites pass an option, grep for the ones that don't.
  `memory/lesson_an_instrument_can_fail_the_feature_it_tests.md`
- **A drill on ONE TERM OF AN `||` is blind unless every other term is false — and a config DEFAULT
  decides that.** Measured 2026-07-29: a design narrowed `chat_feed::HasAny()` to fix an overlay-frame
  leak and specified four drills; all four would have PASSED on a broken build, because `hud.cpp:415-419`
  `IsActive()` is a disjunction whose last term is `voice_chat::Enabled()`, and
  `config_registry_rows.inc:113` defaults `voice.enabled` to **true** (every smoke log shows voice
  starting on every peer). The term under test was unreachable; the defect would have shipped, visible
  only to a player who turns voice off. *Look FIRST:* when the change under test is one term of an OR
  (or one guard in a chain), **enumerate the other terms and find each one's DEFAULT before writing the
  drill** — "every other term false" is a precondition and belongs in the drill spec. Grep the config
  registry for the default, not the code for the flag. And ask who chose the drill's environment: if the
  answer is "whatever the harness does", you are testing the default configuration, which is exactly
  where the bug hides. `memory/lesson_an_environment_default_can_mask_the_thing_under_test.md`
- **Derive a probe's WORST CASE from the dedup key, never from a hand-written row.** Measured
  2026-07-28: `atlas_probe`'s `kWorstFamilyForRole = {0,1,2,3,0}` reads as "every role a different
  family" but hands Toast the same family as Menu, producing **4** faces where the ceiling is **5**
  (four roles share `(16 px, regular)` so four families give four faces; Chat is `(18 px, BOLD)` and can
  never dedup with them). A committed measurements doc then recorded 4 as the ceiling, understating
  every worst-case cell **2x in VRAM and 1.75x in time** — while the default cells stayed correct, which
  is why nothing looked wrong. *Look FIRST:* any instrument row labelled worst / max / ceiling / bound is
  a CLAIM; derive it from the dedup key and assert the probe reaches it (`faces == expected`).
  `memory/lesson_derive_a_probes_worst_case_from_the_dedup_key.md`

- **A log line can VANISH because of its arguments — and then every log-driven gate lies "broken".**
  Measured 2026-07-28: `%ls` in `std::vsnprintf` converts wide→narrow through the C locale, which
  encodes nothing above U+007F; the call returns −1 and MSVC leaves the buffer **empty**, so
  `ue_wrap::log::Write` emitted a bare `[21:04:18] [INFO ] ` with no message. Every line naming a
  Cyrillic, CJK or emoji peer had been vanishing whole since the first Cyrillic nickname — including
  `nickname_arbiter: slot N asked … -> assigned …`, the decision line of the entire naming arc. The
  first arc-D2 gate run therefore reported a **RELAY GAP** (`mp.py` greps
  `roster: client installed cross-peer identity slot=(\d+)`, which had been deleted by the formatter),
  and two runs were spent de-braiding a product failure that did not exist. Probe: `%ls` of `"Пел"` →
  `n=-1`; via `_create_locale(LC_ALL, ".UTF-8")` + `_snprintf_l` → `n=17` and correct UTF-8. Fixed with
  `_vsnprintf_l` + a private `_create_locale(LC_CTYPE, ".UTF-8")` — **never `setlocale`**, since we are
  injected and LC_CTYPE is shared CRT state the game reads — plus a floor that logs the format string
  when args fail. **Both halves of that call were wrong on the first try and both were caught by audit,
  same day:** `LC_ALL` drags `LC_NUMERIC`, so every `%f` in the log became `1,50` on a ru-RU machine
  (301 call sites, and mp.py parses those numbers); and the `_s` variant `__fastfail`s on a malformed
  specifier, PAST SEH — see the two rows in section 8. *Look FIRST:* when a log-driven verdict says a
  subsystem did nothing, prove the LINE can be written before believing it; a negative log grep is
  evidence only if a positive was possible.
  `memory/lesson_a_log_line_can_vanish_because_of_its_arguments.md`

- **Search prior art by the PROBLEM, not by the mechanism you assumed — and a grep whose hits you don't
  open did not happen.** Asked to ship fonts *"as MTA does"*, I grepped MTA for `download|transfer|
  resource|http`, found `CResourceFileDownloadManager`, and put it to the user as **"the MTA option"** in
  a decision fork. MTA does not ship fonts that way at all: `CEGUIFont.cpp:753/:827` rasterises glyphs
  **on demand** on every `getTextExtent`/`drawText`, with page granularity (`:1489`), a substitute-font
  fallback (`:1519`), an LRU stamp (`:1505`) and pages that are genuinely freed (`:1595`); and
  `CGraphics.cpp:1488/:1549` hands the font FILE to Windows (`AddFontResourceEx(path, FR_PRIVATE, 0)` +
  `D3DXCreateFont(..., DEFAULT_CHARSET, ...)`). Both mean *the OS owns glyph supply*; the downloader
  carries `RESOURCE_FILE_TYPE_MAP/SCRIPT/CLIENT_FILE`. **The aggravating detail: the correct grep ran in
  the same message and returned `CGraphics.cpp` + `CLuaGUIDefs.cpp` — I followed the download hits and
  never opened the font hits.** A successful search for the thing you expected feels exactly like
  confirmation; nothing errors, nothing comes back empty. The standing MTA rule was satisfied in letter
  (I grepped, I cited real files) and broken in spirit. *Look FIRST:* key the search on the problem noun
  ("how does MTA get a glyph on screen"), open the file that does YOUR job before citing the precedent
  in a fork you hand someone else, and treat an un-opened hit as a search that did not happen. Checking
  properly also **retired an arc-D2 objection** — "a cache that never shrinks" — because MTA's shrinks.
  `memory/lesson_search_prior_art_by_problem_not_by_assumed_mechanism.md`

- **An assertion you have never watched go RED is decoration, not evidence.** `mp.py`'s `_i18n_checks`
  carries five fail-closed checks and has **no injection harness anywhere** — every one has only ever run
  against a HEAD where both defects it was written for were already fixed. Two of them are worse than
  unproven, they **cannot fire**: `_EMPTY_LINE` hunts a blank-bodied log line that `log.cpp:189`'s
  `[args unformattable]` fallback made impossible, and `_read_log_strict`'s docstring still claims it
  catches a mid-sequence cut that `log.cpp:192-209` now repairs before writing. Neither could ever have
  caught the defect that motivated the instrument (`'?'` is well-formed ASCII; a blanked name is an
  absence). `nick_gate.ps1` had been injection-proven two days earlier by the same hand — the discipline
  existed and was not carried across. *Look FIRST:* write the must-FAIL fixture in the SAME commit as the
  check; when you fix a product defect, grep for the assertions that were watching for it, because a fix
  can silently retire its own detector; and remember fixtures prove the CHECKER while only a
  defect-carrying build proves the PIPELINE — so land a detector on a live defect and watch it fail
  BEFORE fixing, since live defects are the only free positive controls you will get.
  `memory/lesson_an_instrument_never_shown_failing_passes_by_construction.md`

- **One name covering TWO quantities reads as a coherent design and is not — FIVE times in one pass,
  caught by the critic every time and by the primary never (2026-07-29, chat-history `/qf` pass 2).**
  `seq` (local entry identity vs the host's wire order), `alpha` (the store's TTL curve vs the drawn
  composition), `eviction` (live→retained vs retained→gone), "the build gate" (a pinned ImGui frame vs
  a 60 Hz republish), `slot` (display identity vs world-entity handle). The `alpha` one would have
  shipped a feature that **never drew at all** — a retained row's store alpha is 0 by definition and
  the layout predicate `alpha >= threshold` read it. A conflation does not read like an error, it
  reads like concision: every sentence is true of *one* referent, so proof-reading passes, and the
  contradiction only shows when two sentences are held side by side — which a self-written brief
  cannot do. *Look FIRST:* grep your own draft for its load-bearing nouns and count referents before
  the brief goes out; **suspect any word that spans a LAYER BOUNDARY** (all five did: store↔render,
  local↔wire, store↔viewport, identity↔world, design↔measurement); and treat a name introduced *by a
  fix* as the next suspect — three of the five arrived attached to corrections, which is when a design
  is least suspected. As a critic, "which of the two does X read?" costs nothing and found five real
  defects in one pass. `memory/lesson_one_name_for_two_quantities.md`

- **A reframe silently invalidates every earlier answer that CITED what it changed — twice in one
  pass, despite the `/qf` skill already carrying a RE-AUDIT instruction for exactly this
  (2026-07-29).** (a) Round 1 proved "the seed strictly precedes every live line, so no dedup is
  needed"; round 2 then put `ChatLine` into `IsPreWorldSendableKind` to close a principle-8 hole and
  destroyed that premise — un-re-derived until round 11, by which point pre-world rows sorted NEWER
  than the seed **and** `lineSeq > highestApplied` would have discarded the ENTIRE seed with no error
  anywhere. (b) Round 2 WITHDREW "the host composes the name once" because under host-**relay**
  (`session_relay.cpp:95-99` copies verbatim) the host provably could not; the design later became
  host-**authored** and the withdrawal rode forward un-re-tested until round 13, leaving two peers
  with **permanently different names for one message**. *Look FIRST:* when you change a GATE, LANE,
  AUTHORITY or THREAD, grep your own prior answers for the thing you changed **by name, not by
  memory**; keep **withdrawals** on the same re-test list as claims ("was that rejected on a ground
  that still holds?"); and know that **a fix which closes a hole is the highest-risk reframe**,
  because it does not feel like a premise change.
  `memory/lesson_a_reframe_invalidates_answers_that_cite_it.md`

- **A reclassification that leaves the OBSERVABLE unchanged is a relabel, not a dissolution
  (2026-07-29).** A row vanishing at full opacity when the reveal block hit its height budget was
  "dissolved" by reclassifying it from a store exit to a **viewport** event and using
  `ImDrawList::PushClipRect` (real: `imgui.h:3071`) so the oldest row would "slide up and out". It had
  every signature of a good dissolution — simpler mechanism, deleted state, a real in-tree primitive.
  One round later: **`hud.cpp:369` recomputes `y = anchorBottomY - totalH` every frame**, so a new
  line jumps the block one `rowH` in ONE frame. Nothing slides; the clip turned an instant vanish into
  an instant clip. The true answer was that smooth motion needs cross-frame state AND was out of scope
  (Minecraft, the user's own reference, does not animate chat scroll). *Look FIRST:* after any
  reclassification, **state the observable before and after in one sentence each — if they are the
  same sentence, you relabelled the defect**; and grep for the motion verb (*slides/eases/scrolls*)
  and confirm something actually interpolates the position, because a per-frame recomputation from
  current state cannot animate. `memory/lesson_a_reclassification_is_not_a_dissolution.md`

### 1b. Standing working agreements (previously indexed NOWHERE)

Measured 2026-07-27 by a full pairing sweep of `memory/` against this file: **all 194 `lesson_*`
files are paired here (0 missing), but 39 `feedback_*` standing rules were referenced in neither
this ledger nor `MEMORY.md`** — reachable only through inline `[[...]]` citations in `CLAUDE.md`, so
in practice unfindable. Several are load-bearing, and one of them
(`feedback_install_idempotent_o1_steady_state`) describes *exactly* the bug shipped and re-measured
the same day. Indexed here so the ledger is complete; the full text stays in each `memory/` file.

**Verification / shipping discipline** — `feedback_post_ship_audit` (audit every shipped change with agents) ·
`feedback_audit_every_time` (immediately, not "next cluster") · `feedback_audit_prompt_hot_path_reentry` (audit prompts
MUST force per-function pump-reachability enumeration) · `feedback_install_idempotent_o1_steady_state` (any
`Install`/`Register`/`Setup` reachable from a pump is O(1) in steady state — **the rule today's
`roster_token_selftest` broke**) · `feedback_codebase_familiarity_before_new_install` (read the sibling
pattern before writing a new one) · `feedback_no_handoff_without_smoke_test` · `feedback_interaction_smoke_not_join_smoke`
(a join smoke says NOTHING about grab/throw/convert paths) · `feedback_no_smoke_while_user_on_pc` ·
`feedback_autonomous_lan_named_windows` · `feedback_always_deploy_after_build` · `feedback_always_use_user_test_poses` ·
`feedback_show_screens` (READ the captured PNG so the user sees it) · `feedback_modular_file_size_rule` (800 soft /
1500 hard) · `feedback_clean_rebuild_after_global_move`.

**How to work with the user** — `feedback_no_technical_user_questions` + `feedback_resolve_technical_decisions_via_agents`
(adjudicate via agents, not by asking) · `feedback_never_rush_research_first` · `feedback_deliver_results_fast` (use
wait-time productively, never "standing by") · `feedback_commit_and_push_without_asking` (superseded for THIS
repo by the push-leak-audit rule — commit freely, ask before push) · `feedback_commit_authorship` ·
`feedback_user_run_requires_root_bat` (one-click `.bat` at the project ROOT) · `feedback_user_prefers_1080_windows` ·
`feedback_dev_features_in_imgui_menu` (ONE categorized menu, not ad-hoc hotkeys) ·
`feedback_documentize_manual_status_reconciliation` · `feedback_deep_re_no_iteration` ("Deep RE" forbids
try-it-and-see) · `feedback_version_tagging`.

**Engine / RE technique** — `feedback_islive_unsafe_on_freed_cached_pointer` (`IsLive` AVs on a GC-purged
pointer; use `IsLiveByIndex`) · `feedback_processevent_interceptor_misses_bp_internal` (BP→BP goes through
`ProcessInternal`) · `feedback_crash_firewall_requires_eha` (the SEH firewall MUST be `/EHa`, or an absorbed
task-AV permanently freezes the host) · `feedback_iskeyed_interactable_resolves_classes` (not cheap; never
promote above the session gate) · `feedback_registry_register_mirror_pattern` (required reading for any new
wire-driven receiver) · `feedback_no_direct_memory_write_crutch` · `feedback_re_related_functions` (RE **all** related
functions, not just the hooked one) · `feedback_granular_per_event_sync_method` (one doc per event) ·
`feedback_check_mta_and_document` · `feedback_ida_rename_and_save` · `feedback_no_ue4ss_dependency` +
`feedback_prefer_cpp_probes_over_ue4ss` · `feedback_code_with_agents_and_security` · `feedback_never_winxy_zero_multimonitor`
(black screen + runaway RAM).

## 2. Join-window identity & the DUP-prone zone (measure before touching)

- **Row field vs per-slot state: does it describe the PERSON or the LINK?** The arc-A design put the
  `joinSent` latch in the roster ledger's row beside nick/guid/skin. It cannot live there: a CLIENT
  sends its Join to slot 0 BEFORE the host's Join arrives, so row 0 does not exist yet, the
  occupancy-gated setter drops the write SILENTLY (which is correct for every identity field), and
  the client re-sends its Join on all 125 ticks per second forever. The tell is direction: an
  OUTBOUND fact ("we did something to that slot") is link-scoped and exists before anyone is
  identified; an INBOUND one ("that peer told us who they are") is person-scoped. Link-scoped state
  still needs replacement-clearing, so it goes in `PerSlotState<T>` (which registers its own clear in
  its constructor), never a bare array. *Look FIRST:*
  `memory/lesson_link_scoped_state_cannot_live_in_a_person_row.md`

- **A recycled slot's replacement carries no ABSENCE — detect occupancy, don't observe departure** —
  slots are handed out lowest-free (`session_status.cpp:87-93`) and the close/accept pair runs on the
  NET thread, so a slot can go occupant X -> occupant Y with no empty state between and the polled
  falling `IsSlotReady` edge (`event_feed.cpp:127-135`) can miss BOTH transitions inside one 8 ms GT
  tick — routine on loopback, i.e. our own 2-peers-on-one-machine rig. Cure: an occupancy TOKEN read
  as current state (host-minted generation internally, session-monotonic `playerNo` on the wire) plus
  a periodic reconcile as its executor, since after a departure with no successor no packet arrives to
  trigger a "check at use". *Look FIRST:*
  research/findings/join-identity/votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md T1-T5.
  `memory/lesson_recycled_slot_replacement_carries_no_absence.md`
- **Validate WHERE YOU READ, not against a mirror — or staleness fails OPEN** — a destructive action
  that re-checks its captured token against the game-thread MIRROR only narrows its race: the net
  thread can clear a slot and accept a successor before the GT runs, so the stale mirror still holds
  the predecessor's token, the check passes, and the address is resolved from the LIVE connection.
  Take a value FROM the mirror and compare it against the AUTHORITY you are about to read, atomically
  with that read ("resolve this slot's address IFF its generation is still G") — then staleness fails
  CLOSED. Put the token in the API SIGNATURE so a tokenless call cannot compile; a hand-listed call
  set is not a defence (a manual census missed `scoreboard.cpp:285`; a mechanical grep found eight
  `moderation::` sites, six slot-addressed). Measured instance: `BanSlot` uses a slot captured when
  the modal OPENED, across an arbitrary typing delay -> a permanent IP ban can hit the successor.
  *Look FIRST:* same design doc, T11. `memory/lesson_validate_where_you_read_not_against_a_mirror.md`
- **A SILENT passive identity mint is a zombie factory: mint only where you ANNOUNCE** — the client
  census minted keyed Elements "for its own tracking" (broadcasts nothing) and the keyed adopt stacked
  a mirror over each (~2200 double-rows per join, reverse stolen by RegisterMirror). v122: a passive
  walk may key-INDEX, never mint; the bind funnel enforces one-actor-one-row by authority. *Look
  FIRST:* votv-stable-id-no-passive-mint-DESIGN-2026-07-18.md; MarkPropElement's EnrollSource branch;
  identity_create.cpp A' block. `memory/lesson_silent_passive_identity_mint_zombie_factory.md`
- **An accidental cure can RIDE the corruption you are fixing** — the 2026-06-10 ghost-twin cure
  worked host-side ONLY via the mirror-stack + reverse-steal (the fuzzy path even REKEYED the host's
  actor to the client's key before any bind); a bare reject would have silently re-broken it. Closing
  a hole and keeping every flow that leaned on the hole are two separate obligations (v122 H handback
  = enroll + re-express replaces the stolen function). *Look FIRST:* remote_prop_spawn.cpp
  HostAuthorityHandback_. `memory/lesson_accidental_cure_rides_the_corruption.md`
- **A client grab/drop of a host-owned keyed prop = a MOVE with TWO same-key halves** (SPAWN author + a
  co-fired grab-hook DESTROY); the DESTROY is usually the killer. *Look FIRST:* the destroy-seam, not the
  spawn author. `memory/lesson_client_keyed_prop_move_two_wire_halves.md`
- **A host OnSpawn log line != VISIBLE** — check the immediate same-tick OnDestroy (destroy-by-key kills
  the newest). `memory/lesson_onspawn_log_not_proof_check_immediate_destroy.md`
- **REUSE the proven author (with its gates), don't raw-reimplement** — a raw MarkPropElement broke the
  E-grab decline. **Extended 2026-07-27:** the same question applies to PRIMITIVES across lanes — a
  UTF-8 nickname codec was derived from scratch for three `/qf` rounds before a grep found the CHAT lane
  had shipped it on 2026-07-04 (`chat_sync.cpp` `SanitizeUtf8`/`NickUtf8` with surrogate handling/
  `TrimAndCap` with character-boundary back-off), with TWO copies already in the tree. Grep the sibling
  lanes that carry the same payload kind before designing. **Ending, 2026-07-28 (`6e1156da`): the two
  copies were STILL COMPILED four days after `coop/text/utf8_codec` was promoted, and had DIVERGED —
  both emitted CESU-8 for an unpaired surrogate where the owner drops it, so ill-formed UTF-8 could
  reach the chat wire. Worse, the codec header ASSERTED they had been absorbed. Promoting an owner is
  half the job; the other half is deleting the copies and making the header's claim true.**
  `memory/lesson_reuse_proven_author_not_raw_reimpl.md`
- **A join reconcile that DESTROYS local actors needs a quiescence gate + caps.** `memory/feedback_join_reconcile_sweep_safety.md`
- **An op applied BEFORE the state it reads is ready recurs** — gate/defer (snapshot-before-state-ready). `memory/feedback_snapshot_before_state_ready.md`
- **chipPiles persist in `primitivesData`; off-kerfurs in `objectsData`** (different save lanes). `memory/lesson_chippile_saved_in_primitivesData_not_objectsData.md`
- **DELIVERY-axis: join DELIVERY vs IDENTITY are separate; ONE owner = `prop_snapshot`.** `memory/feedback_deliver_missing_owner_delivery_axis.md`
- **Check the EXISTING barrier's ANCHOR before building compensation layers** — the two-authority
  join seam (4 roots, 4 layers, 3 days) was ONE mis-anchored edge: the v56 gate/replay WAS the MTA
  join barrier, but ClientWorldReady fired at "world up" (seconds before the loadObjects tail
  settled). *Look FIRST* on a window/race class's SECOND compensation layer: does a READY edge
  exist, what PREDICATE fires it ("world up" != "world settled"), is a stronger client-local
  signal already trusted elsewhere (the doom sweep's probe was). Moving an edge beats compensating
  for it. `memory/lesson_check_existing_barrier_anchor_before_compensating.md`
- **Harness TimelineThread call sites: an Arm() stays ATOMICS-ONLY (or Post to the GT)** —
  DriveMenuModeJoinWorldBoot runs OFF the game thread (harness/session_runtime.cpp:253, s27 cut); extending a directly-
  called Arm to touch plain GT-owned state is a silent data race (2026-07-12 audit CRITICAL: 8
  fields + a std::string; fixed `7847021e` via an atomic request flag consumed by the GT ticker +
  UE_ASSERT_GAME_THREAD on GT-only entries). *Look FIRST:* the caller's thread — grep the enclosing
  harness function for TimelineThread comments / Post-and-wait siblings.
  `memory/lesson_harness_timeline_thread_arm_sites_atomics_only.md`
- **In-episode wire expressions WERE provisional — the JOIN BARRIER removed the window (2026-07-12
  `bbf91f39`, SUPERSEDED AT SOURCE):** ClientWorldReady now announces at load-tail quiescence
  (`coop/session/world_load_episode` probe latch — the MTA INITIAL_DATA_STREAM shape), so no wire
  prop expression can arrive mid-churn; the capture/revalidation/netting machinery (takes 1-4) was
  RULE-2 deleted. The measured record (fresh mirrors churn-killed ~2 s; converge targets recreated
  only from save-WORLD records; phase replay inverting a destroy->spawn pair; doom judges LAST)
  stays the FIRST read for a prop bug in the DEGRADED mode ("latching DEGRADED" in the client log)
  or the TRAVEL window (no travel-start gate yet). *Look FIRST:* client log — "load-tail QUIESCED"
  must precede "ClientWorldReady announced"; NO [SPAWN-DEFER] lines exist anymore (one appearing =
  someone resurrected the dead machinery); per-doom cls/key/loc lines + the dead-row tripwire still
  live in the sweep. `memory/lesson_join_window_wire_expression_provisional.md`
- **VOTV's OWN save ships DUPLICATE interactable Keys** (85 trashBitsPile_C across 4 keys — save-born
  clone families; the 06-24 sweep silently doomed "80 trashBitsPile" for weeks). Key uniqueness is OUR
  invariant: the HOST re-keys duplicates at enroll (MarkPropElement, the one owner; GT-gated setKey;
  dead incumbent = churn recreate inheriting identity). Take-4: the `2fefd161` re-key was INERT (162x
  "setKey not found" — trashBitsPile is actor_save_C lineage; repaired by the SuperStruct-climbing
  resolver, `460da7e4`). *Look FIRST:* host-log "KEY-UNIQUENESS ... re-keyed -> 'rk_'" SUCCESS burst
  ("re-key FAILED" = the authority is NOT working); same-key multiplicity histogram in the adopt burst.
  `memory/lesson_votv_save_ships_duplicate_interactable_keys.md`
- **MirrorManager\<Prop\> MIXES census LOCAL rows with wire rows (one actor can carry BOTH)** — an
  actor->eid reverse meaning "established cross-peer identity" must filter `IsMirror()`
  (`ResolveMirrorEidByActor(wireMirrorOnly)`), else it kills the Gap-I-1 divergent-key dedup.
  *Look FIRST:* mirror_manager.h "MIXES" block. `memory/lesson_prop_mirror_manager_mixes_local_and_wire_rows.md`
- **A NEW generic catch/express lane must inherit EVERY existing owner boundary; "UNTRACKED = mine"
  premises die when per-tick claimers ship** — spawn_authority Inc-1's seam drain (07-10) lacked the
  kerfur OWNER BOUNDARY the census lane already carried → the 5 Hz kerfur converge lost every race,
  silently released the dead NPC, no KerfurConvert → the take-8 five-for-five toggle dupe. Fix = kerfur
  FIRST REFUSAL at the express chokepoints (TryAdoptFreshKerfurProp: UNTRACKED + dead-NPC-watch match,
  event-driven at the spawn edge; the poll stays as backstop), `ded3f793`. *Look FIRST:* a poll-class
  "converge found nothing" WARN right after a `spawn-seam adopted`/generic express of the same
  class+position = the race; when adding a lane, DIFF its gates against every sibling lane.
  `memory/lesson_new_generic_lane_must_inherit_owner_boundaries.md`
- **ChildActorComponent children are OUTSIDE the world-object universe** — a kerfur eye cam
  (prop_camera_good_C) passes every "world prop" filter (keyed, Aprop lineage, live) but the game's
  own rule is `Aprop_C::ignoreSave = ignoreSav || IsChildActor()` (prop_base bytecode): its Key is
  per-peer random, cross-peer identity impossible in principle. Enrolling/broadcasting them = floating
  CCTV mirrors on the joiner + the joiner's own eye cams doomed (take-7). And a SEND-side exclusion
  must gate EVERY payload builder — the steady re-seed express bypassed enrollment (elementId=0 keyed
  payloads) while the gated destroy seam made its orphans PERMANENT (audit CRITICAL). *Look FIRST:*
  `ue_wrap::engine::IsChildActor` (six consult surfaces, `c93617be`); any NEW prop enumeration must
  consult it. `memory/lesson_child_actors_excluded_from_world_object_universe.md`
- **A module's "I am the ONE owner" is an INTENT; the callers are the fact.** Measured 2026-07-29:
  `coop/text/utf8_codec.h:1` opens *"ARC D1: the ONE owner of text encoding"* and `:18-21` states the
  receive boundary *"decodes STRICTLY and rejects a whole ill-formed field"* — and both sentences had
  been copied forward into `CLAUDE.md` and `COOP_SYNC_MAP.md:83` as settled architecture. Against HEAD:
  `chat_sync.cpp:32` defined its **own** byte-identical `SanitizeUtf8`, `:45` re-implemented
  `CapUtf8Bytes`, and `:114-124` `OnReliable` did **no decode at all**, pushing raw wire bytes to TWO
  render surfaces — in a file whose line 5 includes the owner. The claim held on the NICK path and was
  false on **chat, the one attacker-controlled string in the process** (`docs/security/TRACKER.md`
  **W11**, fixed `84e0a4e3`). *Look FIRST:* a sole-ownership claim is a hypothesis about the CALLERS —
  `grep -rn "<FunctionName>" src/` for definitions outside the owner, and check that each includer
  actually CALLS it. **An anonymous-namespace function with the same name as a public one is a silent
  override** — no warning, no link error. And a doc quoting another doc inherits nothing: three files
  agreeing is one measurement copied twice. Corollary from the same dig: **"validated" is not one
  property** — the tracker's own "every wire string clamps length" was accurate and was read as "wire
  strings are validated"; length-clamped is not well-formed.
  `memory/lesson_a_sole_ownership_claim_is_a_claim.md`
- **Before replicating a store, census what is IN it — and ask whether it is a RECORD or a VIEW.**
  Measured 2026-07-29: a 21-round `/qf` designed "seed the late joiner with the host's chat history" for
  eighteen rounds before anyone ran `grep -rn "chat_feed::Push("`. It returns **15 sites**, including the
  local player's own first-person UI notices — `nick_color.cpp:92` *"Nickname color: applied"*,
  `local_body.cpp:83` *"Skin: X"*, `player_handshake.cpp:469` *"Connecting to X's game..."* — plus two
  `[1c-test]` debug lines, with **no field distinguishing them from typed chat**. Seeding the host's
  retained set would have shipped the host's own UI confirmations to a joiner as "the lobby's history".
  And the host's set is the host's **VIEW**, bounded by its own cap, TTL and `Reset` — so the lobby's
  history **did not exist anywhere** and has to be CREATED (a host-owned canonical log, local feed
  demoted to a view of it; the MTA shape). *Look FIRST:* the store's NAME is the trap — run the census on
  its WRITE API before designing replication, and ask "is this a record or a view?" A store with a cap,
  a TTL or a reset that exists for DISPLAY reasons is a view, and replicating a view propagates one
  peer's rendering policy as shared truth. A single write API with several semantic kinds inside it is a
  DEFERRED discriminator decision, and the bill arrives the first time something wants a subset.
  `memory/lesson_census_what_is_in_a_store_before_replicating_it.md`
- **Identity-critical log lines carry cls+key+loc (USER RULE)** — a class histogram alone makes
  per-entity RCA impossible; cold paths only, never at the POST-native destroy seam (PendingKill),
  throttle mass arms. `memory/feedback_identity_logs_carry_key_and_loc.md`

- **A display PLACEHOLDER must never be stored where identity is read.** Measured 2026-07-28 (arc B):
  the host installs a roster row when a slot reaches READY, which is BEFORE that peer's Join carries its
  name — so the first row about a joiner legitimately has an empty nick. `SanitizeNickname("")` minted
  the display fallback `"Player"` and stored it as a name; the joiner then ADOPTED it as canonical and,
  under the persist decision, wrote it over the human's real name permanently
  (`nick: host renamed us 'Client1' -> 'Player'`). The placeholder had been correct for years — the bug
  was created by a NEW READER, in a file that was not edited, and is invisible in the diff. *Look FIRST:*
  before promoting any displayed value to an identity, ask "what is this when it is not known yet, and
  does that sentinel round-trip?"; keep the fallback at the LAST layer (the renderer), one copy, never in
  the store. `memory/lesson_a_placeholder_must_never_become_an_identity.md`
- **Census the DIRECTION, not only the operation — a widen census is blind to a narrow.** Measured
  2026-07-28 (arc D1): the design censused per-byte WIDENS and found 7 sites; fixing all seven changed
  NOTHING VISIBLE, because two sites in front of them were NARROWS — `harness.cpp` replacing non-ASCII
  with `'?'`, and `ReadEnv` using `GetEnvironmentVariableA` (Windows holds the environment as UTF-16; the
  A-variant converts down to the ANSI codepage, so a Cyrillic env var arrived as cp1251 and the correct
  new strict decoder produced a row of U+FFFD). The debugging trap is the worse half: because the
  destroyer was UPSTREAM, each correct fix produced no visible change — the exact signature of "my fix is
  wrong". *Look FIRST:* census both directions AND every Win32 `...A`/`...W` pair on the path; and when a
  correct fix changes nothing, log the value at the seam you are about to trust instead of reading
  further. Third instance of `lesson_census_the_operation_kind_not_only_the_sites`, which it sharpens.
  `memory/lesson_census_the_direction_not_only_the_operation.md`

## 3. Sync architecture (owners, routers, lifecycle)

- **The player inventory is TWO stores, and our lane polls the wrong-shaped one** (2026-07-24, bytecode +
  runtime). LIVE = `UpropInventory_C` → `saveSlot.GObjStack[propInventory.Index]` (what play mutates; the
  player's own inventory IS a container, `Aprop_inventoryContainer_player_C : Aprop_container_C`).
  SAVE-SIDE PROJECTION = `saveSlot.inventoryData`, a *different field* (0x02E0 vs GObjStack 0x0198),
  written by `mainGamemode::saveObjects`. A container slot press = `getObject → addObject →
  K2_DestroyActor` and touches `inventoryData` **zero** times — zero refs across `prop_container`,
  `prop_inventoryContainer_player`, `uicomp_playerInvContainerSlot`, `ui_playerInventory`. Our
  `player_inventory_sync` reads ONLY `inventoryData`/`equipment`/`hold`. On a CLIENT the projection never
  refreshes: `save_block` holds `gamemode.disableSave=true` and `saveSlot_C::save` checks it at op03
  BEFORE `saveObjects` (op12) and `saveToSlot` (op19) — a deliberate 2026-07-04 mandate documented in
  `save_block.h` Part 3. Measured gaps were CONSTANT (client 0-vs-6, host 4-vs-5) = an ORIGIN mismatch,
  not accumulation; and host vs client projections have DIFFERENT AUTHORS (game save/load vs our
  join-apply), so "refresh it more often" cannot be one fix for both. *Look FIRST:*
  `research/findings/inventory-items/votv-player-inventory-two-layer-RE-2026-07-24.md` + its scope brief.
  `memory/lesson_player_inventory_is_two_layers_live_and_projection.md`
- **A probe's side effects travel through OUR OWN lanes, not just the engine's** (2026-07-24). A dev probe
  called `mainGamemode::saveObjects` and was declared read-only because it skips
  `saveToSlot`/`SaveGameToSlot` — true about the engine's disk path, false about the system: the refresh
  changed `inventoryData`, `player_inventory_sync` polls that at ~1 Hz and ships on a HASH CHANGE, the host
  persisted it, and `coop_players/<guid>.json` went 2204 → 4848 bytes holding a state no organic run
  produces (restored from `.bak`; the next run would otherwise have started from an impossible state).
  *Look FIRST:* before a probe calls a game verb, census EVERY consumer of the state that verb touches —
  grep our own lanes for polls/hooks on those fields — not only the engine path you deliberately avoided.
  Any change-detecting poll downstream means the probe is not read-only.
  `memory/lesson_probe_side_effects_travel_through_our_own_lanes.md`

- **One cache per QUESTION, not per write-moment** — a hash/state map written at ONE instant but read to
  answer TWO questions is a latent bug: the answers diverge the moment the peer mutates locally, and
  nothing about the WRITE reveals it (same line, same value; only the reads differ). The R11b container
  lane needed **four** maps and each collapse produced a different LIVE failure, one per smoke:
  `published` fused into `sent` → the targeted connect seed recorded nothing, so the host **refused every
  client write after a join**; `base` fused into `applied` → clearing on a local edit (right for the
  no-op gate) made the peer **declare base 0** and be refused again, while NOT clearing made the host's
  corrective re-publish hash identically to the pre-edit blob and get skipped, so **a refused peer never
  converged**. **Look here FIRST:** when adding a second reader to an existing cache, ask what event
  makes the two readers want different values — in a sync lane it is almost always the peer's own local
  mutation. Split, and NAME each map after its question.
  `memory/lesson_one_cache_per_question_not_per_write_moment.md`

- **The HOST's Join always reaches the client BEFORE the client's own Join goes out** (the
  client's send waits for its Element allocation post-AssignPeerSlot), so any symmetric per-side
  Join validation fires CLIENT-side first and the host-side branch is a trust-boundary backstop
  only reachable by a peer that skips its own check — role-swapped drills can NOT cover it; use a
  NOVAL drill build (validation compiled out one line, never shipped). Measured s29 drills B/B2/B3
  (2026-07-19, v122 version gate). *Look FIRST:* `player_handshake_version.cpp`
  ValidateJoinVersionOrRefuse + `player_handshake.cpp` MaybeSendJoinToSlot's eid-wait.
  `memory/lesson_join_handshake_host_first_ordering.md`

- **Per-UNIT device identity exists ONLY at the AIM seam — 5 of 7 enterable families render ONE
  shared widget instance** (ui_console_C = ALL SAT consoles; gamemode.laptop = base laptop + every
  portable PC), so widget→owning-unit is architecturally underivable at any widget-side surface
  (rising-edge / lost-race denies). Capture the unit's native name at InpActEvt_use PRE
  (ReadMainPlayerLookAtActor) — the s23 busy-notice memo shape; and note the race LOSER (whose own
  E-press wrote the memo ~RTT ago) is who gets force-exited, so the memo is fresh by construction.
  *Look FIRST:* `device_occupancy.cpp` OnUseInputPre memo + `votv-base-computers-RE-2026-06-11.md` §1.1.
  `memory/lesson_shared_widget_unit_identity_at_aim_seam.md`
- **A claim-gated intent lane must cover EVERY entry surface of the device.** v111 routed desk knob
  intents over the claimed-occupant lane, but the claim engages only on the intComs `activeInterface`
  edge — the download unit's WORLD-SPACE buttons never raise it, so the lane was structurally dead for
  the unit it was built for (bugs 1/2/3 of the 2026-07-16 hands-on = ONE axis fact). *Look FIRST:*
  enumerate the device's verb surfaces (widget focus / world-space press / hold-E / overlap) and grep the
  claim writer for the edge each raises. `memory/lesson_claim_gated_intent_lane_must_cover_every_entry_surface.md`
- **A mirrored float feeding a native `>= X` latch needs EXACT-SNAP, not an asymptote.** v111 BUG-4:
  SimInterp's window reopens on every 10 Hz packet -> never snaps to exactly 1.0 -> sub-ulp freeze just
  under the detector latch -> the client's unsuppressed native block re-crosses the threshold every frame
  -> stuck beep. Check (a) the interp actually emits exactly X under packet cadence, (b) the local
  crossing side-effect is suppressed/idempotent. **v112 corollary: the exact-snap must be PER-CHANNEL**
  (a whole-vector skip never fires while any channel moves — decoded accrues every packet), and a
  DISCRETE channel (0/1 flag) never rides the ease at all — snap on arrival. **v115b BOUNDARY:
  exact-snap does NOT extend to EVENT-FIRING machines — the ping FSM's stage transitions are
  `==1.0` checks whose consequences are events/spawns/append-text; snapping values onto any
  un-parked machine fires them locally = double events. Such machines: single-author only.**
  *Look FIRST:* desk_sim_sync.cpp SimInterp (v112 per-channel).
  `memory/lesson_mirrored_threshold_latch_needs_exact_snap.md`
- **connected()-gated poll lanes EAT pre-connect edges — every such lane owes a connect-edge seed
  from GROUND TRUTH** (v115b audit CRIT-1: a SOLO host's ping edge was absorbed by the unwired
  baseline → a mid-ping joiner got no FSM-hold; desk_input gates its BOOKKEEPING on connected()
  while device_occupancy gates only the SEND — two adjacent lanes, opposite gating, one silent
  hole). Seed in ConnectReplayForSlot by reading the ENGINE state, never the baseline; never
  clobber a live wire attribution. *Look FIRST:* `desk_input_sync::SeedPingAttributionFromMachine`
  + `desk_snd_fx::QueueConnectBroadcastForSlot`.
  `memory/lesson_connected_gated_poll_needs_connect_seed.md`
- **Edge-authority polls: classify wire-replay transients by STATE PREDICATE, not flags/timers**
  (v115b root-3: the catch replay's ResetDownloadMachine made the dish mesh transiently invalid
  ~24 s → the ARM/DISARM edge poll broadcast a false DISARM that stomped the fresh catch. A
  one-shot flag loses the legit ARM on fast respawns; a timer is a guess. The predicate: mesh
  down + signalData LIVE = re-init window — a real disarm deletes signalData FIRST). *Look
  FIRST:* `dish_sync.cpp HostArmPoll` reinitWindow.
  `memory/lesson_edge_authority_poll_wire_transient_state_predicate.md`
- **Presser-authored STATE broadcasts, never intent lanes, for EX-invisible verbs.** The verb has
  ALREADY run locally (incl. RNG rolls + id mints) before any seam can see it — "intent -> host
  executes" cannot exist; detect the local change (PE seam > raw-field poll > VM-bracket dirty-mark),
  broadcast field-granular deltas, receivers apply+prime in the same GT task, host relays EXCLUDING
  the originator (an echo reverts a newer local value = the eaten-scroll race). *Look FIRST:* the
  all-units design doc + coop/desk_input_sync (the v112 template).
  `memory/lesson_presser_authored_state_not_intent_for_invisible_verbs.md`
- **The desk's `active_*` unit toggles are SETTER-EVENT-managed; `powerChanged` is FUSED.** Raw field
  writes leave mirror hums/lights dead (half of bug 3); the only native setter (powerChanged, 5 bools)
  runs EVERY unit's block incl. an UNCONDITIONAL stopSound — replicate each field's effects reflected
  instead. *Look FIRST:* ue_wrap/desk/console_desk.cpp ApplyActiveToggleEffects + uber [1113-1156].
  `memory/lesson_active_toggles_setter_events_powerchanged_fused.md`
- **Follow MTA architecture when possible** (vendored `reference/mtasa-blue/`). `memory/feedback_follow_mta_architecture.md`
- **A new `ReliableKind` wires in SEVERAL places and a miss is SILENT** — SITE LIST CORRECTED 2026-07-22:
  the old "third place" (`event_feed.cpp`'s family case list) was DISSOLVED 2026-06-28 (SyncRouter
  consolidation, verified at `event_feed.cpp:499-517` — each `Handle*Event` returns true iff the kind is
  in its family, so the FAMILY switch is the single membership declaration). **Do not touch
  `event_feed.cpp` for a new kind.** Current: `protocol.h` (enum+payload+static_assert+version bump) →
  `event_dispatch_<family>.cpp` → `session_lanes.h` (relay whitelist + lane) → `subsystems.cpp` wiring.
  Event-driven kinds still need real wire proof (an idle smoke never fires them).
  `memory/feedback_reliablekind_router_checklist.md`
- **Host TRACKING/enroll gates on HOSTING, never `connected()`.** `memory/lesson_tracking_gates_on_hosting_not_connected.md`
- **EVERY session-end path runs the FULL teardown fanout** — AND session-scoped UI (chat feed/input/
  bubbles/nameplate/voice_panel) dies at the FLEE funnel (`FleeToMainMenu`), NOT `DisconnectAll` (which
  also runs on the HOST keeping its world, so it must not clear on-screen UI). **A RESET alone is not
  enough if a per-tick EDGE DETECTOR re-fires AFTER it:** `session.Stop()` flips slots -> the next
  `event_feed::Update` re-Pushes "Host left the game" into the just-cleared feed (client self-quit,
  `e02343c4`) -> also disarm the producer (`SuppressPeerLeaveEdges`). *Look FIRST:* net_pump.cpp:184
  (chat-leak-into-menu, 2026-07-15). `memory/lesson_every_session_end_path_full_teardown_fanout.md`
- **A host-auth FROZEN mirror displaying slightly stale = a TRANSPORT+CADENCE bug, NOT authority.** Fix
  the refresh (periodic UNRELIABLE absolute snapshot at display cadence, pose-stream pattern); do NOT hand
  the client simulation + clamp it back (lateral/regression + a per-broadcast site-list). Clock design F
  (v110, `2dde3e16`): client stays frozen mirror, clock streams `ClockPose=37`. *Look FIRST:* smooth-sun
  needs advancing `totalTime` through `ReceiveTick` which fires every `newMinute`/`newHour` -> that path
  is gated on enumerating those consumers. `memory/lesson_frozen_mirror_desync_is_transport_not_authority.md`
- **`subsystems::Install` is called EVERY net_pump tick (idempotent contract)** — net_pump.cpp:1014, "one-
  shot install ... idempotent"; each sub-Install MUST latch its noisy/expensive work or it re-runs per
  frame (desk_diag ENABLED banner ~37k/session, `2de202ed`). *Look FIRST:* add a `static bool` latch to
  any new Install that logs/allocates/hooks/resolves. `memory/lesson_subsystems_install_runs_every_tick_must_latch.md` (SHARPENED v120: a success-only latch whose FAILED retry re-runs FindClass = a 60 Hz pre-world array-walk bomb — put every resolve retry behind a throttled gate or a cached resolver)
- **One bool latch fusing DISTINCT terminal states (success vs DISABLED) makes some consumer's gate
  wrong for one of them** — pre-s27 kerfur_convert `g_installed=true` meant BOTH "ready" and "module
  disabled", so the request gate PASSED requests in the disabled state (the exact zeroed-frame
  over-read the disable existed to prevent; latent, found by the s27 split's per-state gate
  re-derivation). Enumerate (terminal state × consumer gate) before touching any init latch; one
  latch = one meaning. *Look FIRST:* kerfur_convert_host.h (the documented fail-closed deviation).
  `memory/lesson_single_latch_fused_states_gate_semantics.md`
- **Every client-side SUPPRESSION is a LOAN, not a purchase (N=3: weather 06-11, serverbox 07-09,
  garbage_sync 07-10).** Persistent-state neutralizations (tick-disable, field-zero, TimeScale=0,
  suppress flags) need an EXPLICIT OnDisconnect restore; fn-body PRE-cancels SELF-restore ONLY when
  gated on `s->running()`/`connected()` — a bare `role()==Client` gate keeps suppressing in SOLO play
  forever (Stop never resets cfg_.role). ADDED 2026-07-16: the restore must RE-LOOKUP a live
  instance (never PE-dispatch on the cached parked ptr = UAF; caught twice — serverbox 07-10,
  dish tickers 07-16 audit F1). *Look FIRST:* name the restore mechanism in the SAME commit;
  census: grep bare role-gates without running(). `memory/lesson_suppression_needs_paired_restore_or_running_gate.md`
- **Killing a BP latent frame-loop by clearing its gate flag exits at the loop HEAD — the whole
  arrival/END chain is skipped and stale** (dish: looping motor cues stay Active forever;
  `activeDishes[i]` stuck true → the OnKeyDown ping gate blocks that peer permanently); and
  CO-WRITING a live loop's component never oscillates — it STARVES the loop's arrival check
  indefinitely (it re-reads fresh, steps toward its LOCAL target, checks its own post-write value).
  Park = kill + explicit end-chain cleanup; mirror only onto a DEAD loop. COROLLARY 07-16: a
  one-shot sweep can't outrun a PENDING latent (movePow re-arms audio at the delayed resume,
  AFTER the sweep) — pair the kill with a standing 1 Hz reconciler over a watch-set
  (dish_sync ClientParkLatch as-built). *Look FIRST:*
  `votv-dish-impl-RE-2026-07-16.md` §2-3. `memory/lesson_bp_latent_loop_kill_skips_end_chain.md`
- **`init_objectRenderer` (inside every formDownload) pre-DELETES the previous display actor then
  SPAWNS a fresh one (class from the signal DT row)** — back-to-back formDownload CONVERGES (safe
  to overwrite an arm with host values); but a field-zeroing un-arm (`ResetDownloadMachine`)
  leaves the rendered signal object ALIVE — the native un-arm chain calls `deleteSignalActor` and
  a mirrored disarm must too (as-built: DishArm=99 armed=0 apply). *Look FIRST:*
  `votv-dish-L4-impl-DESIGN-2026-07-16.md` D4. `memory/lesson_objectrenderer_init_spawns_display_actor_converges.md`
- **Wire packets: check `kMaxPacketBytes`=256 / `kMaxReliablePayload`=228 FIRST; quantize u16.**
  The L4 draft shipped 312/388 B structs before reading the caps; the shipped pattern = u16
  centidegrees (`QuantDeg`, 0.01 deg vs the 1.0-deg native tolerance) + u16/65535 scalars →
  full-24-dish packets fit (168/196/100 B). Oversize-by-design = the chunking precedents, not a
  bigger datagram. *Look FIRST:* protocol.h QuantDeg + the static_asserts.
  `memory/lesson_wire_packet_caps_check_first_quantize_u16.md`
- **A CLIENT-born Aprop_C crosses at the SPAWN seam, never "at place"** — client spawns don't
  broadcast (local ghost) and a plain drop/throw fires NO FinishSpawn (only pocket→place does);
  the reusable seam = the F2 client FinishSpawn drain (Init already minted the NewGuid key) →
  class-gated intent → `HostSpawnPlacedProp` born-ASLEEP → the held-prop stream drives it →
  adopt-by-key. First instance: ReelEjectIntent=104 (L7 v114 `ba8ce297`). *Look FIRST:*
  `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md` D4 + prop_drop_intent.cpp.
  `memory/lesson_client_prop_birth_crosses_at_spawn_seam_not_place.md`
- **A save-scalar birth channel must be filled at EVERY birth/author path** (live express + join
  snapshot + container extract + BOTH client intent kinds) via ONE shared per-class reader —
  missing one path = a CDO-default mirror re-broadcast as truth (the L7 correctness CRITICAL:
  pocket→place respawned a blank tape). **R14-16 `/qf` CONVERGED 2026-07-21 → DESIGN `d14b6644`
  (NOT built):** generalize the birth carrier from a `savedScalar` FLOAT to a per-class CONTENT trailer
  (reel→Progress; drive→`DC::ReadDriveRow`+`signal_wire`), inline, RETIRING savedScalar; a client drop
  is a genuine inventory→world birth (`PropDropIntent` carried only the float). **Birth-state ≠
  steady-state** — KEEP `drive_sync`'s `DrivePayload` (a disc mutating in a rack is not a birth). *Look
  FIRST:* prop.h savedScalar block; grep `ReadSavedScalarForClass`;
  `votv-drive-disc-content-birth-DESIGN-2026-07-21.md`. **EMISSION SITS ABOVE CONTENT (measured
  2026-07-22):** `prop_drop_intent.cpp` is ONE funnel — `:279` `if (!parked && !freshBirth) continue;`
  decides WHO is announced, `:293` decides WHAT the survivor carries (this lesson's axis), `:307` is
  gated by the same whitelist. `freshBirth` is a CLASS whitelist (reel v114 → module v118 → drive
  v119), so `d14b6644` works for drives but any non-whitelisted class (an ordinary item a client
  extracts from a container) is discarded at `:279` and the content channel is never called. **One
  design, two axes, not a stack.** Widening `:279` is not a one-liner: `freshBirth` also sets
  `pf::kSleep`. *Look FIRST:* check `:279` before designing what a client birth carries — if the class
  cannot pass the gate, the payload question is moot.
  `memory/lesson_saved_scalar_birth_channel_covers_every_birth_path.md`
- **A held/collected keyed prop is PER-PLAYER INVENTORY data, not a world actor** — a `SaveRecord` in
  `saveSlot.hold[]`/`inventory[]`, streamed+persisted per-GUID by `player_inventory_sync` SEPARATELY
  from the world save. So a client pickup CORRECTLY destroys the world actor (it went into inventory);
  a host-side world custody-actor for it DOUBLE-WRITES → two props on reload. Before designing any
  "keep the prop alive / custody across a client pickup," check `mainPlayer.hpp playerTryToCollect` —
  if collectible, custody IS the inventory blob; a pickup is an inventory↔world BIRTH to sync, not an
  actor to keep. *Look FIRST:* `player_inventory_sync.cpp`; `inventory.h SaveRecord`.
  `memory/lesson_held_collected_prop_is_per_player_inventory_not_a_world_actor.md`
- **When the prop's own refresh verb re-applies `SetActorTickEnabled`, a client-tick PARK is
  un-holdable — ship the host exact-snap CORRECTOR instead** (valid only for RNG-free,
  deterministic, clamped sims; sawtooth ≤ 1 native increment). Pick rule + instance table in
  `docs/COOP_WORLD_PROP_DIVERGENCE.md`; as-built ReelPose=40 (L7). *Look FIRST:* the L7 design
  D2. `memory/lesson_unholdable_tick_park_use_corrector_shape.md`
- **Rollover- and sell-derived saveSlot state is HOST-ONLY FOR FREE** (client daynightCycle
  frozen at TimeScale=0 + client drone tick suppressed → createNewTask/processTask/sell never
  run client-side) — census the writers, then ship a host MIRROR (TaskNewState=103 shape), not
  an intent lane. Applies to L9 meadow / any daily-graded state. *Look FIRST:*
  `memory/lesson_rollover_sell_state_host_only_for_free.md` (the census) + daily_task_sync.cpp.
- **Pre-world subsystems Install at StartCoopSession, NOT world-gated.** `memory/feedback_preworld_install_at_startcoopsession.md`
- **When a release VERB can't be caught, STREAM THROUGH the state** — and when the observed state has
  its own POST-release dynamics (the desk cursor's focus-UNGATED glide integrator, ~12.4 s max decay),
  the CLAIM is not the stream's lifetime: the sender streams until the VALUE settles; the receiver
  decouples apply from the claim axis (v115 `c5ff11a4`, 2nd instance).
  *Look FIRST:* `memory/lesson_stream_through_release_not_verb.md` + `desk_cursor_sync.cpp` v2.
- **An e2e assert must DISCRIMINATE the axis it claims.** `memory/lesson_e2e_assert_must_discriminate_the_axis.md`
- **The join-window PropSnapPos POSITION reconcile is eid-generic at the receiver** — a new
  save-authoritative pos reconcile is SEND-SIDE ONLY (capture baseline + flush); the chip overlay
  auto-skips a non-chip eid, so no dup. *Look FIRST:* `FlushDivergedSavePositionsForSlot` +
  `UpdateChipHostPos`. `memory/lesson_pos_reconcile_generalizes_via_generic_receiver.md`
- (Mirror STATE, not the verb — not because the verb is invisible (the GNatives substrate can now see EX_Local*), but because state-mirroring is convergent, path-agnostic, and handles the client's autonomous mutator. Verb brackets are for identity-flip / intent-attribution, where the fact you need exists only inside the verb's execution window.)**To sync a VOTV world SYSTEM (servers/alarm/…), mirror the STATE + drive the notify-free re-applier
  from the host — NEVER intercept the mutating verb** (breakServer/runTrigger are `EX_Local*` invisible).
  Poll the state field 1 Hz → broadcast on change → client raw-writes + reflected `check()`/`runTrigger`;
  client neutralizes its own autonomous mutator (disable ticker tick / zero the data array). *Look FIRST:*
  `coop/world/alarm_sync.cpp` (one instance) + `coop/interactables/serverbox_sync.cpp` (an array).
  `memory/lesson_votv_world_system_sync_mirror_state_not_verb.md`
- **`coop/world/email_sync` is PEER-SYMMETRIC** (each peer forwards its OWN new inbox rows) — so a client's
  FALSE self-authored email/notice is broadcast to the host + all peers = permanent SHARED-inbox pollution,
  not a cosmetic flash. Before designing a "hide a client's wrong notice" fix, check the channel's
  direction. `memory/lesson_email_sync_peer_symmetric_client_false_notice_pollutes_shared.md`
- **`server` naming/placement:** a signal-SERVER sync goes with its signal siblings in
  `coop/interactables/` (signal_sync/console_state_sync), named after the engine class (`serverBox_C` →
  `serverbox_sync`) — NOT `coop/world/` (a 14-file catch-all), and NOT "server_sync" (ambiguous with the
  NETWORK server that saturates this mod). Instance of `memory/feedback_folder_per_domain_concept_rule.md`.
- **VOTV shared-world RNG concentrates in 2 directors (`daynightCycle`/`mainGamemode`) + ~30 `ticker_*`/
  event spawners + signal/server/loot rollers — host-ownable via mirror-step-3, but our `npc_sync`
  suppress is an ALLOWLIST (15 of ~40 spawn classes) so it inherently lags** → the rule-1 root is
  STRUCTURAL (client runs NO world-spawn ticker; allowlist = MIRROR set only). Only 3 systems seed a
  `RandomStream` (garbagePileSpawner/radiotower/xmaslight) → seed-replicate; all else unseeded → suppress
  or intent. Every gap row is STATIC-INFERRED → run a LIVE client-roll probe before any fix. *Look FIRST:*
  `docs/COOP_RNG_AUTHORITY.md` (living tracker) + `memory/lesson_votv_rng_host_ownable_at_ticker_director_layer.md`.
- **RNG IN a per-peer sim's RATE/output formula = a MECHANIC desync, not a display bug** — mirroring the
  output chases the divergence forever (the client re-sims with its own RNG between ticks); the host must
  own the SIM and roll the RNG, client SUPPRESSES its tick. Found only by reading the RATE block
  byte-by-byte (desk `DL_downloading @66736`: `RandomFloatInRange` needle + `RandomFloat` noise sit IN the
  rate). A "numbers differ between peers" report on anything that ACCUMULATES → read the rate, don't
  output-mirror. **COROLLARY (v111 AS-BUILT):** measure SEEDED-vs-unseeded + STORED-vs-transient first —
  the desk `noise` is unseeded AND transient (never stored; 0 RandomStream) → seed-sync structurally
  impossible → host-auth FORCED; and if the client sim writes only display-local, you OVERWRITE the
  outputs (client sim runs harmlessly), you don't suppress the tick — except an APPEND buffer (log) which
  a scalar mirror can't overwrite (kept separate). *Look FIRST:*
  `research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md` (AS-BUILT section).
  `memory/lesson_rng_in_rate_path_is_mechanic_desync.md`
- **"Derived output converges for free once inputs mirror" is valid ONLY if the WHOLE input read-set
  mirrors** — enumerate every field the derivation reads; a single un-synced input silently diverges the
  output on a screen you thought was covered. Desk gate 2: frData/poData read a filter-size UPGRADE with
  NO live sync lane → would diverge on a mid-session purchase; fix = stream the OUTPUT host-auth (2 extra
  scalars) instead of trusting native convergence. *Look FIRST:*
  `memory/lesson_converges_for_free_needs_complete_input_readset.md`

- **Classify an ambient spawner's tier by its ANCHOR read** (minutes in the dump): player-camera source
  → OWNER-EFFECT; absolute float coords → world host-auth; navmesh random-walk var → world roamer; a
  PRODUCT that stalks the local player → OWNER-ENTITY. Two wrong name-and-vibes calls reversed in one
  day (pinecone wrongly suppressed; sky wisps wrongly per-peer). *Look FIRST:*
  `research/findings/world-systems/votv-ambient-anchor-audit-RE-2026-07-10.md` + the tier table in spawn_authority.h.
  `memory/lesson_ambient_spawner_anchor_read_decides_tier.md`
- **Peer-keyed mirror lanes have 3 measured traps** (owner_entity_sync audit): a CLIENT has no transport
  edge for another client's slot → leaver teardown must be HOST-FANNED; your own mirror spawn re-enters
  every BeginDeferred hook → ScopedMirrorSpawn-exclude EVERYWHERE incl. the rng census; collision must
  drop INSIDE the deferred window (BeginPlay overlap runs during Finish). *Look FIRST:*
  `coop/creatures/owner_entity_sync.cpp` (reference impl). `memory/lesson_peer_keyed_mirror_lane_traps.md`

- **Continuously-MOVING display state needs an unreliable pose-rate stream, not reliable snapshots** —
  the hand-item swing rendered at "1 fps" under a 0.5 s drift-gated reliable resend; split identity
  (reliable announce) from motion (MsgType::HandPose=35, RagdollPose plumbing end-to-end). AND the
  mirror interp must DEDUPE identical-target packets + ADAPT its window to the position-CHANGE cadence
  (EMA 25..80 ms, not fixed 33 ms) — a sender fps dip staircases a fixed window (v115 cursor jerks).
  *Look FIRST:* `memory/lesson_continuous_motion_needs_pose_stream.md` (v109 `a3c55529`; interp v115
  `desk_cursor_sync.cpp CursorInterp`).
- **The client join world-load episode now guards TWO consumers** — the v106 keyed-destroy broadcast
  suppression AND the email shadow diff (2026-07-11 `848a1fc0`: priming/diffing saveSlot.emails across
  the client's own load mis-read 2 swapped default rows as player deletes → EmailDelete broadcast →
  host rows deleted). Any poll-diff over save-backed state must gate on `world_load_episode::InEpisode()`.
  `src/coop/world/email_sync.cpp` + `coop/session/world_load_episode.h`.

- **Mirroring a multi-entry engine array needs NO lock-free scheme if all readers are GT UFunctions** —
  census the readers by disasm first; when every reader is game-thread and none caches the array across
  the write, GT run-to-completion makes a single-GT-task clear+repopulate atomic w.r.t. them (the only
  tear is splitting it across frames). No build-then-swap, no generation counter — just overwrite in one
  fn call + notify-free re-apply of derived state. Measured for container `GObjStack[Index].obj`
  (`recalculateNames`/`getObj`/`updateVolumesAndMass`/UI-copy all GT, 2026-07-15 `bp_reflect`).
  *Look FIRST:* `memory/lesson_gobjstack_mirror_single_gt_task_overwrite_atomic.md`

- **A peer-DEPARTURE notify (a "<X> left the game" toast) gates on the PRESENCE edge (`IsSlotReady` =
  `peerLanesConfigured_`, Connected callback), NOT the transport edge (`IsSlotConnected` = `peerConns_`,
  set already in the Connecting callback)** — a doomed browser connect to a dead/ghost host stays in
  `ConnState::Handshaking(1)`, holds a conn handle (IsSlotConnected TRUE) but never latches lanes, so a
  connected-edge detector fires a FALSE "Remote player left the game" (default nick, Join never processed)
  that leaks into the menu. `net_pump.cpp:791` already gated its disconnect edge on `IsSlotReady`;
  `event_feed.cpp`'s leave edge was the inconsistent one. You can only "leave" a game you were PRESENT in.
  Fix 2026-07-16: `g_lastConnectedBySlot`->`g_lastReadyBySlot`, leave edge on the IsSlotReady falling edge
  (`SuppressPeerLeaveEdges` — the separate "WE are leaving" axis — kept). *Look FIRST:*
  `memory/lesson_departure_toast_gates_on_ready_edge_not_transport.md`
- **A gate anchored on a claim the GATED EVENT itself releases = lost by construction** (2026-07-17,
  the v116 lost-catch root: the catch wrote signalData at 17:04:46, the SAME success released the desk
  FSM-hold at :47, and the 1 Hz claim-gated detector + the host holder-validator both raced it; the
  baseline roll-forward made the loss PERMANENT). Derive authority from the event's OWN evidence (the
  unprimed change-edge, writer set enumerated), not from concurrent occupancy. *Look FIRST:*
  `signal_catch_sync.h` header. `memory/lesson_claim_anchored_gate_races_its_own_release.md`
- **Census the GENERIC lifecycle channels BEFORE building lane-side capture** for any consume/slot
  machine (2026-07-17: one read of `prop_destroy_seam.cpp` — the v106 K2_DestroyActor seam crosses
  keyed destroys BOTH roles — dissolved the laptop lane's whole planned BndEvt eid-capture; births
  ride the watcher/F2/eject-intent channels). The lane owns only the residue (scalars + content).
  `memory/lesson_census_generic_channels_before_new_lane_capture.md`
- **Client-birth SIDE-DATA (strings > savedScalar) correlates via the ADOPTION eid-binding** — park
  pending data on the local actor, drain until the eid lands, ship {eid, chunks}; no nonce, no intent
  format change (v116 disc content, qf R8-Q3). *Look FIRST:* `laptop_sync.cpp DriveEjectContentWatch`.
  `memory/lesson_adoption_eid_binding_correlates_client_birth_sidedata.md`

- **Overlap-triggered halves of a mirrored world FSM SELF-SIMULATE on receivers** (the pose
  stream drags the prop into the trigger; Delay(0) decouples the capture from any wire-apply
  scope); verb-triggered halves NEVER self-sim — classify every transition trigger BEFORE
  designing the lane; the self-simmed half wants idempotent state lines, the verb half needs
  the wire event mandatorily (v119 driveSlot: insert self-sims, unsynced eject = permanent
  occupied-by-ghost). `memory/lesson_overlap_half_of_world_fsm_self_simulates.md`
- **Deferred wire applies (pending-until-resolvable) stash the target state AT QUEUE and DROP
  on replay if it moved** + one pending per target, newest supersedes — blind replay resurrects
  a superseded state and self-primes the baseline so no sweep ever heals it (v119 audit CRIT-1;
  the inverse of op-before-state-ready). `memory/lesson_pending_deferred_apply_stash_state_and_drop.md`
- **Deny/refund/reap handshakes correlate by ITEM CONTENT, never sender alone** — the v118
  module reap was safe only because the BYTE discriminated; single-class items (drives) need
  the payload content hash + the reap moved to the adoption-payload seam (v119 audit MAJOR-1:
  slot-only matching silently eats a legitimate same-peer birth).
  `memory/lesson_deny_refund_correlates_by_content_not_sender.md`
- **First-sight-in-sweep != birth authorship**: a joiner's save-loaded entities materialize
  AFTER its connect prime and would re-author the host's own rows under a first-sight
  broadcast rule (v119 smoke-measured) — note authorship at the local birth drain (actor+TTL);
  clients broadcast only noted births, the host its organic world.
  `memory/lesson_first_sight_is_not_birth_authorship.md`

- **A move/sort verb on a BP array store invalidates POINTER identity AND positional diffs**
  (sortSignal = Array_Get copy + Remove + Insert -> FString deep-copy -> new ptrs every move; the v65
  RowKey + prefix-walk + ptr-keyed caches all died in one v120 pass — they were valid ONLY because the
  deck list has no move verb). Identity for such stores = content-hash MULTISET {hash->count} (move =
  no-op, duplicates = counts). **SHARPENED v121: the rule is TWO-SIDED — census the store's verb
  GRAMMAR first. NO move verb (laptop buffer quad: removeAt + tail-append only) -> an EXACT greedy
  edit script (index-anchored) keeps converged arrays order-converged with NO order lane
  (laptop_buffer_sync DeriveArray).** LOOK FIRST: meadow_db_sync.cpp vs signal_sync.cpp vs
  laptop_buffer_sync.cpp. `memory/lesson_bp_struct_copy_kills_pointer_identity_at_moves.md`
- **Lane FIFO orders HAND-OVER, not authorship** — a line deferred to a pending/retry queue is outside
  the shared-lane pin; a later cross-REFERENCING line (order/permutation/canonical-by-instance) that
  sends immediately overtakes it and the receiver skips the unknown reference (v120 order HIGH-1:
  permanent order divergence). Gate cross-referencing sends on an EMPTY pending queue (poll + rebroadcast
  + seed paths all). LOOK FIRST: meadow_db_sync.cpp "FIFO guard" comments.
  `memory/lesson_lane_fifo_covers_only_handed_to_gns.md`
- **The B2 not-ready skip makes join-window lines a PERMANENT loss** (SendReliable + relay `continue`
  past !IsSlotWorldReady — nothing queues, nothing retries; a no-reconcile lane diverges at every
  mid-activity join until the NEXT join). Root idiom: per-slot snapshot at save_transfer OnRequest (the
  g_blobKeys precedent) + ready-edge seedDelta(h)=cur-snap-unmaskedPending (op-counter masks) + a client
  send gate on own ClientWorldReady. UN-RETROFITTED sharers: signal_sync (deck), email_sync. LOOK FIRST:
  meadow_db_sync.cpp CaptureJoinSnapshot/QueueConnectBroadcastForSlot.
  `memory/lesson_join_window_b2_skip_is_permanent_loss_seed_delta.md`
- **A canonical-as-ack on the blob transport must be BOUNDED and SEND-CHECKED** — blob_chunks
  hard-caps a blob at MaxBlobBytes() (56,100 B) and returns false WITHOUT sending; an ignored result
  on an ack-bearing path = the authority primes believing it delivered = silent permanent divergence
  in exactly the content-heavy case (v121 CRIT-1; the laptop buffer is native-unbounded). Bound via
  deterministic tail-drop + WARN; refused send = no prime + retry arm. LOOK FIRST:
  laptop_buffer_sync/floppybox_sync PackCanonicalBounded + HostBroadcastCanonical; blob_chunks.h
  MaxBlobBytes. `memory/lesson_canonical_ack_needs_bounded_blob_and_checked_send.md`
- **A send-gate must use the send path's OWN readiness predicate** — IsSlotReady (transport) vs
  IsSlotWorldReady (the B2 gate SendReliable* itself enforces) differ exactly inside the join
  window; gating on transport-ready = every send refused + a 4 Hz no-prime/detector-refire loop for
  the whole load window (v121 smoke-caught). Zero world-ready peers = prime SILENTLY (the ready-edge
  connect replay covers the joiner); WARN on the arm transition only. LOOK FIRST:
  laptop_buffer_sync/floppybox_sync AnyClientReady; session.h:293 vs :377.
  `memory/lesson_send_gate_predicate_must_match_the_send_paths_own_gate.md`
- **A persisted BP field can be a DERIVED MIRROR regenerated from per-peer widget arrays** —
  laptop.floppyBuffer is rebuilt FROM ui_laptop.bufferSlots by updFloppy at EVERY refresh (incl. the
  refreshes OUR wire applies trigger via WriteSlot/ClearSlot); genFloppyBuffer's only caller is
  loadData; nothing native clears bufferSlots -> a raw field write without a widget rebuild is
  stomped at the next refresh. Wire apply = write fields + the native loadData recipe
  (RemoveFromParent-each + num=0 + genFloppyBuffer + updFloppy) + prime. LOOK FIRST: ue_wrap
  laptop.cpp WriteQuadAndRebuild. `memory/lesson_derived_persisted_field_regenerated_from_widget_arrays.md`
- **Measure a "sibling device"'s BINDING before designing its lane — it may be a remote TERMINAL** —
  prop_portablePc binds the BASE laptop at BeginPlay (bindPC(gamemode.laptop.laptop)); its screen is
  a delegate-bound mirror (pcLaunched) that converges FREE once isOpened syncs; its whole "device
  lane" reduced to one lid bool, and the TRACKER's "own floppyTypes/floppyData" premise was a
  misattribution (the arrays are prop_floppyBox_C's). Dump the uber: BeginPlay binds? delegate
  mirrors? Only the remainder needs a lane. LOOK FIRST: the v121 design doc SS0/SS3;
  prop_portablePc.json. `memory/lesson_sibling_device_may_be_remote_terminal_measure_binding.md`
- **A host eid is NOT a cross-peer-stable identity for a SAVE-LOADED entity.** The Build-3 sidecar bound
  by a LOAD-ORDER cursor (assuming client load order == save-array ordinal), which diverges under
  async-load / GC churn. Reconcile by an INTRINSIC key (save Key / save position), never by the bound
  eid. Born from the 2026-06-29 hands-on regression: a kerfur off->active retire-by-eid destroyed the
  WRONG kerfur on both peers. *Look FIRST:*
  `memory/lesson_eid_not_cross_peer_stable_loadorder_bind.md`
- **Classify engine READS into four kinds before pricing an extraction.** "This module reads the
  engine" hides four unrelated things: *intent production* (read the local player — STAYS forever),
  *handle validation* (is this pointer live — DISAPPEARS, the extracted side holds ids not pointers),
  *outcome capture* (what did the engine machine decide — STAYS, you record it), and *canon derivation*
  (read the engine to BUILD the authoritative state — the ONLY work, invert to write-only). Measured
  2026-07-20: `device_occupancy` 9/9 intent, `drive_sync` ~12/15 handle-validation, `signal_catch_sync`
  5/6 outcome-capture — so the heaviest-reading lane was the cheapest to move and read COUNT is
  uncorrelated with migration cost. Canon derivation totalled **32 sites in 9 lanes** (a name-shape
  grep = order of magnitude, NOT a verified list — keep that caveat beside the table).
  LOOK FIRST: `docs/COOP_SERVER_MODEL.md` §6-§7.
  `memory/lesson_classify_engine_reads_before_pricing_extraction.md`
- **Anchor an accumulator; never stream it.** A value that is a function of elapsed time
  (`dryTimer += DeltaSeconds`) should not get a sync channel — store ONE start stamp and let each peer
  compute it. Buys, for free: no stream, impossible divergence, **late join solved** (the joiner gets a
  stamp and is instantly correct — no snapshot cadence, no mid-activity window), and an empty server
  can FREEZE. Precedent measured: MTA's `CClock.cpp` is 58 LOC of pure formula with **no tick**. Valid
  ONLY if the RATE is constant — so measure the input set of the RATE, not the value; a rain/indoor/
  temperature gate sends the element back to a syncer. Park the brain regardless, or the local
  accumulator fights the computed value. LOOK FIRST: `docs/COOP_WORLD_PROP_DIVERGENCE.md` (2026-07-20
  section) + `docs/COOP_SERVER_MODEL.md` §4. `memory/lesson_anchor_the_accumulator_dont_stream_it.md`
- **Storing state you cannot parse: the DONOR owns everything your canon doesn't cover.** An
  engine-free arbiter holds the world save opaquely (bytes + hash; GVAS is never deserialised outside
  the engine). Two consequences the storage decision itself never mentions: (1) whoever DONATES the
  blob silently authors the entire unsynced remainder — so donation must be host/admin-only, recorded
  as `docs/security/TRACKER.md` **F1** before any donation path exists; (2) **the blob's re-donation
  cadence is the INVERSE of canon coverage** — which converts "the server's authority grows with every
  sync lane" from a slogan into an observable metric, and its complement is the trust exposure. The
  trap: blob-then-overlay already exists in the tree (joiner loads the host save, `prop_snapshot`
  reconciles by key), and that familiarity hides the new authority question. LOOK FIRST:
  `docs/COOP_SERVER_MODEL.md` §5b. `memory/lesson_opaque_blob_custody_donor_dictates_the_remainder.md`

## 4. Dispatch, hooks & input seams

- **A net-delta array-diff POLL is the wrong tool for a DISCRETE user event** — it (a) LAGS the
  mirror by up to the poll interval and (b) SILENTLY DROPS any change that returns to the baseline
  within one window (fast spam nets to zero → never sent). Measured 2026-07-21 take-4: `desk_input`
  250 ms net-delta lost a spam polarity toggle (R2); `signal_sync`/`comp_sync` 1000 ms lagged the
  export/import list (R17); `laptop_sync` 250 ms power poll = slow PC-on (R7). Invisible in steady
  state → slips smoke; only fast input exposes it. Fix = capture at the native VERB edge (PE/Func
  hook), not "poll faster". Distinct from the transient-predicate poll lesson. *Look FIRST:* any Tick
  with a `kPoll*` interval + a `g_baseline` scalar/array diff; the take-4 findings doc.
  `memory/lesson_netdelta_poll_aliases_and_lags_discrete_events.md`
- **Presser-local SOUNDS/effects mirror at the NATIVE effect seam, never by classifying inputs** —
  Func-patch `AudioComponent:Play` + `ActorComponent:SetActive/Activate` and pointer-whitelist the
  target COMPONENTS (the whitelist doubles as the owner filter: the laptop's same-named comps
  self-exclude). Func-visibility is decided by the CALLEE's NATIVENESS, never the call opcode —
  EX_VirtualFunction on a native target funnels through `UFunction->Func` (286-asset census, v115).
  Echo = a GT wire-apply depth guard around EVERY wire apply; both-peers-organic callers must be
  censused FIRST; loops are STATE (join re-assert + host-owned leaver teardown), one-shots are events.
  An e2e wire self-test must OUTWAIT the receiver's world-load (+5 s fx dropped at the unresolved
  desk; +20 s from connected landed). *Look FIRST:*
  `memory/lesson_audio_effect_mirror_func_patch_native_seam.md` + `desk_snd_fx.cpp` (v115 `c5ff11a4`).

- **A test/automation bot drives at the human-INPUT seam, NEVER the effect seam** (2026-07-23, the
  Baritone-analog director DESIGN). Issue actions at the UFunctions a human's input hits
  (`AddMovementInput`, `InpActEvt_use`, forced `lookAtActor`), never the downstream mutator
  (`propInventory_C::takeObj` etc.). Driving INPUT makes the bot authority-equivalent to a human client,
  so the scenario runs the REAL detection->broadcast->authority path; driving the EFFECT mutates state
  locally and tests nothing (or fakes a bug). `take`/`grab`/`press` = "aim + input-verb" composition.
  Scope: callable input-seam UFunctions only (out: EX_Local-only/widget/analog-held/drag; resolve on the
  DECLARING class per the FindFunction trap). Anchor: `autotest_chippile` drives real InpActEvt_use through
  the real wire (v85 PASS). **REFINED 2026-07-23:** the "input seam only" ideal is fully achievable only for
  MOVEMENT; discrete input-ACTION verbs are inert via reflection (next row). *Look FIRST:*
  `memory/lesson_drive_test_bot_at_input_seam_not_effect_seam.md` + the director DESIGN §B4.
- **Discrete input-ACTION verbs are INERT via reflection; only movement input is input-seam-faithful**
  (2026-07-23, director green run). A `CallFunction` on a `mainPlayer_C` `InpActEvt_*` discrete verb
  (`InpActEvt_drop`, `InpActEvt_use`) fires the ProcessEvent OBSERVERS but does NOT run the BP body -- the
  engine drives the ubergraph from the real key press-edge, not the reflected stub (measured: hand still
  full 20 ticks after `InpActEvt_drop`; chippile found the same for the grab body). So a driving bot must
  "arm the observer (`InpActEvt_*`) + call the EFFECT verb" (`throwHoldingProp` / `door_C::doorOpen` /
  `playerGrabbed`), and say so honestly (`drop-input-seam-faithful=0`). ONLY `AddMovementInput` is a pure
  input-seam drive. *Look FIRST:*
  `memory/lesson_discrete_input_action_verbs_inert_via_reflection.md` + the director DESIGN §6b.
- **A driven Character's movement input must land EVERY FRAME or friction brakes it to a crawl**
  (2026-07-23, director). `CharacterMovement` consumes+clears `ControlInputVector` per frame, so a
  worker-loop `AddMovementInput` at a 20ms tick (game ~9ms/frame) leaves most frames zero-input and VOTV's
  ground friction decelerates the body between pushes -> ~5cm/s "turtle"; `kTickMs=4` (2-3 inputs/frame) =
  full speed. `GT::Post` runs on the PE detour (~2050/fr) so the round-trip is sub-ms -- the Sleep was the
  bottleneck. Measure per-TICK displacement, not aggregate (a shove/respawn inflates the aggregate). *Look
  FIRST:* `memory/lesson_movement_input_must_land_per_frame.md` + the director DESIGN §6b.
- **A high-priority CLEANUP process must EXCLUDE the goal state from its activation** (2026-07-23,
  director). In a priority-arbitrated process system, a cleanup process on a raw predicate (ClearHand active
  when the hand is full) preempts + undoes a lower-priority goal process if the goal's SUCCESS satisfies the
  predicate: Grab put the grabbed clump in `grabbing_actor` -> ClearHand woke + dropped the goal clump. Gate
  the cleanup to exclude the goal-completion region (ClearHand active only out-of-range + `!grabbed`); give
  it the goal/blackboard so it can. *Look FIRST:*
  `memory/lesson_cleanup_process_must_exclude_the_goal_state.md` + the director DESIGN §6b.
- **An engine with a baked NavMesh collapses a movement-bot's whole pathfinder** (2026-07-23). VOTV levels
  ship RecastNavMesh/NavMeshBoundsVolume as cooked-umap exports + NPCs pathfind via AIMoveTo, so a
  Baritone port DELETES A*+Moves+Movement+ActionCosts+chunk-cache and uses one engine call:
  `FindPathToLocationSynchronously` (controller-agnostic `UNavigationPath`) + per-tick `AddMovementInput`
  steering on the possessed player (NOT AIController MoveTo — that needs an AIController; mainPlayer_C =
  AddMovementInput x7 + CharacterMovement x82, AIMoveTo=0). Trap: navmesh ACTORS authored (umap export =
  measured) != navmesh BUILT+traversable (runtime HALT probe). *Look FIRST:*
  `memory/lesson_engine_navmesh_collapses_movement_bot_pathfinder.md` + the director DESIGN §A2/§B2.
- **SET-state syncs as VALUE-ops + a host-canonical container, never slot deltas** (v118 L8,
  2026-07-18). A native uniqueness gate (the plug dup-check) makes the positional-looking array a SET:
  slot-keyed deltas lose an element permanently on a concurrent same-slot race and diverge index-read
  layouts forever; value-ops (add/remove{value}) + the host's canonical full-container broadcast +
  drain-before-adopt + a deny/refund op make divergence structurally impossible. *Look FIRST:*
  `memory/lesson_set_state_syncs_as_value_ops_plus_canonical.md` + `physmods_sync.cpp` (v118).

- **The HOST's organic change never rides the remote-op apply path** (v118 L8, 2026-07-18; BOTH
  audits independently). A remote-op apply assumes NOT-YET-APPLIED state -- the host's own organic
  change is already in its authoritative state, so self-routing it hits the dup/absent branches
  (a phantom refund spawn per host plug + no canonical broadcast). Host organic diff = broadcast
  canonical directly; only CLIENT ops ride the op path. *Look FIRST:*
  `memory/lesson_host_organic_change_never_rides_the_remote_op_path.md` + DrainLocalDiff (v118).

- **A GEN GUARD decouples correctness from an INFERRED dispatch-visibility fact** (v117 L6,
  2026-07-18). When an edge-suppression rule hangs on unmeasured visibility (fin()'s PE dispatch was
  doctrine-inferred, live-unmeasurable pre-hands-on), don't prove-first (blocked) or ship-on-inference
  (the crutch class): make the mechanism NON-LOAD-BEARING — the session-start edge mints max(seen)+1,
  the end edge carries the gen it terminates, receivers drop stale/duplicate ends, starts apply
  unconditionally + realign. The inferred bracket demotes to spam suppression. *Look FIRST:*
  `memory/lesson_gen_guard_decouples_inferred_visibility.md` + `deck_play_sync.cpp` (v117).

- **BP INNER calls (`EX_CallMath`/`EX_*`) BYPASS ProcessEvent** — a PE hook won't fire. THIRD instance
  2026-07-10: the T1 probe's PE-table interceptors on `Delay`/`K2_SetTimer*`/`SetActorTickInterval`/`QuitGame`
  were BLIND for a whole smoke (caught by its own positive control; moved to the Func-patch seam `7109efd1`).
  BONUS: a Func-patch POST hook's `sourceObject = FFrame::Object` = the CALLING BP actor — free per-caller
  attribution, no param stepping. FOURTH instance 2026-07-10 eve (the INVERSE trap): the STATIC dump
  `$type` cannot PREDICT visibility either way — garbagePile/pinecone read `EX_CallMath` yet measurably
  FIRE the PE POST; chipPile reads the same and doesn't. Only a live catch classifies a caller.
  *Look FIRST:* the dispatch map's MECHANISM row + its live-catch evidence, never the dump alone.
  `memory/lesson_ex_callmath_invisible_to_processevent.md`
- **`R::FindFunction(cls, name)` is EXACT-OWNER — no SuperStruct climb**: a parent-class UFunction
  (AActor::SetLifeSpan) looked up on a BP leaf returns NULL every call + pays a futile full-array walk
  (audit CRITICAL 2026-07-10: the ambient-mirror lifespan backstop was silently dead). SECOND STRIKE
  2026-07-11: BOTH spawn-by-key sites resolved `setKey` on the LEAF wire class → prop_crowbar_C mirrors
  spawned keyless → field key diverged from the wire binding → pickup-destroys missed the host = the
  host-side crowbar DUPE (rocks masked it: a rock IS prop_C). THIRD STRIKE 2026-07-12: the take-3
  KEY-UNIQUENESS re-key silently no-opped — trashBitsPile_C's setKey lives on actor_save_C, outside the
  hardcoded Aprop_C fallback; fixed by `R::SuperStructOf` + a chain-climbing ResolveSetKeyFn
  (`460da7e4`) — reuse that resolver shape. Resolve on the DECLARING class + cache;
  when adding any reflected-call site, grep for other leaf-class resolves of the same fn.
  *Look FIRST:* the SDK header for which class declares the fn + the RCA finding
  `research/findings/props-lifecycle/votv-crowbar-mirror-key-divergence-RCA-2026-07-11.md`.
  `memory/lesson_findfunction_exact_owner_no_superstruct_climb.md`
- **VOTV damage NEVER touches UE TakeDamage/ApplyDamage** — melee = `mainPlayer.attack` →
  per-class `addDamage`/`damageByPlayer`, ALL EX_Local-invisible inward from `attack`; the ONE
  Func-patchable choke is `VictoryFloatMinusEquals` (every prop+creature health write; FFrame::Object =
  target). A client's hits are LOCAL-ONLY today (user live 2026-07-11: zero damage cross-peer, silent
  crowbar door hits). The mannequin is a PROP (`Aprop_mannequin_C : Aprop_C`), not a Character.
  *Look FIRST:* `research/findings/player-puppet/votv-melee-damage-path-RE-2026-07-11.md` (chain + ranked hook seams).
  `memory/lesson_votv_damage_bypasses_ue_takedamage.md`
- **A SCRIPT-fn called via `EX_Local*` is invisible to BOTH the PE hook AND the Func-patch** — patch the
  NATIVE calls inside it. **Boundary sharpened 2026-07-13: this is THE ONLY remaining invisible class,
  and it's SOLVABLE (GNatives swap = a third hook primitive); EX_CallMath was NEVER part of the wall
  (native targets = Func-patchable). Check the CALLEE's nativeness before declaring a wall.**
  **Spike-measured 2026-07-13:** `GNatives_table`@`0x144D8ECD0`; LocalVirtual=op 0x45@`0x1414751A0`
  (12-byte FScriptName operand), LocalFinal=op 0x46@`0x141474FB0` (8-byte UFunction*). **0x45 IS the
  kerfur flip opener — LIVE-CONFIRMED [V] hands-on (STEP 1.0 v3, 2026-07-13): `dropKerfurProp`
  (Context=`kerfurOmega_C`) / `spawnKerfuro` (Context=`prop_kerfurOmega_C`) both fire via 0x45 on both
  peers.** *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` +
  `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`.
  `memory/lesson_script_fn_invisible_to_func_patch.md`
- **The `EX_LocalVirtualFunction` (0x45) operand is a 12-byte FScriptName `{ComparisonIndex@0,
  DisplayIndex@4, Number@8}`** — NOT `{CmpIdx, Number@4, Display@8}`. Shipping build: `CmpIdx==DispIdx`
  so bytes 0-7 read as the DUPLICATED index (`Init_904`=`0x0000038900000389`); real `Number` is `op[2]`
  (@byte 8), =0 for a clean verb name. Match `op[0]==StringToFName.ComparisonIndex && op[8]==Number` —
  raw bytes-0-7 vs an 8-byte FName NEVER matches (v1 probe's silent-miss). LIVE-measured; the probe-first
  STEP 1.0 caught it BEFORE the un-removable swap. *Look FIRST:* dump the live operand as THREE int32s,
  expect `op[0]==op[1]`. `memory/lesson_fscriptname_operand_layout_cmpidx_dispidx_number.md`
- **A guard/suppression that never LOGS is indistinguishable from one that never FIRES** — the client
  kerfur menu-cancel hooks the PE-VISIBLE menu entry, but the conversion verb is `EX_LocalVirtualFunction`
  (PE-invisible), so the cancel NEVER reached it (*"cancel/queue lines never appeared in any real session"*,
  `kerfur_convert.cpp:97,402`) — the client has been converting LOCALLY then reconciling after the fact, and
  that dead guard is the mechanism that made take-9-bug1 possible ("kerfur deleted on both peers"). THREE
  this session, same shape (dead cancel · Model-B eid-reuse [§3 said rebindInPlace, code mints per-form eids]
  · "the two-phase arm record" = actually FOUR converge mechanisms): the DOC describes intent, the CODE
  describes behavior, nobody diffed them. Instrument the SUPPRESSION path (one line on every fire); before
  building ON a documented mechanism, grep its fire-line in a real session to prove it RUNS. **INVERSE
  INSTANCE 2026-07-14 pm:** a no-log guard can be REACHED-BUT-DECLINING, not only never-fired — I read
  `TryCaptureKerfurPropDestroy`'s zero log lines as "structurally dead" and nearly deleted it; it was reached
  on every client turn-on and declined SILENTLY (its no-qualified-B path logged nothing). Instrumenting the
  DECLINE path (not just suppress) revealed it. So instrument EVERY exit, not just success. *Look FIRST:*
  grep the guard's log line in a real log — no line = never fires OR silently declining; add it on the fire
  AND decline paths and find out which BEFORE building on it or deleting it. **SECOND INVERSE 2026-07-14 eve
  (log LIES the OTHER way):** `prop_lifecycle.cpp` Init POST logged `"HOST broadcasting SPAWN"`
  UNCONDITIONALLY, above the sole-express suppress gate — so a SUPPRESSED conversion still printed a broadcast
  that never happened; behavior right, log invented a failure, and it cost a detour chasing a phantom
  double-broadcast in the 20:20 take. Same root one layer down (log-SITE vs code-PATH): **a fire-line must be
  emitted from INSIDE the branch it describes — logging before a gate logs an INTENTION, not an event**, and an
  intention-log makes a working seam read as broken. Fixed `6b246201`.
  `memory/lesson_guard_that_never_logs_is_a_dead_guard.md`
- **A VM-dispatch bracket (GNatives-swap wrapper / self-bracket) runs MID-BYTECODE — do ZERO engine
  calls in that window** — capture data only (pointers, eids off a LIVE actor, class checks) + a pure DATA
  STORE (no engine dispatch); DEFER every engine call (register, park=ProcessEvent, broadcast, converge) to
  the deferred barrier. A nested ProcessEvent pump mid-verb corrupts (measured `kerfur_convert.cpp:11-20`;
  park=PE `kerfur.cpp:132`). **KERFUR MID-VERB STORE CORRECTED 2026-07-14 pm (DRAIN retired, measured-false):**
  the mid-verb store is just CAPTURING B (the successor actor pointer + index) — NO drain, NO repoint, NO
  migrate. The FINAL fix feeds that captured-B to the DEFERRED converge (`TryCaptureKerfurPropDestroy` /
  `ConvergeAfterConversion`), which does its normal per-form eid mint + KerfurId re-key UNCHANGED; only WHICH-B
  is fixed (see `[[project-vm-dispatch-2a-capture-2026-07-14]]`). Core discipline (zero engine mid-verb, defer to barrier)
  is reusable for the whole VM-consumer class (kerfur/melee/smart-items). *Look FIRST:*
  `docs/COOP_VM_DISPATCH_PLAN.md` §3 (SUPERSEDED banner points at the A+ spec).
  `memory/lesson_vm_bracket_zero_engine_mid_verb.md`
- **The kerfur conversion verbs are SYNCHRONOUS bodies (no latent node) — so the form spawn
  (`FinishSpawningActor`) AND the `K2_DestroyActor(self)` both fire INSIDE the 0x45 bracket, every
  toggle** → capture-in-window is sound. `dropKerfurProp` 30 stmts, `spawnKerfuro` 23 stmts, both
  standalone, whole-body latent scan = NONE, none between any `BeginDeferred`/`FinishSpawning`.
  [V] two ways: import-resolved body walk + 18/18 hands-on (`722fbe18`). This is the load-bearing 2a
  premise — settled, do NOT re-dig. *Look FIRST:* `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`
  (body walk + runtime). `memory/lesson_kerfur_verbs_synchronous_capture_in_window.md`
- **When a design MIGRATES identity at birth, it must cover EVERY identity map keyed on the entity** —
  GENERAL principle, holds for any repoint/rebind/re-key. TRAP that made it: "the eid" is not the whole
  identity surface — the kerfur HOST has a SECOND table (`g_actorToKerfurId`/`KerfurRecord.actor`,
  `kerfur_entity.cpp:62-64`; client is eid-based, no KerfurId map) that an eid-only rebind does NOT re-key →
  mid-window KerfurId resolves DEAD-A while eid resolves LIVE-B (heisenbug). **KERFUR RESOLUTION CORRECTED
  2026-07-14 pm (drain retired): kerfur migrates NOTHING and DRAINS NOTHING at birth** — the eid is per-form
  + K stable (`kerfur_convert.cpp:188-258`), and the FINAL fix leaves the existing converge (per-form eid mint
  + KerfurId re-key) UNCHANGED, fixing only WHICH-B (feed the captured successor to the guard). So the 2nd-map
  heisenbug cannot arise — nothing is migrated at birth to go half-done. The GENERAL enumerate-every-map
  principle still holds for ANY design that DOES migrate. *Look FIRST:* grep the entity's
  id/type + enumerate every keyed map BEFORE any migration design; `[[project-vm-dispatch-2a-capture-2026-07-14]]`
  for the A+ resolution. `memory/lesson_identity_migrate_at_birth_covers_every_map.md`
- **Before installing a PERMANENT / un-removable seam (process-lifetime GNatives swap, never un-swapped),
  measure its real cost in a THROWAWAY removable probe FIRST** — including the ENABLED=false disabled path
  (the eternal tax the process pays forever) and a WORST-CASE frame, not idle. You can't roll back a
  permanent seam; the probe you can delete. (impl /qf: the gate-2.2 probe used a simpler filter → 0.013/
  0.038 was a LOWER bound, not the real-filter gate.) *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` §2.0
  (STEP 1.0). `memory/feedback_probe_first_for_unremovable_seams.md`
- **A destroy-seam consult runs POST-destruction: engine reads on the dying actor return ZEROS** —
  `GetActorLocation` on it reads (0,0,0) (RootComponent gone) while class/name/key MEMORY reads still
  work, so a proximity matcher silently mis-filters (take-10: the capture never fired all session).
  AND: matcher decline paths must NEVER be silent — take-10's two unlogged declines cost a full test
  cycle to localize. Positions come from caches (watch/stamps/element rows), never live dispatches on
  the dying actor. *Look FIRST:* `docs/COOP_VM_DISPATCH_PLAN.md` (the superseding temporal-pairing
  design). `memory/lesson_post_destroy_seam_reads_zeros_and_silent_declines.md`
- **BP-JSON call censuses: text-grepping an export for a NATIVE fn name gives FALSE NEGATIVES** — imported
  callees are bare `StackNode` indices; resolve `Imports[-idx-1].ObjectName` first (2026-07-10 twice:
  updateHold "no attach", delEmail "removes=[]"). *Look FIRST:* the resolver pattern in
  `tools/rng_census_analyze.py`. `memory/lesson_bp_json_grep_resolve_imports.md`
- **use-HOLD (`canBeUsedHold`) bypasses InputAction press-sims** — bind identity on the ENTITY-sim. `memory/lesson_use_hold_bypasses_press_seams.md`
- **An InputAction can have MULTIPLE delegate bindings — hook ALL.** `memory/lesson_input_action_multiple_delegate_bindings.md`
- **Every global `GetAsyncKeyState` hotkey poller gates on `!IsOverlayCapturingText()` too** — else it
  fires while the user types in chat (T then G triggered voice). `memory/lesson_hotkey_pollers_gate_on_overlay_text_capture.md`
- **A gated probe that "didn't fire": FIRST verify the GATE reads true.** `memory/lesson_gated_probe_verify_the_gate.md`
- **BP dynamic-multicast delegate UNBIND from C++ is an UNPROVEN capability here** (zero
  `RemoveDynamic`/`Unbind`/`ClearDelegate` in-tree) — before designing "suppress a BP event by killing its
  delegate handler", PROVE the unbind (layout RE + probe); it's a BUILD GATE. Prefer the proven
  caller-neutralization (disable a ticker / zero an array) or host-authoritative state.
  `memory/lesson_bp_delegate_unbind_unproven_capability.md`
- **A completion latch makes every LATE registrant a SILENT no-op.** `vm_dispatch`'s
  `TickResolvePending` opened with `if (g_allResolved) return;` — read as "stop re-trying", it was a
  PROCESS-lifetime latch, so a verb registered after the first "all N resolved" moment was never
  FName-resolved and its callback never fired. Every signal said success: registration returned
  `true`, the consumer's own banner printed, the verb even appeared in the log with a slot number.
  Cost TWO RED hands-on takes, both mis-attributed (first to entity identity, then to a whole
  delivery-pipeline RE with a wrong root) because the lane's code was never reached. The trigger was
  ORDERING: it was the tree's first consumer to register from `Tick()` instead of install time.
  Fixed `3027aeed` (registration clears the latch — per-PASS, not per-process).
  *Look FIRST:* a callback that "is registered" but never fires — prove it ENTERED before
  re-deriving any domain root; compare the timestamp of the substrate's "all N resolved / ARMED"
  line against your own "registered" line. Any `if (allDone) return;` in a subsystem that accepts
  dynamic registration is this bug waiting for its first late registrant.
  `memory/lesson_late_registrant_inert_after_all_resolved_latch.md`
- **A UMG click handler gated on HOVER state no-ops via a bare reflected call — set the hover + drive
  the BOUND slot, don't fall back to the effect seam.** `uicomp_playerInvContainerSlot::pressButton`
  (the container-slot click handler) has bytecode that calls `setHoverContainerSlot(self)` on its Owner
  UI + references `IsHovered`, so the take is keyed on which slot the UI considers HOVERED. A reflected
  `CallFunction` moves no mouse → `pressButton` returned `true` and took NOTHING (count unchanged) when
  driven on a stray `uicomp_playerInvContainerSlot_C` with no hover set. Fix (measured, count 2→0):
  drive `ui.slots_prop[i]` (the UI's OWN bound slot) after `ui.setHoverContainerSlot(slot)` — NOT the
  effect-seam `prop_container::extract(Index)` (which drives a seam a human never touches, breaking
  authority-equivalence). `em_take`/`makeSelected` are PLAYER-side, not the container take. The UMG
  analog of the input-inert trap; verify by the state DELTA, never the call's return.
  *Look FIRST:* driving any VOTV UMG widget action by reflection — read its click-handler bytecode
  (kismet-analyzer `to-json` on the `.uexp`) for an `IsHovered`/hover/selection gate, satisfy that
  state, drive the parent's bound child (`slots_*`), not a stray instance. Pairs with
  `[[lesson-discrete-input-action-verbs-inert-via-reflection]]` +
  `[[lesson-findfunction-does-not-walk-the-superclass-chain]]` (containers are a class family; the
  verbs live on the base `prop_container_C`). `memory/lesson_umg_click_handler_gated_on_hover_state.md`

## 5. Engine / UE4 facts

- **`R::FindFunction` does NOT walk the superclass chain** — it matches `OuterOf(fn) == owningClass`
  EXACTLY (`ue_wrap/core/reflection.cpp:427`; no chain-walking variant exists anywhere in
  `reflection.h`). So `FindFunction(ClassOf(instance), "SomeInheritedFn")` returns **nullptr** whenever
  the function is declared on a BASE class — the normal case for any BP subclass — and callers that skip
  a null do nothing, silently, forever. CONFIRMED CASUALTY (measured 2026-07-22): the container lane's
  `updateVolumesAndMass` re-derive resolved from `ClassOf(owner)` = `Aprop_inventoryContainer_drone_C`
  while the function is declared only on `Aprop_container_C` (SDK `prop_container.hpp:32`) → the
  re-derive had **never once run** on any peer, which is the `686`-vs-`0.0` currVol the user
  photographed. The buggy code carried a CONFIDENT comment reasoning about override-vs-layout hazards —
  sound about the wrong hazard, and it hid this one. Blast radius is NOT all 414 call sites: the
  dominant idiom (`FindClass("SceneComponent") -> K2_GetComponentLocation`) is correct by construction;
  the risk class is the **19 sites passing `ClassOf(instance)`** plus sites naming a BP class for an
  inherited function (`door_probe.cpp:81` `SetActorTickEnabled` = a second near-certain inert case).
  **AUDITED 2026-07-22 -- the risk class was swept and is CLEAN, but the PREDICATE was wrong.** All 19
  `ClassOf(instance)` sites are SAFE (library CDOs calling their own function, or instances whose exact
  class declares the verb). The one dead resolve in the batch — `coop/dev/door_probe.cpp:81`
  `SetActorTickEnabled` on `door_C`, declared on `AActor` — passes `FindClass(L"door_C")`, **not**
  `ClassOf(instance)`, so the `ClassOf(` filter would have missed the only corpse. The real predicate is
  **"a LEAF class resolving a BASE-declared function, however that class was obtained"**. It is never
  invoked, so nothing breaks; the damage is an instrument printing `setTick=0000000000000000`
  unremarked. Eight cached-BP-class shipping sites re-checked on the corrected predicate: all SAFE.
  Latent, and NOT one item — split by consequence, not mechanism: **`spawn_menu.cpp:130`/`:165` are
  LOAD-BEARING** (they gate the input-mode restore the file itself calls "THE LOAD-BEARING UN-STICK";
  a wordless death after a recook traps the player's input in `GameAndUI`), while
  `save_browser.cpp:188` is a deliberate fail-open whose worst case is a mis-listed save name.
  **The audit closed the RESOLVE axis only — that a call LANDS (ParamFrame, param names,
  `EX_*`-invisible dispatch) was never checked.** **Look here FIRST:** the ownership authority is
  `Game_0.9.0n_HOST/.../Win64/CXXHeaderDump/*.hpp` (2645 files, each block = only that class's OWN
  functions). Check which class DECLARES the function before writing any `FindFunction`, filter on
  leaf-vs-base rather than on the call shape, and always LOG a failed resolve.
  `memory/lesson_findfunction_does_not_walk_the_superclass_chain.md`

- **Container CONTENTS live in ONE global `saveSlot_C::GObjStack`, never on the container** — every
  container in the game (world props, backpacks, the drone delivery container, AND `mainPlayer`'s personal
  inventory) reads its contents from that single `TArray<struct_mObject>`, addressed by
  `propInventory_C.index` (an `Array_Add` append position, guarded by `index >= 0`, persisted via the
  owner's `struct_save.ints[]`). `struct_mObject = {obj: TArray<struct_save>}`; each entry is a FULL
  generic save-record (`class`/`transform`/`key` + 10 uniform jagged primitive arrays + `signals[]`), so a
  wire codec is ONE generic serializer + the existing `coop::signal_wire` — no per-class codec. Three traps:
  (1) **asymmetric authority** — the player's personal inventory shares the array, discriminator is
  `propInventory_C.player`; a blind host-authored write wipes client inventories; (2) **dispatch** — every
  mutating verb (`addObject`/`takeObj`/`addLoot`) is `EX_LocalVirtualFunction`, invisible to BOTH the
  ProcessEvent detour and the Func patch → `vm_dispatch` 0x45 is the only seam; (3) **nesting by
  indirection** — a nested container's record carries a `GObjStack` INDEX, so a flat copy ships a pointer
  into the sender's array (silent corruption, not an honest empty); detect via
  `WalksToBase(cls, prop_container_C)` `[RD]`. *Look FIRST:*
  `research/findings/inventory-items/votv-container-contents-gobjstack-RE-2026-07-22.md` §8 + §10.
  `memory/lesson_container_contents_live_in_one_global_gobjstack.md`
- **A container take RE-CREATES the record — identity does NOT survive the transfer** (2026-07-24,
  33-statement decode + 2-run live confirm). `Aprop_container_C::extract`: `takeObj` yields the source
  record → `BeginDeferredActorSpawnFromClass` takes **only its CLASS** (the deferred window applies NO
  properties — it is all pawn-transform math) → `FinishSpawningActor` → `putObjectInventory2` →
  `addObject` → **`getData` CAPTURES the freshly-spawned carrier** → `K2_DestroyActor` → and only THEN
  `loadData(takeObj_Output)`. **Mint at spawn, captured at add.** Live: across two runs of one save the
  four save-loaded items kept byte-identical keys while the one taken item got a new key each run
  (matching the destroy-seam line for the carrier). **DIRECTION-SPECIFIC** — the player-container
  override is the same function minus exactly two statements (`putObjectInventory2` +
  `K2_DestroyActor`), so its `loadData` DOES restore a live actor; never state this about `extract` in
  general. *Consequence:* any custody/anti-dup design must contend over the **SOURCE record's key**,
  which exists in the container's `GObjStack` slot before any spawn — the destination key is downstream
  of the contention. *Does NOT mean the item lands empty* (predicted, then FALSIFIED: `{b5,f3,nm2}`);
  whether the saved VALUES survive is still OPEN. *Look FIRST:*
  `research/findings/inventory-items/votv-player-inventory-two-layer-RE-2026-07-24.md` §3.2a/§3.2b.
  `memory/lesson_container_take_recreates_the_record.md`

- **A PLACED actor's cross-peer identity = its BAKED level-export FName, NOT a gamemode-assigned save
  Key.** VOTV keys `AtriggerBase_C` descendants (doors/garage) via a ONE-SHOT, sublevel-gated gamemode
  pass (`mainGamemode::loadObjects` → `GetAllActorsWithInterface` + `loadTriggers`, gated by
  `isSublevelAllowed` — kismet-analyzer bytecode); an actor mid-recycle at that instant stays `Key=None`
  and is dropped by any None-key filter FOREVER. The export FName is serialized in the cooked package →
  identical on both peers by construction + present regardless of keying. take-4 R9: host garage index
  1→0 (unkeyed) through a menu→save reload while 50 same-keyed doors survived; `door_box` FName identity
  (same `untitled_1` package) came through the SAME reload byte-identical cross-peer. Fix (v123): mirror
  `door_box::GetNameKey`, delete the Key path (RULE 2). Look FIRST: `ue_wrap::garage::GetNameKey` vs
  `ue_wrap::door_box::GetNameKey`; do NOT broadly migrate working Key channels (principle 4).
  `memory/lesson_placed_actor_identity_use_baked_fname_not_gamemode_key.md`
- **A SAVE-LOADED prop's runtime FName is NOT cross-peer stable — key it by its persistent SAVE KEY.**
  The baked-FName rule above covers LEVEL sublevel exports (`door_box`, serialized into the cooked
  `.umap`, identical on every peer). A **save-loaded** prop (a container, an item — spawned at world-load
  by `loadObjects`, NOT baked into the level) gets its FName *Number* from **spawn ORDER at load**, which
  differs between the host's load and a joining client's save-transfer load: measured 2026-07-23, the same
  file cabinet was `prop_container_..._2147472736` (host) vs `..._2147471758` (client). Selecting a
  shared/synced target by FName thus picks DIFFERENT actors per peer. The stable-by-construction key is the
  prop's **persistent SAVE KEY** (the FName the game writes into the save), via
  `coop::prop_element_tracker::CollectKeyIndexEntries → {actor, key}`; post-s22 its keyed eid is
  host-authoritative. This **overturned /qf R11-Q3** ("shared target by baked FName" — its premise was
  false for containers). The fallback of keying by WORLD POSITION is the `RULE-1 hope` /qf R2-Q2 rejected
  (a property of the current save's geometry, not an invariant); nav-reachability is a test-feasibility
  filter, not the identity. Validated: two peers picked the SAME container by save-key + the cross-peer
  count summed to 1. *Look FIRST:* level-baked (baked FName ok) vs save-loaded (use the save KEY) before
  keying any cross-peer decision on a prop. `memory/lesson_save_loaded_prop_fname_unstable_use_save_key.md`
- **NEVER raw-write a UE field the game sets via a setter UFunction** — call the setter. `memory/feedback_no_raw_write_of_setter_managed_fields.md`
- **UE `TArray<struct>` stride = 16-ALIGNED size, NOT the raw `Size:`.** `memory/feedback_tarray_stride_aligned_not_raw_size.md`
- **plain `IsLive` passes a RECYCLED slot** — cached instances need `IsLiveByIndex`. A written lesson is NOT
  proof its enumeration was run: this lesson named `daynightcycle.cpp Cycle()` "good" but the sweep was never
  done → `weather_sync.cpp ResolveCycle` (setRainParticles crash, exit-to-menu 07-15) + TWO more
  (`world_actor_sync.cpp:380` OnDisconnect drain + `world_actor_mirror.cpp:208` OnDestroy — K2 on a cached
  mirror actor) all slipped it. Re-run the grep for real (`\bIsLive\s*\(` minus fresh/same-frame + autotest,
  keep cached-ptr + UFunction-CALL + teardown-reachable); all fixed 07-15.
  `memory/lesson_islive_recycled_slot_blind_use_by_index.md`
- **A runtime-spawned `AStaticMeshActor` is STATIC mobility** → set Movable BEFORE `SetActorLocation` (a
  Static root silently no-ops the teleport). `memory/lesson_runtime_staticmeshactor_must_be_movable.md`
- **SEH shields must NEVER absorb `0xC00000FD`** (stack overflow). `memory/lesson_never_absorb_stack_overflow.md`
- **nlohmann JSON: an ITERATIVE parser can still crash on its RECURSIVE `~basic_json` destructor** —
  deeply-nested untrusted JSON (within any byte cap) parses fine, then overflows the thread stack on
  scope-exit destruction; the SEH `0xC00000FD` is NOT caught by C++ `try/catch`. A hostile/MITM master
  crashed every client (fixed `7e8b1d2c`: depth-32 cap via the parse callback in
  `json_util.h::ParseObject`). *Look FIRST:* any parse of UNTRUSTED JSON must cap depth at parse — never
  rely on the byte cap / iterative parser / try-catch. `memory/lesson_nlohmann_deep_nesting_recursive_destructor_crash.md`
- **A bare proxy can NEVER be `lookAtActor`** — use a camera-ray cone. `memory/lesson_proxy_never_lookatactor_use_camera_cone.md`
- **`serverBox_C.check()` re-skins PURELY from raw `IsBroken@0x378` (never `damaged`)** — notify-free, so a
  visible break mirror = raw-write IsBroken + reflected `check()`. Offsets (CXXHeaderDump): servers@0x3F0 /
  brokenServers@0x8A0 / eff@0x400/0x404. **A base runs ~54 serverBoxes** (a farm, not a handful) — never
  assume a small fixed count; a 32-cap dropped 22 (smoke-caught). `memory/lesson_serverbox_check_reskins_from_isbroken.md`

- **VOTV `.sav` = uncompressed GVAS serialized DELTA-VS-CDO** — an absent property means "CDO default"
  (Points=10, health/maxHealth=100, Version=""); row metadata is readable OFF-THREAD via a tag-walk that
  seeks past payloads (`ue_wrap/gvas_meta`); never drive `LoadGameFromSlot` N times on the game thread
  for display data (the 2026-07-11 picker freeze). `b_` = the SANDBOX prefix, not a backup marker.
  `memory/lesson_gvas_savefile_delta_vs_cdo.md`

- **Injecting a native-parity UMG menu button = 5 gotchas** — (1) the style clone-source `tex_btnStart` is
  NULL at inject time → cloning silently falls back to Roboto/Center/white; set font/colour/justify
  DETERMINISTICALLY. (2) A spawned `UButtonSlot` (content slot) defaults to `HAlign_Center` → indented;
  set `HAlign_Fill(0)`+zero padding after SetContent (UMG.hpp:314-318; `Fill=0/Left=1/Center=2`). (3) An
  external-poll click on a real UButton must fire on the RELEASE edge (down-edge → overlay swallows the
  UP → button stuck DOWN). (4) Keep FSlateSound `ResourceObject`(0x00), zero ONLY the trailing TSharedPtr
  cache(0x08) → native `buttonclick`/`buttonrollover` play without aliasing. (5) Play VOTV sounds via
  `PlaySoundAtLocation` (null att = 2D) so the game's SoundClass/mix apply; the menu's press bg-dim is the
  submenu/loadLevel fade, NOT a per-button style (replicate with a modal ImGui backdrop).
  `memory/lesson_umg_injected_menu_button_native_parity.md`

- **UMG runtime injection = 3 traps (native version label, 2026-07-16)** — (1) raw property writes work
  ONLY pre-Slate-attach; after `AddChildTo*`, UMG has baked props into Slate, so changes MUST be setter
  UFunction dispatches (`SetColorAndOpacity` etc. — a raw write silently doesn't repaint; the "no cyan"
  bug). (2) The insert-at-top reorder (snapshot→ClearChildren→re-add) DESTROYS every slot and creates
  DEFAULTS — save each child's slot layout region before Clear + restore onto the RE-READ new slot; never
  reuse a pre-reorder slot pointer (`InsertAtTopOfVBox`, engine_widget.cpp). (3) Never assume the parent
  panel type — resolve the target's slot chain in `research/bp_reflection/<widget>_fixed.json` first
  (txt_version = a HorizontalBox row in VerticalBox_138, NOT a canvas child; the canvas-API attempt
  rendered inline-RIGHT). Look here FIRST: reuse `InjectTextRowAbove`/`SetTextBlockColorDispatch`.
  `memory/lesson_umg_runtime_inject_traps.md`

- **The literal string "None" trips WriteFNameField's failed-intern check** (StringToFName("None") ==
  {0,0} == NAME_None, indistinguishable from a failed intern; ReadStruct renders NAME_None back AS
  "None" -> string round-trips asymmetrically fail). Express NAME_None with the EMPTY string. Cost a
  3-smoke dig (v120 selftest). LOOK FIRST: signal_dynamic.cpp WriteFNameField.
  `memory/lesson_none_string_trips_fname_intern_check.md`

## 6. Assets, models, geometry

- **Curating GAME assets = census EVERY asset** — games ship broken leftovers. `memory/lesson_game_asset_census_before_curation.md`
- **`mainPlayer_C` renders TWO overlapping bodies — apply mesh to BOTH slots.** `memory/lesson_attachparent_visibility_two_body.md`
- **Cooked UE meshes store CW-outward winding — MEASURE + match** (signed volume). `memory/lesson_winding_match_template_signed_volume.md`
- **Porting SCS templates: copy behavior flags BIT-EXACTLY** (dormancy). `memory/lesson_template_faithful_scs_dormancy.md`
- **Anim nodes INSIDE a state contribute NOTHING when it exits** (post-BUA seam). `memory/lesson_animbp_state_hosted_nodes_post_bua_seam.md`
- **A learned per-bone profile is exact ONLY on its source skeleton — MEASURE fit.** `memory/lesson_converter_fit_measured_not_assumed.md`
- **NEVER strip geometry from a shipped model on geometric heuristics** (need visual proof). `memory/lesson_never_strip_shipped_geometry_without_visual_proof.md`
- **VOTV's own fonts:** `FSEX300` = Fixedsys Excelsior (font_terminal, pixel); `ShareTechMono` = font_ui
  (subtitles, Latin-only subset). `memory/reference_votv_fonts.md`
- **"Unsupported text just shows boxes" is FALSE.** ImGui returns ONE `FallbackGlyph` for EVERY
  absent codepoint (`imgui_draw.cpp:3699-3712`, chosen from `{U+FFFD,'?',' '}`), so two DIFFERENT
  unrenderable strings render IDENTICALLY. **CORRECTED 2026-07-28:** this row also claimed a cmap
  sweep found U+FFFD in `FSEX300` ONLY — that is FALSE, **all seven faces carry it** in their (3,1)
  subtable (the original sweep must have read Roboto's MacRoman table, where it is genuinely absent).
  The fallback really was `'?'`, for a different reason: no glyph RANGE ever asked the atlas for
  U+FFFD, and the builder bakes only what a range names. See the next row. *Look FIRST* before
  designing any graceful-degradation behaviour: `memory/lesson_imgui_missing_glyphs_collapse_to_one_fallback.md`

- **Presence in a FONT is not presence in an ATLAS — a fallback glyph must be ASKED FOR.** Measured
  2026-07-28: all seven embedded faces have U+FFFD, `ImFont::BuildLookupTable` (`imgui_draw.cpp:3700`)
  picks the fallback from `{U+FFFD,'?',' '}` among **baked** glyphs, and
  `GetGlyphRangesCyrillic()` (`:3525`) stops at U+A69F — so nothing ever requested it and the fallback
  fell to `'?'`. The arc-D2 design had asserted that cross-merging the families would supply it;
  merging cannot add a codepoint no range names. **The fix is one range entry.** Corollary that bit
  immediately: once U+FFFD is baked it is a normal character a name may contain, so the nickname fold's
  sentinel had to BECOME U+FFFD — a U+FFFF sentinel would leave `"中"` and `"�"` with different keys
  and identical pixels, the same defect one level down. *Look FIRST:* for any "the font supports X"
  claim about a rendered surface, check the RANGE passed to `AddFont*`, not the cmap — only their
  intersection renders; and assert a design's claimed SIDE EFFECTS directly, because a side effect
  nobody asked the code for is the claim no reviewer checks.
  `memory/lesson_a_fallback_glyph_must_be_asked_for.md`

- **In an ImGui atlas the bill is the MAXIMUM codepoint, not the COUNT.**
  `ImFont::GrowIndex(max_codepoint + 1)` (`imgui_draw.cpp:3669`) allocates `IndexAdvanceX` +
  `IndexLookup` as dense arrays indexed by codepoint — 8 bytes an entry under `IMGUI_USE_WCHAR32`,
  **per deduped face**. Twemoji's cmap ends at U+E007F with ten TAG characters (subdivision-flag
  spelling, uncomposable without shaping); baking them moved the max from U+1FAF6 and the tables from
  **1.04 MB to 7.34 MB per face** — 22.0 MB instead of 3.1 MB on the default config, 36.7 vs 5.2 on the
  worst. Nineteen megabytes for ten codepoints that can only draw as the fallback box. The whole
  Unicode `Default_Ignorable_Code_Point` set is now subtracted from the baked repertoire. *Look FIRST:*
  print `max(cmap)` as well as `len(cmap)` before merging any donor, and for any
  dense-array-indexed-by-key structure ask whether the cost follows the key's RANGE or its COUNT.
  `memory/lesson_the_highest_baked_codepoint_prices_the_whole_atlas.md`
- **Astral text (emoji, CJK ext) is gated by a vendored DEFINE, not by fonts.** `imconfig.h:65` keeps
  `IMGUI_USE_WCHAR32` commented → `ImWchar` 16-bit, `IM_UNICODE_CODEPOINT_MAX 0xFFFF` (`imgui.h:2515`),
  a glyph range cannot express U+1F300, and `imgui.cpp:1512` DROPS the reassembled astral codepoint on
  input. Same family of silent capability bounds: `FT_DISABLE_PNG ON` kills CBDT colour-emoji donors,
  `FT_DISABLE_HARFBUZZ ON` + no ImGui shaping kills ZWJ/skin-tone/FLAG composition. **And you cannot
  edit the file you just read** (2026-07-28): `third_party/imgui` is a git SUBMODULE, so the switch must
  ride a `PUBLIC` compile definition on the `imgui` target as `IMGUI_ENABLE_FREETYPE` does
  (`CMakeLists.txt:106`). It is **not optional for emoji** — 1,232 of Twemoji's 1,418 codepoints are
  astral, so a BMP-only build has no U+1F600 — and it costs **2.94-4.90 MB of permanent host RAM** once
  ONE astral glyph is baked (`GrowIndex(max_codepoint+1)`, `imgui_draw.cpp:3669`; per-PRESENCE, and the
  merged font's cmap ceiling does not leak into it). *Look FIRST — read the defines before pricing
  fonts:* `memory/lesson_imgui_astral_codepoints_need_wchar32.md`
- **Uniqueness enforced on STRINGS is not uniqueness on SCREEN.** Measured 2026-07-28: the nickname
  arbiter suffixes on fold-key equality over `std::wstring`, while `ImFont::FindGlyph`
  (`imgui_draw.cpp:3830-3838`) returns ONE `FallbackGlyph` for EVERY absent codepoint. So 张伟 and 李明
  have distinct keys, get **no suffix**, and draw as the **same nameplate** — the user's literal ask
  ("everyone has a unique nameplate") was false on screen for exactly the players the feature was being
  extended to serve, while every selftest and log reported success. Buying glyphs shrinks the broken set
  but can never close it (Hangul, Thai, rare hanzi always sit outside any budget), so a coverage-based
  guarantee is **budget-shaped**. The fix is a LAYER choice: fold every out-of-repertoire codepoint to
  ONE sentinel in the AUTHORITY, so names that render alike collide and take the suffix that already
  ships — uniqueness becomes font-independent and the donor set demotes to a legibility knob. Also
  recorded there: escaping per codepoint just moves distinctness into the LAYOUT, and folding against
  the live atlas would make one machine's font install the authority for everyone's name. *Look FIRST:*
  when a guarantee is about what a HUMAN PERCEIVES, ask "what renders identically that my key treats as
  different?" and name the layer that can make it TOTAL.
  `memory/lesson_uniqueness_on_strings_is_not_uniqueness_on_screen.md`

## 7. Performance

- **`GetActorLocation`/`GetComponentLocation` are UFunction DISPATCHES, not raw reads** — never bulk-call
  per-tick over thousands of actors (invisible on a fresh save, hitches the host on a mature world);
  throttle / pre-filter / read the raw transform. *Look FIRST:* `engine.cpp GetActorLocation`. `memory/lesson_getactorlocation_is_a_ufunction_dispatch.md`
- **Per-tick `GUObjectArray` walk: cheap class check BEFORE `NameOf`.** COROLLARY (v114): a
  class resolver reachable from another module's hot path (savedScalar reader at every PropSpawn
  express) carries its negative-result backoff INSIDE itself — call-site throttles don't survive
  new callers. `memory/lesson_full_array_walk_cheap_filter_before_nameof.md`
- **A periodic FPS hitch by PERIOD COINCIDENCE is not causation** — measure the real source. `memory/lesson_periodic_hitch_not_the_walk_by_period_coincidence.md`
- **A fixed-capacity hook table + ASYMMETRIC roles = a half-working fix.** `memory/lesson_hook_table_capacity_asymmetric_peers.md`
- **ImGui COMPOSITE widgets: commit via a DEBOUNCE on value-changed.** `memory/lesson_imgui_composite_commit_debounce.md`
- **`coop::subsystems::Install` is a per-tick RETRY PUMP, not boot code** — `net_pump.cpp:720` calls it
  every pump tick (and `session_runtime.cpp:648` when idle in gameplay) so unresolved modules retry;
  its own comment says "One-shot install ... (idempotent)" but the idempotency is each MODULE's job.
  A module added without its own latch fires ~57x/SECOND (measured: 14,095 identical "armed" lines in
  4 minutes, vs exactly 1 for every latched neighbour). A perf-audit agent classified the same call
  site COLD/boot from its location. *Look FIRST:* `net_pump.cpp:720`; grade a smoke with `grep -c`, not
  `grep`. `memory/lesson_subsystems_install_is_a_per_tick_retry_pump.md`
- **The FString PIN doctrine ("mint engine-side, never free") holds ONLY for FRESH buffers** —
  repeated in-place mints on the SAME live object's fields LEAK on receivers (no native reassign ever
  runs there); swap-and-EngineFree instead (v116 perf audit finding 1). *Look FIRST:*
  `ue_wrap/devices/laptop.cpp FreeFStringSlot`. `memory/lesson_fstring_pin_doctrine_fresh_buffers_only.md`

## 8. Build / deploy / git hygiene

- **A CRT `_s` "safe" variant can be the DANGEROUS one inside an injected DLL.** Measured 2026-07-28:
  `_vsnprintf_s_l` routes a malformed conversion specifier (`%q`) to the CRT invalid-parameter handler,
  which raises `__fastfail` — the probe **terminated, exit 127**, where plain `_snprintf_l` printed
  `bad q here` and carried on. `__fastfail` is not an SEH exception, so `RenderFrameGuarded`'s `__try`
  and every per-callback wrapper in this mod are structurally unable to contain it, and a logging typo
  could kill a player's game. Compounding: no `_set_invalid_parameter_handler` exists in the tree, and
  `log.h`'s `Write` has no `_Printf_format_string_` SAL annotation, so MSVC never checks a format
  string against its arguments. Note the shape — **the compiler's own C4996 tells you to make this
  change.** *Look FIRST:* read any `_s` variant as "what does it do INSTEAD of the bad thing?" — if the
  answer is `__fastfail`/`abort`, it converts a local defect into a crash of a process we do not own.
  Suppress C4996 at the call site with the reason.
  `memory/lesson_a_safer_crt_variant_can_be_the_dangerous_one.md`

- **`LC_ALL` is not "the UTF-8 one" — a locale is wider than the conversion you wanted.** Measured
  2026-07-28: `_create_locale(LC_ALL, ".UTF-8")` was reached for to fix `%ls` and also moved
  `LC_NUMERIC`, so on this ru-RU machine every `%f` in the log became `1,50` — 301 `UE_LOG*` sites
  carry a float and `mp.py`/`coverage.py`/`roster_shot.ps1` parse those numbers, so two testers on
  different Windows languages would produce logs that no longer diff. `".UTF-8"` names only a CODE
  PAGE; language/country come from the OS user default for every category `LC_ALL` covers.
  **`LC_CTYPE` alone fixes `%ls` identically** (measured `n=9` both ways) and leaves numerics at `"C"`.
  Two independent audit agents flagged it, neither asked about locales — after the change had already
  passed a build, four selftests and a 4-peer smoke. *Look FIRST:* name the narrowest locale CATEGORY
  that does the job, and diff a REAL artifact before/after rather than only the case you were fixing.
  `memory/lesson_a_locale_is_wider_than_the_conversion_you_wanted.md`

- **Deletable platform objects (git tags, GitHub releases) cannot hold an append-only invariant** — a
  yanked tag silently frees its "consumed" build number for different bytes; record consumption in an
  append-only LEDGER file on the protected branch (repo's own history = the only mechanically
  append-only store), demote tags/releases to drift detectors, and record the negative states
  (burn/retracted) as rows — absence must never encode a state the invariant distinguishes.
  Sharpened 2026-07-25 R17-21: record the positive closure too (`published` row); TERMINAL rows are
  PUSH-IMMEDIATE (until on origin the invariant rides the deletable API); ledger owns "MAY publish",
  the release object owns "did THIS tag complete" (own work product, not a gate). Look FIRST:
  `research/findings/tooling/votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md` §3 D3.
  `memory/lesson_deletable_platform_objects_cannot_hold_append_only_invariants.md`
- **A fetch-push of untrusted content EXECUTES its workflows** — mirroring a fork PR branch into the
  base repo is itself a push event: a contributor-added `on: push` workflow runs at that instant with
  base-repo token capabilities (an explicit `permissions:` key elevates past the read-only default,
  measured). Gate the PUSH, not a later dispatch: sanitize-by-default (mirror script replaces
  `.github/workflows/` with main's copy; explicit -KeepWorkflows after line-by-line review). Also
  measured verbatim: a run executes the EVENT commit's YAML; workflow_dispatch requires the file on
  the DEFAULT branch. Look FIRST: the CI design doc §3 D1 + §2.
  `memory/lesson_fetch_push_of_untrusted_content_executes_its_workflows.md`
- **Drills must run the REAL gate on REAL identifiers** — a fake test namespace (build numbers
  b9000+) violated the gate's own preconditions (proto==N unreachable), so every drill refused on
  the wrong branch and masked the branch under test; MUST-PASS drills were impossible; the terminal
  `drill` row class made the publish drill self-refuse. Fix: real numbers via the real ritual +
  a NO-short-circuit labeled verdict vector (each drill asserts its NAMED line; a fused guard makes
  a drill indiscriminate — the robot-tag drill had to declare contents:write to reach the ruleset
  under test) + stated-and-CHECKED preconditions + a positive control for zero-assertions + a
  cleanup step whose misses fail closed. Look FIRST: the CI design doc §4 + §3 D3. (Validated
  2026-07-25: the executed matrix caught a REAL workflow bug on its first campaign — the no-op
  exit-code fall-through below.) `memory/lesson_drills_must_run_the_real_gate_on_real_identifiers.md`
- **A GH Actions pwsh step exits with the last CHILD's `$LASTEXITCODE`** — even after your script
  HANDLED that code in a switch and fell through to script end (the Actions shell wrapper propagates
  it). A special-exit-code protocol (judge exit 10 = ALREADY_PUBLISHED no-op) needs an EXPLICIT
  `exit 0` on every mapped branch, else the designed-green path runs red and every `needs:`-dependent
  job silently skips. Caught live by the double-dispatch drill; fixed `e4c5e503`. *Look FIRST:*
  `.github/workflows/release-core.yml` judge step.
  `memory/lesson_gha_pwsh_step_exits_with_last_child_code.md`
- **PowerShell defaults are case-INSENSITIVE everywhere** (`-match`, `-eq`, `-contains`,
  `-notcontains`, AND hashtable keys) — three separate instruments bitten in one day (2026-07-25):
  the tag fixture caught `-DEV` matching; the verdict-diff `@{}` collapsed `Player_Guid`/`player_guid`
  into a FALSE product alarm; `-notcontains` dropped case-twin keys. Instruments over case-sensitive
  artifacts use `-cmatch`/`-cnotmatch`/`-ceq` + `Dictionary(StringComparer.Ordinal)`. *Look FIRST:*
  `tools/release/tag_regex_selftest.ps1` (the fixture shape that catches it cheaply).
  `memory/lesson_powershell_defaults_are_case_insensitive_everywhere.md`
- **PowerShell UNWRAPS a one-element result — wrap it in `@()` at the CALL SITE** (the `@()` inside the
  helper does not survive the return). Bit two instruments in one hour (2026-07-27): on 5.1 a bare
  `[pscustomobject]` has NO `.Count` (yields `$null`, pwsh 7 yields 1) so `peerconn_gate.ps1` accused
  its own detectors; and `$a + $b` on two bare `MatchInfo` throws `op_Addition`, which killed
  `replacement_drill.ps1`'s grading pass AFTER its `finally` had torn down the peers being graded.
  Corollary: put teardown AFTER grading, or make grading re-runnable off the logs on disk.
  `memory/lesson_powershell_unwraps_one_element_results.md`
- **Ruleset `update` restriction on main: direct pushes AUTO-bypass, PR merges need `--admin`** —
  with RepositoryRole-admin bypass_mode=always, `git push` prints "Bypassed rule violations" and
  sails; `gh pr merge` refuses ("base branch policy prohibits the merge") until `--admin`. So the
  robot-blocking push restriction costs the daily direct-push flow NOTHING; budget `--admin` per rare
  PR. Measured live on ruleset 19728708. 2nd surface (2026-07-25, the b126-dev push): the v-tags
  creation restriction prints "Cannot create ref due to creations being restricted" yet the SAME
  push output shows `* [new tag]` — the scary prose is the rule-evaluation notice; trust the
  ref-update lines. *Look FIRST:* `docs/RELEASE.md` invariants + CI design D7.
  `memory/lesson_ruleset_update_restriction_pushes_bypass_merges_need_admin.md`
- **PS comma binds TIGHTER than `+`: a concat inside an array literal silently array-appends** —
  `@("a", "b" + $c, "d")` parses as `(("a","b") + $c), "d"`: the intended one string becomes TWO
  elements, and a fixture writer emits a silently split line (bit the arc-2 ini corpus builder,
  2026-07-25; minimal repro same day). Parenthesize `("b" + $c)` or interpolate `"b$c"`.
  *Look FIRST:* any `.ps1` building a line list with `+` inside `@( )`.
  `memory/lesson_ps_comma_binds_tighter_than_plus_in_array_literals.md`
- **An aborted batch-edit script has ALREADY mutated the tree; its re-run's "0 changes" lies** — a
  mid-walk crash (non-utf8 third_party header) left the arc-3 C3a sweep FULLY applied while the
  hardened re-run printed `files: 0`, reading as "never ran" (2026-07-25). The runner's counters
  describe THE RUN, not the tree. Exclude vendored dirs up front; verify by residual-grep of the OLD
  pattern + opening one known site; treat "0 changes" after an aborted run as suspicious.
  *Look FIRST:* any os.walk/-Recurse mutator over src/ — its dir-exclusion list, then `git diff --stat`.
  `memory/lesson_aborted_batch_edit_already_mutated_verify_by_site.md`
- **`deploy-all.ps1` deploys Release** → ALWAYS build Release + hash-verify. `memory/lesson_deploy_sources_release_config_not_relwithdebinfo.md`
- **Filtered tool output HIDES verdicts — twice-bitten:** s22 a grep+tail filter ate a LINK error (a
  STALE DLL deployed; the SHA-256 build-vs-deployed compare caught it), s23 `smoke | tail -4` cut the
  `--- VERDICT ---` line on a run that was genuinely ABNORMAL (host log restarted, no verdict printed)
  — the truncation masked an invalid run. Verdict-bearing commands pipe wide (`tail -40`+) or
  unfiltered; a MISSING expected verdict/marker = INVALID RUN (rerun), never "probably fine".
  `memory/lesson_filtered_tool_output_hides_verdicts.md`
- **A "pure refactor" claim becomes a MEASUREMENT via the three-commit shape: dedups first, then a
  FROZEN standalone instrument (dev TU over public APIs — the refactor commit physically can't touch
  it) + digest BASELINE x2 on the UNSPLIT code, then the move + same scenario → digests byte-equal
  cross-peer AND cross-commit** (+ literal git-diff of moved bodies, symbol-level negative grep, a
  reconnect cycle for the connect/prime/teardown surface). Digest = content-only (proven
  eid-independent). Born: the rack extraction `73dc9ba1` (2026-07-18); executed again for
  session_streams `06921557` + the net_pump decomposition `de249463` (both 2026-07-18/19) with the
  NONDETERMINISTIC-surface variant: live streams admit no content digest, so the package = literal
  stripped-line body diff (known-positive script) + a MUTANT-PROVEN live matrix (a routeSlot/peerSlot
  swap FAILED the 4-peer cross-peer verdict; a 2-peer smoke structurally cannot see relay routing) +
  the adjacent frozen digest; for a mega-FUNCTION decomposition add the bool-return early-return
  preservation + shared-local/atomic observation-point enumeration + the caller-sweep single-token
  verifier. RENAME-vs-DISSOLVE variant (s26 autotest `f299107c`+`cc4c93c3`): a fused mv+strip lands
  under git's 50% rename-similarity threshold (grab residual ~41%) → `--follow` history silently
  severed; extract FIRST (same filename shrinks in place), then a PURE `git mv` as its own commit
  (99-100% detected) — verify with `git log --follow` before calling it done.
  SPAN-EDGE variant (s27 vitals, audit-caught `de304643`): a per-TU scaffold span can drag NEIGHBOR
  comment lines across a function boundary, and the instrument's permissive "//"-prefix allowance is
  BLIND to it — verify comment-block ownership at every span edge; the closing audit covers that
  blind spot.
  INSTRUMENT-BLIND-SPOT variant (s28 puppet `ca12e11d`, mutate-caught): a scaffold WHITELIST catches
  ADDED lines but is structurally blind to DROPPED lines — the m6 deletion mutate (internal.h decl
  drop) PASSED it; small seam/generated files get an EXACT-CONTENT sequence compare, and the mutate
  battery includes a DELETION mutate per checked file (the mutates test the INSTRUMENT, not just the
  cut). Also: span header-census greps must cover UNQUALIFIED name forms (`Call(` vs `call::` — the
  qualified-only pattern shipped a missing include the build caught).
  *Look FIRST:* `votv-s27-three-cuts-DESIGN-2026-07-19.md`; `votv-rack-extraction-DESIGN-2026-07-18.md` §4-5+§8;
  `votv-session-streams-extraction-DESIGN-2026-07-18.md`;
  `votv-netpump-decomposition-DESIGN-2026-07-18.md`; `votv-autotest-dissolve-DESIGN-2026-07-19.md`;
  `coop/dev/drive_selftest.cpp`.
  **THE TIMING HALF (2026-07-22): equivalence proves "after == before", NEVER "before was correct."**
  The whole recipe is silent on whether the behaviour was worth preserving — and that silence is
  dangerous precisely because the instruments come back GREEN. Measured: `container_contents_sync.cpp`
  at 853 LOC (soft cap 800) looked like ideal no-PC extraction work, but its arbitration compares the
  hash of the WHOLE container, and the already-approved fix for that rewrites `HostAcceptsClientWrite`,
  `g_publishedHash`, `g_baseHash` and the blob format together. Extracting first would freeze an interim
  shape and rewrite the new file wholesale a step later, while a byte-equal body-diff certified the move
  as faithful — faithful to a shape about to die. **Rule:** before extracting, check whether an OPEN
  measurement or an approved change targets the code being moved; if so the extraction WAITS. Over the
  soft cap is an audit flag, not a blocker (soft 800, hard 1500). *Look FIRST:* the open-thread ledger's
  gate column — if a still-open row names the code you are moving, the shape is provisional.
  `memory/lesson_refactor_equivalence_frozen_digest_instrument.md`
- **A positional resolve table makes a mid-row removal SILENTLY corrupting — and BOTH the literal-diff
  instrument AND the compiler are blind to a missed index shift** (2026-07-19 comp_pane /qf R1: an
  unshifted `FieldPtr(d, 7)` line is an exact HEAD match to a set-diff AND still compiles, so it reads
  the WRONG FIELD with zero signals; a fresh-world smoke doesn't discriminate it either — zeros both
  ways). Root fix = kill the class before extracting: self-binding `{L"name", &g_offVar}` rows + named
  derefs in their OWN verified commit (`f74d05dc`), correspondence script w/ --mutate known-positive
  (a swapped binding is invisible to ANY runtime dump — C++ can't reflect variable identity; the
  lexical script is the only swap detector). *Look FIRST:*
  `votv-comp-pane-extraction-DESIGN-2026-07-19.md`; `ue_wrap/desk/console_desk.cpp` FieldSlot rows.
  `memory/lesson_positional_resolve_table_silent_shift.md`
- **Any env-gated autotest scenario (`VOTVCOOP_RUN_*`) rides a standard mp.py smoke with ZERO tool
  changes** — set the var in the invoking shell (mp.py copies `os.environ` at launch, mp.py:425; the
  SpawnIf gates live in `harness/autotest/autotest_dispatch.cpp` — helper :22-29 + the
  SpawnEnvGatedTests table; scenarios self-gate by role).
  Gate discipline: import the shipped verifier's LITERAL patterns verbatim (lan-test.ps1 weather verdict
  :440-449), run the gate on a BASELINE run first (pattern counting 0 on baseline = broken instrument),
  min-count FLOORS on periodic diag lines (caught a parallel-audit-shrunk 90s window as 29<30 in s25),
  compare WITHIN-RUN convergence never cross-run absolutes (organic RNG differs; peers move together).
  COMBINING scenarios in one run needs a ONE-WRITER-PER-AXIS census (s26): two scenarios writing the
  same game-state axis (grab+clump on the host held-item; clump+clumpvis on the garbage-clump wire
  lane) give nondeterministic verdicts NOT absorbed by baseline-first — split them across runs; the
  s26 dissolve exercised all 10 routines via two pairs, 36 verdict keys identical.
  *Look FIRST:* `autotest_dispatch.cpp` for the scenario list; lan-test.ps1 for verdict literals.
  `memory/lesson_smoke_env_passthrough_scenarios.md`
- **A NEGATIVE existence claim in a design brief ("no ue_wrap file for X exists") is a measurement, not
  an assumption** — grep for EXISTING wrappers/modules of the target engine class BEFORE deciding an
  extraction/placement axis. Born s25: `ue_wrap/world/daynightcycle.{h,cpp}` (the cycle's CLOCK half)
  existed through a 7-round /qf whose axis argument asserted its absence; the decision survived only
  because the concepts (clock vs weather) don't overlap. Census every wrapper of the class + which
  concept each owns, put it IN the brief. *Look FIRST:* `find src -iname "*<class>*"` + grep ue_wrap/.
  `memory/lesson_axis_decision_census_existing_wrappers.md`
- **env/.bat host = HIDDEN lobby by design; the scoreboard listed-checkbox mirror LIES on that path**
  (2026-07-17: absence from the server browser after a .bat launch is NOT a bug — v56 rule, test
  lobbies must not pollute the list; but `AnnounceEnvHostHidden` bypasses `session_manager::SetListed`
  so `g_listedState` stays true → the checkbox shows ON while hidden; toggle off+on re-lists.
  FIX SHIPPED `2de5ad31` 2026-07-18 — the mirror is seeded in AnnounceEnvHostHidden's success path;
  checkbox visual = a take-4 hands-on item). *Look FIRST:* `session_manager.cpp
  AnnounceEnvHostHidden` vs `HostWithSave`'s mirror seed.
  `memory/lesson_env_host_hidden_listed_mirror.md`
- **A NEW shared box invalidates the provision script's box-#1 assumptions — verify each service from
  OUTSIDE.** Measured on the 2026-07-16 Cloudzy migration: ufw was active default-deny (old box ran
  none) — all services green on-box, ALL dead from the internet; and dual-stack `curl ifconfig.me`
  answered v6 → the master handed unbracketed-IPv6 URIs (`curl -4` fix, `d56a4f69`). *Look FIRST:*
  survey the new box (ufw, `ss -tulnp`, its own port map) + external curl/socket check after provision.
  `memory/lesson_new_shared_box_verify_from_outside.md`
- **Endpoint move: enumerate EVERY config layer — a key ABSENT from an ini silently rides the COMPILED
  default.** 2026-07-16 VPS cutover: HOST's ini had no `[net]` block, CLIENT_3 no ini at all — a
  value-grep found only CLIENT_1/2 and would have left half the installs on the dead box. Also: a
  duplicated default literal with a "keep in sync" comment = drift bomb — alias the ONE definition
  (`cd6faf81`). *Look FIRST:* grep the OLD value repo-wide AND check each install for key-ABSENCE;
  flip `protocol.h` constants in the same change. **Sharpened 2026-07-20 (Tier B arc 1): THE REMOTE
  SERVICE'S OWN ENV IS A CONFIG LAYER.** The s29d sweep still left the master's `COOP_SIGNALING_URL`
  a bare IP, and that value is handed to every client and **overrides** their configured signaling
  URL — so the compiled hostname only served the master-down path while real sessions dialled the IP
  (which can never pass TLS hostname validation). If a field arrives in a response, the *sender's*
  config is one of your layers. `memory/lesson_endpoint_move_enumerate_config_layers.md`
- **A green `certbot renew --dry-run` proves NOTHING about the renewal landing — it skips deploy
  hooks; and `LoadCredential=` is a START-TIME SNAPSHOT.** 2026-07-20: the first dry-run said "all
  simulated renewals succeeded" while the hook never ran (no syslog line, `ActiveEnterTimestamp`
  unmoved); `--run-deploy-hooks` moved it 10:05:10→10:19:43 with all 4 listeners back. Sandboxed
  (`DynamicUser`+`ProtectSystem=strict`) services get the cert copied into `/run/credentials/` at
  START, so a renewed file on disk never reaches a running process without a restart. *Look FIRST:*
  `tools/cert_check.py` — the alarm lives OFF the box and reads the cert the listener actually
  SERVES, which proves renewal+hook+restart+snapshot in one handshake (an on-box log reproduces the
  very blindness it guards). `memory/lesson_certbot_dry_run_skips_deploy_hooks.md`
- **CF-PROXIED root passes only HTTP(S) — custom-port services need the GREY-CLOUD subdomain; an
  IP→hostname flip needs a per-consumer RESOLVER check first.** 2026-07-19 s29d: root `multivoid.dev`
  resolves to Cloudflare proxy IPs (web works, master :10001/:10443 / signaling :10000/:10442 / STUN :3478 would be
  dead — "half-alive" failure); the constants use `master.multivoid.dev` (grey cloud → the box). Flip
  was safe only because both consumers resolve natively (WinHttpConnect http_client.cpp:81; getaddrinfo
  signaling_client.cpp:234) — an `inet_pton`-only consumer would silently fail on a hostname. *Look
  FIRST:* the `kOfficialMasterUrl` comment in protocol.h. `memory/lesson_cf_proxied_root_breaks_custom_ports.md`
- **GitHub org setup is fully scriptable via `gh` EXCEPT repo pins — no API exists** (GraphQL Mutation
  introspection: only pinIssue/pinEnvironment). Pins = web UI only (Overview → "Customize pins",
  owner-only; hidden on mobile layout + behind the new-org onboarding block — use Desktop site). Org
  profile README = the `<org>/.github` repo `profile/README.md`; description/topics/visibility all
  `gh repo edit`. `memory/lesson_github_org_pins_no_api.md`
- **Pre-push leak audit (PUBLIC repo) catches ASSOCIATION leaks, not just secrets; a commit REBUILD
  danglees every doc'd SHA.** 2026-07-16 s13b: the migration commits leaked zero credentials but tied
  both VPS IPs to the other tenants' service names — for a proxy stack that IS the payload; scrubbed +
  commits rebuilt (`d56a4f69`/`cd6faf81`/`c653a538`), which dangled 9 already-written SHA refs across
  docs+memory. *Look FIRST:* `gh repo view --json isPrivate`; grep the diff for service names/hostnames
  near IPs (a leftover hit is OK only as a REMOVAL line: `grep -vE '^[0-9]+:-'` on the hits = empty);
  after any rewrite grep the OLD SHAs across docs/ research/ memory/.
  `memory/feedback_push_leak_audit_service_ties_and_sha_rewrite.md`
- **Git-Bash (MSYS2) MANGLES remote `/abs/paths` → `C:/Program Files/Git/...`** — any argv that looks
  like a POSIX absolute path is Windows-ified BEFORE the child sees it, so `vps.py put <local>
  /opt/x/y` uploads to a REMOTE path literally named `C:/Program Files/Git/opt/x/y` (silent, no error).
  Prefix `MSYS_NO_PATHCONV=1` for ANY remote-host op (ssh/scp/`vps.py run|put`/docker exec) that
  references a Linux path. Symptom: a "successful" op whose target is `C:/Program Files/Git/...`, or a
  Linux box growing a top-level `C:` dir. PowerShell is unaffected. `memory/lesson_msys_no_pathconv_mangles_remote_paths.md`
- **ANY wire-format change bumps `kProtocolVersion`** (new/removed `MsgType`/`ReliableKind`, changed
  payload, changed reliability/cadence) — else two builds differing on the wire connect at the same
  version + silently degrade; the gate (`session.cpp:352-371`) HARD-CLOSEs on a mismatch instead. Caught
  by the `/documentize` sweep 2026-07-15 (clock F added `ClockPose=37` + dropped the reliable periodic on
  v109; bumped to 110). *Look FIRST:* any diff touching `protocol.h` enums/payloads or a `Send*` flag.
  `memory/feedback_wire_format_change_bumps_protocol_version.md`
- **The smoke HOST slot `s_1234` is STATEFUL — restore `coop_backup` FIRST.** `memory/lesson_s1234_host_slot_stateful_coop_backup.md`
- **`multivoid.log` is TRUNCATED at boot (no rotation)** — copy a peer's log to the scratchpad BEFORE any
  mid-run relaunch or the previous life's evidence is destroyed (2026-07-10: an 18-min census slice lost).
  **Idle death claims BOTH peers — ROOT = STARVATION, now keepalive-fixed** (2026-07-10 night: harness
  save starts food=24.4, idle drain ~2.3 food/min, measured by the ticker's own pre-refill log;
  `[dev] vitals_keepalive_sec=180` `0211b9c5` pins vitals -> 65-min continuous run, zero deaths)
  (client ~18 min, HOST ~80 min — the 14:27:16 "LOCAL PLAYER DIED
  role=HOST" real log): a later "connect timed out" against such a host is CORRECT, not a join bug (the
  2026-07-10 "stale-slot race" candidate was exactly this, refuted from the saved logs). Long exposure
  runs must keep peers alive or script around per-peer deaths.
  `memory/lesson_copy_peer_log_before_relaunch.md`

- **OWNER-EFFECT RULE (user, 2026-07-10)** — player-proximity ambient effects (color wisps, fireflies,
  autumn leaves): the local peer KEEPS rolling them (never host-rolled, never suppressed) but a
  cross-peer mirror makes them visible to all peers. Shipped precedent = `coop/world/firefly_sync`
  (v51) — generalize THAT shape, don't invent. Look FIRST: docs/COOP_RNG_AUTHORITY.md USER DECISIONS +
  `memory/feedback_owner_effect_rule.md`

- **A one-shot session-start pass over world state parks/indexes NOTHING — the world materializes
  LATER** (2026-07-10, spawn_authority: initial park pass found 0 instances; the fix is
  hunt-until-first-hit at 1 Hz then relax). Instance #4 of the snapshot-before-state-ready class.
  Look FIRST: `memory/feedback_snapshot_before_state_ready.md`

- **Making a static/absent MIRROR MOVE can WAKE dormant per-peer OUTPUT generation** (2026-07-15, desk
  cursor v109). The jaggy-cursor fix animated the host's coords-panel cursor mirror; the host's native BP
  then began appending LOCAL `MOVE_*` coordLog lines from that motion (silent while the mirror was frozen),
  and `ProduceLogLines` running on "EVERY peer" shipped them → host shipped 78 log lines vs client 13 = a
  NEW divergence the fix created. You test the axis you fixed (cursor = smooth) and miss the downstream axis
  the motion now DRIVES (the log). Rule: after animating any mirror, enumerate every per-peer producer that
  reads the now-moving field (log producers, tick-sims, ship counters) and gate it to the owner / suppress
  the non-owner path. A "mirror the input" change is incomplete until "don't ALSO generate the output
  locally" is done. **2nd instance (v115b `de31889e`): the wake needs NO animation — ONE wire-applied
  BOOL (coord_isPing) started a phantom ping FSM on every observer (latent tick machine, analogd uber
  @82980 → @80105). Before mirroring ANY BP field, classify it: display scalar vs a latent machine's
  RUN-FLAG — run-flags NEVER raw-mirror.** Family of OWNER-EFFECT + mirror-STATE-not-verb. Look FIRST:
  `memory/lesson_smooth_mirror_wakes_dormant_per_peer_generation.md`,
  `memory/project_desk_console_sync_2026-07-15.md`
- **NEVER `git add -A`/`<dir>` over held WIP — explicit paths or stash.** `memory/lesson_never_git_add_A_over_held_wip.md`
- **Held-WIP files inside a tree-wide refactor: commit the MECHANICAL hunks index-side**
  (`git show HEAD:f | rewrite | git hash-object -w | git update-index --cacheinfo` -> status `MM`;
  the WIP semantics stay uncommitted, the committed tree stays self-consistent — the ue_wrap split
  `9d24ac0c`). `memory/lesson_held_wip_index_side_include_commit.md`
- **pwsh7 -> nested Windows PowerShell 5.1 inherits a poisoned PSModulePath** — built-in cmdlets fail
  as "not recognized" (Get-FileHash, 2026-07-17 smoke deploy). Run mp.py/deploys from the BASH env.
  `memory/lesson_pwsh_nested_powershell_psmodulepath.md`
- **AUTONOMOUS pile test loop harness** (reference). `memory/reference_pile_test_harness.md`
- **A gitlink with no `.gitmodules` entry breaks EVERY fresh clone, and only fresh clones.**
  `third_party/opus` was committed as mode 160000 with no registration, so `--recursive` skipped it
  silently and CMake died far away on an empty directory; a Discord user hit it, we never could —
  the maintainer's working copy already had the checkout. Cross-check
  `git ls-files -s | awk '$1=="160000"{print $4}'` against `git config -f .gitmodules --get-regexp path`.
  General: **when a newcomer reports a failure you cannot reproduce, suspect your own working copy
  first.** *Look FIRST:* `memory/lesson_gitlink_without_gitmodules_entry_is_silently_skipped.md`
- **A seeded config value OVERRIDES the code default — a "documentation" seeder is a drift bomb.**
  `ReadIniValue` returns `def` only when the key is **ABSENT** (`config.cpp:96-104`), so writing
  `key=value` into a user's ini pins that value for that install forever. Proof it is not theoretical: a
  hand-written ten-line sketch got **4 of 10 defaults wrong** (port 7777 vs 47621, ui.scale 1.0 vs 1.25,
  net.master empty vs its `DEFAULT` sentinel, one font key vs five roles). Config files carry user
  STATE; a catalog of defaults belongs in a generated `*.example` that is never read as config, emitted
  commented. **And the bomb was already assembled (2026-07-24):** `release/votv-coop.ini` sat in the repo
  carrying `Pelmentor` / `net.master=DEFAULT` / `net.port=47621` as ACTIVE values — nothing deployed it,
  no `RELEASE.md` line mentioned it, untouched since 2026-06-23 (pre-rebrand name). An artifact nothing
  references does not become harmless, it becomes *unmaintained while still readable*; on a public repo
  that is worse. Deleted by user ruling (RULE 2). *Look FIRST:*
  `memory/lesson_seeded_config_value_overrides_the_code_default.md`
- **"First match wins" is a property of the READER, not of the file format.** `multivoid.ini` has two
  readers with two different rules: `ReadIniValue` breaks on the first matching **KEY** whatever its value
  (`config.cpp:96-104`), `LookupTriState` on the first **RECOGNIZED VALUE**, skipping `key=garbage` above
  `key=1` (`:457-473`) — plus opposite case-sensitivity, opposite comment handling, and no mutex (so the
  file's own "Readers take it too" comment is FALSE). A design doc had stated the rule once, for "the
  file", and three separate decisions leaned on it — including "insertion is a MOVE, never an ADD", which
  is **not** behaviour-preserving: moving one occurrence past another inverts a flag verdict. Count the
  readers before writing any "how the file is read" fact; collapse duplicates BEFORE reordering.
  **RESOLVED (pass 3, 2026-07-25, converged):** occurrence selection was UNIFIED — authoritative line =
  first case-insensitive KEY occurrence, one rule for reader/writer/report; layers differ only in value
  vocabulary; first-RECOGNIZED retired. Safety measured (zero ci collisions across all 109 keys). Bonus
  find: today's case-sensitive writer can make the two readers disagree with ONE write (`Enabled=1` +
  write `enabled` → EOF duplicate). *Look
  FIRST:* `memory/lesson_first_match_is_a_reader_property_not_a_format_property.md`
- **When N incompatible readers already ship, no unification preserves them all — CHOOSE, then
  ENUMERATE.** Four truthiness readers coexist in `multivoid.ini` (`1|true|0|false` whole-line; `!= "0"`
  where *anything* is true; `== "1"` strict where `true` reads FALSE; a 4-token chain), so
  `nameplate=true` works today and `ui.netstats=true` silently does not. Two `/qf` rounds were burned
  engineering a preserving vocabulary (narrow → breaks `net.master.custom=yes`; union → the `!= "0"`
  readers accept every string, so any shared vocabulary narrows them). Write *"no behaviour-preserving
  option exists"* down first, choose on merits (obey what the user literally wrote), enumerate every
  moved verdict — then check each against "does the new reading match what they wrote?". That check
  **dissolved an entire subsystem** here (a meaning-change report, its persisted state, its deferral, four
  legacy predicates): a mechanism that exists only to apologize for a change usually means the change was
  described wrong. *Look FIRST:*
  `memory/lesson_choose_and_enumerate_when_no_behaviour_preserving_option_exists.md`
- **Never host a report about X behind a gate that X controls.** The config-review panel for
  `multivoid.ini` was placed in `dev_menu`, which is gated on `MasterEnabled() && devkeys` (post-arc-3: `ResolveFlag(rows::devkeys)`)
  (`dev_menu.cpp:539`) — i.e. the user must hand-edit the very file he is asking for help with. The two
  other candidates failed the same test differently: `multivoid.log` has **no owner-reader** ("reported"
  with no reader is a fiction), and the loader's boot dialog is a different severity whose `Arm` is
  single-slot (a second message silently drops the first, `boot_warning_dialog.cpp:29-35`). Before picking
  a surface, ask what turns it ON and whether the audience already looks there. **Second instance one gate
  DEEPER (2026-07-25):** the multiplayer menu itself is ini-gated (`multiplayer_menu_off`,
  `multiplayer_menu.cpp:310`); the surface that survived measurement is the HUD root (`imgui_overlay` +
  `hud` — zero ini reads, boot-installed `harness.cpp:469`). Grep the candidate's RENDER PATH for gates,
  not just its label. *Look FIRST:*
  `memory/lesson_diagnostic_surface_gated_by_what_it_diagnoses.md`
- **ABSENT/UNREADABLE conflation makes every mint-then-persist path destructive.** `ReadIniValue`
  returns its default for BOTH "key absent" and "file unreadable" (`config.cpp:96-104`), and the guid
  mint (`:407-434`) persists immediately on default — so a transient lock releasing between the failed
  read and the write lets the fresh guid **overwrite the user's stored identity** (skin `:390-405` is
  byte-identical; `:431` logs "persisted" unconditionally though the writer is `void` and aborts on
  locks). Deeper: `fgets` NULL conflates EOF with stream error and the loops never check `ferror`
  (`:99-102`) — a mid-stream failure verdicts every later key as authoritative-ABSENT. Fix shape: a
  tri-state read (`value/ABSENT/UNREADABLE`, authority only on clean EOF; errno discriminates —
  measured: locked=`EACCES` vs missing=`ENOENT`), mint gated on authoritative ABSENT, writer returns
  `bool`. *Look FIRST:* `memory/lesson_absent_unreadable_conflation_makes_mint_paths_destructive.md`
- **One file format, ONE parse primitive.** `multivoid.ini` is parsed by three different fixed buffers —
  `char[128]` (`LookupTriState`), `char[256]` (reader), `char[512]` (writer) — and a 380-char line
  already mints a **live phantom key in 2 of 4 real inis** (fgets splits it, the tail contains `=`). The
  reader's split is ephemeral; the **writer's would be persisted**, i.e. the 2026-07-02 data-loss class.
  Raising the number moves the wall; use an unbounded read, and for a local file the user owns, log an
  over-long line — never drop it. *Look FIRST:*
  `memory/lesson_one_file_format_needs_one_parse_primitive.md`

- **A Windows text-mode writer defeats any byte compare-first; a both-outcomes-tolerant assert
  cannot see the dead branch.** `AtomicWriteLines` opened `L"w"` — the CRT translated every `\n` to
  CRLF on disk, so the T8 catalog's `existing == fresh` (binary read vs LF-built string) was
  PERMANENTLY false: the "identical→skip" branch was dead, every boot logged "regenerated" — and the
  drill stayed green because its assert accepted BOTH `Regenerated|UpToDate`. Fix `42fabf77` (writer
  → `wb`); revival proven by TWO consecutive boots (run 2 logs "up-to-date"). Check BOTH fopen modes
  of any write/read byte-compare pair; give every compare branch a scenario that forces it. *Look
  FIRST:* `memory/lesson_text_mode_write_defeats_byte_compare.md`
- **Editing `build-core.yml` = do the fingerprint re-commit ritual in the SAME workstream.** The
  release judge pins `build_core_sha256`; the b127-dev run refused pre-build (`FINGERPRINT: FAIL`,
  run 30168572721) because `ad15ae7c` added the registry-gate step without re-smoking. The refusal
  is designed — but the recovery (cacheless build ~40 min + local smoke of the CI bytes + commit
  `fingerprint-dump.json` + re-run) costs ~1.5 h AT RELEASE TIME. Dispatch the cacheless build right
  after the build-core edit lands, not when the judge refuses. **CRLF addendum (2026-07-26):** the
  hash is the RUNNER'S autocrlf (CRLF) view — a local LF checkout hashes differently by design;
  LF→CRLF conversion reproduces the dump byte-exact. Commit the run's dump VERBATIM, never a
  locally-computed hash. *Look FIRST:*
  `memory/lesson_build_core_edit_requires_fingerprint_recommit.md`
- **Dev releases via GitHub Actions are a RARE, END-OF-SESSION act (USER DIRECTIVE 2026-07-26).**
  The CI cacheless build is ~40 min; the local build ~1 min. Never block a session on the CI lane:
  fire the release workflows as the session's LAST action and let them finish unattended
  (published-row bookkeeping rides the next push). Iteration always runs on local builds. *Look
  FIRST:* `memory/feedback_dev_releases_rare_end_of_session.md`
- **"Reliable" wire loss happens at ENQUEUE time, silently, in contiguous runs.** GNS ARQ covers
  only messages that ENTERED the stream; under buffer-full (rc=-25, pendRel ≈ 512KB cap)
  `bDeleteFailedMessages=true` DELETES the message and 60+ `SendReliableToSlot` callers ignore the
  false return. Measured (b125 tester log): ALL 164 losses in ONE join second; loss contiguous in
  the drain's eid-ascending order (`registry.cpp:291`) — the join drain paces by CPU, blind to the
  link. Same class as the B2 not-ready skip: ONE delivery-guarantee owner. *Look FIRST:*
  `research/findings/votv-tester-log-triage-b125-2026-07-26.md` §R-A +
  `memory/lesson_reliable_enqueue_loss_is_silent_and_contiguous.md`
- **An identity-minting migration must census the wire SENDERS, not only the identity maps.** v122
  no-passive-mint demoted client keyed minting, but TWO client-reachable express paths
  (`prop_container_extract.cpp` takeObj-POST — no role gate; `trash_collect_sync.cpp`
  EnsureHeldItemBroadcast) still stamp `elementId=(eid==kInvalidId)?0:eid` → clients emit 0 → the
  host range-gate silently drops → client-born items stillborn for 3+ builds (tester's lost
  hamburger). When changing minting rules: grep every payload-stamp site reachable on the demoted
  role. *Look FIRST:* `research/findings/votv-tester-log-triage-b125-2026-07-26.md` §R-B +
  `memory/lesson_identity_mint_migration_must_census_wire_senders.md`

---

- **A public hostname is not a public origin IP — know which one you are redacting.** The service
  hostname is compiled into a DLL we distribute (`protocol.h:1113`), so hiding it is theatre; the ORIGIN
  IP behind the root domain's Cloudflare proxying is a different asset, and publishing it in four tracked
  docs defeated that proxying outright (origin bypass) plus named the host. Ask *which of the two* and
  *from whom* before redacting. Also: "not a secret" and "fine to publish" are different judgements —
  casual discoverability is a real axis; say which one you are buying. `docs/security/TRACKER.md` **A11**,
  `b2c4b3ef`. *Look FIRST:* `memory/lesson_public_hostname_is_not_a_public_origin_ip.md`

- **A rule an agent wrote comes back cited as the user's rule — check provenance before obeying it.**
  "`tools/mp.py` is NEVER committed" was quoted as policy by `MEMORY.md`, by `tools/net/departure_drill.ps1`'s
  header and by my own `/qf` brief. Measured: the sentence entered `docs/OPUS_48_DISCIPLINE.md` in that
  doc's **own authoring commit** `1e3c81f5`, **author Claude, 2026-07-06**; no user utterance created it;
  `git check-ignore` matches nothing; and mp.py had been committed **8 times** (2026-06-15 .. `c1403fd7`)
  before the rule existed. The cost: `HEAD:tools/mp.py` is 2,532 lines with **zero** `selftest`, while
  `docs/RELEASE.md` step 0 names two machine assertions produced only by an uncommitted working tree —
  **the release gate has not been runnable from a clean clone since 2026-07-02**, and every session read
  the ritual as intact. It survived because it was *plausible* and because it shared a sentence with
  `"kerfur skins icons/"`, which has a REAL justification (copyrighted art) — the true half lent
  credibility to the invented half. *Look FIRST:* run `git log -S"<the sentence>"` before obeying a rule
  that constrains the repo; a prose rule nothing enforces is not a rule (put it in `.gitignore`/CI with
  the why, or label it a preference); tag "the user's rule" in a `/qf` brief as the provenance CLAIM it
  is; and fix a false attribution IMMEDIATELY — that is factual and yours — while the ruling on the
  rule's content stays the user's. `memory/lesson_an_agent_authored_rule_becomes_a_user_rule.md`

## 9. Security (threat model, trust boundaries, peer identity)

Canonical home: **`docs/security/`** — `README.md` is navigation, `THREAT_MODEL.md` + `SUBSTRATE.md`
are the facts, `TRACKER.md` is the ranked findings list, `EXECUTION.md` is the board, `RULES.md` is
S1-S6, and `PLAN_01..05` are the fix plans. Read those before any security, transport,
authority-boundary or website work. Everything below is the durable *lesson*; status lives in the
tracker.

- **Write the THREAT MODEL before designing any security mechanism.** Tier B/C ran **26 `/qf` rounds**
  across two sessions and shipped two arcs without anyone asking what adversary, with what access,
  gains what. Writing the model took under an hour and showed the lobby list is public, the token is a
  shared secret, and the real gap was peer authentication — which TLS does not touch. A converged
  design pass proves the design is coherent, **not** that the goal is right; critics escalate *within*
  the frame you hand them. `memory/feedback_threat_model_before_security_mechanism.md`
- **GNS encrypts (AES-256-GCM) but does NOT authenticate peers.** The opensource build defaults
  `IP_AllowWithoutAuth = 2` ("don't attempt authentication") with the warning deliberately suppressed
  (`csteamnetworkingsockets.cpp:88-91`), and we never override it. So passive eavesdropping already
  fails but an ACTIVE attacker at the rendezvous can sit in the middle — and **the control plane is the
  only place peer identity can be established.** GNS ships the whole CA (certstore + certtool +
  `SetCertificate`); Ed25519 sign/verify already links into our process.
  `memory/lesson_gns_encrypted_but_peer_unauthenticated.md`
- **A secret handed to every user is not a secret.** `signalingToken` is one static value returned to
  every client, so protecting it in flight is theatre — and a shared bearer cannot authenticate anyone,
  which is how a stranger with `nc` can register someone's host identity and evict them. Ask "who
  receives this?" before "how do we protect it?". `memory/lesson_shared_secret_handed_to_every_user_is_not_a_secret.md`
- **A FALSE security comment is worse than no comment — and a HALF-true one is worse still.** Two
  found in one day (`master.rs:498-499` asserted a join-secret challenge that exists nowhere;
  `session_trashcarry.cpp:61-62` claimed float validation its apply path does not do), and in both
  cases the comment is *why* the gap survived — a reassurance is never grepped. The trashcarry one is
  a **fused claim**: its ctx-freshness half is TRUE, so spot-checking confirms the whole sentence. One
  claim per comment sentence; verify every conjunct separately. Both comments corrected in `6f0c2bf8`.
  Name your own gap instead, as `event_dispatch_entity.cpp:259-264` does.
  **3rd instance 2026-07-24 — `.gitignore`, and it left a private key exposed for four days.** The rule
  `tools/coop-server-rs/*.pem` sat under a comment reading "tlstest/ is ignored above"; `grep -n tlstest
  .gitignore` returns EXACTLY that comment and no rule, so `tools/coop-server-rs/tlstest/key.pem` was
  untracked-but-NOT-ignored, one `git add -A` from a public commit. `git check-ignore -v` answers in a
  second — nobody ran it *because the comment said not to bother*. That is the mechanism: a false
  assurance suppresses the cheap check. NOT an incident (verified: nothing ever committed, material was
  expired self-signed `CN=localhost`). **Second takeaway: a path-scoped rule for a KIND of artifact
  grows a new gap every time a directory appears** — scope by what the thing IS, not where it lives
  (now `*.pem`/`*.key`/`*.p12`/`*.pfx`/`*.jks`/`id_rsa`/`id_ed25519`, global, `3f1a4e4a`). *Look FIRST:*
  for any ignore/allow/deny rule, verify with the tool, not the comment beside it.
  `memory/lesson_false_security_comment_worse_than_none.md`
- **Before CAPPING an allocation driven by a wire value, ask whether the allocation is needed at
  all.** `save_transfer.cpp:857` reserved from an unvalidated wire `u32` (one packet → 4 GiB →
  process death). The planned fix was a `kMaxSaveBlobBytes` cap; a use census showed the `reserve()`
  was a **pure allocation hint** and deleting it removed the bug with no number to get wrong. A cap is
  a *policy limit* that can silently reject an honest, grown save — a guard around an unnecessary
  primitive is worse than not having the primitive. Ask: needed at all → bounded by something already
  known → only then invent a limit, and describe what happens to the honest user who exceeds it.
  `memory/lesson_delete_the_allocation_dont_cap_the_wire_value.md`
- **An ANNOUNCE and its PAYLOAD must be handled on the SAME thread.** `save_transfer` announced a
  blob on the game thread while its chunks landed on the net thread, so bytes could legitimately
  arrive with no announced size — an unbounded buffer. The wire order was already guaranteed (one
  in-order lane), which is exactly why the pre-announce branch *looked* like dead defensive code: **a
  per-lane ordering guarantee says nothing about processing order across threads.** Sizing that
  window would have been a guessed constant; refusing early bytes would have broken real joins. The
  fix is CO-LOCATION — divert the announce to the payload's thread and retire the old handler (RULE
  2) — after which "refuse bytes with no announce" is correct and no constant exists.
  `memory/lesson_announce_and_payload_must_share_a_thread.md`
- **Before generalizing a pattern to N sites, grep whether it was RETIRED at one site — and why.** A
  syncer design proposed a `holder == sender` receive check across 68 kinds; that exact shape had been
  retired three days earlier as a RULE-1 root fix (v116: a validator anchored on a claim the validated
  event itself releases loses by construction). The abstraction is what hides the match — a retirement
  reads as local cleanup, so nothing connects "the general shape" to "the instance someone deleted".
  Resolution was a distinction, not abandonment: MTA's syncer is a long-lived ASSIGNMENT, our
  `device_occupancy` is a per-interaction CLAIM. Mirror image of "never retire a fix on theory".
  `memory/lesson_check_whether_the_pattern_you_are_generalizing_was_retired.md`
- **Census a field's WRITERS, not just its USES.** A use census answers "what depends on this"; only a
  writer census answers "**who can move it, and from which thread**". The second question found W1b —
  `OnBegin` had no guard against a *second* `Begin`, letting a hostile host move the completion
  denominator and CRC out from under an in-flight transfer. Two audit agents and two `/qf` rounds
  missed it while the field sat in the brief, described by its readers. Announce-then-stream lanes owe
  an "announced exactly once per arm" invariant. `memory/lesson_census_writers_not_just_uses.md`
- **Send-side caps are not caps — validate on APPLY.** The classic bug classes are largely absent in
  this tree (lengths checked, strings clamped, paths allow-listed, zero non-literal format strings);
  the real exposure is every "the peer is a well-behaved copy of this build" assumption. Peers come
  from a public lobby list — they are strangers. Never `reserve()` from a wire integer, and diff a new
  lane against its siblings (a missing role gate shows up as an asymmetry).
  `memory/lesson_send_side_caps_are_not_caps.md`
- **Split fused options before comparing architectures.** A "TLS vs GNS vs response-signing" trilemma
  dissolved once "control plane over GNS" (expensive) was separated from "our own CA for peer certs"
  (cheap, transport-untouched) — only the second was ever needed, and the preferred third option turned
  out to patch one vector rather than close the class. An option named after its most striking feature
  carries that feature's whole cost bundle. `memory/lesson_split_fused_options_before_comparing_architectures.md`
- **Anti-cheat is a SEPARATE layer from the authority model — do not fuse them.** Measured from MTA
  2026-07-20: its anti-cheat is CLIENT-side only (`CAntiCheat.cpp` under `Client/` only; the Server
  tree has none); server position validation is THREE arithmetic checks (proximity vs the server's own
  records + rate + dimension, `CUnoccupiedVehicleSync.cpp:490-492`) with **no plausibility/geometry
  check at all**. So (a) an engine-free arbiter can do MTA's entire spatial validation — distance+rate
  yes, geometry no, and MTA skips geometry too; (b) anti-dupe is architecture (own the values,
  serialise intents -> second spend rejected), not a detector; (c) "make it secure" = TWO layers of
  different size — correctness/authority (phase 2, where dupes die) vs anti-cheat (a tunable, optional,
  losing arms race). Separate them before scoping. Rules: `TRACKER` F2-F6. LOOK FIRST:
  `docs/security/MTA_PRECEDENT.md` §11. `memory/lesson_anticheat_is_a_separate_layer_from_authority.md`

- **2026-07-26 — INFO log lines are BUFFERED; a killed process loses them.** `log.cpp:152` flushes only
  on WARN/ERROR (a deliberate 2026-05-27 perf fix), so an INFO gate line written just before a
  force-kill/idle period is simply absent — which reads as "execution stopped here" and cost a build
  cycle re-architecting the code that follows it on a deadlock theory that never happened. Any line a
  gate or drill asserts must call `ue_wrap::log::Flush()`. A log that stops growing in an idle process
  is NOT evidence of a freeze. LOOK FIRST: the level of the missing line.
  `memory/lesson_info_log_lines_are_buffered_until_a_warn.md`
- **2026-07-26 — D3D12 gives you the device off the swapchain, but NEVER the presenting QUEUE.**
  Measured on the rig: `GetDevice(ID3D12Device)` hr=0 and `QI(IDXGISwapChain3)` hr=0, but no API says
  which `ID3D12CommandQueue` presents -> capture it by hooking `ExecuteCommandLists` (vtable[10]) and
  scoring the last DIRECT same-device submit before each Present (600/600 on one queue; a COPY queue's
  5 calls = the known-positive). `CreateSwapChain`'s `pDevice` IS that queue, but only if our boot
  PRECEDES creation — probe armed, never fired, so that route is unavailable. Also: the rig presents
  **R10G10B10A2**, so take the RTV format from the desc, never a literal; never create a D3D12 device
  inside the Present detour; retire a capture hook with Disable-without-Remove. LOOK FIRST:
  `src/votv-coop/src/ui/overlay_backend_dx12_capture.cpp` header +
  `research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md`.
  `memory/lesson_d3d12_has_no_api_for_the_presenting_queue.md`
- **2026-07-26 — a branch that only logs on FAILURE cannot be drilled.** The DX12 resize drill produced
  a correct-looking screenshot and ZERO log lines, because `ResizeBuffers` logged only its failure
  branch — so "did the rebuild path run?" stayed unmeasured (ImGui's layout follows GetClientRect
  independently of our render target, so the visual proves nothing). One success line turned the same
  drill into a measurement. Rare-event branches (resize/recreate/reconnect/migrate) get a success log.
  LOOK FIRST: grep the branch for a success-path log BEFORE running the drill.
  `memory/lesson_a_success_path_that_never_logs_is_undrillable.md`
- **2026-07-26 — in a PUBLIC repo, an un-annotated superseded decision is ammunition.** A critic quoted
  `docs/FEASIBILITY.md:25` in a public thread; five lines below sat "Chosen approach: UE4SS +
  reflection" — reversed the NEXT DAY by RULE 3 (standalone) but never annotated, and §0.3 still said
  "we do not hand-roll a present hook" months after we did. Annotate the ORIGINAL line in place
  (`SUPERSEDED <date> -> <rule/commit>` + why); sweep for the retired approach's vocabulary every
  `/documentize`. LOOK FIRST: grep the doc tree for the old approach's name, check each hit against the
  current rules. `memory/lesson_stale_planning_docs_are_public_ammunition.md`
- **2026-07-26 — `find | xargs wc -l | tail -1` reports only the LAST batch.** It undercounted the
  tree by 23% (113,045 vs the real 146,347) and the wrong number was minutes from going into a public
  technical argument where the other side had already counted ~144k. `find A B -name x -o -name y`
  without `\( \)` is a second, compounding trap. Count with `find -print0 | xargs -0 cat | wc -l`,
  always run a **parts-sum check** (the pieces must add to the whole — that is what exposed it), and
  quote a file count next to the line count. LOOK FIRST: any size/LOC claim that leaves the repo.
  `memory/lesson_xargs_wc_tail_truncates_the_total.md`
- **2026-07-26 — unequal per-asset download counts are UPDATERS, not broken installs.** A release page
  showing payload 11 / loader 7 reads as "users install only half the mod" and argued for bundling both
  DLLs into one zip asset (changing the publish step's asset invariant + every install instruction).
  Measuring ALL releases killed it: the first public release split 13/12 (every downloader a fresh
  install), and the later gap is exactly the updater population — the loader DLL does not change
  between builds, so a correct update fetches the payload only. LOOK FIRST: before designing anything
  from a download asymmetry, pull the counts for EVERY release and ask which assets change per build.
  `memory/lesson_unequal_asset_downloads_are_updaters_not_broken_installs.md`
- **2026-07-26 — a staleness ban must match the VARIABLE half, or it kills the placeholder.** The
  install-doc lint had to reject `multivoid-0.9.0n-128.dll` while accepting
  `multivoid-0.9.0n-<N>.dll`; the natural regex `multivoid-\d` rejects BOTH, because the game target
  `0.9.0n` starts with a digit. Anchor on the variable half with the target interpolated as a literal
  (`multivoid-<escaped target>-\d+\.dll`) and ship a must-PASS placeholder fixture beside the
  must-FAIL literal — a FAIL-only fixture set passes while the gate rejects every valid doc. LOOK
  FIRST: any lint separating "a template" from "a concrete instance".
  `memory/lesson_placeholder_bans_must_target_the_variable_half.md`
- **2026-07-26 — a remote page's CONTENT cannot be a build-blocking gate: GitHub serves stale release
  bodies from BOTH endpoints** (corrected twice in one session). A lint comparing each live release's
  `## What's new` against its git notes file FAILED seconds after publish ("list endpoint lags a flip"),
  FAILED again hours later (so: not a flip window), and then FAILED through the confirm-read added to
  fix it — while a manual per-tag fetch that same minute was byte-identical (845 chars) and **5/5
  consecutive lint runs returned 0 FAIL with the file untouched**. A single pass cannot distinguish a
  cached read from real drift, so the check was demoted to a labeled WARN ("RE-RUN first — both
  endpoints cache"); every FAIL-carrying gate now reads a LOCAL file (judge NOTES_OK, the publish
  backstops, the notes-file-missing branch). Drilled: clean -> poisoned notes = WARN exit 0 -> deleted
  notes = FAIL exit 1. LOOK FIRST: before any remote-content comparison may refuse a build, run it 5x
  and ask what it does with a cached copy.
  `memory/lesson_release_body_list_endpoint_lags_the_flip.md`
- **2026-07-26 — "that dependency is unavailable" needs a positive control before you publish it.** The
  claim "we cannot build against UE4SS's C++ core" was first observed as an SSH submodule clone
  failure, which is indistinguishable from a missing key. The publishable measurement is anonymous
  HTTPS + the REST API (`ls-remote` -> "Repository not found", `gh api` -> 404) run against a
  KNOWN-PUBLIC repo on the same transport as the control — plus a date, because availability varies
  with time. Second independent leg: the release channel's dev asset ships zero headers/libs.
  Sharpened same day: also test BOTH confusingly-similar org spellings (`UE4SS-RE` vs `Re-UE4SS`) and
  the fork network (`gh search repos` -> zero mirrors GitHub-wide); the vendored gitlink dir EXISTS
  but is EMPTY — say that precisely. Now machine-re-verified per release (tripwires.ps1 wire-a).
  Sharpened AGAIN same day (user probe, post-decision): the transport was right, the CONCLUSION still
  overclaimed — the project's OWN README (vendored, lines 80-82) + issue #577 document a self-service
  Epic-linkage access path a transport probe cannot see; the §11 record needed a same-day correction.
  LOOK FIRST: any "X is missing/broken/unavailable" claim that will leave the repo — anonymous
  transport + positive control + date + **read the target's own docs for a sanctioned access route**.
  `memory/lesson_a_private_dependency_core_is_measurable_with_a_positive_control.md`
- **2026-07-26 — a "we'll revisit if X changes" wire is wallpaper unless it has five properties.** Built
  as `tools/release/tripwires.ps1` + VERSION_MIGRATION §11 (the UE4SS-switch record), each property
  extracted by a /qf round: (1) a ritual host that already runs (RELEASE.md step 0, output in the
  written handoff — an unattached check "fires only if a human remembers"); (2) tri-state
  QUIET/FIRED/CHECK-UNREACHABLE so network-down never reads as not-fired (positive transport control);
  (3) the baseline is a frozen DECISION constant, not the live toolchain fact; (4) re-quiet = a dated
  `TRIPWIRE-DECISION <wire> <date>:` line + the constant update in the SAME commit (the anchor carries
  the wire name so a routine doc edit cannot clear it); (5) a mechanical overdue detector (committed
  state file + ledger grep -> OVERDUE-DECISION). All verdict shapes force-drilled incl. the
  must-NOT-clear control; monitor-less doors named honestly instead of pretending a boot WARN is one.
  LOOK FIRST: copy the shape from `tools/release/tripwires.ps1` for ANY watch-this-condition artifact.
  `memory/lesson_decision_tripwires_need_tristate_and_overdue_detection.md`
- **2026-07-26 — price a build-vs-adopt question by REPAIR HISTORY, not by line count, and classify the
  diffs.** "Should the substrate move onto UE4SS?" was being argued on LOC share (1.6%), which answers
  "how big", not the actual claim ("cheaper because someone else maintains it"). Git history answers it:
  5 repair commits in 1,282, the AOB constants edited once at creation. Then reading and classifying all
  26 `reflection.cpp` diffs found the concession — **2 of the 5 repairs WOULD have come free** with the
  dependency's API (`GetPropertyByNameInChain` walks the SuperStruct chain); the first draft said "none"
  and was wrong. LOOK FIRST: `git log --follow` + a hand classification (repair / growth / refactor) +
  a check of whether the candidate's API absorbs each repair. Publish the concessions.
  `memory/lesson_price_a_dependency_by_repair_history_not_by_line_count.md`
