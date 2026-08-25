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

- **Your own memory file can silently EDIT the plan of record, and you will read it back as the plan.**
  2026-08-25: on "go next" I was one call from implementing a security root (A54). `[V]` The design doc
  of record ordered `A52 -> B3 -> B4 -> checklist`; A52 had shipped, so B3 was next -- and B3/B4 are the
  user's own two remaining reported symptoms. The only thing putting A54 next was a memory entry I wrote
  in the previous session, which promoted it to position 2 **without ever saying it was displacing
  anything**. A memory NEXT feels identical to the plan's NEXT, because carrying NEXT is exactly what
  memory is for. *Look FIRST:* when memory and the design doc both say what is next, the DOC wins and
  the disagreement is itself the finding; and when WRITING a memory NEXT that reorders, say that it
  reorders. If the displaced items are things the user reported and can still SEE, the burden is
  entirely on the reordering. `memory/lesson_your_own_memory_file_can_edit_the_plan_of_record.md`
- **An instrument can be blind FOUR different ways in one design pass, and reasoning catches none of
  them.** 2026-08-25, B3: (1) a seam the module's own header records as never having fired; (2) a seam
  field-counted at 5 host vs 19 client lines over DISJOINT entities; (3) a reader that shares its offset
  with the code under test, so a wrong read prints AGREEMENT; (4) a read off the actor's ROOT component
  when the bytecode names a different one -- null on both peers, agreeing by construction. Three are one
  root (sampling the wrong population); the fourth is "the instrument is a mirror". The bitter part:
  `coingun_collect.cpp:317` cites the parent lesson BY NAME, and instrument #1 would have gone four
  lines below it -- a lesson filed as a fact about one seam does not fire as a question about a new one.
  *Look FIRST:* `grep -c` the seam in a real log on BOTH peers before instrumenting it (0 = never fires;
  differing counts = ask whether they cover the same entities); then ask what the instrument reads that
  the code under test does not -- if "nothing", it cannot fail.
  `memory/lesson_four_blind_instruments_in_one_design_pass.md`

- **Grep the field logs before declaring a measurement impossible — then separate the SERIES inside
  them.** 2026-08-25: asked for a distribution of real per-pose deltas, I answered that it "does not
  exist on disk (poses are never logged, by design)" — and in the same pass derived a world diameter
  from coordinates found in those same logs. `pos diag:` prints `local actor=` **and** `puppet world=`
  at 0.5 Hz; **808 player samples** were sitting in the 2026-08-24 logs the whole time, and they carried
  the entire distribution the design needed (host local max **692 cm/s**, puppet max **606**, exactly
  one discontinuity in 11.5 minutes). The second half is the sharper one: the first extraction mixed
  those samples with `[WA-TRACE]` prop coordinates and one pre-join sample from the client's *preLoad
  world*, producing a "2.34 km world diameter" that was wrong twice over — the two players never left a
  **35 m patch**, and the world's real diagonal (**~5.53 km**) comes from the level dumps with the
  engine sentinels filtered. **Look here FIRST:** grep the logs for the noun and for the subsystem's
  own name, then label every series before computing anything over it.
  `memory/lesson_grep_the_log_before_declaring_a_measurement_impossible.md`


- **AN EQUIVALENCE INSTRUMENT THAT STRIPS COMMENTS CANNOT SEE A COMMENT DELETED — AND YOUR OWN
  MUTATE CONTROLS WILL NOT TELL YOU.** 2026-08-25, the `a290a466` coingun extraction. `[MEASURED]`
  The commit claimed all three moved bodies *"line-for-line identical modulo four substitutions"* and
  cited four mutate controls shown RED first. An independent re-diff found **six** differences,
  including a **3-line comment DELETION** in `OnCollectPre` and a 2->3-line structural rewrite. The
  normalizer did `ln.split('//')[0]` then dropped now-empty lines, so a deleted comment was invisible
  BY CONSTRUCTION — and all four controls were CODE mutations (flip a gate, neuter a check, delete a
  code line, change a constant), i.e. exactly the class the normalizer was built to keep. Controls and
  normalizer came from the same head in the same ten minutes, so the blindness could not be discovered
  by the controls. No behavioural difference existed; the overclaim is the defect, in a lane whose
  defining failure is false comments.
  **LOOK HERE FIRST:** (1) never write "line-for-line identical" about a NORMALIZED compare — state the
  blind set in the commit ("compares CODE only; comment/whitespace changes are invisible"); (2) write
  one control **per normalizer rule**, not per failure you imagine — every strip/replace/regex is a
  blindness you chose and owes a control that proves it is no wider than intended; (3) for a pure move
  also run a raw `diff -u` and read the hunks, which needs no normalizer and is what actually finds a
  dropped comment; (4) a third party re-running your comparison beats another control you write
  yourself — brief them "report ANY difference, including ones the substitutions would explain; I want
  your list, not my list".
  `memory/lesson_a_diff_instrument_that_ignores_comments.md`

- **A VERDICT THAT ALREADY PASSES ON THE BROKEN BUILD MEASURES NOTHING — two of four did, and it took
  27 rounds to notice.** 2026-08-24, `/qf` rounds 42-44. `[MEASURED]` Verdict 2 was *"a client picks up
  a host coin and the HOST's balance moves by that coin's denomination"* — satisfied by the **unfixed**
  build: `HOST/…/multivoid.log:32118` retires coin 6182 (`PE-invisible self-destroy`) and `:32119` is
  `balance_sync: host Points -> 350`, **+1 in v137**, because *the host collected its own coin*
  (`HOST:32117` carries `grab_hook[InpActEvt.use]` one line earlier — attribution, not adjacency).
  Verdict 1 (*"both peers agree on the coin set"*) passed **vacuously**: a client sale was refused 3/3,
  so it compared **two empty sets**. Verdict 4 had the same shape one level down — the CDO default
  `points = 5` renders bronze, so a **1-point** coin is accidentally correct on a mirror.
  A related trap in the same family: verdict 1's original wording measured a **collect**, because
  `[V]` **a sale does not credit at all** — `sell` mints coins worth the price and the balance moves
  only when one is collected (`HOST:28763` mint, no movement; `HOST:29410` +25 on the dead-retire) — so
  it would have FAILED on a perfectly correct sale lane.
  **LOOK HERE FIRST:** before trusting any acceptance verdict, run it against the FAILING build's own
  logs and ask three questions in order — (1) does it already pass? (2) can it pass *vacuously* (empty
  set, coinciding default, no-op)? (3) does it measure MY axis or a neighbouring one? Prefer an
  **attribution read** over an instruction to the tester ("a credit with no host use-press in the
  preceding N ms") — the field run violated the instruction by accident. And note the dual: splitting
  verdicts for clean attribution can leave the **user's actual criterion** tested by no rung at all;
  keep one composite ACCEPTANCE verdict alongside the diagnostic ones. Full:
  `memory/lesson_a_verdict_that_already_passes_on_the_broken_build.md`.

- **THE FAIL-SAFE YOU ADD TO REMOVE AN ASSUMPTION CAN ITSELF BE FAIL-OPEN.** 2026-08-24, `/qf` round 38.
  A mirror-park owner was given "read `bSimulatePhysics` before writing, and skip components already
  false" precisely so it would stop *assuming* which components simulate. `[MEASURED]` but
  `bSimulatePhysics` is **not** a `UPrimitiveComponent` property — it is a packed bitfield on
  `FBodyInstanceCore` (`PhysicsCore.hpp:8`, seven `uint8` all at `0x0010`) reached only via
  `UPrimitiveComponent::BodyInstance` (`Engine.hpp:1011`) — so `FindPropertyOffset(primClass,
  L"bSimulatePhysics")` returns **-1**, which "skip if already false" reads as **already false**: the
  owner would have skipped every component and parked **nothing**, silently, in the one commit two of
  the four reported symptoms rested on. **LOOK HERE FIRST:** when adding a read-before-write guard, ask
  what the guard does when the READ FAILS, and make that branch do the guarded ACTION, not skip it —
  the purpose is to park, so failing to park *is* the defect and the read is only an optimisation.
  Read a bitfield via `FindBoolFieldBits` (`reflection.h:277-290`), never `FindPropertyOffset`.

- **2026-08-24 — STANDING USER RULE: scope is NEVER a reason to hold back.** Verbatim: *"Я даю зеленый
  свет даже на самые радикальные решения, если они окажутся верными и правильными."* This is the
  standing form of RULE 1's per-request green light — it does not need re-asking. Dissolving a module,
  inverting a shared primitive's default across hundreds of call sites, retiring a shipped lane whole,
  changing how the GAME behaves: none of these is a reason to design something smaller. The operative
  test: **a design that is second-best because the best was judged too big has NOT converged.** The
  boundary is the user's own wording — *"верными и правильными"* — so the licence covers SCOPE and
  BEHAVIOUR only and never a skipped measurement, a skipped audit, or a shipped bug. Now enforced
  mechanically: `.claude/skills/qf/SKILL.md` carries it in the critic prompt, in the brief assembly
  (name every option you rejected for being large), and as a convergence condition.
  *Look FIRST:* when you write "X would be the right fix, but it would mean rewriting Y", X is the live
  option and Y's size is not an argument.
  `memory/feedback_scope_is_never_a_reason_to_hold_back.md`
- **2026-08-24 — RUN A NEW DECISION RULE ON ITS OWN MOTIVATING CASE, BEFORE YOU COMMIT IT.** I wrote a three-step test for the act-as-host rule (`COOP_SYNCER_MODEL.md` §2b) and committed it (`d5d56eac`) without running it on the **sell gun** — the case the user raised to motivate the whole rule. A `/qf` critic asked me to walk it out loud and it **could not decide that case**: the deciding question, *can the arbiter OBSERVE the trigger?*, was not one of the three steps, so the test returned "build it" for something that cannot be built until the `0x45` `vm_dispatch` substrate lands (`[V]` all 19 credit sites are `EX_LocalVirtualFunction`). Added as step 2 in `b007dac0`. **The missing step is usually the important one — the motivating case is motivating BECAUSE it is hard, so a procedure validated only on easy cases encodes the easy questions.** *Look FIRST:* before committing any rule/test/checklist/taxonomy, execute it verbatim in writing on the originating example AND on a case it should REJECT; if the motivating case is still being measured, that is a reason to WAIT, not to commit and revise (I revised twice in one session). `memory/feedback_run_a_new_decision_rule_on_its_own_motivating_case.md`
- **2026-08-24 — If the fix GROWS every round, you have not found the root yet.** Across a 9-round
  `/qf` the W10 fix accreted a per-sender share cap, a full per-connection drain, a shared-drain
  pause, a `FatalCloseSlot` terminal, an hConn-stamped connection park with counters, and a
  largest-contributor rule — **every one designed, defended, then discarded** — before converging on
  *moving one existing check above a role split*, with no new constant and a net-negative diff. Each
  intermediate design was locally defensible (it answered the previous round's objection), which is
  exactly what makes the pattern invisible from inside: accretion feels like progress. Complexity
  growth is a free per-round proxy for "the root is still mis-stated" — and it was: the register had
  named a fairness bug when the defect was one-role-only backpressure. *Look FIRST:* state the fix's
  SIZE in every `/qf` brief (lines, new constants, new state, new API surface); two consecutive
  rounds of growth means stop designing and go re-derive what the defect IS in mechanism terms; four
  or more discarded mechanisms is the same signal. And when a fix does converge small, name the
  discarded designs in the commit so the next session does not re-propose them.
  **SHARPENED 2026-08-24 with its counter-case: shrinking is a SIGNAL, not a PROOF, and the failure
  runs in this direction too.** v137's impl `/qf` deleted a suppression, a heal and a whole refusal
  branch in one round and treated the shrink as validation; the justification ("a refusal then degrades
  to exactly today's behaviour") was FALSE BY CONSTRUCTION, because the client's suppression of its own
  artifacts was unconditional while the authorization was conditional and decided later — the field
  found it as a total loss. The follow-up pass then deleted the same suppression a SECOND time on
  arguments that, re-derived, only showed it was unnecessary on the HAPPY path. *Look FIRST, on a
  sudden shrink:* which branch just disappeared, and was it load-bearing on the FAILURE path? A fix
  that shrinks by deleting the failure branch has stopped modelling failure, not converged.
  **AND THE ORIGINAL DIRECTION, INSTANCED 2026-08-24 (`/qf` 46-47):** round 46 answered a cross-module
  verb-identity defect with a **cross-cutting `.inc` id ratchet touching every consumer**; round 47
  measured that the substrate **already owned a globally unique handle and simply was not publishing
  it** (`RegisterVirtualVerb` requires the verb NAME to have static lifetime; the table stores that
  pointer), collapsing the fix to ~4 lines plus the one offending consumer. **The tell was the shape,
  before any measurement: the fix's blast radius exceeded the defect's** — one module misreading one
  value. And the ratchet would not have been one: **an `.inc` of ints makes duplicates impossible only
  if the registration signature becomes TYPED**, else it is convention wearing a ratchet's hat. ASK, when
  a fix grows: *what does the substrate already own that is unique by construction?* — the answer is
  often in the registration path, unpublished.
  `memory/feedback_a_converged_fix_should_shrink_not_grow.md`

- **Your own session-end SUMMARY is a free measurement — read it before arguing mechanism.** 2026-08-24
  (`/qf` 46-47). A kerfur gate defect was traced correctly (`av.active` is true for ANY registered verb,
  so a foreign bracket inverts the gates) and elevated to **"SIXTH live shipped defect, ordered STRICTLY
  BEFORE any new verb registration"** on that reasoning alone. `[V]` Both field logs already carried the
  verdict, printed by that same module at `OnDisconnect`: `[kerfur_asm] CONTAINMENT SUMMARY
  (session-end): catch{off=0 on=0} ... otherIn=6/7` — **zero conversions in the whole run**, so the
  defect was latent; and `otherIn=6/7` further showed the bracket window WAS live with every in-window
  spawn correctly class-rejected. The log had been read for several rounds — but **by searching for the
  SYMPTOM** (the coin's eid, the credit lines, the drive traces), and a session-end summary sits at the
  END, one line, answering a question nobody had asked. LOOK FIRST: before writing "live shipped defect"
  or ordering work on urgency, grep the artifacts for the module's own counters (`SUMMARY`,
  `session-end`, `STATS`, `GATES`, `containment`) — this project dumps them at `OnDisconnect` precisely
  so a run can never end having measured nothing, and that investment is wasted unread. **Grep for your
  SUBSYSTEM'S name, not only for the symptom.**
  `memory/lesson_your_own_session_end_summary_is_a_free_measurement.md`

- **An instrument must report its INPUTS, not only its verdict.** 2026-07-31: one probe printed
  `PREDICATE DEAD` on four consecutive runs for four UNRELATED reasons — it targeted a class default
  object; then a widget-tree template (filtering only the immediate outer removed 3 of 400, because a
  template's immediate outer is a `UWidgetTree` exactly like a live instance's); then an off-screen
  widget where `SetKeyboardFocus` is a no-op; and only then the right one, where the verdict became a
  real finding. Separately a key probe fired on `WM_MOUSEMOVE` (the swallow `switch` shares one body
  across mouse and key cases), and a control-matrix run was invalidated wholesale because a
  PowerShell helper stole foreground, VOTV auto-paused, and `pause=1` answered every question first.
  **Every one was caught by a field the probe printed about ITSELF** — what it targeted, how many
  candidates survived each filter, which message kind it saw, and the ambient state
  (`[capture=1 chat=0 pause=1]`). **Look FIRST: `focused=0` is exactly what a CORRECT probe prints on
  an idle game, so a verdict-only instrument makes "my selection logic is broken" and "the feature
  does not work" byte-identical — and confirmation bias picks the second. Print the subject and the
  population (`instances=327 focused=0`), assert the precondition inside the run, and treat every
  helper that touches global UI state as a confound.**
  Full: `[[lesson-an-instrument-must-report-its-inputs-not-only-its-verdict]]`.
  **NEW MEMBER 2026-08-24 (security W10).** W10 deleted the host's silent reliable DROP and put a receive PAUSE in its place — and the pause was **silent on both roles**: nothing logged when it fired, nothing logged how deep the inbox got. The owed RED drill was therefore blind BY CONSTRUCTION, since "no pause fired" and "the depth never got there" print identically. Fixed `0de1c1dc`: an exact enqueue-side depth high-water plus per-trigger counters (depth vs apply-park, kept SEPARATE so a seeds-arc park can never be misread as a depth pause), every ~1 Hz **including the quiet case**. Second-order: the drill's THRESHOLD is now chosen FROM the measurement rather than guessed, which is why the drill knob was deliberately left unbuilt in the same commit.

- **A DECLINED PRODUCT QUESTION DOES NOT GO AWAY — it just gets answered after you build it
  (2026-07-30).** The `/qf` critic asked in **round 1** and again in **round 2** whether the homoglyph
  fold was a product choice for the user. I declined both times, citing "never ask when the
  least-crutches option is obvious", and ran 13 more rounds. The design converged — 444 measured pairs,
  both drills RED, a four-peer lobby proving it end to end — and the user's answer was *"I don't care
  if Alex and Аlex play together"* / *"Alex (latin) and Аlex (cyrillic) is normal"*. Built and reverted
  the same session. The autonomy rule covers **HOW** to build, never **WHETHER the user wants the
  outcome**. **Look FIRST: before a design pass, answer "whose complaint is this?" — if the answer is
  "mine", spend one sentence in text and keep working while it sits. A critic naming something
  product-feel TWICE is a stop condition, not a point to concede rhetorically and route around; and
  "I'll surface it prominently in the handoff" is not handing the fork back, because by then the user
  is being asked to approve or discard something that already exists.**
  → [[feedback-a-declined-product-question-does-not-go-away]]

- **A USER REQUIREMENT NEVER OUTRANKS RULE 1 (USER RULE, 2026-07-30).** Verbatim: *"If one of my
  requirements blocks a per rule 1 better solution, then drop that requirement."* A stated preference —
  a budget, a scope wish, a "lets not do X" — is an INPUT to the design, not an axiom of it. The moment
  you write *"X would be the right fix, but the user said not to"*, that requirement is a candidate for
  dropping; building the second-best thing around it is the same failure as a suppressive filter.
  **Look FIRST: drop it, build the proper fix, and NAME the dropped requirement in the handoff —
  silently overriding a stated preference is the worse failure. Covers requirements about HOW; does NOT
  cover a product decision about WHAT the user wants.** → [[feedback-drop-my-requirement-if-it-blocks-rule-1]]

- **RUN THE `/qf` LOOP TO CONVERGENCE — do not hand it back every round (USER, 2026-07-30).** Verbatim,
  after a third consecutive hand-back: *"Why are you always stopping running qf, run qf."* The loop's
  terminal state is the critic's **"that holds"**, not "the primary has something to report"; handing
  back makes the user the scheduler for a process with its own stopping condition. Measured on the pass
  that prompted it: rounds 1-11 ran one at a time with a hand-back each; rounds **12-22 ran back to
  back and EVERY ONE landed a material finding**, three of which would have shipped broken — an exclude
  set that was a silent no-op, a deny table that would have made Thai/Tamil/Thaana unwritable, and a
  security bound guarding one of three surfaces. Convergence came at round 22. **Look FIRST: stop only
  on "that holds", on a question only the user can answer, or on a finding that changes the ask —
  never on "the findings are getting narrower" (rounds 17-21 all looked narrow and all landed).**
  `memory/feedback_run_the_qf_loop_to_convergence.md`

- **A protective table defined by a PROPERTY can outlaw the thing it protects.** 2026-07-30: three
  `/qf` rounds refined a name-DENY table defined as `Mn ∪ Me ∪ Mc` (to stop `"A"+U+0301` folding
  differently from `Á`), arguing about its derivation, its font-independence and its migration effect.
  Nobody printed its **contents** until round 15: censused against the shipped faces it is **337
  codepoints — THAI 16, TAMIL 14, THAANA 11, ARABIC 19, HEBREW 8** — and Tamil needs `Mc` vowel signs
  while Thaana is written *entirely* in `Mn`, so the table would have made those scripts **unwritable
  as names in the very commit whose headline advertises them**. A property-defined table reads as
  principled, so review interrogates its derivation and never its membership.
  **VINDICATED AND PARTLY CORRECTED 2026-07-30 (`244b1320`).** The core holds and then some: the marks
  were ADMITTED, and five scripts stopped rendering as base letters with boxes where their marks belong.
  But this row's own remedy clause was wrong, and its numbers were wrong. Measured: **844 is the RAW
  two-part decomposition count** (30 are `Composition_Exclusion`, so 814 compose); "41 marks that ever
  compose / 296 that never" are really **35 / 302**; and **NFC was DROPPED, not built** — its premise
  (`"A"+U+0301` is pixel-indistinguishable from `Á`) is false in our renderer, 1 of ~3,560 face-pair
  combinations, because ImGui does no shaping and there is no GPOS anchor. So "reach for NORMALIZATION
  before prohibition" is the wrong generalisation from a right instinct: the correct move was neither
  denying nor normalising, but **measuring whether the defect exists here at all** — it did not. See
  `memory/lesson_a_standards_equivalence_is_not_your_renderers.md`.
  **Look FIRST: print the members intersected with the domain you
  actually ship; ask what the table forbids a legitimate user from doing; and treat a review that keeps
  refining HOW to build a thing as a signal nobody asked WHETHER it should exist.**
  `memory/lesson_a_protective_table_can_outlaw_what_it_protects.md`
- **A library can SYNTHESISE the very thing your filter governs, on a path the filter does not sit
  on.** 2026-07-30, the first live run of the new superset invariant: our `GlyphExcludeRanges` table
  excludes U+0009 (TAB is `Cc`, hence `no-ink`), four independent checks agreed, and the atlas baked
  it anyway on every peer. `ImFontAtlasBuildSetupFontBakedBlanks` **synthesises** the tab glyph from
  the space glyph's advance and calls `ImFontAtlasBakedAddFontGlyph` with **`src == NULL`**, so it
  never reaches `ImFontAtlasBuildAcceptCodepointForSource` and no exclude list on any config can
  suppress it. The four checks were all correct — they were checks about the TABLE, and the table was
  never consulted. **Look FIRST: a filter is a property of the paths that CONSULT it, so grep the
  dependency for every caller of its add/produce primitive and see which ones route through the
  predicate; when one bypasses, ask what the invariant is FOR rather than which value to exempt — a
  codepoint exemption would have blinded the instrument to the next synthesised glyph, whereas
  "was it RASTERISED" (`PackId != Invalid`) is self-maintaining, and it also distinguishes this from
  the case where the SAME codepoint is a genuine offender (U+0009 is in the FSEX300 and Roboto cmaps,
  and reappeared as a real one under the no-exclude drill).**
  `memory/lesson_a_library_can_synthesise_the_thing_your_filter_governs.md`
- **A text-scanning gate reads prose as code — including the prose that documents the gate.**
  2026-07-30, `tools/text/atlas_regime_gate.ps1`'s first run, two independent failures that CANCELLED
  OUT in the log: it reported `clears=1` on a tree with zero flag clears (the comment explaining the
  deletion quotes the deleted line verbatim, and a regex cannot tell code from documentation — the
  better the deletion is explained, the more likely the gate fails on it), and then printed **PASS**
  anyway, because its escape hatch matched `MEASURED-UPLOAD-VERDICT:` **unanchored** and the design
  doc's own sentence *describing that escape hatch* satisfied it. **An escape specified in prose is
  satisfied by its own specification.** Only the injected `-Drill` control found either. **Look
  FIRST: strip comments before classifying source text (two regexes, and it fixes both directions —
  a clear hidden in `#if 0` was equally invisible); anchor any escape token and require a payload
  (`(?m)^TOKEN:\s*\d{4}-\d{2}-\d{2}`), so writing the exception is a dated act and not an accident
  of phrasing; and read a drill's GREEN lines as failures.**
  `memory/lesson_a_text_scanning_gate_reads_prose_as_code.md`
- **A sentinel-terminated list cannot contain its sentinel — and zero is a legal codepoint.**
  2026-07-30: a designed, reviewed and ten-rounds-argued `GlyphExcludeRanges` table would have excluded
  **nothing**. The array is zero-terminated (`imgui_draw.cpp:4539-4542` and the sizer at `:3111-3113`
  both walk `while (p[0] != 0)`), and the table began at `no-ink`'s first member, `Cc` = **U+0000** —
  which `fontTools` confirms is in the cmap of FSEX300 and both Robotos. The walk ends at index 0,
  every codepoint is accepted, and the fold-set == render-set invariant breaks for ~1,800 codepoints.
  **Nothing would have complained:** `IM_ASSERT((size & 1) == 0)` passes (0 is even), `IM_ASSERT(size
  <= 64)` passes (0 is small), and `NDEBUG` strips both anyway. Ten rounds interrogated the table's
  membership; none interrogated its encoding. **Look FIRST: when a dependency takes a
  sentinel-terminated list, ask whether the sentinel is a LEGAL MEMBER of the value domain (for
  codepoints, offsets, indices and IDs it usually is); check the FIRST element specifically, because a
  sentinel at index 0 makes the feature vanish rather than half-work; and put the guard in the
  generator you own, since that is the failure nothing downstream can observe.**
  `memory/lesson_a_sentinel_terminated_list_cannot_contain_its_sentinel.md`
- **A bound placed at a render site is a site list — bound the data where it ENTERS.** 2026-07-30: a
  per-frame cap on attacker-driven glyph rasterisation was specified inside the chat FEED draw loop.
  Censused, remote text reaches the rasteriser through at least **three** surfaces — the feed, the
  overhead bubble (`hud.cpp:184`, whose `CalcTextSizeA` **bakes on a miss** before any budget could be
  consulted) and the scoreboard (`scoreboard.cpp:249`) — so the cap guarded one door of three, and the
  two it missed are the ones an attacker would use. Moving it to the receive boundary (`utf8_codec`,
  already the single owner of decoding) bounded every surface **by construction** and **deleted** the
  soft cap, the row-deferral rule and the forward-progress guarantee. The trap: the hazard was
  *discovered* in the chat feed, so the fix was designed there — and **machinery accreting around a
  mechanism is evidence it is at the wrong layer, not evidence it is maturing.** *Look FIRST:* census
  every reader of the attacker-controlled value before bounding its effect; prefer the ingestion
  boundary; and note the follow-on — a boundary-side ledger needs its own thread and lifetime rules
  (ours had to be a game-thread monotone set, not a read of the render thread's `IndexLookup`).
  `memory/lesson_a_bound_at_the_render_site_is_a_site_list.md`
- **A shared cache is priced by the SUM over live configurations, not by one slice.** 2026-07-30: an
  atlas-capacity discharge priced the glyph set at ONE font size and reported a 3x margin. The atlas is
  one texture shared across every live size, and its GC is **pressure-triggered** — `DiscardBakes` has
  exactly two call sites, `MakeSpace` (`imgui_draw.cpp:4244`) and `TextureCompact` (`:4306`) — so the
  resident set is everything drawn since the last pressure event. Re-measured as the sum over the sizes
  that can carry remote text: 0.345x + 0.282x + 0.228x = **0.856x of the ceiling, a 17% margin, not
  200%**. The same session had already retired a 12.8 Mpx figure that erred the *other* way by summing
  sizes no single surface can demand. **Look FIRST: ask "resident set = data x WHAT?" and enumerate the
  second axis (sizes, DPI scales, LODs, formats, per-peer variants); find the eviction TRIGGER before
  trusting a bound, because "unused for N frames" can be pressure-only; and report the margin, since
  17% and 200% imply different amounts of instrumentation.**
  `memory/lesson_a_shared_cache_is_priced_by_the_sum_over_live_configurations.md`
- **A pass can measure every MECHANISM and never measure the DELIVERABLE.** 2026-07-30: nine `/qf`
  rounds on the ImGui atlas flip measured the init-time sampling instant, the 64-value exclude cap, a
  per-rescale leak, an unbounded fence wait, which face carries U+E0B0, three asserts `NDEBUG` strips —
  and corrected sixteen false claims. **Not one round asked what appears on screen that did not
  before.** The design's own summary said the commit "ships zero new visible glyphs"; round 10 refused
  the premise and two greps settled it — today's fold table is 2,517 codepoints and the em dash, both
  curly-quote pairs, the ellipsis and the ruble sign are **outside** it, while the shipped faces union
  to 9,478 and carry all four (plus Hebrew/Thai/Arabic). The flip turns **+4,741 codepoints from
  fallback box into glyph for zero new DLL bytes** (this row said +5,078 until the build corrected it
  on 2026-07-30: that figure priced an exclude set round 11 later rejected; shipped is 7,258 - 2,517) — the user's actual ask — and was about to be built
  describing itself as delivering nothing. It survived because it was **true of the previous build**
  (s15 deliberately clears the flag) and rode into the section about the commit that deletes that
  clear, and because it was **pessimistic**: every review reflex here is tuned to catch optimism, so an
  understatement passes unchallenged. **Look FIRST: state the deliverable in the user's own units and
  measure it like any other claim; re-derive any sentence copied across a build boundary; give a
  pessimistic status claim the same evidence bar as an optimistic one.**
  `memory/lesson_a_pass_can_measure_the_mechanism_and_never_the_deliverable.md`
- **"BUILT, drilled, green" is a statement about the DRILLS, not about the code.** 2026-07-29: chat
  history shipped with two purpose-written drills PASSing 4/4, both shown RED under injection, zero
  probe warnings, a measured frame rate — and a `/qf` phase IMPLEMENTATION pass over the REAL DIFF found
  **twelve defects in four rounds**, one certain to fire in the first 30 seconds of a hands-on. **Not one
  of the twelve would have turned either drill red.** Two earlier `/qf` passes over the design BRIEF (21
  and 17 rounds) found none of them either. A brief is the primary's own prose about its own plan and can
  be argued with forever; code can only be measured — and drills written FROM a design, by its author,
  inherit its blind spots by construction. **Look FIRST: run the implementation pass over the diff before
  a human sees the feature, and never report a drill verdict as the feature's status — state what it
  asserts AND what it therefore cannot see.** An `--inject` control proves ONE BIT (this detector is not
  blind to this one defect), never coverage.
  `memory/lesson_built_and_drilled_is_not_the_same_as_correct.md`
- **A harness WORKAROUND silently deletes the path it works around.** 2026-07-29: `mp.py`'s `_type_chat`
  closes chat with Enter-on-empty, commented "NOT Escape — Escape raises the pause menu and suppresses
  the HUD pass". Correct reasoning; it also removed the user's OWN specified close path ("close like
  minecraft" = ESC) from all coverage, where three stacked defects were later found by hand. The
  instrument was never blind — it was ROUTED, and a routed instrument reports nothing about the road it
  did not take. Worse, the path was avoided *because the product behaves differently there*, which is
  exactly the condition that deserves a test. **Look FIRST: grep your own harness comments for "not X",
  "rather than", "avoid" — each is a coverage debt written in the imperative; print the drill's coverage
  BOUNDARY next to its verdict.** Fifth member of the instrument-blindness family: it can see, and it
  went around. `memory/lesson_a_harness_workaround_removes_a_path_from_coverage.md`
- **ONE capacity expressed in THREE places will disagree.** 2026-07-29, `chat_feed`: `CapRetained` allows
  `kMaxRetained*2` = 200 while a reader is paged back, `Republish` publishes at most `kMaxRetained` = 100
  from the FRONT, and `Snapshot::lines` is sized `kMaxLines+kMaxRetained` = 106 and cannot hold 200 at
  all — so rows 101+ are storable, unpublishable and invisible, and a live row overflowing into that zone
  **disappears off the screen**. Each site is locally right (memory / per-tick copy cost / fixed
  allocation) and two spell the constant identically, so a grep makes them look consistent. **Look FIRST:
  a conditional multiplier on a capacity (`x * (frozen ? 2 : 1)`) is a SECOND capacity wearing the
  first's name — when you raise a ceiling, read every site that bounds the same set by DIRECTION (stores
  / copies / publishes / allocates). And suspending ONE exit from a set does not suspend the others.**
  Capacity-shaped instance of `[[lesson-one-name-for-two-quantities]]`. **SECOND INSTANCE 2026-07-29,
  the same pass failing its own lesson: the fix's own direction-grep listed `Republish` under "copies"
  and never opened ITS bound (`chat_feed.cpp:328`), so raising `Snapshot::lines` alone would have
  re-created the identical invisible zone — THREE sites move together. And a capacity can disagree
  ACROSS A PEER BOUNDARY, which no single-process grep finds: the record's cap is the PUBLISHER's
  ceiling, the joiner's `CapRetained` is the HOLDER's, and a joiner is NOT paged back — so the
  publisher's ceiling must equal the holder's UNPAGED ceiling or the seed is evicted on arrival.**
  **THIRD INSTANCE + FIXED 2026-07-29 (`293692d7`): building the fix took the count from three sites to
  FIVE. The fifth was `chat_view.cpp`'s hand-sized `kRowCap = 512` — a bound on the SAME quantity
  spelled in a DIFFERENT UNIT (wrapped rows, not lines) in another file at another layer, so no grep
  for `kMaxRetained` could ever reach it; at 206 entries it stopped the build loop after ~128 and
  paging could never reach the oldest history. So the name-grep is necessary and NOT sufficient — also
  walk the data store → publish → layout → draw and check each hop's own bound. Now one BASE
  (`kMaxRetained`) + one NAMED allowance (`kRetentionFreezeFactor`, `chat_feed.h:55`), everything
  derived and `static_assert`ed (`chat_feed.h:64`), and the reader's-window mechanism DELETED rather
  than fixed.**
  `memory/lesson_one_capacity_expressed_in_three_places_will_disagree.md`
- **A font-coverage gap can be UNICODE's hole, not the font's.** 2026-07-29: adding Greek to
  `build_repertoire.py`'s `BASE_RANGES` made the `EXPECTED_BASE_GAP` gate FAIL with nine "missing"
  codepoints — `U+0378, 0379, 0380-0383, 038B, 038D, 03A2`. **All nine are permanently UNASSIGNED in
  Unicode**, so no font can carry them and the embedded faces were in fact complete over every assigned
  Greek character; the earlier "Greek 135/144" figure was exactly this arithmetic (144 block slots − 9
  reserved) and reads like a 94% shortfall when it is 100%. A block is written `(0x0370, 0x03FF)` like a
  dense range, so a set-difference against a cmap returns real holes and reserved slots mixed and
  indistinguishable — the same output carried one TRUE hole (`U+A69E`, a Cyrillic letter we really lack)
  formatted identically. The false positives push toward the expensive wrong fix (hunt a fifth donor,
  ship more bytes, narrow the range). **Look FIRST: `unicodedata.name(chr(cp))` raising `ValueError` is
  the one-line assigned/unassigned test — run it before treating a gap as a font deficiency, record the
  unassigned ones in the expectation set WITH the reason, and never ask an atlas for a codepoint nothing
  can draw (it bakes nothing and renders as the fallback box).**
  `memory/lesson_a_coverage_gap_can_be_the_character_sets_own_hole.md`
- **A per-frame filter inside a loop you MEMOIZE may be structural, not cosmetic.** 2026-07-29
  (`293692d7`, defect #10): `chat_view`'s word-wrap loop carried
  `if (drawnAlpha(i) < kAlphaFloor) continue;`. Alpha changes every frame, so keying the memo on it
  defeats the memo — and dropping it looks free, because a fully transparent row draws nothing. But the
  emitted rows feed a fixed array that the ENTIRE paging computation indexes (`bottom`, `shown`,
  `floorBottom`, page size, the pin-anchor search): a filtered row does not merely draw nothing, it does
  not OCCUPY A LINE, so letting it through leaves a visible GAP where a faded message was, everything
  below shifted. Memoisation forces you to enumerate a loop's true inputs and the pressure runs one way
  — toward shrinking the key — so a term gets reclassified as noise exactly when it is load-bearing.
  **Look FIRST: ask what CONSUMES the loop's output, not what the output looks like; if anything
  downstream indexes or counts by emission order, every filter is part of the contract. And when a
  per-frame term blocks a memo, do not delete it — find the condition under which it CANNOT fire
  (here `reveal >= kAlphaFloor` makes filtering impossible, so the memo applies exactly where the cost
  is and the small filtered case just rebuilds).**
  `memory/lesson_a_filter_inside_a_loop_you_memoize_may_be_load_bearing_for_indices.md`
- **A requirement about CONTENT is not a requirement about AUTHORITY.** 2026-07-29: the user wrote
  "players should get all history, including chat event feed messages" — a statement about what history
  CONTAINS. The design written from it opened "the lobby record OWNS every history line" and derived a
  host-authorship architecture: the `Keep` enum deleted, two client mirrors deleted, both slot-0
  exclusions moved, two defects dissolved, one new defect found, proto bumped. The next pass's round 1
  asked which of those the ASK needs: **only the proto bump.** Everything else was manufactured by the
  authority reading — which was also WRONG, on an impossibility. The trap is that the authority reading
  is the more engineering-shaped one (it names an owner, an invariant, a seam) and it GENERATES WORK,
  which feels like progress. **Look FIRST: name the axis a user statement constrains — WHAT (content) /
  WHO (authority) / WHEN (ordering) / HOW MUCH (capacity) — and write it down before designing. The
  tell for this error: a one-sentence clarification that produces deletions and an authority migration;
  a clarification normally SHRINKS the design.**
  `memory/lesson_a_reframe_about_content_read_as_a_claim_about_authority.md`
- **~~An event that narrates a LINK's death cannot be authored across that link.~~ FALSIFIED BY
  MEASUREMENT 2026-07-29 — the surviving lesson is the OPPOSITE one.** The original claim: the host
  cannot author `"X left the game"` on a TIMEOUT, "over the link whose failure the sentence describes".
  A whole architecture (RECORD-ONLY) was built on it. Then the user said *"just follow what minecraft or
  mta does"*, and the vendored source settles it: **`CGame.cpp:1581-1586` broadcasts `CPlayerQuitPacket`
  to all joined peers EXCEPT the parting one, INCLUDING on `QUIT_TIMEOUT`** — and the client composes
  the sentence locally from a reason enum (`CClientGame.cpp:3393-3415`). The sentence never needed the
  DEAD link; it rides everyone ELSE's live links, and only the host can see the timeout at all. The
  error was conflating **"the departing peer cannot be told"** (true, and nobody needs it) with
  **"the host cannot author it"** (false, and MTA has shipped the opposite for 15+ years). The residue
  of the original claim governs exactly one line — `"the host left"` — which is a PRIVATE local notice
  and was never part of the lobby's record. **Look FIRST: an impossibility argument about a message and
  a link must name WHICH RECIPIENTS the link serves. "Cannot be delivered to X" is not "cannot be
  authored"; check the other N-1 recipients before deriving an architecture from it — and check the
  prior art before deriving one at all.**
  `memory/lesson_a_link_death_sentence_cannot_ride_the_link.md` (updated with the falsification)
- **A required PARAMETER can be the ANSWER to the question you are about to re-ask.** 2026-07-29: six
  `/qf` rounds went into a private-constructor type to replace `chat_feed`'s `Keep` parameter, on the
  (normally correct) smell that a per-site classification argument is a habit rather than an invariant.
  `chat_feed.h:13-18` had already recorded the experiment that settles it: a data predicate WAS tried
  and **"got 12 of 15 wrong, failing on exactly the lines that matter"**. The parameter is a measured
  conclusion, and a private ctor whose friend list is the admitted sites is that parameter in a type's
  clothes. The primary had even QUOTED that line earlier in the same pass, for a different purpose.
  **Look FIRST: grep a parameter's declaration for a comment explaining WHY it is a parameter before
  replacing it. Then ask which of three properties is actually missing — mandatory / visible /
  CENSUSABLE. A required parameter already gives the first two; most "make it a type" designs are
  really after the third, which a gate delivers without touching the type.**
  `memory/lesson_a_parameter_may_exist_because_a_predicate_was_already_tried.md`
- **A seed-only record can give a JOINER more history than someone who was present.** 2026-07-29: the
  chat record admitted `"<nick> was turned away: <reason>"`, which is HOST-ONLY
  (`player_handshake_version.cpp:116` gates on `role()==Host`; the refused client gets a popup). Present
  clients never see it — but a peer joining an hour later would receive it in the seed, making the
  newcomer's history a strict SUPERSET of the history of someone who was there the whole time. The
  census asked "is this a shared-world fact the host observes?", which turned-away passes; it never
  asked **who else already knows**, which is a property of DELIVERY, not of the fact. Invisible from
  either side alone — only holding a present client's view next to a joiner's exposes it, and no drill
  compares two peers' histories to each other. **Look FIRST: for any join-delivered record, census the
  DELIVERY per row — which peers learn of this LIVE? A `role() == Host` guard on a push site is the
  tell. Admissibility rule: a surface belongs in a seeded record only if every present peer already
  learns of it live.** `memory/lesson_a_seed_only_record_gives_the_joiner_more_than_the_present.md`
- **An instrument's assertion WINDOW must not be movable by the thing it measures.** 2026-07-29
  building chat history: the drill's window was bounded by a log marker emitted at the END of the draw
  function, BELOW an early return taken when there was nothing to draw. On the INJECTED run (history
  emptied) the marker did not fire until the next message arrived — so that message landed OUTSIDE the
  window it was supposed to be inside and an unrelated assertion went red for the wrong reason. The
  clean run could never expose it: with content present, marker and surface state agree exactly. **Look
  FIRST: ask literally "can the bug I am testing for delete this marker?" — put window markers on the
  LIFECYCLE state, above every content-dependent return; and read the injected run's failures one by
  one, naming which the injection should legitimately break.** This is the fourth instrument-blindness
  family: it can SEE the phenomenon and still frame it out.
  `memory/lesson_an_instruments_window_must_not_move_with_what_it_measures.md`
- **A state change a later clamp undoes has still fired its SIDE EFFECTS.** 2026-07-29: PgUp set the
  chat scroll to PINNED inside the key handler; a clamp two statements later moved the view back and
  un-pinned it. Net state: unchanged, correctly. But the setter LOGS an edge and freezes the store's
  retention, so a view that never moved announced a successful page-back and the drill believed it.
  Set-then-correct is idiomatic and harmless for a plain variable, and unsafe the instant the setter is
  a function. **Look FIRST: before writing `X = v` early and correcting below, OPEN the setter — if it
  is not an assignment, compute an intent and commit ONCE after every clamp. If you are testing the
  same condition on both sides of a clamp, the order is wrong.**
  `memory/lesson_a_speculative_state_change_still_fires_its_side_effects.md`

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
  count before building. **2026-08-25 adds a third level: the SEVERITY is a claim too, and it is the
  one you are most likely to accept.** An audit filed a CRITICAL whose arithmetic was exactly right
  and re-derived independently — and whose severity was wrong: the constant it attacked TIGHTENS the
  bound rather than loosening it, so the behaviour described was inside what the module already
  declares. Accepting the label would have added an invented constant to a MEASURE-ONLY build whose
  stated purpose is to measure the number that constant needs (`[[feedback-probe-dont-guess-rule]]`).
  What was real in the finding was something else entirely, one level over — and finding THAT is what
  produced the fix. Two sentences of arithmetic settled it ("what does the bound alone permit over the
  same interval?"). A correct calculation attached to the wrong verdict is more persuasive than either
  half alone, because the calculation checks out and the verdict rides in behind it.
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
  **SECOND INSTANCE, DIFFERENT AXIS (2026-08-22, the RTSS/OBS overlay arc):** the blind spot is not
  always an omitted ELEMENT — it can be the wrong PIPELINE. The acceptance gate first proposed for
  "OBS can't capture our overlay" was `capture_window.ps1`, i.e. `PrintWindow`/`BitBlt` = the
  **DWM/GDI** path; OBS game-capture is an inline **Present-hook backbuffer copy**. The entire defect
  is about WHERE in the Present chain our pixels land, so a DWM grab cannot adjudicate it — only real
  OBS can. (The RTSS half of the same arc IS answerable by a window grab: "is the overlay in the
  rendered frame at all".) *Look FIRST, additionally:* **if the claim is about WHERE IN A CHAIN
  something happens, the instrument must live in that same chain.**
  **THIRD INSTANCE — A FILTER IS AN INSTRUMENT (2026-08-23, the crash-dump module list):**
  `tools/debug/parse_dump.py` printed "modules of interest" filtered by matching the **BASENAME**
  against a loader-shaped key set (`main.dll`/`ue4ss`/`shim`/`xinput`/`votv`/`dwmapi`/`crash`) —
  right for the question it was born for (which UE4SS mod is which), wrong for the question the 19:17
  dump was later re-read against (who else is on the DXGI Present chain). `NahimicOSD.dll` was loaded
  in that dump the whole time and **could not appear**: it matches none of those keys and lives in a
  vendor folder (`C:\ProgramData\A-Volute\...`) whose basename says nothing. Nastier than the camera
  cases because the tool is correct AND its output is a positive list, which reads as complete — a
  section headed "modules of interest" quietly asserts that what is absent is uninteresting. Fixed by
  matching the FULL PATH against a widened graphics/injector key set and printing the path.
  *Look FIRST:* **when you re-use a tool to ask a question it was not built for, audit its FILTER
  before you trust its silence.** SIXTH member (2026-08-23, the seeds drill's RED-1): a
  TIMING-dependent drill whose scripted action RACES the phenomenon's anchor -- the "in-window"
  email authored 3 s after connect landed BEFORE the save snapshot, rode the save, and the loss
  case silently went unexercised while the run "passed" arrival. The ORDERING is the phenomenon:
  assert the in-log ORDER (authored-line AFTER the "captured at blob instant" line), not arrival.
  SEVENTH member, the INVERSE polarity (2026-08-23, the smoke4 "relay gap"): a verdict window
  anchored on the last client's wire-CONNECT while the phenomenon (its first relayed pose) needs
  save transfer + world load + ~13 s ready→pose INSIDE it — the smoke save grew to ~19 MB and the
  window silently starved the last joiner, producing a false POSITIVE bug ("RELAY GAP") that got
  filed in three docs as pre-existing. Same bytes, 120 s window = clean PASS; relay healthy. Fix:
  anchor the window on every client's world-ready edge (mp.py `--ready-timeout`). *Look FIRST:*
  **a growing input (save/world/log) silently re-opens any wall-clock window that was once wide
  enough — anchor verdict windows on the pipeline's own settle edge, not on wall clock.**
  COROLLARY (same day, the R-2 acceptance): an instrument that PERTURBS the system contaminates
  the gate it certifies — ForceSyncFullPass's settle reset queued ~15 re-settle fulls and dragged
  the numeric gate 10.0x → 5.8x; force exactly the one action (a one-shot flag), never a state
  reset. The same drill falsified grime's "collision-free grid" comment by measurement
  (1,023 decals → 1,021 cells).
  EIGHTH member (2026-08-23 eve, R-2b): **a gate observable that counts ATTEMPTS at the call
  site cannot go red** — the reseed summary's `bumps=` was incremented beside
  `BumpSeedGeneration_()` instead of inside it past the mute; the RED-calibration run showed
  `bumps=19` with the generation FROZEN. The counter must live past every early-out of the
  mechanism it certifies; both R-2b instrument defects were caught ONLY by running the gates
  red/loaded, never by green paths.
  `memory/lesson_an_instrument_blind_to_the_phenomenon_always_passes.md`
  - *TWO MORE MEMBERS 2026-08-24 (A34), both about a SILENT instrument being indistinguishable from a DISABLED one, which cost two smoke runs in one session:* the `store_table_probe` ini key landed **inside a comment line**, so an empty log read as "ran, found nothing" rather than "never enabled" — it now warns once when ENABLED but unresolved; and `order_selftest`'s settle was a **tick count copied from another probe** (1800), which never elapsed inside a 26-second connected window because a "tick" here is a frame — it is now wall-clock and announces ARMED once. **If an instrument can be off, it must say when it is ON.**

- **A PER-ITEM TIME BUDGET THAT EXCLUDES THE AFTER-LOOP DELIVERY HALF IS NOT A BUDGET.**
  Measured 2026-08-23 (R-2b acceptance run 1): the reseed drain's ~1 ms QPC loop bounded
  nothing — `DeliverLateRegisteredProps` ran AFTER the loop on the whole chunk, landing ~800
  adoptions' payload+send on ONE tick (140/254 ms [WALK-TIME], the exact stall class the drain
  existed to remove); and a per-8-items clock check let 8 back-to-back ~170 us items overshoot
  2.4x. *Look FIRST:* any budgeted/sliced drain — verify the WHOLE per-item cost (including
  express/broadcast halves staged "for later") executes inside the clock check, granularity
  matched to the worst single item. Reference impl: `prop_census.cpp DrainReseedQueue`.
  `memory/lesson_a_budget_that_excludes_the_after_loop_delivery.md`

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
  it.** *(Re-verified 2026-07-30 against **v1.92.9**: the strip is now `imgui_impl_dx12.cpp:**987**`, and
  `:894` additionally asserts "Only 1 simultaneous texture allowed with legacy ImGui_ImplDX12_Init()
  signature!". The finding is unchanged and was the decisive fact of the upgrade measurement — but note
  what it does NOT say: it is DX12-only, so a straight port ships **DX11 WITH the dynamic atlas and DX12
  without it**, i.e. one build with two drawable repertoires selected by the player's RHI.)*
  Measured 2026-07-28 pricing ImGui 1.92: `imgui_impl_dx12.cpp:984` (v1.92.8 line) does
  `io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures` when the legacy `Init` signature is used. It
  does not assert; it degrades. The upgrade would have paid its whole cost (a submodule bump, a semantic
  sweep of four kept redirects) for none of its benefit — 16-64 MB and a 139-416 ms atlas rebuild instead
  of 0.25 MB — **DX12-only, clean compile, no warning**, on the RHI least likely to be tested. Worse, the
  headless probe SET THAT FLAG ON ITSELF, so it measured a configuration the product never reproduces
  (`grep RendererHasTextures src/` = zero). *Look FIRST:* when an upgrade's VALUE rests on a capability
  flag, grep the vendored backend for `&= ~<FLAG>` before believing any benefit number, and ship a boot
  assertion that the flag survived on EVERY backend.
  **UPDATED 2026-07-30: this row's recommendation is now BUILT** — `ui/atlas_watch.cpp` asserts the
  regime every frame and `tools/text/atlas_regime_gate.ps1` censuses it in CI, so do not re-propose
  it. Also stale in the fact base: `grep RendererHasTextures src/` no longer returns zero (the flag
  is READ by the watch and CLEARED nowhere, which is the point).
  `memory/lesson_a_capability_can_be_silently_stripped_not_asserted.md`

- **An upgrade can demote your INPUT to a hint, with a clean compile.** MEASURED 2026-07-30 against ImGui
  **1.92.9**: `ImFontConfig::GlyphRanges` — the array we pass on every font add, and on which arc D2's
  whole *"one generator mints both what FOLDS and what BAKES, so they cannot drift"* construction rests —
  is still a public field, still accepted, still warning-free, and **read in exactly ONE function**
  (`ImFontAtlasBuildLegacyPreloadAllGlyphRanges`, `imgui_draw.cpp:3540`) called only under
  `if (atlas->RendererHasTextures == false)` (`:3498-3499`). With the dynamic atlas on it bounds
  **nothing**: the atlas draws whatever the face cmap carries (8,148 cp for our faces vs the 2,517 we
  fold). The on-demand path doesn't consult it at all — `ImFontBaked_BuildLoadGlyph` (`:4562-4571`) gates
  only on `Locked`/`NoLoadGlyphs` and uses `GlyphExcludeRanges`, a **different field**. So a naming
  invariant is not violated but **DISSOLVED**, and a prior 9-row classified diff had filed this as
  *"ranges unnecessary — 1 site — and this is the win, not a cost"*. **CORRECTED 2026-07-30 by
  measurement:** the same-day corollary here claimed that because on-demand baking is not gated on the
  flag, a mechanical port keeping the old init is *"not behaviour-identical"*. **It is.**
  `UpdateFontsNewFrame` (`imgui.cpp:9089-9094`) turns the missing flag INTO `atlas->Locked`, which IS
  `BuildLoadGlyph`'s first test — so inside a legacy frame nothing bakes and drawing matches 1.91.5
  exactly (probe arm L: 7 out-of-repertoire codepoints unbaked, 0 texels diverged, `TexIsBuilt` held).
  The regime is a TIME WINDOW, and only code outside a frame escapes it —
  `[[lesson-a-capability-flag-may-be-a-per-frame-lock]]`. *Look FIRST:* grep the new version for **who
  READS each field you set**,
  not whether the field still exists; if the only reader sits behind a capability guard you now have two
  behaviours in one build. `memory/lesson_an_upgrade_can_demote_your_input_to_a_hint.md`

- **Querying a lazy cache POPULATES it — so the question you asked is not the one answered.** MEASURED
  2026-07-30: `ImFontBaked::FindGlyphNoFallback` (`imgui_draw.cpp:5361-5373`) sets `LoadNoFallback = true`
  and **calls `ImFontBaked_BuildLoadGlyph`**. The `NoFallback` in the name governs failure behaviour, not
  mutation. Under 1.91.5 the same call is a pure read of an eagerly-built atlas, so `ui/fonts.cpp`'s boot
  selftest means *"the eager bake included X"*; under 1.92 it means *"this face CAN produce X"* — a cmap
  fact `build_repertoire.py` already knows at generate time — and the instrument now **mutates its own
  subject** (four glyphs baked) before later assertions read atlas bytes. It does not become
  pass-by-construction (a codepoint no face carries still returns NULL, so `U+4E00` stays a valid RED) but
  its verdict must be **relabelled or it lies about what it proves**. **RESOLVED 2026-07-30 (`b33aae30`), and the damage was bigger than a label:** the bake also flips `TexIsBuilt` to false, and a legacy backend uploads once, so from that one query EVERY later frame raises the `imgui_draw.cpp:2815` user error permanently. The fix was NOT a relabel — the assertions were split by what they actually ask: *"can this build DRAW X"* -> `ImFont::IsGlyphInFont` (`imgui_draw.cpp:5391`, a pure cmap walk, which is what finally made a RED case possible: `U+4E00` must be ABSENT), and *"did a COLOURED emoji reach the texture"* -> a baked lookup behind a helper that REFUSES any codepoint outside the repertoire. *Look FIRST:* before founding an
  assertion on an accessor into any lazily-populated store, open the accessor body and check for a
  build/insert on the miss path. Sibling: `[[lesson-a-fallback-glyph-must-be-asked-for]]` — presence in the
  FONT is not presence in the ATLAS, and now asking about the atlas changes it.
  **UPDATED 2026-07-30 (`0d84cc5a`): the cited code MOVED.** The selftest is no longer in
  `ui/fonts.cpp` — it lives in `ui/atlas_watch.cpp`, runs IN-FRAME, and the "refuse any codepoint
  outside the repertoire" helper is RETIRED (post-flip it would have made the only COLR instrument
  green-by-skip). It now bakes one emoji deliberately; every presence check is cmap-only
  (`IsGlyphInFont`). The lesson's core — querying a lazy cache POPULATES it — is unchanged.
  `memory/lesson_querying_a_lazy_cache_populates_it.md`

- **A clean build after a major dependency bump may be riding OBSOLETE SHIMS.** MEASURED 2026-07-30:
  building the whole mod against ImGui 1.92.9 gave errors in only 3 UI files — and three call sites a
  prior classified diff had listed as breaks produced **no error at all**, because `imgui.h:4133` keeps
  `inline void PushFont(ImFont*)` (plus `CalcWordWrapPositionA` at `:3968` and `SetWindowFontScale`) inside
  `#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS` — even though the same header says the single-arg version was
  *"REMOVED"*. The same diff's *"`GetTexDataAsRGBA32`/`Build`/`IsBuilt` obsoleted — **zero** sites — free"*
  row was **wrong in the other direction**: 10 references to removed *fields* (`TexPixelsRGBA32`/`TexWidth`/
  `TexHeight`) plus an explicit `io.Fonts->Build()`. A changelog + call-site grep mis-prices in BOTH
  directions (shims absorb rows; a removed FIELD never appears in a list of removed FUNCTIONS), and its
  line numbers rot (a cited file didn't exist yet). *Look FIRST:* **don't price a bump from the changelog —
  build it** as a throwaway spike (snapshot beside the pin, `-D<DEP>_DIR`, keep the log, revert), then
  **build again with the obsolete layer OFF** — only the second build shows the true migration surface.
  Every surviving shim call is RULE-2 baggage with its own line in the plan.
  `memory/lesson_a_clean_upgrade_build_may_be_riding_obsolete_shims.md`

- **A capability flag can be implemented as a per-frame LOCK, so "the regime" has a TIME WINDOW.**
  MEASURED 2026-07-30 (ImGui 1.92.9): `ImFontBaked_BuildLoadGlyph` (`imgui_draw.cpp:4562-4571`) gates on
  `atlas->Locked`, never on `ImGuiBackendFlags_RendererHasTextures` — which reads as "the dynamic atlas is
  ungated". But `UpdateFontsNewFrame` (`imgui.cpp:9089-9094`) does
  `if (flag == 0) atlas->Locked = true`, and `UpdateFontsEndFrame` (`:6173-6175`) clears it. So the flag
  IS the gate, one indirection away, **for the duration of a frame only.** Inside a legacy frame nothing
  bakes (7 out-of-repertoire codepoints unbaked, 0 texels diverged, `TexIsBuilt` held); **outside** one,
  a single `FindGlyphNoFallback` bakes AND flips `TexIsBuilt` to 0, after which every later frame raises
  the `imgui_draw.cpp:2815` user error forever, because a legacy backend uploads once. The victim is real:
  `ui/fonts.cpp`'s boot selftest runs from `Load()`, before `ImGui_ImplWin32_Init`, always outside a frame.
  A probe that inspected AFTER `Render()` "confirmed" the wrong answer — it measured its own instrument.
  **SECOND MECHANISM, same flag, measured 2026-07-30 in the implementation `/qf`: it is also SAMPLED at
  an INSTANT during init.** `ImFontAtlasBuildUpdateRendererHasTexturesFromContext`
  (`imgui_draw.cpp:2760-2772`, called from `ImFontAtlasBuildMain` at `:3497`) copies the flag off the
  context at build time and decides the legacy preload from the copy — and `ui::fonts::Load()`
  (`imgui_overlay.cpp:295`) runs **eight lines before** the backend sets it inside `InitRenderer`
  (`imgui_impl_dx12.cpp:931` via `:303`). So deleting the clears would have flipped every LATER build and
  left the **boot** atlas eagerly preloaded, with `GlyphRanges` still load-bearing exactly where it had
  been declared dead. Upstream names it by hand at `imgui_draw.cpp:2818`. Fix = an ORDERING change, and
  it makes boot match a path already exercised every session (`MaybeRescale` calls `Load()` with the
  backend live). *Look FIRST:* ask **when** a regime holds, not just whether; grep for who WRITES the
  derived state, not only who reads the flag; **ask at what INSTANT the flag is sampled** — setting it
  after something has already copied it is indistinguishable, at the setting site, from setting it in
  time; and take a probe's observation **inside the same window** as the behaviour it claims to
  describe. **UPDATED 2026-07-30: our build no longer runs the locked regime** — both clears are
  deleted (`0d84cc5a`), so `atlas->Locked` stays false and glyphs bake during the frame. The
  out-of-frame victim this row names (`fonts.cpp`'s boot selftest) no longer exists.
  `memory/lesson_a_capability_flag_may_be_a_per_frame_lock.md`

- **The subsystem you are about to build may ALREADY BE RUNNING.** MEASURED 2026-07-30: four `/qf` rounds
  designed a ~200 LOC DX12 texture-servicing subsystem to replace `ImGui_ImplDX12_UpdateTexture`, whose
  every upload ends in `WaitForSingleObject(..., INFINITE)` (`imgui_impl_dx12.cpp:564-565`) on our render
  thread where every wait we own is bounded at 2000 ms. Round 5 measured that
  `imgui.cpp:5973` assigns `draw_data->Textures` **unconditionally** and both backends service that list
  **ungated by the capability flag** (`imgui_impl_dx12.cpp:236`, `imgui_impl_dx11.cpp:181`) — so the path
  "we were going to replace" already executes at boot and twice per rescale on a 16 MB upload, in the
  shipped build, **and had never been timed**. Every justification was a property of the POST-flip
  regime, so the subsystem was justified only by what the flip enables; and the leak it had to fix was
  one **it introduced itself** by taking ownership of a resource ImGui frees correctly. The flag gates
  the capability's QUALITY (incremental updates), not its INVOCATION. *Look FIRST:* before designing a
  replacement, grep the incumbent's call sites and ask what actually gates them — not what the flag's
  NAME implies — and if its cost is unmeasured, ship a 15-line timed wrapper at the same seam instead:
  it produces the number AND occupies exactly the seam the replacement would.
  `memory/lesson_the_subsystem_you_are_about_to_build_may_already_be_running.md`

- **An upstream assert your build STRIPS is not a guard.** MEASURED 2026-07-30: our Release build defines
  `NDEBUG`, so every ImGui `IM_ASSERT` compiles away — and THREE invariants the flip design leaned on
  exist *only* as asserts: the `GlyphExcludeRanges` 64-element cap (`imgui_draw.cpp:3115`), the atlas
  pack-failure check (`:4818`, whose `return false` degrades to **the fallback box** — symptom-identical
  to the bug the whole glyph saga exists to remove), and *"backend set Destroyed but did not clear
  TexID"* (`:2875`, the one that let a per-rescale resource+descriptor leak through a design that had
  already passed several rounds). All three fail SILENTLY. *Look FIRST:* grep how a dependency's
  invariant is ENFORCED, not whether it is stated; move the check to a layer you control (a generator
  `sys.exit` for offline data, a CI source gate for structure, our own predicate at runtime); for a
  silent-degradation invariant build a DETECTOR and prove it distinguishes the failure from the ordinary
  state that looks the same; and never sit exactly on a stripped limit (the semantically-widest exclusion
  set landed on precisely 32 of 32 permitted ranges — the set with four ranges of margin was chosen for
  that reason alone). `memory/lesson_an_upstream_assert_your_build_strips_is_not_a_guard.md`

- **A comment citing a DEPENDENCY's line number rots silently, and the confident ones rot worst.**
  MEASURED 2026-07-30 (`683f8214`): `ui/fonts.cpp` carried TWO such comments and the `v1.91.5 -> v1.92.9`
  bump invalidated both. `:175` credited *"imgui_freetype.cpp:515 refuses to overwrite a glyph an earlier
  source already provided"* for the merge-order policy — `:515` is now FreeType render-mode selection and
  the refusal is gone, though the POLICY survives at `ImFontBaked_BuildLoadGlyph`
  (`imgui_draw.cpp:4590-4602`), so the conclusion was right and all its evidence was dead. `:400` cited
  `GetTexDataAsRGBA32` + `TexPixelsRGBA32`, **neither of which appears anywhere in the 1.92.9 DX11/DX12
  backends**, and closed with *"do not 'simplify' by removing the GetTexData path"* — a stale comment
  that **forbids**, costing the next reader the dig plus the confidence to act. Same file, same bump:
  `built ? ... : "FAILED TO BAKE"` is unreachable because `Build()` returns true unconditionally. *Look
  FIRST:* after any submodule bump, grep your own tree for citations INTO the dependency and re-verify
  each; cite the SYMBOL and let the line number be a convenience; state the OUTCOME you rely on
  separately from the MECHANISM you observed; and never close a comment by telling the reader not to
  change the thing — if it is load-bearing make it a test, a gate or a `static_assert`.
  `memory/lesson_a_comment_citing_a_dependency_line_number_rots_silently.md`

- **A correction in a NEW SUBSECTION leaves the headline stale — and the headline is what gets quoted.**
  MEASURED 2026-07-30: a design doc's §2.7 led with *"+4,772 codepoints"* while §2.7a **immediately
  below**, titled *"computed for real"*, said **+5,078**. Both stood one screen apart for a session, and
  the stale figure had already propagated to a second doc (`votv-nickname-arbitration-...-2026-07-27.md`
  :1293) **citing §2.7** — the correction was invisible to whoever copied it. `+4,772` reproduces under
  **none** of five set-algebra formulations the generator's own tables can build, so it was never a
  measurement: an unlabelled ESTIMATE that a real computation replaced without deleting. A third value
  (+5,164) is also legitimate under a different subtraction, so each number now has to carry its
  CONSTRUCTION. The append-only instinct is right for reasoning and wrong for values. *Look FIRST:* edit
  the ORIGINAL sentence and put the correction note inside the same section; `grep` the tree for the
  figure you just retired before finishing; and label estimate-vs-measurement at the moment of writing.
  **SECOND INSTANCE 2026-08-25, and the worse shape: the supersession note EXISTED and was CORRECT —
  it just lived in the superseding section.** `UE4SS_ARC.md` §7.6 ended *"that argues for shipping
  skins as a **separate package**"*; §7.7c part 1, ~180 lines below, recorded the user's opposite
  decision and said outright *"this supersedes the 'ship skins separately' suggestion in §7.6"*. §7.6
  itself carried no mark and read as current — so two days later **Claude wrote two new sections of
  that same file and cited §7.6's recommendation as live, twice**, one of them calling a freshly
  measured size gap *"the strongest practical argument for §7.6's conclusion"*. Pointing FORWARD from
  the new text protects nobody: readers reach §7.6 from a grep, a heading list or a cross-doc
  citation, and never see §7.7c. *Look FIRST, for an overturned DECISION:* the stamp goes on the
  **superseded** text (both ends need marking, only one is optional); **strike** the retired sentence
  rather than leaving it plain; **keep the rationale, retire the conclusion** — §7.6's premise (a
  third-party takedown removes the whole mod) stayed TRUE and was a risk ACCEPTED, so deleting it
  would have invited re-deriving the split, while leaving it unmarked invited quoting it; and before
  citing any section, grep the file for a supersession note aimed at it — reading top-to-bottom does
  not protect you when the note is 180 lines away.
  `memory/lesson_a_correction_in_a_new_subsection_leaves_the_headline_stale.md`

- **A legacy wrapper is not a translation shim — read what it does AFTER the forward call.** MEASURED
  2026-07-30: `ImGui_ImplDX12_Init(device, frames, fmt, heap, cpu, gpu)` looks like an adapter onto the
  `InitInfo` overload, but its body (`imgui_impl_dx12.cpp:961-987`) also **creates its own command queue**
  (`:973`, `commandQueueOwned = true`) and **strips `RendererHasTextures`** (`:987`). So migrating to
  `InitInfo` silently ENABLED the dynamic atlas for DX12 only — one build, two drawable repertoires chosen
  by the player's RHI, where two peers would agree who collided and disagree what the names LOOK like. It
  was caught by a **runtime artifact**, not by reading: the second `Load()` logged
  `atlas baked in 0.0 ms (512x128)` against DX11's `1024x2048 in 72.7 ms` — a capability turning on makes
  a SMALLER, FASTER artifact, which reads like an improvement. The font selftest could not catch it (its
  lookups are guarded to in-repertoire codepoints, and a lazy atlas just bakes those). *Look FIRST:* open
  the legacy overload's body and read past the forward call; treat every parameter it SYNTHESISED as a
  decision you now own, not a blank to fill with the nearest handle.
  `memory/lesson_a_legacy_wrapper_does_more_than_translate.md`

- **A generated build constant must not be derived from data the TOOLCHAIN supplies.** MEASURED
  2026-07-30: widening the nickname fold table needs "subtract codepoints that render no ink", and the
  principled source is Unicode general category. But `Cn` (unassigned) covers **412 codepoints in our own
  fonts' cmaps** according to the generator machine's Python (3.11 ships Unicode **14.0.0**; `U+1CC21` and
  the `U+1CD00` block were assigned in Unicode 16). Subtracting them would fold visibly-distinct names to
  the sentinel AND make the fold table **a function of the generator's Python version** — two builds cut
  from one commit on two machines disagreeing about which player names collide, which is exactly the
  machine-dependence arc D2 exists to prevent, arriving through the toolchain instead of the font. `Cn` is
  NOT subtracted; the narrow set (`Cc/Cf/Cs/Zl/Zp/Zs` minus `U+0020`) is 105 codepoints and contains all
  33 of the shipped no-ink defect without naming one. Found only because the number was **computed instead
  of estimated** (+5,078, not the design's unrecorded +4,772 guess). *Look FIRST:* for every input to a
  generated constant ask "is this a property of the artifact, or of the tool reading it?", and assert the
  result's count/max so a data-table bump fails the build.
  `memory/lesson_a_generated_constant_must_not_depend_on_the_toolchains_data_version.md`

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
  **SECOND INSTANCE 2026-07-30, different root (fixed `c142d077`):** the same scenario failed the same six rows on BYTE-IDENTICAL DLLs — run 1 `FAIL (6)`, run 2 `PASS`, differing only in how long the last client took to join (35 s vs 21 s). The messages were never SENT: `T` is swallowed while any interactive surface owns input, and the gate was a `"Joined "` log line plus a flat 4 s sleep. It cost a detour hunting a font regression from the ImGui port that had landed minutes earlier. *Fix, and the generalisable part:* an instrument that INJECTS a stimulus must confirm the stimulus ARRIVED before judging the response — the sender renders its own chat line, so its own log is a receipt; wait for it and retype. A readiness gate plus a fixed sleep is a guess with a timestamp, and it fails in the direction that looks like a product bug. `memory/lesson_an_instrument_can_fail_the_feature_it_tests.md`
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
  **THIRD FAMILY MEMBER (2026-07-30): an instrument whose ABSENCE is indistinguishable from its
  SUCCESS.** `tools/mp.py:1539` asserts the font selftest by *negative* grep (`("selftest: FAIL",)`),
  which is sound only while the selftest runs unconditionally at boot; the ImGui flip makes it fire on
  an `atlas->TexData->UniqueID` edge, and at that moment "passed" and "never ran" are the identical
  log. The same file already carries the right shape 700 lines earlier — `mp.py:848` asserts the
  POSITIVE line `"config-selftest: DONE fail=0"`. The negative form is not wrong when written; it rots
  the instant the instrument becomes conditional, and nothing in the diff that makes it conditional
  touches the assertion. *Look FIRST:* assert a positive success line carrying its counts, and when a
  change makes an instrument conditional, grep for who asserts on it.
  **FIXED 2026-07-30 (`0d84cc5a`), in the shape this row recommends:** the selftest emits
  `font selftest: DONE fail=0 (8/8) -- ...` and `tools/mp.py` asserts that line's PRESENCE. Its
  must-FAIL control ran against fixtures (a log with the line passes, one without goes RED) — which is
  FIXTURE-level plus green live runs, **not** a defect-carrying build, and it executes only under
  `--assert-i18n` (`smoke_i18n` / `smoke4`), not the default `smoke`.
  **CLOSED 2026-07-30, and the closing is its own lesson: the sentence above NAMED the hole and the
  hole shipped anyway.** A defect-carrying build (font selftest mutated to fail 2 of 12 rows) was
  smoked, and the default `smoke` printed **PASS** with `fail=2` and two `[ERROR]` rows in both peer
  logs. The assertion now runs from `cmd_smoke` too, over both peer logs, and the same mutant then
  failed with all four findings. *Look FIRST:* a documented gap is not a mitigated one — when a
  sweep writes "…but only under X", that clause is a WORK ITEM, not a caveat, and the next reader
  will treat it as coverage.
  `memory/lesson_an_instrument_never_shown_failing_passes_by_construction.md`

- **A check's NAME decides where it gets called, so a scenario-shaped name is a coverage decision
  (2026-07-30, `tools/mp.py`).** Four assertions — strict UTF-8, no line formatting to nothing, no
  `selftest: FAIL`, and the positive `font selftest: DONE fail=0` — were correct, complete, and
  called from exactly **one** scenario, because the function was named `_i18n_checks`. Nothing in the
  body is about mixed scripts. A build with two deliberately-failing selftest rows and `fail=2` in
  BOTH peer logs printed `PASS` from the plain smoke; renamed `_peer_log_health` and called from
  `cmd_smoke`, the same run failed with all four findings. This is the family's fourth dimension and
  the strangest: the detector had **no defect** — every earlier instance was a check that missed
  something, this one was simply never asked. The name answers "when do I call this?" before anyone
  reads the body, and answers it wrongly and confidently; a scenario-named helper is *born* accurate
  and rots in the same commit that generalises its body. *Look FIRST:* name a check after the
  PROPERTY it asserts, never the scenario that motivated it; when a doc says "the tool asserts X",
  write WHERE; grep a new assertion's own call sites and treat "one call site for a general property"
  as the tell; and read "my mutant passed" as a finding about the HARNESS, never as a reason to
  strengthen the mutant. `memory/lesson_a_checks_name_decides_its_blast_radius.md`

- **A standard's equivalence is not your renderer's equivalence — measure the pixels, not the
  spec (2026-07-30, commit 2 of the ImGui flip).** A 22-round design specified 814 NFC
  composition pairs + canonical combining classes on the fold key, justified by one sentence:
  *"`A`+U+0301 is pixel-indistinguishable from `Á`"*. That is a claim about rendering, and it was
  never rendered. Measured through FreeType with no shaping — what ImGui actually does,
  `FT_DISABLE_HARFBUZZ` is on — the pair matches its precomposed form in **1 of ~3,560 face-pair
  combinations**, because without a GPOS anchor the mark sits at its own left-side bearing. The
  machinery would have collided things that do not look alike to prevent a collision that does
  not occur. Two more inversions in the same hour: 844 was the RAW decomposition count (30 are
  `Composition_Exclusion`, so 814 compose; "41/296 marks" are really 35/302 — nobody had run
  `normalize()`); and rasterising the WHOLE fold set found **476 pixel-identical pairs, only 23
  canonical** — the real collisions are homoglyphs Unicode has no equivalence for **The arc that followed was BUILT and then DECLINED by the user (2026-07-30): mixed-script lookalike names are wanted, not a defect. The measurement stands; do not re-open it as a bug.**
 
  (`A`≡`Α`≡`А`, `C`≡`С`, `3`≡`З`), already shipping. Note the near-miss: after NFC died, "just
  fold the 31 canonical singletons" was proposed AND approved, and it closes 23 of 476 — a site
  list inside the real invariant that survives review precisely because it is standard-derived.
  *Look FIRST:* when a justification is a sentence about what the user SEES, render it (20 lines
  of PIL) before building the table; measure through YOUR pipeline's config, not the library's
  defaults — with HarfBuzz on, the same design would have been right; census the whole space
  (hash every glyph bitmap, group by hash) before folding a subset of it; and before building on
  NFC/NFKC/confusables/collation, state which property of YOUR system it proxies and check the
  proxy holds. `memory/lesson_a_standards_equivalence_is_not_your_renderers.md`

- **A negative control proves the POLICY is right — not that anything APPLIES it (2026-07-30,
  `ui/atlas_watch.cpp`, the §7.4 probes).** Three probes were specified to catch "a missed
  `GlyphExcludeRanges` on a *specific* config". Written exactly as specified —
  `IsGlyphInFont(cp) && InExcludeSet(cp)`, cmap-pure — they are correct, they go RED under mutation,
  and **they cannot catch that failure**: `InExcludeSet` reads our table, so it answers "does the
  policy forbid this codepoint", never "did this `ImFontConfig` receive the policy". Measured: under
  `dev.atlas_no_exclude_drill` (field removed from every config, table untouched) all three stayed
  green (`fail=1`, not `fail=4`). The failure needed a fourth check — a per-source census over
  `atlas->Sources` comparing each config's list by CONTENT — which fired **4 seconds before** the
  superset invariant that was supposed to be the backstop, because the invariant must wait for
  something to be DRAWN and "a config nobody's text exercises" never triggers it. Two shape traps
  came with it: ImGui `ImMemdup`s the list (`imgui_draw.cpp:3116`) so pointer identity is false by
  construction, and comparing against the accessor rather than the generated table would pass
  `NULL == NULL` under the very drill it detects. Survived a 22-round `/qf` because a specified
  *mechanism* stops attracting the question "does that mechanism observe the stated failure?".
  *Look FIRST:* for every check say which of the two it asserts — THE RULE IS RIGHT vs THE RULE IS
  APPLIED HERE — and test a policy check by removing the WIRING, not by corrupting the TABLE.
  `memory/lesson_a_negative_control_proves_the_policy_not_the_wiring.md`

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
  **SECOND SHAPE 2026-08-25, and it is the worse one: the reframe was a FOUNDATIONAL constraint and
  the casualty sat in ANOTHER DOC, THREE MONTHS EARLIER.** `MULTIPLAYER_UI.md`'s approach table
  (2026-05-22) rejected the BPModLoader widget route for exactly one stated reason -- *"ties us to
  UE4SS"*. On 2026-08-21 the F2/D-3 decision made Multivoid a UE4SS mod **on purpose**, and
  `BPModLoaderMod` has been installed in the profile ever since; the row still read as a live verdict
  until the user asked, on 2026-08-25, for the very feature it had deferred. Its neighbour deferred
  the sibling-pak path *"until a public-server-phase widget (server browser with sortable rows)
  actually needs it"* -- both halves of that trigger came true and nothing noticed. Why this shape is
  nastier: the reframe and the casualty are in **different files**, so re-reading either proves
  nothing and the in-pass `/qf` re-derivation habit cannot reach it; the casualty is phrased as a
  settled **VERDICT**, which is the last place anyone looks for rot; and a foundational reframe has an
  unlisted blast radius by definition -- RULE 3 was cited tree-wide as the reason for many small
  choices, and D-3's ledger tracked only the ones inside its own arc. *Look FIRST:* when a
  FOUNDATIONAL constraint is reversed, do not just update the doc that owns the decision -- **grep the
  whole tree for the constraint's own wording** (`ties us to`, `depends on`, `without X`, the rule
  number) and re-read every verdict citing it, **rejected and deferred rows included**. Deferred rows
  get their own grep: they carry a *trigger condition*, and a trigger condition is a claim about the
  future that can quietly come true.
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
- **Re-anchor on the ORIGINATING ask, not the thread you are currently in (USER CORRECTION
  2026-07-29).** The user, mid-build: *"We started the chat saga when I wanted it to support glyphs,
  remember?"* The arc was glyphs → "glyphs in chat too" → a look at chat → "there's no history" → chat
  history, and that last branch consumed **five `/qf` passes, 75 rounds** (21+17+4+14+19), two full
  architectures and a build — while the ORIGINATING request had a free, measured, **explicitly ungated**
  deliverable sitting in its own NEXT list (`+LatExt+Greek`: zero bytes, +7 ms, +1,787 glyphs). It
  shipped in one commit in twenty minutes once actually looked at. **`/qf`'s ANSWERS-THE-ACTUAL-ASK
  angle did not catch this and structurally cannot**: it holds the design against the CURRENT thread's
  ask, and every round was well-aimed at chat history. Nothing ever asks *"what was the request that
  spawned THIS request?"* A sub-thread inherits full legitimacy from its parent and then competes with
  it for the entire budget, and because each round is individually correct the drift is invisible from
  inside. **Look FIRST: when a request spawns a sub-request, write down the parent and treat its NEXT
  list as LIVE WORK — read it before opening another design pass, and prefer an ungated+measured+small
  item over an interrogated+large one. Depth on a sub-thread is not progress on the parent.**
  `memory/feedback_reanchor_on_the_originating_ask_not_the_current_thread.md`
- **Five objections that share one root are ONE objection and a list.** A design was carried forward for
  two sessions as *"DEAD on five counts, three of them measured"*. Taken apart (2026-07-30): count 4 was
  **false** against the very prior art it cited (MTA hands a FILE to the OS via `AddFontResourceEx` +
  `D3DXCreateFont`; `MapCharacters` is a discovery API MTA never touches), count 2 was a **product
  question**, count 1 a **survivable cost**, count 6 an **invariant to preserve** — and count 3, the only
  real kill, was a **CONSEQUENCE** of the missing dynamic atlas, not an independent fact. A numbered list
  reads as cumulative evidence and nothing in its shape prompts you to ask whether the items are
  independent. *Look FIRST:* classify each count (kill / cost / constraint / product question / false)
  before counting it; then ask of every surviving kill *"cause or consequence?"* — **if prior art runs the
  same mechanism safely at scale, the difference between them and you IS the root.**
  `memory/lesson_several_objections_may_share_one_root.md`
- **Decompose a rejected mechanism into AXES before telling the user their choice is dead.** The user chose
  *«lets not ship them in dll, lets ship them as mta does»* — OS fonts, on demand. A later `/qf` killed
  "on demand" and the verdict was carried as *the user's mechanism is dead*. It contains **two orthogonal
  axes**: SOURCING (DLL donor vs the player's fonts — what the user actually chose) and TRIGGERING (eager
  vs on-first-sight — the only axis the objection touched). Six rounds argued mechanism against a decision
  already made, while the half the user cared about went unexamined. *Look FIRST:* write the chosen
  mechanism as a conjunction of axes, attach every objection to ONE named axis, and report per axis
  ("the sourcing half stands; the triggering half fails, because…") — never "your mechanism is dead". If
  the surviving axes still don't solve it, say so **with the reason it is mechanism-independent**.
  `memory/feedback_decompose_a_rejected_mechanism_into_axes.md`
- **A cost number must carry its REGIME, or it will be quoted in the wrong one.** Three fusions in ONE
  `/qf` pass, each after conceding the previous: eager-bake numbers quoted as the price of *demand* bake
  (demand ships **zero** DLL bytes — the trade is not freeze SIZE but **who holds the lever**, local-rare
  vs remote-unbounded); an in-game 58-80 ms measurement glued to an offline 173 ms probe cell as one
  "58-173 ms" range; and a LEGAL price (`+FULL CJK`, 4096x16384) fused with an ILLEGAL one (`EVERYTHING`
  @worst@x2.0, 4096x32768 — no font texture at all), which reads as "there is no YES branch" when there
  is. The mechanism: regimes live in the surrounding prose while numbers travel alone. *Look FIRST:* put
  the regime **in the label** (`+FULL CJK eager @default x1.0: 64 MB / 810 ms`); never merge an in-game
  number with a probe number into one interval; re-derive from the source table, never from your own
  previous summary. `memory/lesson_a_cost_number_must_carry_its_regime.md`

- **Answer a short proposal's SCOPE, not its widest reading -- a well-evidenced rebuttal of the version
  they did not propose can kill a correct idea.** 2026-08-25: the user asked **"А может графы тоже
  сделаем?"** (four words). I answered the maximal reading -- author the browser's logic in Blueprint
  graphs -- measured the MTA precedent, listed four costs and recommended against it. Their next
  message: **"Я про графы для биндинга делегатов"** -- a Blueprint purely to OWN a UFunction so a
  delegate has a target. Narrow, precise, and RIGHT: it dissolved a premise this codebase had polled
  around for months, and the final shape needed no Blueprint at all. Note the failure mode is not the
  wasted message -- it is that **"you would click every node" and "debugging a graph in a cooked pak is
  painful" are true of the wide version and irrelevant to a 3-node sink**, so a deferring user would
  have dropped a good idea to objections it never earned; confidence transfers across scopes. LOOK
  FIRST: when a short proposal touches a settled decision, **ask which scope** (one line; this is asking
  what was SAID, not which option to take, so it does not conflict with
  `feedback_no_crutch_questions_act_autonomously`) -- or failing that, **answer the NARROWEST useful
  reading first** and offer the wide one in a sentence. Watch the word **"тоже"/"also"**: it scopes an
  ADDITION to what is already agreed, not a replacement. And when a proposal attacks a premise you are
  defending, **take the premise down to the studs before defending the design on top of it**.
  `memory/feedback_answer_the_proposals_scope_not_its_widest_reading.md`

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


- **A HASH ANSWERS THE QUESTION YOU ENCODED, NOT THE ONE YOU ASKED (2026-07-30).** To answer "do
  these two codepoints DRAW the same", the instrument hashed each glyph as `(contours, ADVANCE)`.
  Advance is not ink — it positions the NEXT glyph — and including it split ten pairs that an
  independent bitmap census found identical at **59 of 59** sizes (`C`≡`Ⅽ`, `Κ`≡`K`, `ѕ`≡`ꜱ`…).
  Dropping it: 382 → 419 pairs. The extra field looked like RIGOUR ("same shape AND same metrics"),
  which is why it survived review. **Look FIRST: enumerate the hash's fields and strike each against
  the question's WORDING; and when a cheap noisy instrument keeps finding what your rigorous one
  misses, suspect the rigorous one — noise adds findings at random, systematic misses do not.**
  → [[lesson-a-hash-answers-the-question-you-encoded-not-the-one-you-asked]]

- **A CRITERION CHOSEN FOR STABILITY MAY NOT ANSWER THE DEFECT — try inverting the QUANTIFIER
  (2026-07-30).** A census of "codepoints identical at SOME sampled size" grew **774 → 963** as the
  grid went from 6 points to 59: an answer that moves when you sample harder is a site list, not an
  invariant. The cure was not sampling harder but flipping ∃ to ∀ — "identical at EVERY sampled
  size" is an intersection, so a denser grid can only SHRINK it. **Look FIRST: when a sampled
  measurement will not converge, invert the quantifier before widening the sample; and state which
  DIRECTION the residual runs — "over-folded, safe direction" was half a characterisation while the
  same table was under-folding by a set nobody had counted.**
  → [[lesson-a-criterion-chosen-for-stability-may-not-answer-the-defect]]

- **A TABLE DESCRIBING ANOTHER SUBSYSTEM'S EXTENT ROTS WHEN THAT SUBSYSTEM GROWS (2026-07-30).**
  `FoldCase` case-folded ASCII, Latin-1 and Cyrillic, and its comment called those "exactly the
  cased scripts the repertoire draws". TRUE when written. Two later widenings (Latin-Ext/Greek, then
  the flip's +4,741 codepoints) never touched the function, so nothing compiled differently and no
  test spoke — and **649 of 890 cased-and-drawable codepoints silently folded to THEMSELVES**. None
  folded WRONG: incomplete, never incorrect, which is why it was invisible (a missing fold just
  leaves two names undeduplicated, and a lobby with no collision looks like a healthy one).
  **Look FIRST: grep for comments of the shape "exactly the X that Y" — any constant whose
  justification quantifies over a set owned ELSEWHERE needs a CENSUS, not a sentence. And the cure
  is TWO mechanisms: freeze the data (reproducible) AND census it live (the claim); freezing alone
  reproduces the defect with better provenance.**
  → [[lesson-a-table-describing-another-subsystems-extent-rots-when-that-subsystem-grows]]

- **A CONVERGED DESIGN PASS CAN REST ON AN UNMEASURED ROOT.** 2026-07-31: a 20-round `/qf`
  converged on a cursor-OWNERSHIP design (per-frame invariant, seed target, shadow rollout, three
  gates). The FIRST measurement run after it ended found a completely different root — a truncated
  multiplier — and every ownership construct dissolved. The warning was inside the transcript three
  rounds running: R16 *"the stated root explains the PROBE's run, not the USER's symptom"*, R17 the
  stated mechanism recomputed to the OPPOSITE verdict, R16 again `mm=1` meaning the mouse may never
  have entered the window. The pass answered each by designing better GATES around the premise
  instead of going and measuring it — "measure before building" had quietly become "keep designing
  until it is time to measure". Convergence means the critic stopped finding problems **with the
  design**; a fresh critic reads the brief the primary wrote, so a wrong-but-coherent root makes
  every question land on the superstructure and every answer genuinely improve it. *Look FIRST:* the
  moment a round says the root is not established, STOP DESIGNING AND MEASURE — and ask "which of
  my rounds survive if the root flips?" If the answer is *none*, the pass is on credit. (This does
  NOT repeal [run `/qf` to convergence] — run it, but not on an unmeasured root. The same 20 rounds
  paid for themselves on the KEY-BINDING half, whose facts WERE measured as it went.)
  `memory/lesson_a_converged_design_pass_can_rest_on_an_unmeasured_root.md`

- **ONE DOMAIN WORD CAN NAME TWO DIFFERENT SUBSYSTEMS — and the collision happens in the PLANNING
  layer, where nobody is measuring.** 2026-07-31, caught by the USER in one sentence: *"What do you
  mean VOTV console on F10? By console initially i meant in game server console."* VOTV has TWO
  consoles that share nothing — UE4's **developer** console (opened by a KEY,
  `UInputSettings.ConsoleKeys`, `Tilde` cooked / **`F10`** on this box because the player rebound
  it) and the **in-world SAT server terminal** (`Uui_console_C` / `panel_SATconsole`, **no key** —
  walk up and press `E`, types through Slate focus on its `UEditableTextBox` @0x0268). Issue #5's
  `sv.request` is the SECOND. The fact base had them separated correctly; a GATE was nevertheless
  planned as "press F10 and type", which would have tested a surface no affected player ever opens.
  Both measurements were RIGHT — they were merged by a NOUN, in prose, and then survived 20 rounds
  of interrogation because every question about "the console" got a coherent answer about one of
  them. *Look FIRST:* when a domain word appears in two measurements taken for different reasons,
  write both definitions side by side before either enters a plan; the user's own vocabulary is the
  arbiter (*"server consoles"*, plural, was in the first message); and **a test whose SHAPE is
  suspiciously easy is a warning** — convenience selected the wrong subject here, the same pull as
  [search prior art by problem, not by assumed mechanism].
  `memory/lesson_one_domain_word_can_name_two_different_subsystems.md`
- **Kill-teardown discards buffered INFO log lines — an "empty" log is not "nothing ran".** 2026-08-21,
  spike drill b2: `log.cpp Write()` flushes only WARN/ERROR (the 2026-05-27 fps decision), and every
  autonomous teardown is `TerminateProcess` (mp.py `kill_all`, cell scripts) — the INFO tail dies in the
  CRT buffer. b133 booted FULLY (its module sat in the kernel module list) while its log held one line,
  the fflushed Init banner; it nearly read as "never booted". The spike-lane logs looked complete only
  because the new code `Flush()`es at its own milestones. *Look FIRST:* after any killed process, trust
  only lines at-or-before the last WARN/ERROR/explicit-Flush; corroborate "did X run" with kernel facts
  (module lists, files created); a drill that NEEDS an INFO line to survive puts a Flush in the path
  under test or ends with WM_CLOSE. **Sharpened 2026-08-22: a QUIET SCREEN (the main menu) buries the
  tail indefinitely — two D1 differential runs read as "no injection" because the re-injection INFO
  lines sat buffered for 25+ s until the kill. Recipe: any instrument whose evidence is INFO lines read
  after a kill OWNS a `ue_wrap::log::Flush()` at its final marker (menutravel DONE + wire_census Tick
  both do now).** `memory/lesson_kill_teardown_discards_buffered_info_log_lines.md`

- **2026-08-22 -- Python piped through the harness Git-Bash heredoc HALVES backslash runs even with
  a QUOTED delimiter; any script containing `\` must go through the Write tool to a .py file.**
  Hit 3x in one session (0-hit exact matches on C++ containing `L"\/"`, unicodeescape and
  unterminated-literal SyntaxErrors) + once in the 08-21 spike; the identical bytes via a file ran
  clean every time. LOOK FIRST: a heredoc script mysteriously not matching / erroring on a
  backslash = rerun via a Write-tool file before debugging the script itself.
  `memory/lesson_bash_heredoc_halves_backslashes_in_python.md`
- **Mechanical tractability is NOT reachability — census the world before picking which member of a
  defect class to fix first.** A 7-round `/qf` converged on `prop_pointSack` as consumer-one of the
  credit lane on entirely sound mechanical grounds (class-constant value, plain destroy-edge artifact,
  none of the coin/RNG/pickup machinery that got the coin gun cut) — and it is placed in **0 of the
  261 cooked `.umap` files**, fewer than the gun already cut for being unused; the most-placed paying
  props (`atm` 10, `prop_miner` 7) are the ones blocked on other lanes. The two axes are independent
  and here they were inversely ordered. **A `/qf` cannot catch this for you** — the critic interrogates
  the frame it is handed, so a target named in the brief's first line gets refined, never replaced.
  The tell was in plain sight: zero occurrences in `docs/`. LOOK FIRST: run
  `grep -rl <class> --include=*.umap research/pak_re/extracted` at ROUND 0, and put the target itself
  in the brief as a question.
  **SECOND INSTANCE 2026-08-24, ONE DAY LATER, same author — this time about SEVERITY.** A kerfur gate
  defect was traced correctly at the mechanism level and written up as the "SIXTH **live shipped**
  defect, ordered STRICTLY FIRST" with no reachability census — while both field logs' session-end
  `CONTAINMENT SUMMARY` already read `catch{off=0 on=0}`, i.e. zero conversions occurred and the defect
  was **latent**. **Generalisation: a correct mechanism trace is not measurement either.** It feels like
  one because it produces real citations, but it establishes only that the defect CAN fire. The sentence
  that earns a severity word is *"and the triggering condition occurred N times in the run, measured at
  `<file:line>`"* — if you cannot write it, do not write the word.
  `memory/lesson_mechanical_tractability_is_not_reachability.md`
- **A CLASS DEFAULT is agreement by construction — check for one before arguing about sync.** Asked
  whether the host's copy carries the same value the client just earned, the reflex was a sync argument
  (keyed, save-transferred). The real answer was one level down: `prop_pointSack.points` is **never
  written** (18-statement ubergraph, read-only, zero `Random*`, no spawner sets it) and lives in the
  class default (base 25, `_xs` 5, `_s` 10, `_l` 50). Every peer loads the same cooked asset, so both
  agree by construction — stronger than any lane argument, and it collapsed a resolve-the-actor branch
  out of the design (the arbiter needs only the CLASS, which `PropSpawn` already carries). **Never
  generalise by field name:** `points` on the sibling `baocoin_C` IS spawner-stamped
  (`SetIntPropertyByName`). LOOK FIRST: grep the owning asset for writes to the field and the corpus
  for a `Set*PropertyByName('<field>')` spawner; if nothing writes it, read the class default and stop.
  `memory/lesson_a_class_default_is_agreement_by_construction.md`

- **A selftest that RE-IMPLEMENTS the path it tests can pass while the shipped code is wrong.**
  `join_seed::RunSelfTest` computes its delta inside the test body, so it verifies the author's model
  of the algorithm and can never fail when the shipped `Seeder` diverges from it. What blocks that is
  making the arithmetic callable: pass the state and the clock IN (`ApplyPose(Row&, gen, pos, stateMs,
  nowMs)`) instead of reaching for a file-scope array and `Clock::now()`. Production passes its row
  under the mutex, the test passes its own -- **same function both times**, the test cannot touch
  production state, and every branch a two-peer LAN smoke cannot reach (a 24-bit clock wrap, an 8 s
  network gap, a slot recycling to a new occupant, a drained receive queue) becomes reachable
  instantly. It found TWO CRITICALs in `movement_ledger` the day it was written. And when the thing
  guarded fails SILENTLY -- a verdict that merely reads wrong -- run it UN-GATED at session start
  rather than behind an env var; this one costs microseconds.
  `memory/lesson_a_selftest_that_reimplements_the_path_it_tests.md`
- **A smoke shorter than an instrument's reporting period proves nothing about the instrument.** A
  35 s smoke returned PASS while the new ledger's 10 s summary line had printed ZERO times: the client
  connected at T+34 s, so the row armed one second before the peers were killed. Nothing warned --
  the driver's verdict is about peer stability and RAM, and "no line" reads identically to the two
  states the module was built to distinguish (`n=0` = armed but starved, no line = dead). Connect
  costs ~30 s in this project, so **set `--duration` from connect-time + N x period**, then GREP THE
  LOG FOR THE NEW LINE AND COUNT IT rather than trusting PASS. Better: make the driver assert it --
  `tools/mp.py` now exits 11 when the ledger's selftest line is missing on either peer, with distinct
  messages for "never ran" and "FAIL rows".
  `memory/lesson_a_smoke_shorter_than_the_reporting_period.md`

- **The number you quoted refutes the label you gave it — read a run's CONDITIONS off its own output.**
  A 120 s autonomous smoke was written up in three documents as the "client idle" baseline, with
  `maxImplied=0..859 cm/s` and `maxStep=0..15 cm` quoted on the very next line. 859 cm/s is between the
  game's walk speed and its sprint ceiling; an idle client reports 0, and three of eight windows did
  exactly that while five did not. A person had joined the autonomous test and played through it. The
  data was fine; the LABEL was invented, and the refutation was on the same screen. It cost real
  strength: the sample was human play, not a floor, which makes `wireVsActor=0 cm` *measured while
  moving* a much stronger claim and adds a live no-false-positive result. Getting the conditions right
  also exposed the honest limit the wrong label hid — a 14.6 cm step is under the ceiling the pre-fix
  arithmetic imposed, so **the broken build would have passed that session too**. LOOK FIRST: before
  writing "idle" / "stationary" / "solo" / "unattended", check the word against the numbers you are
  already pasting. `memory/lesson_the_number_you_quoted_refutes_the_label_you_gave_it.md`

## 2. Join-window identity & the DUP-prone zone (measure before touching)

- **A WINDOW CLOSED BY THE LATCH THAT STARTS THE NEXT PHASE ENDS BEFORE THAT PHASE — BY
  CONSTRUCTION.** Measured 2026-08-23 (R-4a-end, 9-round /qf): the world-load episode's destroy
  suppression lowered on the quiescence latch, the quiescence latch is what permits the
  ClientWorldReady announce, and the host's bracket starts only after the announce — so the
  suppression ALWAYS ended before the join reconcile it existed to cover (field: 1,629 junk
  broadcasts 23 s after "episode CLOSED"; a fast box masks it entirely). *Look FIRST:* any gate
  whose CLOSE edge doubles as the TRIGGER of the work it guards — ask "what does this latch
  START, and must my window span that too?" Fix shape (a second, kind-classified window spanning
  the triggered phase): `world_load_episode` reconcile window +
  `votv-r4a-end-condition-DESIGN-2026-08-23.md`.
  `memory/lesson_a_window_closed_by_the_latch_that_starts_the_next_phase.md`

- **A ZERO FROM AN ACCESSOR MEANS "NOT IN MY HALF", NOT "DOES NOT EXIST" — and that wrong root
  survived ~47 `/qf` rounds because it predicted the symptom perfectly.** 2026-08-25. `[MEASURED]`
  Four sessions carried *"a v122 client mints no Element row for its own save-loaded keyed prop, so the
  eid is 0"* into a design doc, a header, a protocol comment and a commit message. The client HAD the
  row: `CLIENT_1 …:18589` `CreateOrAdoptPropMirror: eid=4196 bound to actor=…0x…1C80
  key='xTy31ERNFExrbjG1NzOkVg'`, six minutes before `…:35483` broadcast that same actor with `eid=0`.
  The 0 comes from `prop_element_tracker.cpp:457-469`, which re-imposes a **LOCALS-ONLY contract** —
  `if (!el || el->IsMirror()) return kInvalidId;` — and names its own twin in the same comment
  (`ResolveMirrorEidByActor`, `remote_prop.h:110`), which would have returned 4196. Two things outlived
  the wrong root: the eid was documented as "unreachable, riding in alignment padding" when it is the
  HOST's own number and obtainable, and the fix resolves **key-first** while the sibling `PropDestroy`
  receiver resolves **eid-first** with an MTA `Packet_EntityRemove` citation saying so in capitals
  (`remote_prop_destroy.cpp:129-146`).
  **LOOK HERE FIRST:** `kInvalidId` and "no row" are the same value — before writing "X has no id",
  open the accessor that returned it and read its CONTRACT; nearly every reverse index in this tree is
  halved (locals/mirrors, host band/peer band, keyed/keyless) and a zero from one half says nothing
  about the other. Grep the log for the IDENTITY (the actor pointer, the key), not only for the
  failure line. And when citing a sibling lane as precedent, read its resolve ORDER, not just which
  primitive it calls.
  `memory/lesson_an_accessor_with_a_locals_only_contract.md`

- **ON A v122 CLIENT THE ELEMENT REGISTRY HOLDS ONLY HOST-BOUND MIRRORS — "local props" must be
  selected by keyed-interactable + NO element bound.** Measured 2026-08-23 (the R-4a-end drill's
  RED run): `Registry::SnapshotActorsByType(Prop)` on a client picked five pile MIRRORS whose
  destruction the mirror layer healed without touching the seam's broadcast path (RED read 1/5).
  The delivery side mirrors the asymmetry: `ExpressIncrementalSpawn`/`DeliverLateRegisteredProps`
  are HOST-only (prop_snapshot.cpp:620/:642) — a client's suppressed place has NO census
  delivery channel. *Look FIRST:* any client-side census/drill needing locals —
  `IsKeyedInteractable(obj) && GetPropElementIdForActor(obj)==kInvalidId`, never the Registry.
  `memory/lesson_v122_client_registry_holds_only_mirrors.md`

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
  **SECOND INSTANCE 2026-07-29, a GATE rather than a conversion, and it survived a 17-round `/qf`:** a
  chat design put `ChatLine` into `IsPreWorldSendableKind` to stop a client's own line vanishing in its
  load window. That gate is the HOST's send gate toward a JOINING SLOT; a client's send toward slot 0 is
  never gated (`ClientConnectEdge` marks it ready immediately, and says so in its own comment), so the
  hole did not exist — and applying the fix would have MANUFACTURED one by letting live rows land in
  front of the join seed. *Look FIRST: before "fixing" a silent drop, confirm which SIDE the gate
  guards; a gate named for a STAGE ("pre-world") rarely says which DIRECTION it faces.*
  **THIRD INSTANCE 2026-07-29, the census of a FUNCTION'S OTHER WRITES:** `chat_input.cpp:62` carries a
  comment that IS a census — "Open/Close are reached from THREE threads … so the store is told through
  `SetChatOpen`, which writes atomics only" — every word true, while the very next lines of `Open()` and
  `Close()` also write `g_buf[0]` and `g_histPos`, both declared **render-thread only** at `:22`/`:29-32`,
  from the WndProc, while `InputTextWithHint` may be mid-frame. The census asked "what do these tell the
  STORE?" and never "what ELSE do these WRITE?". *Look FIRST: a census scoped to one DESTINATION reads as
  a census of the function — enumerate every write target, not every caller.*
  `memory/lesson_census_the_direction_not_only_the_operation.md`

## 3. Sync architecture (owners, routers, lifecycle)

- **Before designing a receiver-side answer to a phenomenon, read the PRODUCER's gate — it may already
  suppress it.** 2026-08-25: A52's design spent four rounds building "forgiveness" rules so a host-side
  movement validator would not punish the join teleport, including a whole source kind
  (`saveSlot.playerTransform`) to cover it. **It was already written down** — `COOP_SYNC_MAP.md:103`
  has carried the "JOIN-JUMP sender gate" row since `614cade8`, in the very doc the reading order names
  for "where does this sync live". Then `net_pump.cpp:765-772` was read: a client emits **no
  poses at all** until `g_worldReadyAnnounced && !g_reAnnounceWorldReady && HasLoadTailQuiesced()`, and
  the gate's own comment says why — *"loadObjects' spawn flux (which contains the player teleport)"*.
  The join teleport **never reaches the wire**, the forgiveness source had no reader, and it was
  deleted. Two true receiver-side facts had pointed the other way and were irrelevant
  (`IsSlotWorldReady` gates sends and relays but not `StoreStreamPacket`; the occupancy generation is
  minted at accept). **Look here FIRST:** when a receiver-side design starts growing exceptions, grep
  who calls the publish function (`SetLocal*`) and what guards that call — this project's gates usually
  carry a comment naming the bug they were built for.
  `memory/lesson_the_producer_may_already_suppress_the_phenomenon.md`


- **When two readiness predicates differ only by a QUALIFIER, the bare one is the WEAKER claim.**
  MEASURED 2026-07-29: `Session::IsSlotReady()` returns `peerLanesConfigured_` (`session.h:397-400`) —
  "GNS lanes are up" — while `IsSlotWorldReady()` returns `slotWorldReady_` (`session.h:305-308`). The
  bare name reads as the general/stronger case and is the opposite: an EARLIER stage a peer satisfies
  ~30-60 s before it has a world. A chat-history design gated a join-seeded broadcast on `IsSlotReady`,
  which would have shipped live rows to a slot before its seed and put the seed UNDERNEATH rows already
  applied — the exact interleave the dedup range exists to prevent, reachable only if someone talks
  during another peer's load window. **Look FIRST: for any predicate gating a JOIN-TIME lane, open it
  and read the FIELD, not the name — the stages here are transport-up → lanes-configured →
  save-transferred → world-ready → replay-sent. And when a lane needs "have I done X for this slot",
  give it its OWN gate rather than borrowing a session predicate that merely correlates today.**
  `memory/lesson_a_readiness_predicate_may_name_a_stage_it_does_not_measure.md`
- **A JOIN SEED needs a CONTIGUOUS applied RANGE, never a high-watermark — and the seed must not
  interleave with live traffic.** 2026-07-29 (chat history, AS-BUILT): a seed delivers rows OLDER than
  anything the receiver holds, so `seq > highest` discards the ENTIRE seed, silently. Two halves make
  the range work: the host holds a **per-slot seed gate** (live rows to a slot start only once its seed
  is sent, reopened on slot turnover), and `ChatLine` is deliberately kept OUT of
  `IsPreWorldSendableKind` — together they make the applied set one interval that only grows upward. A
  GAP is logged loudly as the tripwire if either premise changes. **Any future lane whose seed can
  interleave with live traffic inherits this shape** (`docs/COOP_EVENT_JOIN.md` §3.4 chat row).
  `research/findings/join-identity/votv-chat-history-DESIGN-2026-07-29.md` §18

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
  send gate on own ClientWorldReady. **The sharers are RETROFITTED (2026-08-23 eve, `0676e5a8`): signal_sync + email_sync now
  carry the seed pair via the SHARED helper `coop/session/join_seed` (the extract-on-third of the
  meadow idiom)** — per-slot multiset capture at save_transfer's OnRequest, both-signs seedDelta at
  ConnectReplayForSlot; drill-proven RED (loss reproduced under the capture-disabled mutate) →
  GREEN (exactly-once delivery). The boundary decision stands: the send-backlog deliberately does
  NOT absorb the pre-world gate's skips (queueing them would dupe the connect replay) — seeds are
  the cure, per lane. LOOK FIRST: coop/session/join_seed.h + the lanes' CaptureJoinSnapshot;
  design `votv-signal-email-ready-seeds-DESIGN-2026-08-23.md`.
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
- **2026-08-23 — a documented invalidation can have ZERO callers; grep the call sites, and know the
  absorb hides failure from the caller.** `Registry::InvalidateLocal()` is promised by
  `input_owner.cpp:116` ("cleared on a level change") and has NO call site at `50b78d47` — so after a
  SOLO quit-to-menu the warm cache (slot+serial `Alive()`, no world identity) served the dead world's
  pawn for 44+ s, `GetController(dead pawn)` returned the dead PC, and input_owner's 1 Hz sweep fed it
  into `HasUserFocusedDescendants` on ~2,508 widgets/sweep → ~2,508 absorbed AVs/s for 44 s (78% of a
  12.35 MB field log). Four stacked traps: comment-as-wiring; liveness≠world-validity; the absorb
  lands BELOW `R::CallFunction` so the sweep sees a clean `false` and cannot latch; and the trigger
  flow (solo → quit → join) is ordinary for players yet exercised by NO autonomous smoke (0 instances
  in all 4 local logs). ~~Fix shape: wire the invalidation from the ONE world-change authority~~ —
  **CORRECTED 2026-08-23 eve: wiring it was IMPOSSIBLE; `InvalidateLocal()` was DELETED instead
  (`88e29669`)** — see the next row. *Look FIRST:*
  `research/findings/votv-linux-fps-triage-2026-08-23.md` §2 + §8's status table.
  `memory/lesson_documented_invalidation_with_zero_callers.md`
- **2026-08-23 — invalidating a cache whose REFILL is equally blind is a no-op that looks like a fix.**
  The prescribed cure for the row above was "wire the missing `InvalidateLocal()` edge". Reading the
  refill path killed it: `RescanLocal`'s filter is `IsLive` + non-null `Controller`, and the dead pawn
  passes BOTH — so an immediate re-walk re-caches the identical bug and buys one full GUObjectArray
  walk. What shipped is the missing term at **both** points (a world stamp compared at `Alive()`, a
  world filter in the rescan loop) and `InvalidateLocal()` **deleted**, because once the predicate is
  complete the cache self-heals and a second mechanism for one invariant is RULE-2 baggage. *Look
  FIRST:* before wiring ANY invalidation, open the refill path and ask "would a fresh read return the
  same wrong thing?" `memory/lesson_invalidating_a_cache_whose_refill_is_equally_blind.md`
- **2026-08-23 — a crutch usually NAMES ITSELF in its own justifying comment.** The destroy seam's
  world-load gate was scoped `!keyless`, excused as *"piles are already fixed and the host DEFERS them
  anyway"* — a confession that the traffic was known garbage, tolerated because the RECEIVER swallowed
  it. Cost, reproduced locally 1:1 with the field: **871** spurious `PropDestroy`s per client join
  (field 940), each carrying a client-band eid the host has never seen, all **871** parked and expired
  (field 1,618) — landing in the same minute as **485** sends refused for a full buffer. Deleting two
  words gave 871 → 0 (`65fccd70`). Second confession, same day and same area: `UnmarkAndDestroy`'s
  "drop the eid first so the seam stays silent" was false by construction — `UnmarkKnownKeyedProp`
  DEFERS the Element destruction, so the actor→eid reverse is still live when the seam runs
  synchronously inside the next line's `DestroyActor`. *Look FIRST:* grep for "handles it anyway" /
  "already fixed" / "the receiver dedupes", and price each under a join flood, not steady state.
  `memory/lesson_a_crutch_that_names_itself_in_its_own_comment.md`
- **2026-08-23 — a predicate added to a SHARED cache type lands inside every full-array walk.** Adding
  a world term to `CachedObjRef::Alive()` (~67 uses) reads as O(1) per probe; the post-ship audit
  measured it reached **per object inside six 237k-object GUObjectArray walks**, three levels below the
  loop (`prop::IsClassDescendantOfProp` → `PropBaseClass()` → `g_propBaseCls.Alive()`), the heaviest at
  5 Hz **during a join** — projected +200–300 ms/s on the reporting machine. Cure: guard the expensive
  accessor behind the cheap local field (`if (world_)`), zeroing it for exactly the class/CDO holders
  those loops use. Then the guard **thinned a drive nobody had documented** — `Alive()` was the
  de-facto refresh heartbeat — and the measured travel edge regressed 1 s → 5 s until an explicit
  driver was added. *Look FIRST:* before adding any term to a shared primitive, follow the per-object
  predicates of every `for (i < NumObjects())` loop down to it.
  `memory/lesson_a_predicate_added_to_a_shared_cache_type_lands_inside_every_walk.md`
- **2026-08-23 — a drill that kills boot is worse than no drill; it falsifies what it was built to
  prove.** The rate-latch drill killed the game twice: `nullptr` as the UFunction (`LogObserverAv`
  derefs it via `NameOf`), then running from the top of `Install()` — 123 reflection lookups on the
  loader thread before our own dispatcher existed. Env-gating contains the behaviour, not the
  CONCLUSION: the next reader arms it, sees a dead process, and decides the detector is broken. The
  discriminator that separates "my instrument" from "my change" is one command — a run with the drill
  disabled. *Look FIRST:* place one-shot drills AFTER the subsystem they measure is live, and feed them
  real objects. `memory/lesson_a_drill_that_kills_boot_is_worse_than_no_drill.md`
- **2026-08-24 — an entity-death discriminator must be fail-CLOSED, and the census that settles it is
  the census of DEATHS, not of reads.** A design was going to read "this mirror coin died locally and it
  was not our wire destroy" as "the player collected it", made sound by draining the registry row before
  every one of OUR teardowns kills an actor. That ordering discipline is correct and still insufficient:
  of the eight ways a mirror can die, **four — killZ, level unload, GC purge (which never calls
  `K2_DestroyActor` at all), and another actor destroying it — can never be made to announce themselves
  first.** The design's own earlier finding described a coin drifting away and dying unattended, so the
  discriminator would have read "it fell into the void" as "someone collected it" and the host would have
  **minted real currency**. A false negative costs a missed pickup; a false positive creates money, so
  the argument must come from the expensive side. *Look FIRST:* when inferring an event from a
  destruction, enumerate every killer including the engine's; name the noun your census actually covered
  (reads ≠ deaths); and act on POSITIVE evidence that you are inside the verb, never on the absence of
  the causes you happen to own. `memory/lesson_an_entity_death_discriminator_must_be_fail_closed.md`

## 4. Dispatch, hooks & input seams

- **A MODULE HEADER IS NOT THE CAPABILITY MAP — a whole design cascade was built on one sentence in
  `ufunction_hook.h` while `COOP_DISPATCH_VISIBILITY.md:88` stated the opposite IN BOLD.** 2026-08-24,
  `/qf` rounds 34-35. `[SRC]` `ue_wrap/core/ufunction_hook.h` says a BP-internal call — listing
  `EX_CallMath / EX_FinalFunction / EX_VirtualFunction / EX_Local*Function` — routes through
  `UFunction::Func`, and that *"EVERY dispatch path funnels through Func, so the hook fires regardless
  of the caller's opcode."* That is true **only for a NATIVE callee**. `[MEASURED]`
  `docs/COOP_DISPATCH_VISIBILITY.md:88`: a SCRIPT UFunction called via `EX_Local*Function` is
  *"INVISIBLE to BOTH ProcessEvent AND a Func patch (EX_Local\* never goes through `UFunction::Func`)"*
  and is *"THE ONLY REMAINING INVISIBLE CLASS"*; `docs/COOP_VM_DISPATCH_PLAN.md:300-304` pins it in IDA
  (both handlers branch on `FunctionFlags & 0x400` @`UFunction+0xB0` → `Invoke`/Func@`+0xD8` for native
  vs `ProcessScriptFunction` → `ProcessInternal` for script, which never reads `Func`) and records
  **"Option E ELIMINATED BY MEASUREMENT"** on 2026-07-13. Every Func patch the project ships
  (`BeginDeferredActorSpawnFromClass`, `K2_DestroyActor`, `AudioComponent::Play`) is native — which is
  precisely why the header reads as it does: an accurate description of ITS OWN consumers, and a false
  generalisation about the engine. The cascade it produced ("we can cancel a BP verb, so the
  act-as-host lane simplifies, so F0b's suppression dissolves, so the `0x45` registration and F6
  dissolve with it") was entirely wrong, and the patch would have **installed successfully** (`Func` =
  `ProcessInternal`, non-null, passes the null guard), logged `"patched ufn=… slot N"`, and never
  fired — the same silent-no-op defect class the lane was being fixed for.
  **LOOK HERE FIRST:** before claiming ANY seam can observe, drive, or CANCEL something, open
  `docs/COOP_DISPATCH_VISIBILITY.md` — `CLAUDE.md`'s reading order already mandates it at step 4. A
  module header answers *"how do I use this facility"*; the map answers *"is this possible at all"*,
  and a design rests on the second. When they disagree, **the map wins and the header is a doc
  defect**. **SECOND SHAPE 2026-08-25 — with no map to consult, the NEIGHBOUR became the spec.**
  Answering "should the main-menu buttons move to a pak?", a build plan asserted the MULTIPLAYER
  button is *"a real `UButton` whose `FButtonStyle` is cloned from the live `button_start`"* and
  argued from it that *a clone tracks the game while an authored asset freezes it*.
  `engine_widget.cpp:365-432` says otherwise: it walks `refButton`'s slot to its parent `UVerticalBox`
  and adds ours as a **sibling** (slot/layout parity is structural — that part is real), but the
  label's style is **hardcoded constants measured once** from `bp_reflection/ui_menu.json` (`font_ui`,
  size 16, shadow (2,2), black), the label is **deliberately tinted cyan**, and the comment ends
  **"(Font parity is deferred.)"**. So the argument was backwards — our values freeze exactly like a
  pak's — and a third of the case was retracted inside the hour, in a doc another session had been
  told to build from. The wrong inference came from `InjectTextRowAbove` twenty lines below: same
  argument shape, and its comment **explicitly says it clones**. *Look FIRST:* **a neighbour is not a
  specification** — a shared naming pattern is a reason to read BOTH bodies; **treat a MISSING claim
  as a claim** (if the sibling's header states a behaviour and this one's does not, the default is
  that it does not do it); and **before a mechanism claim becomes an ARGUMENT, open the body** — a
  header is enough to *use* a function, not to reason about what it guarantees. Cheap tell here:
  parity written as hardcoded constants with a `bp_reflection/...` citation is measured-once, not
  tracking; grep the body for any read of the reference object, and if the only reads are offsets
  into our own new widget, nothing is being cloned.
  Full: `memory/lesson_a_module_header_is_not_the_capability_map.md`.
  **SAME FAILURE, TWICE IN ONE SESSION — the general form is CONFIRMATION-STOPPING.** `/qf` round 39,
  minutes later: asked whether MTA heartbeats its entity streams, I read
  `CUnoccupiedVehicleSync.cpp:64` (`ulCurrentTime >= m_ulLastSyncTime + UNOCCUPIED_VEHICLE_SYNC_RATE`)
  and reported "MTA is rate-driven". That is a RATE LIMITER; the actual send site 300 lines later is
  change-gated (`:387-389`, *"If nothing has changed we dont sync the vehicle"*), as are
  `CPedSync.cpp:314-316` and `CObjectSync.cpp:227-229`. The false citation reversed the design's
  direction for a full round. **A gate is not a send; a header is not a map; an entry point is not a
  mechanism — read to the site where the thing you are claiming actually HAPPENS, and cite THAT line.**

- **An overlay that inline-hooks `IDXGISwapChain::Present` LOSES to RivaTuner and is INVISIBLE to OBS
  game-capture — two user-reported defects, one root: we sit too far down the Present chain.**
  2026-08-22. `[AUTH]` RTSS does not overwrite prologues like MinHook/Detours — it unwinds the jmp
  chain, injects after the LAST jump (deliberately last), and runs **hook-integrity control that
  restores its own bytes**, kicking a later inline hooker out of the chain; `[MEASURED]` on this box
  RTSS resolves `Present`/`Present1`/`ResizeBuffers`/`ExecuteCommandLists` by DLL-relative offset —
  exactly our functions. `[SRC]` obs-studio `dxgi-capture.cpp`: with `capture_overlay` FALSE (the
  DEFAULT) OBS copies the backbuffer at the TOP of its own `hook_present`, i.e. BEFORE calling the
  real Present — so a hook installed after OBS's (ours; OBS injects at capture start) draws too late
  to be captured. The working precedents all draw UPSTREAM of the inline chain: MTA via a COM PROXY
  device (`CProxyDirect3DDevice9::Present` -> GUI -> real Present, plus explicit RTSS-coexistence in
  `CreateDeviceSecondCallCheck`), ReShade via a creation-time swapchain WRAPPER. A ReShade-style
  wrapper is unavailable to us (measured 2026-07-26: our boot does NOT precede swapchain creation),
  so the seam is caller-side: `FD3D11Viewport::PresentChecked`. **Look FIRST:
  `docs/OVERLAY_CAPTURE_COEXIST.md` (sole live-tracked doc) before touching the overlay's present
  hook, the DXGI hooks, or the present signatures. And treat "who ELSE patches this function?" as a
  standing question for every inline hook — this is the SECOND instance of the class after the UE4SS
  PolyHook/ProcessEvent collision.**
  **2026-08-23 — that standing question is now CHEAP to answer, and the answer was bigger than assumed:**
  `tools/debug/present_hook_census.py` reads each present-chain prologue on a LIVE process and follows
  the jmp chain to the owning module. Result: `Present` + `ResizeBuffers` are ours, and
  **`IDXGISwapChain1::Present1` is hooked by `NahimicOSD.dll`** — an A-Volute audio-driver overlay
  nobody knew was in the process (so the chain holds us + RTSS-when-armed + OBS-when-capturing +
  Nahimic). Two corollaries worth carrying: **a tool being LOADED is not a tool being ARMED** —
  `RTSSHooks64.dll` is present even at detection level None, and neither the process list nor RTSS's
  ini settles it, only the target's bytes; and **"this box reproduces the bug" is itself a claim that
  needs measuring** — an earlier revision of the coexistence doc asserted it while the user had RTSS
  detection globally off, so the box reproduced NEITHER symptom.
  **2026-08-23 evening — S1's MECHANISM MEASURED, and the "we lost the install race" model is WRONG.**
  With RTSS armed: `imgui_overlay: DX11 bring-up OK` DOES appear, so our detour fires, ImGui comes up,
  the WndProc detour installs and **an RTV is created** — and only THEN does `frames=0/s` set in
  (sustained 8+ s at `PE=243537/s`, against a `frames=120/s` control at the same PE rate 20 min
  earlier with RTSS off). **RTSS lets you install and reclaims the function AFTERWARDS, per-function
  and STAGGERED** — with `Present` already dead our `ResizeBuffers` bracket still ran once and was
  gone ~90 s later. Consequence: being on ANY DXGI vtable function is a losing POSITION, not a race
  lost at install time, so "hook earlier" is not a fix. Diagnostic that settles it in one read:
  `frames=N/s` (the `perf_probe::NoteFrame()` counter, driven from `PresentDetour`) against a
  same-build control — a live per-frame counter distinguishes "unlinked" from "drawing but invisible"
  where a screenshot cannot.
  See `[[lesson-census-the-present-chain-by-following-jmp-to-the-owner]]`.
  Full: `[[lesson-an-overlay-that-inline-hooks-present-loses-to-rtss-and-obs]]`.
  `memory/lesson_an_overlay_that_inline_hooks_present_loses_to_rtss_and_obs.md`

- **A hooked RESIZE bracket is load-bearing: drop it and `ResizeBuffers` FAILS the engine into a
  FATAL.** 2026-08-23, measured with a real crash dialog. `[SRC MSDN]` `IDXGISwapChain::ResizeBuffers`
  returns `DXGI_ERROR_INVALID_CALL` unless every direct AND indirect reference to the back buffers is
  released first — an `ID3D11RenderTargetView` created on the backbuffer is exactly such a reference
  (`overlay_backend_dx11.cpp:34-48`), and `[MEASURED]` UE turns the failure into
  `VERIFYD3D11RESULT_EX` → **`Fatal`**: `LowLevelFatalError ... D3D11Viewport.cpp:298 ...
  DXGI_ERROR_INVALID_CALL`. The trap that makes this expensive: **a post-hoc "detect the backbuffer
  changed" check at the DRAW seam cannot substitute for a release BEFORE the resize** — an overlay
  redesign had exactly that substitution in it, `/qf`-converged over 7 rounds, and it would have
  turned an RTSS-only crash into a crash for every user on every resize. Two more carryables: the
  crash text named the **same function and line** (`D3D11Viewport.cpp:298`) that IDA had identified
  hours earlier from UE's own `VERIFYD3D11RESULT_EX` argument string, so **UE ships its verify
  expressions AND line numbers into the shipping exe and a crash dialog can confirm a static RE**;
  and `[?]` attribution is a separate claim — a co-resident overlay holds its own backbuffer
  references, so "the outstanding ref was ours" needs its own falsifier (resize before your own RTV
  exists). *Look FIRST:* `docs/OVERLAY_CAPTURE_COEXIST.md` §6d.b + §9's CONDEMNED item 4.
  `memory/lesson_a_resize_bracket_is_load_bearing_not_bookkeeping.md`

- **A negative reported before the evidence finishes arriving is worse than no report.** 2026-08-23:
  a resize test was called "did NOT crash" off a single `Get-Process` returning alive — while UE was
  displaying its `LowLevelFatalError` modal and had not yet exited. The dialog reached the user three
  minutes later. Then the relaunch that continued the work **destroyed the crash log** (HOST has no
  `multivoid.prev.log`; CLIENT_1 does), so the decisive finding survived only because it had been
  grepped in-flight. This is the same class as marking a todo done on intention, and it recurred three
  times in one session (a task marked completed for a measurement never made; a `-dx12` rig named in a
  task title and never run; this). *Rule:* write the crash predicate DOWN before the run — **crash = a
  window titled `*crash*` OR process exit, polled over >= 30 s**, never one instantaneous liveness
  poll — and **preserve the artifact before doing anything that rotates it.**
  `memory/lesson_a_negative_called_before_the_evidence_arrives.md`

- **A file-SHAPE assumption hides in PRESENCE TESTS and in PROSE, not just in the enumerator.**
  2026-08-23, migrating skins from one-pak-per-skin to one shared `scientists.pak`. The obvious break
  was the enumerator (`skin_registry.cpp:159` derives a skin's name from the pak file's **stem**), and
  it was not the dangerous one. **`PickRandomStarterSkin()` (`:84-99`) tests presence by asking the
  FILESYSTEM whether `<dir>/<name>.pak` is a regular file (`:98-99`)** — with a shared pak none exist,
  the candidate list is empty, and **every new identity silently falls back to `kDefaultSkinName`**
  (`skin_registry.h:36`). No crash, no WARN that reads as broken; "everyone is the same skin" looks
  like a content decision. That function contains no enumeration and never surfaces in a search for
  "how are skins listed". A tree-wide grep of the extension AND the directory name then found
  **11 surfaces, only 3 of them logic**: +2 **player-facing strings** that become false
  (`local_body.cpp:127`, and `skins_panel.cpp:52` — the F1 browser stating the retired rule AS the
  rule) and +6 **contract comments** (`skin_registry.h:8,40,62,68`, `protocol.h:944,2182`), which are
  what the next implementer reads. Fixing the logic alone ships a build whose own UI lies. I had
  written "three sites" into the doc BEFORE running that grep — a census claim is a claim.
  *Look FIRST:* when changing an artifact's on-disk shape, grep the extension **and** the directory
  name tree-wide, sort hits into logic / user-visible text / contract comments, fix all three classes
  together. **Invariant: presence is asked of the AUTHORITY ("is name X available?"), never of the
  filesystem ("does `<X>.ext` exist?")** — every direct `is_regular_file` on a derived path is a future
  break of this kind. Sub-trap: two of my own citations pointed at a changelog COMMENT rather than the
  definition; cite the definition, the comment is not the authority.
  `memory/lesson_a_file_shape_assumption_hides_in_presence_tests_and_prose.md`

- **Census the present chain by FOLLOWING the jmp to its owner — a trampoline belongs to no module.**
  2026-08-23. `E9` at a hooked function points into a `MEM_PRIVATE` page the hook engine allocated, so
  "which module is this address in?" returns NOTHING and reads as unattributable. One more hop settles
  it: MinHook's relay is `FF 25 [rip+0]` + abs64 (the followJmp-immune form is `48 B8 <imm64> FF E0`) —
  read the pointer slot and a real module name appears. Measured this way in VotV: two patches ours,
  one `NahimicOSD.dll`. Second trap: **third-party injectors live in VENDOR folders**
  (`C:\ProgramData\A-Volute\...`), so a module filter matching only the BASENAME can never show them —
  which is exactly why `parse_dump.py` had hidden Nahimic from every earlier read of the 19:17 dump
  (filter widened to full-path + graphics/injector keys the same day). *Look FIRST:*
  `tools/debug/present_hook_census.py census <pid>` before installing any inline hook, and before
  concluding a coexistence crash is yours. Also read your CHOSEN seam's live bytes — an unpatched
  prologue is what turns "nobody else is on this seam" from a hope into a measurement.
  `memory/lesson_census_the_present_chain_by_following_jmp_to_the_owner.md`

- **The SAME distinction can be needed in BOTH directions, two terms apart — and the second
  direction is the one nobody asks about.** MEASURED 2026-07-31, GitHub issue #5. The fix existed
  because VotV delivers typed keys to its in-world screens through `WidgetInteraction` on a
  **VIRTUAL Slate user**, which `UWidget::HasKeyboardFocus()` (user 0) is structurally blind to —
  the virtual user had to become VISIBLE. The SAME commit then added `HasFocusedDescendants()` to
  the 1 Hz backstop, and that accessor is **ALL-USERS**: the permanently-resident screen widget
  (`ui_consolesAtlas_C`) keeps virtual-user focus forever because nothing clears it, so the scan
  latched `scan=1` and every text-consumable hotkey died for the session. The user reported it
  within the hour. IDA settles it — `HasFocusedDescendants` exec `0x1427130e0` takes **no user
  argument at all**; `HasUserFocusedDescendants` exec `0x1427132e0` steps the PlayerController and
  resolves PC -> LocalPlayer -> `ControllerId` -> Slate user index. The `User` variant is not a
  stricter version, it is the only one that can EXPRESS the question. **Look FIRST: when a fix
  turns on WHO/WHICH-ONE (which user / peer / thread / slot), audit every OTHER accessor in the
  same change for that axis — an accessor that takes no argument for the axis cannot answer about
  it, and "all-users" reads as a safe superset when for an EXCLUDING predicate it is the defect.
  Prefer a term re-derived from a value the game itself clears (`activeInterface`) over one that
  reads residual focus on an object that never leaves memory.**
  Full: `[[lesson-the-same-distinction-can-be-needed-in-both-directions]]`; fact base
  `research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md` §4b; commits `361b6fe2`
  (fix) and `6090706d` (correction).

- **Swallowing `WM_KEYDOWN` does NOT stop `WM_CHAR` — and `WM_CHAR` is layout-translated.**
  MEASURED 2026-07-31 inside our own `WndProcDetour`: `TranslateMessage` synthesises the char from
  the keydown in the PUMP, before `DispatchMessage` ever reaches your window procedure, so returning
  0 from the keydown cannot un-create the character. One physical `T` logged
  `KEYDOWN -> SWALLOWED by the T-chat hotkey [capture=0]`, then `CHAR 0x435 -> SWALLOWED by
  CaptureActive [capture=1]` — eaten by a DIFFERENT gate that the keydown handler had itself
  switched on by opening chat. And `0x435` is U+0435, Cyrillic `е`: the char carries the
  LAYOUT-translated character while the hotkey matches the VK, so **on a RU layout the key that
  opens chat is the key that types `е`** (same shape as VOTV binding `ConsoleKeys=Tilde` AND
  `ConsoleKeys=ё` to the `VK_OEM_3` we swallow). **Look FIRST: instrument the whole sequence for one
  press — KEYDOWN/KEYUP/CHAR/SYSKEY* with the gate state beside each — because message N+1 is often
  decided by a flag message N set. To stop a character, handle the CHAR message.**
  Full: `[[lesson-swallowing-wm-keydown-does-not-stop-wm-char]]`; fact base
  `research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md` §8/M4; commit `f03c04f0`.

- **A "keep the frame alive" predicate is worthless if a gate ONE LEVEL UP can still veto the draw.**
  2026-07-29: `hud::IsActive()` (`hud.cpp:304-309`) carries a `RevealActive()` clause added SPECIFICALLY
  so the chat history's 220 ms fade keeps getting frames — and `imgui_overlay.cpp:359` gates
  `hud::Render()` on `!PauseMenuOpen()` above it. ESC closes the chat AND opens the native pause menu on
  the same keydown (`:214` falls through deliberately), so `chat_view::Draw()` is not called at all: the
  fade draws ZERO frames, the pin release living inside `Draw()` never runs so `SetRetentionFrozen(true)`
  stays latched for the whole pause, and the ramp's own variables freeze mid-transition (resuming either
  flashes at full alpha minutes later or kills the NEXT open's fade-in via `Ramp`'s `target ==
  g_revealTo` early branch). Both gates are individually correct and neither mentions the other. **Look
  FIRST: when you add a predicate whose job is "keep rendering me", walk UP to the render call site and
  ask who else can decide not to call it. And never put a latch release, timer, or cross-module publish
  inside a draw function — release it where the CLOSE happens.** A function that may or may not be called
  is a fine renderer and a terrible state machine.
  `memory/lesson_a_render_gate_one_level_up_defeats_a_keep_alive_clause.md`
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

- **A CATCH-ALL KEY HANDLER IS A TRANSPORT, NOT A BIND.** Measured 2026-07-31 from `mainPlayer`'s
  ubergraph: the `AnyKey` input events BROADCAST `anyKeyEvent`, call `intComs_anyKey`, and forward
  the key into `WidgetInteraction.PressKey/ReleaseKey/PressPointerKey/ReleasePointerKey` — i.e.
  `AnyKey` is the transport by which typing reaches **in-world 3D widgets**, and it consumes
  nothing. Both obvious readings are wrong for the same reason: treat it as binding everything and
  no key is ever free (the feature becomes impossible); dismiss it as noise and you lose the only
  path keys take to those widgets. The consequence that matters: a widget fed by **injection**
  rather than by Slate focus may be **invisible to a `HasKeyboardFocus` predicate** — exactly the
  *"might be other game systems"* the user hedged about. *Look FIRST:* classify a handler by what it
  DOES with the input (consumes / relays / observes), never by how much of it it sees; decode
  before excluding. Decode path: `research/bp_reflection/<asset>.json` + `kdec.py`; the
  `InpActEvt_*` stubs only set a temp and jump, so grep the decoded ubergraph for that temp — and
  note the JSON carries **no statement offsets**, so `range=` filtering silently passes everything.
  `memory/lesson_a_catch_all_handler_is_a_transport_not_a_bind.md`

- **2026-08-22 — two inline hook engines on ONE function collide via PolyHook's `followJmp`; the
  WP-2 UE4SS-lane boot crash was a ProcessEvent DOUBLE-DETOUR.** UE4SS 3.0.1 hooks
  `UObject::ProcessEvent` too — LAZILY (first `Unreal::Hook::RegisterProcessEventPreCallback`; its PE
  dispatcher `UE4SS.dll+0x554da0`, NOT printed in UE4SS.log like the two `<- Built-in` lines —
  **absence of a log line ≠ absence of the hook**). When UE4SS's PolyHook `x64Detour::hook()` runs
  AFTER our MinHook, its `followJmp` follows our `E9` into our MinHook RELAY, and because the relay is
  an INDIRECT `ff25[rip]` it resolves `m_fnAddress` to the relay's POINTER slot (tramp+0x1A) and
  writes its target-patch THERE, clobbering `&ProcessEventDetour` → a non-canonical `jmp` → `#GP` →
  `AV read -1`. MEASURED: **install-order is NOT the variable — we are always-first (20/20 boots)**;
  whether UE4SS's LAZY PE hook ARMS is (0/15 solo, ~2/10 two-peer runs). Impossible on the proxy lane
  (no PolyHook in-process). LOOK FIRST: a crash RIP inside a MinHook trampoline on the UE4SS lane →
  read the relay abs64 at `tramp+0x1A` in a `-fullcrashdump`; who-hooked-first = what OUR trampoline
  HOLDS (real prologue `40 55 56 57 41 54` = us first). **Fix DECIDED (2026-08-22) = B** (followJmp-immune
  relay: `ff25[rip]` → `mov rax,imm64; jmp rax`, so followJmp stops on the `mov` and PolyHook cleanly
  in-place-hooks the relay → both detours chain); **C ruled out** (UE4SS's PE PreCallback returns `void`,
  can't host our ~20 native-call interceptors → we always own our PE detour, B is permanent). **VERIFIED
  2026-08-22 eve (commit `0c14a931`)** — real-env trampoline byte decode (PolyHook in-place-hooked our
  immune relay mid-session; 80 s crash-free) + a DEV boot printing `POLYHOOK-COMPOSED`+`WE-FIRST`.
  SUB-LESSON (same day): the pe_diag classifier's first-match scan read **MinHook's own jump-back stub**
  (`FF25 00000000`+abs64→PE+6, which PRECEDES the relay in the trampoline slot and shares the legacy
  relay's encoding) as "the relay" → printed LEGACY-CORRUPT on EVERY boot regardless of reality; only
  the payload (`==&detour`) discriminates the relay from the jump-back — locate once at the install
  snapshot, classify that offset thereafter. Also: UE4SS 3.0.1 stable in the DEV stack armed its PE hook
  <10 s with NO ArmPE fixture — the "lazy, 0/15 solo" measurement does not generalize across stacks.
  `memory/lesson_two_inline_hook_engines_collide_via_followjmp.md`
- **2026-08-22 — a coexisting VEH crash-reporter mod (CrashContext) pre-empts our SEH-guarded fault
  tolerance.** Our `ue_wrap::reflection::IsLive()` (`reflection.cpp:165`) SEH-guards the read of a
  possibly-dangling `UObject*` and returns false on a fault (deliberate; silent for months). But
  `Moddy-CrashContext` installs an `AddVectoredExceptionHandler`, and **VEH fires BEFORE frame-based
  `__except`**, so it catches our normally-absorbed fault first and pops a crash report. Measured:
  exit-to-menu AV at `main.dll+0x11CC78` = `IsLive` (resolved via the build `.map`), only WITH
  CrashContext present. NOT a bug in IsLive; a Windows exception-ordering coexistence trap. **SHARPENED
  2026-08-22 eve by measurement: it is a FALSE-crash (popup + report), not a crash** — CrashContext has
  NO TerminateProcess/ExitProcess/MiniDump/`__fastfail` imports (byte-scan), the process survived (2nd
  report 9 s later, no UE dump), and faults manifest only when the freed page is DECOMMITTED (DEV runs
  silently clean). LOOK FIRST: an AV whose RIP is in a function you SEH-guard that only reproduces WITH
  other mods → grep their imports for `AddVectoredExceptionHandler`. FIX: the zero-AV discipline —
  design of record `research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md`
  (CachedObjRef + 78-site conversion; the IsLive WARN now names its CALLER via `_ReturnAddress`).
  `memory/lesson_veh_crash_reporter_preempts_seh_guard.md`
- **2026-08-22 — the ProcessEvent double-detour crash is CONFIG-DEPENDENT, not universal.** It fires
  only if something ARMS UE4SS's PE inline detour via `RegisterProcessEventPreCallback` — and NO stock
  mod does (measured: not DebugMod/CrashContext/PBMovement, not jsbLuaProfiler, no UE4SS built-in; grep
  `reference/RE-UE4SS/assets/Mods/`). UE4SS hooks ProcessInternal/LocalScriptFunction/BeginPlay and
  RESOLVES PE's address but installs NO PE detour without a callback registration. The arm = a Lua mod
  calling `ExecuteInGameThread(fn, 0)` (ProcessEvent method, non-default) OR Multivoid's own MP/join
  path (the ~2/10 trigger). So the common modded stack coexists crash-free. LOOK FIRST: to reproduce,
  drop the `ArmPE` fixture; to predict, grep the stack for `RegisterProcessEventPreCallback`.
  `memory/lesson_double_detour_crash_config_dependent.md`
- **2026-08-22 — the real VOTV modded stack + how to test Multivoid in it.** r2modman (== Thunderstore
  Mod Manager, same `ebkr/r2modmanPlus`; TMM = Overwolf wrapper) manages
  `…\VotV\profiles\Default\shimloader\{mod,pak,cfg}`; the game is a SEPARATE install whose Win64 has the
  shimloader `dwmapi.dll` + `ue4ss.dll`, launched via r2modman with `--mod-dir/--pak-dir/--cfg-dir`. The
  community runs EXPERIMENTAL UE4SS (not our 3.0.1) — the double-detour reproduces on both. All C++ mods
  (DebugMod/CrashContext/PBMovement) use UE4SS's OWN API (no second inline hooker). Multivoid drops as
  `shimloader/mod/Multivoid/dlls/main.dll` + `enabled.txt` (Thunderstore mods load via `enabled.txt`, not
  `mods.txt`). ExeDir anchor WORKS under the VFS. **FIXTURE HYGIENE (2026-08-22): revert repro
  fixtures on the coop rig the moment the repro ends — a leftover-enabled ArmPE on HOST/CLIENT_1
  caused 2 phantom intermittent boot fatals hours later (`Mods\ArmPE\enabled.txt` → `.off`).**
  `memory/lesson_realistic_votv_modded_stack.md`

- **A `vm_dispatch` verb NAME is not a gate, and the verb ID is a PROJECT-WIDE namespace.** The 0x45
  substrate matches on the name alone -- its header says the class/authority check is "the CONSUMER's
  job" -- and `[V]` `playerHandUse_LMB` is declared by **146 classes** (knife, hacksaw, flamethrower,
  garbage gun, disintegrator, toolgun...). A v137 consumer gated only on `av.verbId` and would have let
  a client mint coins by destroying a prop with ANY tool; ten `/qf` rounds passed it because the design
  said Context-gated and only the code didn't. Ids collide too: `1` was already used by four modules,
  since `CurrentThreadVerb()` is one global thread-local. LOOK FIRST: grep the `CXXHeaderDump` for how
  many classes declare the verb, gate on `av.ctx`'s class in the SAME function that tests the id, and
  census `kVerb* =` across the tree before picking one. `drive_sync.cpp:47` and
  `container_contents_sync` already did this correctly.
  **THIRD MEMBER 2026-08-24 (`/qf` 46-47) — the AMBIENT read, where `active` is worse than the id.**
  The first two members were consumers reading the `Bracket` handed to their OWN callback (scoped, and
  always correct). `CurrentThreadVerb()` is different: it is a project-wide namespace. `[V]`
  `kerfur_form_assembler`'s six gates tested **`av.active` ALONE** — true for ANY registered verb — so a
  foreign bracket inverted them twice over: an unrelated spawn counted as a kerfur form spawn, and
  `reqScope = !av.active && InReqScope()` went FALSE, blinding the CallFunction route. `[V]` testing the
  id would not have saved it: `container_contents_sync`'s `kVerbDirty`, `meadow_db_sync`'s `kVerbMark`
  and `drive_sync`'s `kVerbPutDriveIn` are all **1** = kerfur's `kVerbTurnOff`, and `kVerbPulledOut = 2`
  is its `kVerbTurnOn`. `[V]` **the substrate already owned the unique handle and was not publishing
  it** — `RegisterVirtualVerb` requires the NAME to have static lifetime and the table stores that
  pointer — so `ActiveVerb` now carries `verbName` (~4 lines) and kerfur routes all six gates through
  one `InKerfurVerb()` (commit `0361b815`). `[V]` **severity as measured, not feared: LATENT** — the
  field run's session-end `CONTAINMENT SUMMARY` reads `catch{off=0 on=0}` on both peers. LOOK FIRST: an
  ambient "what am I inside" window is a CROSS-MODULE namespace, so gate on something unique by
  construction; `active` is never a gate. Contract: `docs/COOP_DISPATCH_VISIBILITY.md` "The AMBIENT verb
  window". `memory/lesson_vm_dispatch_verb_name_is_not_the_gate.md`

- **A "cannot" in a comment is a snapshot of what was TRIED, not a property of the substrate.** `[V]`
  `ui/multiplayer_menu.h:13` justified every polled click in this codebase with *"a reflection-only DLL
  cannot bind the `UButton::OnClicked` FMulticastScriptDelegate (no UObject+UFunction to point it at)"*.
  True as an account of what had been built -- census 2026-08-25 finds **zero** `FScriptDelegate` /
  `InvocationList` code in the tree -- and read for months as an architectural fact, until a USER
  question forced the re-derivation. Every link was already there: `OnClicked` is a plain
  `TArray<FScriptDelegate>` @ **+0x3C8, size 0x10** (`UMG.hpp:284`, with `OnPressed`/`OnHovered`/
  `OnUnhovered` beside it); a delegate-dispatched event is **already PE-VISIBLE and our own map says
  so** (`COOP_DISPATCH_VISIBILITY.md:81`, the game's own inventory buttons, marked "expected"); and all
  five primitives were **already public in our own headers** -- `InternalIndexOf` + `SlotSerial`,
  `EngineAlloc`, `StringToFName` (`fname_utils.h:24`), `RegisterInterceptor` with cancel-on-true
  (`game_thread.h:86,124`). Interceptors key on the **UFunction**, so N sink objects share one function
  name and are told apart by `self` -- no authored asset, no editor. The false premise had already
  propagated into a build plan and nearly bought an entire UE-editor toolchain. LOOK FIRST: **read the
  clause after the "cannot"** -- if it names an ARTIFACT WE LACK ("no X to point it at") it is a backlog
  item wearing a limit's clothes; if it names a MECHANISM THAT DOES NOT EXIST (measured-invisible
  dispatch) it is a real limit. And **a zero-hit census is evidence about US, not the engine**: "we have
  never done X" and "X is impossible" grep identically. Status `[RD]` -- links measured, composition
  NEVER RUN. `docs/MULTIPLAYER_UI.md` 6e.
  `memory/lesson_a_cannot_in_a_comment_is_a_snapshot_of_what_was_tried.md`

## 5. Engine / UE4 facts

- **VOTV's own maximum player speed is NOCLIP, not sprint — and noclip is reachable in ordinary play.**
  Measured 2026-08-25 from the BP dumps while sizing a movement bound: walk is `defSpeed = 400`; sprint
  is `defSpeed * 2.0 * Lerp(1.0, 1.25, agility/100)`, so **<= 1000 cm/s**; the ATV's `speed_turbo` is
  3200; and **no `TerminalVelocity` override exists in any of the 292 asset dumps**, so UE4's ~4000
  default stands. Noclip (`mainPlayer` ubergraph @64501) is `SelectFloat(5000, SelectFloat(250, 1000,
  crouch), run)` applied as **three separately summed `dir * dt * 5000` terms** (@64694..@64974), so its
  worst case is `sqrt(3) * 5000 = 8660 cm/s` — 8.7x sprint. It is gated by `lib_C::isBuoyant`, whose
  name is about buoyancy and whose body is not; the branch that matters returns `gamemode.hasWeapon`,
  a story flag, so **a bound written against "the fastest the game can move a player" must cover 8660,
  not 1000**. Also measured in the same pass, and worth having: `mainPlayer.armLength = 200.0` is the
  default interaction reach, `arm()` is called at exactly **10 sites game-wide** (1000 for the coin gun
  and groundHose, 300 for prop_vacuum2, 0 -> 200 everywhere else), and **`arm()` starts at the
  CAMERA** (`GetPlayerCameraManager().K2_GetActorLocation()`), not the actor root — three shipped
  comments carried a `[V]` tag on "FROM THE PLAYER" until `abc9681b`.
  `memory/lesson_isbuoyant_is_votvs_cheat_gate_not_buoyancy.md`


- **2026-08-25 — A default-on-failure return is FAIL-OPEN whenever the default is a LEGAL value.**
  `E::GetActorLocation` returns a default `FVector` on every failure path and cannot signal it -- and
  `(0,0,0)` is the WORLD ORIGIN, an ordinary reachable position. Inside the coin gun's reach gate (a
  function whose own comment said FAIL-CLOSED) a failed read therefore authorized anything near the
  origin and falsely refused everything else. Same shape one level up: `E::GetActorBounds` returns
  `true` for *the dispatch succeeded*, not *the box is meaningful* -- an actor with no colliding
  components yields `Origin=(0,0,0) Extent=(0,0,0)` with a `true` return. Fixed at the WRAPPER
  (`E::TryGetActorLocation`), not the call site. *Look FIRST:* in any authorization gate, open every
  accessor it reads and ask what it returns on failure; grep `ue_wrap/engine` for `Type Get...()`
  returning by value. `memory/lesson_a_default_return_is_fail_open_when_the_default_is_legal.md`
- **2026-08-23 — An instance index without BOTH a CDO filter and a world filter silently indexes
  the CLASS DEFAULT OBJECT.** turbine_sync (the one scan consumer with no `Default__` skip) had a
  phantom 5th "placed turbine" — the CDO — for its whole life: IsInstance matches the CDO
  (class-pure), IsLive passes (CDOs are live), PosKey quantized its DEFAULT position into a stable
  key IDENTICAL on both peers, so every count/parity/N-match instrument agreed 5==5 and the poll
  "synced" it CDO-to-CDO. Only an ORTHOGONAL discriminator — the R-2 hub's per-MATCH
  `WorldOf(obj)==CurrentWorld()` term (a CDO outers to its package, not a world) — broke the
  symmetry (probe=5 hub=4). The name skip and the world test exclude DIFFERENT non-instances;
  carry both. *Look FIRST:* the WorldOf term (`object_scan_hub.cpp` audit-W-3 comment) + the
  turbine probe-row comment (`autotest_scanparity.cpp`).
  `memory/lesson_an_instance_index_without_a_cdo_world_filter_indexes_the_cdo.md`
- **2026-08-22 — WILDCARDING an AOB can DESTROY its uniqueness: measure occurrences AFTER masking,
  never before.** Deriving `kSigD3D11ViewportPresentChecked`: the LITERAL first 24 bytes of
  `sub_1416F4BA0` occurred exactly ONCE in the image — but the same window with the `/GS`
  `mov rax,[rip+disp32]` displacement wildcarded (mandatory: that disp moves on EVERY rebuild)
  matched **TWO** places, and stayed ambiguous at 32 bytes; it becomes unique only at 40 (shipped at
  48 for margin). Masking removes information, and the bytes that individuate a short MSVC prologue
  are frequently the very displacement you are about to wildcard — so "found a unique prologue, now
  let me wildcard the volatile parts" is the WRONG order and fails SILENTLY (FindPattern just returns
  whichever site comes first -> a hook on the wrong function, which will not look like a signature
  bug at runtime). **Look FIRST: `tools/debug/ida_aob_derive.py sig 0x<ea>` — it prints occurrences
  per window length with the mask ALREADY applied, names the first-unique length and emits a
  margin'd signature; grow into register-move/struct-offset bytes, which survive rebuilds.**
  Full: `[[lesson-wildcarding-an-aob-can-destroy-its-uniqueness]]`; `docs/VERSION_MIGRATION.md`
  (this was the 6th signature).

- **2026-08-22 — UE ships its own `__FILE__` paths into the SHIPPING exe, so an engine source-file
  name is the fastest anchor into that subsystem's functions.** The VOTV shipping exe contains
  `D:/Build/++UE4/Sync/Engine/Source/Runtime/Windows/D3D11RHI/Private/WindowsD3D11Viewport.cpp` (and
  the D3D12/Vulkan twins) as UTF-16 literals, because UE's `VERIFYD3D11RESULT`-style macros pass
  `__FILE__` — plus the failing EXPRESSION as a literal (`"SwapChain->GetFullscreenState(...)"`), which
  names the function for you. Xref'ing one string produced the entire D3D11 viewport present cluster
  in a single pass; symbols are stripped (`sub_XXXXXXXX`) but string literals are not debug info and
  survive. **Decoy warning:** searching the DXGI HRESULTs (`0x887A0005/6/7`) finds the error-FORMATTING
  helpers (`VerifyD3D11Result` family, `GetD3D11ErrorString`), one layer away from the present path —
  good for confirming the neighborhood, useless for locating the seam. **Look FIRST:
  `tools/debug/ida_aob_derive.py file <substring>`.**
  Full: `[[lesson-ue-file-strings-anchor-engine-seams-in-shipping-builds]]`.

- **2026-08-21 — "one binary across plugin-host versions" is decided by the VTABLE HISTORY, not the
  API docs.** Measured on UE4SS v3.0.1 vs main-2026-05: `on_ui_init` INSERTED mid-vtable (+5
  appended, one signature change) = full-API one-binary mis-dispatches by construction, while the C
  loading contract (`GetProcAddress("start_mod")`, `Mods/<n>/dlls/main.dll`, `enabled.txt`) stayed
  byte-stable 27 months. Safe cross-version shape = the C contract + a no-op stub vtable + an
  object coupling censused EMPTY (all fire_* void, never deleted, no field reads); the one hole =
  a future sret virtual (watch it, don't assume it). LOOK FIRST: diff the base-class virtual
  declaration ORDER across the versions users actually hold; census what the host DOES with your
  object. `memory/lesson_one_binary_across_plugin_hosts_is_decided_by_vtable_history.md`
- **2026-08-21 — an ecosystem "standard" names a LANE; check which, and what is actually
  installed.** "UE4SS is the modloader everything uses" was true for LUA and near-empty for C++
  (~8 public repos, two query shapes); "the stable everyone has" was FALSE for VOTV — the
  community pipeline AND its manual guides ship the shimloader bundle (UE4SS.dll PE 2026-02-03,
  experimental-era), not the 1.99M-download v3.0.1. Flipped the pin, the spike order, and the Lua
  plan in one day. LOOK FIRST: the community package index + PE-timestamp the bundled artifact +
  read the community's own install guides; never infer the install base from upstream download
  counts. `memory/lesson_an_ecosystem_standard_names_a_lane_check_which.md`

- **An accessor on a WRAPPER answers about the wrapper, not the thing you meant.** MEASURED
  2026-07-31: `UWidget::HasKeyboardFocus()` on a live, on-screen `UEditableTextBox` reads **false**
  even immediately after calling the engine's own `SetKeyboardFocus()` on that very widget —
  `target{kb=0 desc=0 anyUser=0} iface{kb=1 desc=0 anyUser=1}`. UMG never hands Slate your
  `UEditableTextBox`; it wraps it in an `SObjectWidget` around the real `SEditableTextBox`, and the
  accessor tests the cached WRAPPER exactly. The owning `UUserWidget` reports focus correctly. An
  entire arc (the "is a game text field focused" predicate for GitHub issue #5) had been designed on
  the per-field version across 10 `/qf` rounds; the shipped invariant became "a game `UUserWidget`
  holds keyboard focus". **Look FIRST: round-trip any reflected accessor before designing on it —
  set the state through the engine's own API and read it straight back. When the engine WRAPS the
  object you hold (UMG `UWidget`->Slate `SWidget`, component->scene proxy, actor->physics body),
  assume the accessor answers about the wrapper until measured. And a predicate that can only ever
  be wrong in the `false` direction needs a positive control on purpose** — a healthy build and a
  dead predicate otherwise produce identical logs.
  Full: `[[lesson-an-accessor-on-a-wrapper-answers-about-the-wrapper]]`; commit `f03c04f0`.

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
  keep cached-ptr + UFunction-CALL + teardown-reachable); all fixed 07-15. **PROVEN AGAIN 2026-08-22: a
  fresh full census found 78 bare-IsLive-on-cached sites still live post-07-15** (435 sites total; work
  list + the CachedObjRef fix design =
  `research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md` Appendix A). Two
  sharpenings from that pass: `IsLiveByIndex` never reads FUObjectItem **SerialNumber**, so a
  same-address SAME-SLOT successor passes (ABA — filed residual); and UE assigns serials LAZILY (0 until
  a weak ref exists), so naive serial capture does not close it either. **CONVERTED WHOLE 2026-08-22
  night (`f675de11`..`712fa33b`): the discipline is now a TYPE** — `ue_wrap::CachedObjRef` (+ one-root
  accessors `Element::LiveActor()` / `ActiveDrive::LiveActor()`), policed by
  `tools/reflection/islive_gate.ps1` (CI PASS = 0 bare-IsLive-on-static) + the deterministic decommit
  drill (`VOTVCOOP_RUN_ISLIVE_DRILL=1`). LOOK FIRST now: `ue_wrap/core/cached_obj_ref.h` + the design
  doc's Appendix B fill-site table.
  `memory/lesson_islive_recycled_slot_blind_use_by_index.md`
- **2026-08-22 — a dying world's actors are NOT kill-flagged even after the MENU world is up** (measured:
  `trash_pile` re-indexed 311 of the dying gameplay world's piles AT the menu, 16:46:03 — every one
  passed IsLive's kill-flag check), so ANY liveness-based gate lags GC purge by seconds. Two dependent
  traps: the `worldUp = Registry::Local() != nullptr` gate keeps the session chain running against the
  dying world for the flee poll's ≤4 s window, and a session-install fanout re-running in that window
  indexes soon-to-dangle actors. World-state gates must key on WORLD IDENTITY / travel-start signals,
  never on per-object liveness. NOTE: `engine.cpp` `g_worldContext` is the GAMEINSTANCE (immortal) — not
  a current-world read. **WIRE LEG MEASURED 2026-08-22 (`mp.py wirewindow`, permanent instrument): the
  exit window leaks NOTHING** — an EXISTING gameplay→MENU session-stop edge (`net_pump`) closes the
  session at ~+1 s (not the 4 s flee poll); only ~2 s of pose stream crosses; zero reliables; the
  destroy-seam episode gates held under world-teardown mass K2_DestroyActor → D2 stays deferred, its
  residual harm = wasted local work. LOOK FIRST: the D2 section of
  `research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md` (probe RESULT +
  seam candidates), and `coop/dev/wire_census` for re-measuring. **SOLO asymmetry (2026-08-23): the
  ~+1 s session-stop edge and the ≤4 s flee poll exist only WITH a session — a SOLO quit-to-menu has
  neither, and the dead pawn stayed slot-live 44+ s (until the next world load's purge), vs ~3 s on
  the same log's connected quit. The stale window is transition-path-dependent.**
- **2026-08-23 — `access=FFFFFFFFFFFFFFFF` in a PE-absorb line = NON-CANONICAL pointer deref**
  (#GP-class AV, `ExceptionInformation[1] = -1`, the Windows "address unknown" convention) — the code
  dereferenced scribbled garbage, not a mapped-but-wrong address. Reading pattern: constant ip +
  constant access + VARYING self across thousands of lines ⇒ the poison is a SHARED INPUT to the call
  (an argument/global — in the triage, the stale PlayerController), never per-object corruption.
  *Look FIRST:* `pe_detour.cpp TaskFaultFilter` (field meanings) +
  `research/findings/votv-linux-fps-triage-2026-08-23.md` §2 (worked example).
  `memory/lesson_absorb_log_access_minus_one_is_noncanonical_deref.md`
  `memory/lesson_dying_world_actors_not_killflagged_at_menu.md`
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

- **SCALING A MULTIPLIER IS NOT SCALING A SIZE — and truncating one below 1 DELETES it.** Measured
  2026-07-31, and it is the whole root of the "no cursor showing" bug the user reported 07-27:
  `ImGuiStyle::ScaleAllSizes` ends with `MouseCursorScale = ImTrunc(MouseCursorScale * f)`
  (`imgui.cpp:1649`). Every OTHER field it touches is a **pixel size**, where round-toward-zero is
  sensible; `MouseCursorScale` is a unitless **multiplier** consumed as `pos + size * scale`
  (`imgui.cpp:4131`). Our `ui::scale::Ui()` measured **0.833**, so `ImTrunc(0.833) == 0` and the
  cursor became a **zero-area quad** — with the OS cursor hidden by our own `WM_SETCURSOR` handler,
  that is NO cursor. Present since **v1.91.5**, not a 1.92 regression. It reads as *intermittent*
  only because the factor tracks client size (harmless at >= 1.0). Every health signal stayed GREEN
  throughout (`MouseDrawCursor=1 texOk=1 atlasFlags=0`) because nothing is broken — the geometry is
  multiplied by zero. *Look FIRST:* the LAST lines of any bulk "scale everything" helper, where the
  fields that do not fit its unit assumption cluster; and when something is "sometimes" broken,
  find the axis it varies along and PRINT it beside the symptom (`curScale=0.000 uiScale=0.833`
  ended this in one line). `memory/lesson_scaling_a_multiplier_is_not_scaling_a_size.md`
- **MSVC vtable slots follow OVERLOAD GROUPING, not declaration order.** 2026-08-21, the D-3 spike's
  per-slot dispatch counters: `CppUserModBase::on_dll_load` fired at slot 13 where declaration order
  predicts 9, and the first suspicion (upstream ABI drift) was FALSE — today's header re-diffed
  line-identical to 2026-05. The rule: MSVC clusters ALL overloads of one virtual name at the FIRST
  declaration's position, in REVERSE declaration order (the 4 `on_lua_start` + 4 `on_lua_stop`
  overloads occupy slots 5-12, pushing everything after). Both live eras' censuses fit exactly.
  *Look FIRST:* before numbering any MSVC vtable, group overloads at first-declaration-reversed; a
  census that misses decl order is not necessarily drift. Worked mapping: the F2 design doc §3 AS-RUN.
  `memory/lesson_msvc_vtable_slots_follow_overload_grouping.md`
- **Shimloader owns the xinput error surface — our upgrade dialog never runs under r2modman.**
  2026-08-21, spike drill a2v2: `unreal_shimloader` Rust-panics BY DESIGN on ANY `xinput1_3.dll`
  beside the exe (its anti-2023-UE4SS guard; hits our proxy by filename alone), writes
  `shimloader-log.txt` naming the file with removal steps, and the game idles windowless — NOTHING
  loads, not even our proxy's payload. Our `REFUSE reason=predecessor-*` dialog (live-drilled and
  working on plain-UE4SS installs, cells b2/a3) is structurally unreachable there. *Look FIRST:*
  "game won't start" from an r2modman user → read `Win64/shimloader-log.txt`; INSTALL.md's r2modman
  upgrade language = delete the old standalone install FIRST; and check shimloader's guard strings
  before ever shipping a new file beside the exe.
  `memory/lesson_shimloader_owns_the_xinput_error_surface.md`

- **2026-08-22 -- VOTV crash dumps live in %LOCALAPPDATA%\VotV\Saved\Crashes, and PCallStackHash
  groups identical crashes across runs; a green drill can hide a crashed boot.** The WP-2 UE4SS-lane
  boot flake was attributed by hash identity (3 crashes, one SILENT during the 08-21 spike evening)
  plus a byte-exact match of the dump's faulting IP against our own logged `trampoline <base>` line
  (fault = ProcessEvent trampoline +0x14). The install-dir `VotV/Saved/` does NOT exist -- mp.py's
  `_game_log()` fatal-scan reads a path that never has data (open defect). LOOK FIRST: list the
  Crashes dir by mtime and compare PCallStackHash BEFORE theorizing; decode with python
  minidump+capstone (recipe in the lesson). **UPDATE 2026-08-22 pm:** the default dump is a TRIAGE
  dump (no code/trampoline pages) — pass `-fullcrashdump` (mp.py `MP_FULLCRASHDUMP=1`) for a
  FULL-memory dump; and `AV reading 0xffffffffffffffff` with RIP on a `jmp`/`call` is a
  **non-canonical control transfer** (`#GP` sets no CR2 → address reported as -1), i.e. a corrupted
  jump-target pointer, not a null deref.
  `memory/lesson_votv_crash_dumps_live_in_localappdata.md`
- **2026-08-22 — every UE4SS C++ mod ships as `main.dll` (FOUR modules named `main.dll` in one real-env
  process: Multivoid + DebugMod + CrashContext + PBMovement), so a crash frame naming `main.dll` is
  AMBIGUOUS until matched by module BASE+SIZE** — ours is trivially the ~18 MB one (0x11A9000
  SizeOfImage; stock mods are 64 KB–816 KB); a naive resolver nearly attributed the 19:17 real-env
  crash to the wrong mod. Bonus recipe: a full minidump parse needs no cdb — ~80 lines of Python
  (streams: modules=4, exception=6, MemoryList=5 for the stack scan; `ModuleNameRva` @ +0x14,
  ThreadContext locator @ exception-rva+8+152; DEP-exec AV with RIP=0 = call through a NULLed
  pointer). LOOK FIRST: `tools/debug/parse_dump.py`; symbolize by rebuilding the
  deployed sha's commit for its PDB.
  `memory/lesson_every_ue4ss_mod_is_maindll_disambiguate_dumps_by_base.md`

## 6. Assets, models, geometry

- **2026-08-25 — A converter converts ARTIFACTS, not file extensions.** Asked whether UMG widgets
  authored in UE5 could be downgraded to the 4.27 the shipped game runs, every surface fact says yes:
  UE's refusal to load a newer package is a `FPackageFileSummary` **header** check and patching it is
  a known trick, and third-party *Asset Downgrader* tooling advertises 4.27 and 4.26 as targets. All
  true, and the conclusion is still wrong — **those convert DATA assets** (meshes, materials,
  textures), while a `WidgetBlueprint` is a **compiled `UBlueprintGeneratedClass` with bytecode
  serialized against the compiling engine version**, wrapped around a `UWidgetTree` whose UMG class
  layouts moved across 4.27 -> 5.x. The shared `.uasset` extension is exactly what makes the wrong
  answer sound right: the extension is a container, not a type, so *"tool X handles .uasset"* is
  underspecified in a way that reads as complete. Second trap: the failure arrives **last and
  silently** — not at conversion, but when the shipped game refuses to mount the pak, after the
  editor work, the cook and the package are all done. *Look FIRST:* read the converter's list of
  supported **asset types**, and if it is phrased in extensions that is the smell, not the spec;
  separate DATA from COMPILED (anything with bytecode, a generated class or a serialized graph is
  version-tied in a way meshes are not); and **prefer authoring in the target version over converting
  into it** — here the target engine was already installed with `PythonScriptPlugin` present, so the
  real gap was an *authoring channel* (no MCP supports 4.27), not a format. Fix the channel, not the
  format. `memory/lesson_a_converter_converts_artifacts_not_file_extensions.md`

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

- **2026-08-25 — a ctx GATE belongs in a hot callback; a ctx RESOLVE does not.** Obeying
  `[[lesson-vm-dispatch-verb-name-is-not-the-gate]]` ("check `av.ctx`'s class") put
  `if (!g_gunClass) g_gunClass = R::FindClass(...)` inside a `vm_dispatch` entry callback. `R::FindClass`
  was an **uncached, negative-unlatched walk of ~237k objects with a name render per object** (2026-08-25
  `ca1cd5e4` gave it the `BeginClassWalk` cache its three siblings already had, so a HIT is now O(1) --
  but a MISS still walks, and this defect was a miss EVERY time, so the case below is exactly the case
  the cache does not help), and
  `prop_coingun_C`'s UClass is not resident in the ordinary world (`[V]` the gun is in 3 of 261 maps),
  so the resolve FAILED and re-walked on **every left click of all 146 `playerHandUse_LMB` classes**.
  `Install` was already retrying the identical resolve inside a ~1 Hz throttle. The fix is
  `if (!g_x) return;` -- compare in the callback, produce in the installer. *Look FIRST:* grep
  `FindClass|FindFunction|FindObjectByClass` inside anything reached per-frame / per-ProcessEvent /
  per-overlap / per-0x45-verb / per-packet, and ask what it costs in the world where the feature is
  ABSENT -- an unlatched NEGATIVE turns a memo into a loop.
  `memory/lesson_a_ctx_gate_belongs_in_a_hot_callback_a_ctx_resolve_does_not.md`
- **`GetActorLocation`/`GetComponentLocation` are UFunction DISPATCHES, not raw reads** — never bulk-call
  per-tick over thousands of actors (invisible on a fresh save, hitches the host on a mature world);
  throttle / pre-filter / read the raw transform. *Look FIRST:* `engine.cpp GetActorLocation`. `memory/lesson_getactorlocation_is_a_ufunction_dispatch.md`
- **Per-tick `GUObjectArray` walk: cheap class check BEFORE `NameOf`.** COROLLARY (v114): a
  class resolver reachable from another module's hot path (savedScalar reader at every PropSpawn
  express) carries its negative-result backoff INSIDE itself — call-site throttles don't survive
  new callers. `memory/lesson_full_array_walk_cheap_filter_before_nameof.md`
- **A periodic FPS hitch by PERIOD COINCIDENCE is not causation** — measure the real source. `memory/lesson_periodic_hitch_not_the_walk_by_period_coincidence.md`
- **2026-08-23 — the `[HITCH]` message QUOTES the string `[HITCH-SRC]` in its own prose, so ANY
  substring grep for HITCH-SRC (even `-F "[HITCH-SRC]"`) matches BOTH lines and double-counts** — in
  the Linux triage this produced a false "HITCH-SRC median 74 ms, n=808" (really the [HITCH] frame
  values) and briefly inverted the fps-floor attribution; the clean split showed ONE real HITCH-SRC
  per 1–2 s and our tick <10 ms on the floor frames. Anchor markers at MESSAGE START (strip the
  timestamp+level prefix, then `^\[HITCH\]` vs `^\[HITCH-SRC\]`); instrument-design rule: never quote
  one marker's bracket-name inside another message's prose. *Look FIRST:*
  `memory/lesson_hitch_marker_greps_split_by_label.md`
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

- **An optimisation hoisted ABOVE the bound it was protecting.** To stop never-despawning coins from
  holding the WorldActor pose batch's 28 slots, a delta gate was added -- and computing the delta read
  the transform, moving `GetActorLocation`/`GetActorRotation` above the cap check they used to sit
  below. `[V]` each is a full ProcessEvent dispatch WITH a per-call heap allocation (`ParamFrame`'s
  ctor is documented "per-call, NOT cached"), so a hard 28x2 per tick became 2 per LIVE actor per tick
  at 125 Hz -- ~50k dispatches and ~50k allocs/sec at 200 coins. It bought a bounded resource with an
  unbounded cost. A second defect rode along: recording "last sent" before the truncation check let a
  truncated actor latch its final pose as delivered and freeze its mirror for the session. LOOK FIRST:
  when adding a skip-test to a loop, price the TEST, and place it relative to EVERY existing bound --
  ask what the old worst case was and what the new one is. If the bounded resource really needs
  protecting, fix it INSIDE the bound (fairness / rotating start), not upstream of it.
  `memory/lesson_an_optimization_hoisted_above_the_bound_it_protected.md`

## 8. Build / deploy / git hygiene

- **2026-08-25 — Measuring a tool's OUTPUT and recording it as its INPUT. The doc even said
  "measured", and it was — of the wrong artifact.** `UE4SS_ARC.md` §7.2, *"The package shape —
  measured from a real VOTV UE4SS C++ mod"*, was read off the extracted r2modman profile
  (`...\profiles\Default\shimloader\mod\acitulen-DebugMod\`), a directory that genuinely exists and
  holds exactly what was written down. **But that is the install's output; the zip is its input**, and
  the manager performs three transformations the record had un-done none of: it **inserts**
  `shimloader/` (profile root) and `<Author>-<Name>/` (from `mod.getName()`), and **strips** the
  matched folder's own name (`mod/`). So §7.2's tree put `dlls/` at the zip root — where the rule
  engine, finding no matching route, recurses in, fails to match `.dll` on any extension rule, falls
  to `isDefaultLocation`, and `installSubDir` copies it **by basename** to `Mods/<pkg>/main.dll`.
  UE4SS scans `Mods/<name>/dlls/main.dll`. **The package installs successfully, appears in the
  manager, and the mod never loads — no error anywhere**, so the first report would have been a
  player saying it does nothing. It survived a second pass too: §7.1 said the manager *"extracts it
  whole"*, which is the error stated outright, and read as fact for two days. *Look FIRST:* when a
  tool transforms an artifact, write down the TRANSFORMATION before the shape — what does it add,
  strip, rename? Then subtract. Prefer the input when it is obtainable (`unzip -l` on the five real
  packages answered this in one command). Hunt for the transformer's own TESTS — r2modman ships
  `Shimloader.Tests.spec.ts`, which asserts every source→destination pair and is the cheapest
  authoritative spec there is. And suspect any sentence containing *"extracts it whole"* / *"just
  unpacks"* / *"1:1"*: those claim a transformation is the identity, and they are rarely checked.
  `memory/lesson_measuring_the_output_and_recording_it_as_the_input.md`

- **2026-08-25 — A plugin's real ABI coupling is in its IMPORT TABLE, not in the framework's
  headers.** Asked what D-3's "slim C-ABI contract" actually buys, the instinct is to read UE4SS's
  SDK — which answers *what is available to bind*, an upper bound shared by every consumer, and
  cannot tell a mod using two entry points from one using 130. Parsing the PE import/export tables of
  three shipped VOTV mods answered it as a number: `Moddy-CrashContext` **32**, `Moddy-PBMovement`
  **40**, `acitulen-DebugMod` **130** MSVC-mangled C++ symbols imported from `UE4SS.dll` — with
  `std::` types crossing the DLL boundary (`?on_dll_load@CppUserModBase@RC@@UEAAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@@Z`)
  — versus Multivoid's **0**. Their binding surface is (this UE4SS build) x (this MSVC STL): an
  upstream signature change is a *missing import*, so the loader fails the DLL outright with no
  degraded mode, which is why the whole cohort pins `unreal_shimloader-1.1.7`. Free finding in the
  same glance: all three link the **dynamic** CRT (`MSVCP140`/`VCRUNTIME140`/`api-ms-win-crt-*`) and
  therefore need the VC++ redist; we link static (`CMakeLists.txt:186,691`) and do not — a
  player-facing install fact invisible to source review. *Look FIRST:* for any "how coupled is X to
  Y" / "will this upgrade break us" question about a shipped binary, parse the import table, and
  compare **against the field** rather than only against yourself. No `objdump`/`strings` on this box
  is not a blocker — the PE walk (DOS header → `e_lfanew` → optional-header data directories →
  RVA-to-offset via the section table) is ~40 lines of PowerShell `[BitConverter]` with no dependency.
  `memory/lesson_a_plugin_abi_coupling_is_measurable_in_the_import_table.md`

- **2026-08-24 — Project prose that lives OUTSIDE the repo tree cannot be censused, and it is the
  most public prose you have.** The UE4SS arc's stale-loader-prose census
  (`votv-ue4ss-stale-loader-prose-CENSUS-2026-08-22.md`, ~139 rows) was complete over FILES and
  structurally blind to the GitHub repo's own About blurb, which still reads *"a standalone C++
  DLL"* — the exact claim the D-3 migration falsifies — plus a `dll-injection` topic, and an **empty
  `homepageUrl`** while `multivoid.dev` has been live since 2026-07-19. The user raised it; no
  instrument could have. Everything this project trusts for drift — `grep`, the CI gates,
  `ledger_lint`, `/documentize` Step 0.5 — operates on the working tree, so a field in GitHub's
  database produces no diff and cannot go stale loudly. The census was not sloppy: its scope silently
  equalled "what a grep can reach". *Look FIRST:* the off-tree surface list, kept in
  `docs/UE4SS_ARC.md` §7.0 — GitHub description/topics/homepage
  (`gh repo view --json description,homepageUrl,repositoryTopics`), the Thunderstore package
  description (write it correct the first time), the hand-deployed `site/` copy, Discord topic +
  pins, the release-body template. Sequencing rule: a public "what this IS" statement flips **with**
  the release that makes it true, never before — except a field already wrong on its own terms.
  `memory/lesson_project_prose_outside_the_repo_tree_is_uncensusable.md`

- **2026-08-24 — A running total stated in prose inside an APPEND-ONLY register is stale by
  construction.** `docs/security/TRACKER.md` narrated its own size in three dated notes — "15 OPEN"
  (07-20) → "Now 16 OPEN" (07-23) → "Still 16 OPEN" (07-24) — while rows kept landing (`A8 A9 W7 W8
  W9 W10 A7 S2` among them). Appending a table row is not the same edit as rewriting a paragraph
  three screens up, so nothing ever recomputed it. Thirty days later "16 OPEN" was quoted into
  the local-only docs-arc note twice, a `.gitignore` rationale block and a **public commit message** before a
  census ran: **33 rows — 18 OPEN, 12 BUILT, 1 DESIGN, 1 VERIFIED, 1 DISMISSED** (a neighbouring doc
  carried a third frozen figure, "20 OPEN", from the 2026-07-20 audit). The trap is that every note
  was honest *on its own date*, the file looks maintained, and quoting the number feels like citing
  the source rather than inheriting an inference. **The project had already paid for this exact shape
  in another domain** — `CLAUDE.md`'s modular file-size rule ("catalog is auto-generated, not
  hand-maintained; hand-curated catalogs go stale fast", after one listed `harness.cpp` at 3,126 LOC
  long past its cut to ~778). Second domain = generalise (`[[feedback-recurring-bug-is-architectural]]`).
  *Look FIRST:* never write an aggregate in prose in a document designed to grow — write the COMMAND,
  or a dated snapshot line with the command beside it; and recompute any count before quoting it
  outward, since a count is the cheapest measurement there is.
  `memory/lesson_a_running_total_in_an_append_only_register.md`

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
  link. Same class as the B2 not-ready skip: ONE delivery-guarantee owner. RECURRED 2026-08-23 at
  3× volume (485× rc=-25 → 282 expired container parks, measured end-to-end). **FIXED 2026-08-23
  (R-4b): `coop/net/send_backlog` — per-(slot,lane) FIFO of final wire bytes at the session layer,
  absorbs ONLY -LimitExceeded, FIFO-once-nonempty, hConn-stamped vs slot recycling; save family
  stays the pump's pacing lane (Begin now success-gated); 4MB SendBufferSize; bracket-anchored
  container-park aging; client inbox pause-not-drop. Bool contract: true = WILL deliver, false =
  never will. Drill: RED 956 losses on a pinned-buffer LAN join → GREEN 0, throttled-link episode
  5,783 msgs "all delivered", 0 expired parks. (An earlier "bonus fact" here — "the GNS default
  send RATE is a fixed 256 KB/s clamp; the field bottleneck was the clamp" — was FALSE for our
  binary and is retracted: `session_start.cpp` raises Min/Max globally to 1/25 MB/s since
  2026-06-06, and the field host ran at exactly 1 MB/s; see the next lesson.)** *Look FIRST:*
  `research/findings/network/votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md` +
  `memory/lesson_reliable_enqueue_loss_is_silent_and_contiguous.md`
- **GNS's RECEIVE side is lossless-by-stall at BOTH overflow caps** (measured in the vendored source,
  R-4b): reliable reassembly overflow returns do-not-ACK (`snp.cpp:3479-3493`); decoded-queue overflow
  (`RecvBufferMessages`=1000) propagates as no-ACK with NO stream advance (`snp.cpp:3807-3816`) -- the
  sender retransmits. Stopping/lagging the receive poll = true end-to-end backpressure, never
  GNS-internal loss or a kill. This is what makes the inbox's pause-not-drop (R-4b D10) safe --
  the fear "GNS under me will drop" is measured false. **UPDATED 2026-08-24:** the pause shipped
  CLIENT-ONLY, so the host re-created the very loss this row says GNS never inflicts, one layer up in
  our own code -- fixed in `5dd21bb6`, and the one-sided application is its own lesson
  (`memory/lesson_a_protection_added_to_one_role_only.md`). *Look FIRST:* session.cpp NetThread drain
  loop -- the check now sits ABOVE the `if (role == Host)` split, not inside a branch --
  + `memory/lesson_gns_receive_overflow_stalls_never_drops.md`
- **A library's DEFAULT is not your binary's effective config — census your own init path for
  overrides before recording a "default" as a fact** (SendRate pass, 2026-08-23): R-4b measured GNS
  stock `SendRateMin/Max = 256 KB/s` (`csteamnetworkingsockets.cpp:84-85`) and recorded it as the
  shipped behavior + filed a "raise it?" product question — but `session_start.cpp:79-84` had
  overridden BOTH globally (1/25 MB/s) since 2026-06-06, and the June memory recorded that raise.
  One false fact contaminated 7 prose sites before a critic forced the measurement. DETECTION
  instrument (the positive check, not just the caution): read the effective value your OWN
  telemetry logs — net-diag's `sendRate=` field showed exactly 1048576 in 180/180 field samples;
  a "default" claim that contradicts your own logged effective value is wrong by artifact. Also
  grep init paths for `SetGlobalConfigValue|SetConnectionConfigValue` (or the library's setter
  vocabulary) before asserting any library default. *Look FIRST:*
  `votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md` §7 +
  `memory/lesson_a_librarys_default_is_not_your_binarys_config.md`
- **A fan-out bool that conflates "refused" with "no audience" turns retry into a DUP generator**
  (seeds arc commit-0, 2026-08-23): `SendReliable` returns `anySuccess` = FALSE with ZERO
  world-ready receivers (session.cpp:241-250) -- indistinguishable from a real refusal -- so the
  lanes' retry-until-sent idiom re-broadcast solo-authored rows to the first joiner, whose SAVE
  already contained them: duplicate emails/signals, LIVE since v64 (audit-I-1's deliberate
  hold-and-retry). Once a lane's absentee coverage is save+seed, a zero-audience refusal is a
  VACUOUS SUCCESS (adopt sent=true, `!AnyWorldReadyPeer()`); ~15 other lanes consume the ambiguous
  bool -- censused + FILED, never flip one without its own seed (dup <-> loss trade). The /qf
  critic's "READ the return path before building" overturned three rounds of confident inference.
  *Look FIRST:* the lanes' vacuous-adopt blocks + seeds design doc par.2.6/par.4.
  `memory/lesson_a_fanout_bool_that_conflates_refused_with_no_audience.md`
- **A GT flag gating a NET-thread consumer lags one drain -- give the event two clocks** (seeds
  arc, 2026-08-23): MarkSlotWorldReady flips inside the ClientWorldReady GT drain case
  (event_feed.cpp:230) while the host relay checks it at NET receive (session_relay.cpp:86) -- a
  reliable received in [announce net-receipt, GT flip] was relay-skipped AND applied after the
  seed's cur-read (queued behind the announce, FIFO): lost on both legs. Meadow carried this
  micro-window from v120 unnoticed. Fix shape: a NET-thread view of the same event -- the
  hConn-stamped `relayEligible_` set at receipt AFTER inbox accept, cleared beside every
  FreeSlot; recycled-slot inheritance structurally impossible. *Look FIRST:* any per-slot gate
  consulted from both threads -- "which thread flips it, what does the other consumer do during
  the lag?" `memory/lesson_a_gt_flag_gating_a_net_thread_consumer_lags_one_drain.md`
- **Engine-resolved is NOT lane-settled -- anchor deferred applies on the lane's own predicate**
  (seeds-arc audit F-2/F-3/F-4, 2026-08-23): after a world travel `EnsureResolved()` succeeds
  while the array is still ASYNCHRONOUSLY FILLING (email's own 2026-06-19 prime-stabilize
  discovery), so the new apply park draining on bare resolve re-opened the clobber window one
  level down. Folds: drain gates on `g_primed` (count-stability); unsettled arrivals park too;
  stuck-front retries pace at 1 Hz REAL time (30 per-frame retries burned in ~0.4 s); tombstone
  expiry pauses while the park is non-empty. The trap: the lane ALREADY owned the settle
  machinery -- reuse it, never parallel it with a weaker generic predicate. *Look FIRST:*
  DrainApplyPark's "Audit F-2" comment + the prime-stabilize block.
  `memory/lesson_engine_resolved_is_not_lane_settled.md`
- **A wall-clock TTL on a park that waits for data from a DIFFERENT lane is a silent drop one level
  up** (R-4b round 4): ContainerContents rides Normal, its PropSpawn rides Bulk -- lanes deliver
  independently, so under backpressure the contents SYSTEMATICALLY arrive first, and once delivery is
  guaranteed a slow link holds the Bulk stream past ANY fixed TTL with zero wire loss (282 expired
  parks in the field). Fix shape (D9, built): anchor aging to the EVENT that proves
  arrival-or-orphanhood (SnapshotComplete is lane-ordered after every spawn it brackets); the TTL
  survives as a leak-guard only. Ask this of every fixed-TTL park whose feed rides another channel.
  *Look FIRST:* container_contents_sync.cpp `NoteJoinSnapshotBracket` +
  `memory/lesson_wall_clock_ttl_on_cross_lane_dependency_is_a_drop.md`
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
  `b2c4b3ef`. **RECURRED 2026-08-23:** the same address was still public for another month in
  `research/` — a committed raw session log (four lines: stun / turn / signaling resolve / P2P listen)
  and a findings doc — because the A11 sweep was scoped to `docs/`. Fixed by unpublishing the corpus
  (`cf3780d2`), not by a fourth grep; the durable form is the leak-sweep-scope row above.
  *Look FIRST:* `memory/lesson_public_hostname_is_not_a_public_origin_ip.md`

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
  rule's content stays the user's. **SECOND INSTANCE 2026-07-29, and this one constrained BEHAVIOUR:**
  `imgui_overlay.cpp:210-213` claims ESC-falls-through-to-the-pause-menu is *"the user-requested 'ESC =
  chat gone, user lands in the menu' behavior"*, and it was about to outrank the user's own *"close like
  minecraft"* in a live design decision. `git log -S"user lands in the menu"` returns ONE commit —
  `f32ed1b0`, a 250+-file backlog flush of sessions 11-16 — whose message records no such request.
  Asked in plain text, the user chose Minecraft-shaped, which made a CRITICAL defect **dissolve** rather
  than need a workaround. **The check is now two instances and one command: any comment saying "the user
  requested" that is about to DECIDE something gets `git log -S` first.**
  `memory/lesson_an_agent_authored_rule_becomes_a_user_rule.md`

- **2026-08-22 -- PowerShell: FullName is RESOLVED-absolute, so a prefix built with an unresolved
  `..` breaks every `Substring($prefix.Length+1)` rel-path.** install-ue4ss.ps1 truncated 9 chars
  off every extracted rel-path (`Mods\ActorDumperMod` -> `rDumperMod` beside the exe, three
  installs); files whose rel was exactly 9 chars landed correctly by accident, masking it. Fixed
  `fd4a5b71` by `Resolve-Path` before the length math. LOOK FIRST: any `FullName.Substring`
  rel-path computation -- confirm the base went through Resolve-Path.
  `memory/lesson_powershell_unresolved_dotdot_breaks_substring_relpaths.md`

- **2026-08-24 — A PUBLIC DOC THAT GIVES COORDINATES INTO REMOVED-BUT-HISTORICAL CONTENT *IS* THE
  LEAK.** Unpublishing a directory removes it from the TIP, not from history — and that was a
  considered decision. `[V]` What was not considered: a row in the arc doc's findings table -- then a tracked public file --
  named the exact path AND line range of a committed crash log, plus the four log lines it held, as
  *evidence that the origin VPS IP had leaked*. The value was in **zero** tracked files; the MAP was
  published. Two trivial steps, and the second is only findable because the first was printed. **Axis 2
  of the pre-push audit — the secrets grep — passed clean**, as it always does in this failure mode.
  The sentence was written BY the remediation (a precise citation is a better record — and a better
  treasure map), and it survived a full docs audit whose rule was about *citation bleed*, i.e. aimed
  one category away: that rule polices sentences about FINDINGS, this was a sentence about a FILE.
  *Look FIRST:* after unpublishing anything, **grep the still-public tree for coordinates INTO it** —
  `git grep -nE "<removed-prefix>[A-Za-z0-9_./-]+[:0-9-]*[0-9]"` over tracked docs; removing a
  container while keeping its index is not removing it. **Distinguish "reachable" from "advertised"
  when you write the decision down** — "history is not rewritten" reads as risk-accepted-and-closed,
  and it accepted reachability, not advertising. Name the CLASS, withhold the coordinates, say you are
  withholding them. And prefer unpublishing the CONTAINER to patching the row.
  `memory/lesson_a_public_doc_that_gives_coordinates_into_removed_content.md`

- **2026-08-24 — MOVING A SECTION OUT OF A FILE THAT AGENTS ARE *INSTRUCTED* TO READ silently costs
  them that corpus, and a tidy stub is worse than a broken link.** Section 9 of this very file was
  split out to a local-only path; every referential check passed (no dangling public link, new file on
  disk, honest stub). `[V]` But the `/qf` critic prompt tells its agent *"read docs/LESSONS.md ... it
  is your PRIOR-ART index"* — so every future critic would open this file, find prose where §9 was, and
  move on, losing the peer-identity / trust-boundary / receive-boundary / authority-signature rows.
  **The user caught it, not me.** What I had checked was REFERENTIAL integrity (does every pointer
  resolve); what I had not checked was INSTRUCTIONAL integrity (is anything told to read this file, and
  does that instruction still get what it came for). A broken link announces itself; a well-written
  stub reads like a complete short section, so the tidier it is the quieter the loss — and a critic
  that never saw the corpus just asks slightly worse questions forever, with no signal anywhere.
  *Look FIRST:* before moving a section out, **grep who is INSTRUCTED to read the file** (skills,
  agent prompts, CLAUDE.md's reading order) and ask what each reader came for. Fix it in BOTH places —
  an agent-addressed directive in the stub AND the instruction itself naming both files. Note the
  knock-on: this ledger's 1:1 `memory/` pairing now SPANS two files, so a pairing check that greps one
  reports false violations. `memory/feedback_moving_a_section_out_of_an_agent_read_file.md`

## 9. Security (threat model, trust boundaries, peer identity)

> **READING THIS FILE AS AN AGENT (`/qf` critic, auditor, reviewer): OPEN
> `docs/security/LESSONS_SECURITY.md` TOO.** It is on disk in every local clone and it holds this
> section's rows in full. A prior-art scan that stops at this stub silently loses the entire security
> corpus — peer identity, trust boundaries, the receive-boundary lessons, the authority-signature
> ones — which is exactly the class of question a critic is most useful for. Treat the two files as
> one ledger: the pairing rule (`memory/lesson_*.md` twin for every row) spans both.

**These lessons are LOCAL-ONLY as of 2026-08-24 — they live in `docs/security/LESSONS_SECURITY.md`,
which is untracked and `.gitignore`d. The file is on disk; this pointer resolves in a local clone and
does not resolve on GitHub, deliberately.**

Why this section and not the rest of the file: sections 1-8 are lessons about the ENGINE, the
architecture and how to work — publishing them costs nothing and helps anyone reading the code. This
section is different in kind. It carries reasoning about weaknesses that are **still open** (peer
authentication being the standing one), and it cites the finding register by ID and by code location.
That is the same shape as the register itself, which left the public tree on 2026-08-23 — so keeping
its digest public was an inconsistency, not a decision.

The split is deliberate about what it does NOT claim: nothing here is secret, the repo's history is
not rewritten, and removing a map does not close a hole. The public statement of what Multivoid does
and does not protect is the root **`SECURITY.md`**, and that is the right place to send anyone asking.

Lessons about a SHIPPED fix are still fine to state publicly and several do appear in sections 1-8 —
attribution of a closed finding reveals nothing. What moved is the live-weakness reasoning.

---

Back to: section 0 (the DIG-RULE) · section 1 (how to work).
