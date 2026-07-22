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
  `memory/feedback_probe_must_count_not_confirm.md`

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
  `/v1/host`), not a guessed folder. `memory/lesson_negative_grep_verify_against_known_positive.md`
- **A SYNTACTIC marker set over the class dump CANNOT express a SEMANTIC property** ("is this coop-relevant
  / can it diverge between peers"), and the reason is an ERROR ASYMMETRY, not a tuning problem: the
  false-NEGATIVE side is measurable, the false-POSITIVE side is uncalibratable in principle (there is no
  ground truth for "should have been counted"), so over-inclusion can never be bounded. Measured 2026-07-22
  while trying to GENERATE a coop-readiness % with the denominator taken from the game: a filter
  (`ReceiveTick` | `getData`+`loadData` | interaction verbs) over 2305 BP classes yielded 686 and threw 16
  of 45 already-synced classes (**36%**) into "inert content" — including `UsaveSlot_C` (32 own fns), where
  `GObjStack` lives. The set is Actor-shaped + player-interaction-shaped, structurally blind to non-Actor
  carriers (`USaveGame`/`UActorComponent`/`UUserWidget`) and to spawner/ticker/event behaviour. Two
  instrument defects on the way, both plausible-looking: ancestor-walking to engine roots returned "100%
  covered" (every class reaches `AActor`), and exact-name grep scored `Anpc_krampus_C` + all 11 `Awisp_*_C`
  as never-touched while our code matches them by SUBSTRING at runtime. **The dump ENUMERATES (2305 classes;
  838 with zero own functions are a structural floor that cannot diverge by construction) — it does not
  CLASSIFY.** Corollary that decided the status column, measured the same day: of 44 doc-named classes only
  **3** are hands-on VERIFIED, 11 AS-BUILT, 30 carry no status token at all — a boolean "coop-correct"
  column filled from doc claims would have scored ~44 green, a **14x** overstatement on the only rung that
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
- **A cross-cutting axis has ONE owner** — handlers CAPTURE, never apply (anti-smear). `memory/feedback_one_owner_order_axis.md`
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

## 2. Join-window identity & the DUP-prone zone (measure before touching)

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
  E-grab decline. `memory/lesson_reuse_proven_author_not_raw_reimpl.md`
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
- **Identity-critical log lines carry cls+key+loc (USER RULE)** — a class histogram alone makes
  per-entity RCA impossible; cold paths only, never at the POST-native destroy seam (PendingKill),
  throttle mass arms. `memory/feedback_identity_logs_carry_key_and_loc.md`

## 3. Sync architecture (owners, routers, lifecycle)

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
- **The FString PIN doctrine ("mint engine-side, never free") holds ONLY for FRESH buffers** —
  repeated in-place mints on the SAME live object's fields LEAK on receivers (no native reassign ever
  runs there); swap-and-EngineFree instead (v116 perf audit finding 1). *Look FIRST:*
  `ue_wrap/devices/laptop.cpp FreeFStringSlot`. `memory/lesson_fstring_pin_doctrine_fresh_buffers_only.md`

## 8. Build / deploy / git hygiene

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

---

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
