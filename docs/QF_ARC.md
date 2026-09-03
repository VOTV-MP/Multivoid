# /qf arc — revising the Question-Form skill on its own evidence (LIVE doc)

> **Canonical LIVING doc for the `/qf` skill revision.** Opened 2026-09-02 on the user's ask (§0).
> The skill is `.claude/skills/qf/SKILL.md` (396 lines; its critic prompt, lines 127-210, is one
> 8,773-character string); its companions are `docs/QUESTION_FORM_AGENT.md` (the critic's format +
> patterns), `docs/QUESTION_FORM_QF_SKILL.md` (the skill's shape vs `/qf-workflow`) and
> `docs/OPUS_48_DISCIPLINE.md` (the lens the critic must quote every round).
>
> Status tags, same vocabulary as the other arcs: **DECIDED** · **AS-BUILT** · **PENDING** ·
> **DESIGN** · **`[V]`** measured (the instrument is named beside every number) · **`[A]`** taken
> from the lessons ledger, not re-measured here · **`[?]`** unverified.
> **TIER 1 IS AS-BUILT (2026-09-02 evening, user green light "Ok I green light it"): step 0 `03748f56`,
> WP-3 step 1 `62825440`, WP-6 `d43888cd`, WP-3 steps 2-3 `c3960b01`, WP-1 `c2eb4f60` — see §10.**
> **THE ANCHOR VERIFIER REFUSED TWO TRUE ANCHORS AND WAS FIXED TWICE (2026-09-03, `057379dd` +
> `432beeb6`, drilled by the NEW `tools/qf/ledger_drill.py`).** Both surfaced in one 20-round pass and
> both are the same false-DEAD class as `lessons_gate`'s `CITE_ROOTS`: (1) a **bare basename** — the
> critic wrote `status_census.py:602`, a file tracked at exactly ONE path, and the verifier resolved
> only against the repo root; resolution is now unique-or-refuse, and the pre-fix code silently
> resolved `README.md` to the ROOT readme, so it also closed a false-VERIFIED against a file the critic
> never opened. (2) a **counting pipeline's first FIELD** — `uniq -c | head -1` prints `14 2026-08-30`
> and the verifier demanded a bare number; the claim may now match the first field, and only a digit
> field, so a date can never answer a year-shaped claim. Five drill arms, both directions.
> WP-4 / WP-5 / WP-2 remain DESIGN below the cut. §8 is the audit log — three passes folded
> 2026-09-02 (one in-session, two independent agents, 44 findings, all dispositioned); §9 the
> build order with the cut line; §10 the as-built record and what the build found.

---

## 0. THE ASK (USER 2026-09-02, verbatim)

> *"How can we improve the /qf skill - what are you suggestions? Only the best really beneficial ones"*
>
> then: *"Put the planned changes in a hot doc, then audit the doc"*

So the deliverable is the SHORT list — the changes with the largest measured benefit — not a
rewrite of the skill, and not every idea that could be argued for. §3 opens with the ranking and
the cut: **three changes ship now (WP-1, WP-3, WP-6); three are parked behind the first census
(WP-4, WP-5, WP-2).** §5 records what was looked at and deliberately left alone.

## 1. What the skill is, in one paragraph

`/qf` automates the copy-paste ritual of `docs/QUESTION_FORM_AGENT.md`: the primary (the main
session) writes a BRIEF about its own work, spawns ONE fresh read-only critic agent that must reply
in 2-4 questions only, relays the questions, answers them in the open, appends the round to
`<scratchpad>/qf_thread.md`, and repeats up to N rounds (`/qf N`, clamped 1..15, re-invoked to
continue) until the critic returns "that holds". The skill's history is a sequence of blind-spot
patches, each appended to the critic prompt after a defect slipped a pass. `[V]` The dated marks
in the file itself (`grep -o '2026-0[6-9]-..'`): **2026-07-09** (x2, the 15-round floor, after a
pass stopped at 6 and shipped a dupe), **2026-07-13** (x5: selective trust AND identity-map
completeness), **2026-07-14** (x4: carried framing + reframe-surface), **2026-07-22** (x2:
answers-the-actual-ask), **2026-08-24** (x4: the radical mandate); the one 2026-06-12 is a cited
RE-doc date, not a patch. One standing user rule is NOT in the file: the 2026-07-30 *"run the loop
to convergence, do not hand it back every round"* lives only in memory and the ledger — see WP-6.
Each patch was right. The measured question this doc answers is whether the patches are being
EXERCISED.

## 2. Evidence base

Two sources. (a) The skill's own output: **64 `qf_thread*.md` files across 11 session scratchpads**
(44 of them in one) under `%LOCALAPPDATA%\Temp\claude\D--Projects-Programming-VOTV-MP\<session>\scratchpad\`,
dated 2026-07-13 .. 2026-09-02 (local-only; they never enter the repo). (b) Every `/qf` row in
`docs/LESSONS.md`. Commands are in Appendix A; each row names what the number actually counts,
because several of these instruments are UPPER BOUNDS, not exact rates, and one (E3) turned out to
measure mostly the wrong party.

### 2.1 The thread files `[V]` (instrument: grep census, Appendix A; every number re-derived blind by the pass-2a agent)

| # | Measurement | Value | What it actually counts / limitation |
|---|---|---|---|
| E1 | Rounds recorded | **606** | Markdown headers matching `round <n>` over all 64 files. Exactly ONE identical pair by md5 (two 3,218-byte archives), so duplication is negligible; still an upper bound on distinct rounds because a thread continued across sessions is two files |
| E2 | Question records | **850** | Lines opening with `Q<digit>` (optionally bolded) |
| E3 | Question records carrying a `file:line` reference | **87 of 850 — mostly the PRIMARY's** | The files mix at least three formats (pass 2a, a 21-line spread sample over 14 files): about 27 % of `Q` lines are one-line `topic -> answer` records, about 19 % are wrapped full-sentence critic questions, the rest compressed paraphrases. The `?` counts (341 of 850 contain one, 248 end with one) are depressed by hard-wrapping (343 `Q` lines continue on the next line, 126 of those carry the `?`), so they are not evidence of paraphrase. A 15-of-87 citation sample: 13 primary answers (`"Q1 MEASURED: IsSlotConnected reads ... (session.h:374-377)"`), 2 critic questions. So the critic's own citation rate is **unmeasured** from these files — a non-zero lower bound exists, no rate does — and becomes measurable only when the critic's text is persisted verbatim (WP-3 step 1) |
| E4 | Mentions of a lessons row anywhere in the files | **15** | `LESSONS.md`, `lesson row`, `the project already learned`; counts primary AND critic text (9 files, max 3 in one), so the critic's rate is at most 15 in 606 rounds. An upper bound on MENTIONS: a critic that read the ledger and found nothing to cite also shows 0 |
| E5 | Rounds whose record says proof-of-read was verified | **33 of 606** | `proof-of-read` / `proof of read` mentions; measures the RECORD, not the act — the skill requires the check but nothing makes it observable |
| E6 | Discard / re-spawn events recorded | **12** | `re-spawn`, `respawn`, `discarded the critic`, `format fail` |
| E7 | The six capitalised angle labels used, all six combined | **15 in 606 rounds** | `FRAMING-PROVENANCE` 4, `CROSS-ANSWER` 5, `SOURCE-CONSISTENCY` 2, `UNDONE-CHEAP-MEASUREMENT` 2, `IDENTITY-MAP-COMPLETENESS` 1, `ANSWERS-THE-ACTUAL-ASK` 1. The ten lower-case angles are ordinary prose and were not counted |
| E8 | Of the 25 largest threads BY BYTE SIZE, those with ZERO `that holds` in the file | **11 of 25** | (Corrected in pass 1 from "8", a miscount on a by-round-count list, which gives 9.) A thread may have converged in other words or continued in another session's file; still, nearly half of the longest passes carry no recorded verdict |
| E9 | Round counts of those 25 | median 14, max 43 | 12 of 25 exceed the 15-round cap, i.e. were re-invoked; A54 ran 40 rounds in one 127 KB file |
| E10 | Ledger the critic is told to read every round | **820 KB / 7,615 lines** (as of `504b5c90`) | `docs/LESSONS.md` 759,592 B / 7,092 lines + `docs/security/LESSONS_SECURITY.md` 60,058 B / 523 lines (`wc`); append-only, so it only grows. Section 1 "How to work" alone is 271 rows / 379 KB, and **all twelve L-rows below sit in it** (pass 2b) |
| E11 | The critic prompt | **8,773 chars** | `SKILL.md` lines 127-210, one string literal, carrying 16 named angles (10 lower-case, 6 capitalised), two "Born ..." histories and two dated notes |
| E12 | Words the skill never contains | `git diff` 0 · `SIZE` 0 · `oscillat`/`reversal` 0 · `product` 0 · `screenshot`/`render` 0 · `substrate already`/`writers`/`census` 0 · `2026-07-30` 0 | `grep -c -i` on `SKILL.md`; the same words are absent from `QUESTION_FORM_AGENT.md` except `product` (1) and `render` (1) |

### 2.2 The lessons ledger `[A]` (each row cited by its title in `docs/LESSONS.md`; all twelve re-found by both audits with matching dates)

| # | Dated instance | What it says about the ritual |
|---|---|---|
| L1 | *"BUILT, drilled, green" is a statement about the DRILLS* (2026-07-29) + `memory/project_chat_impl_qf_twelve_defects_2026_07_29.md` | An IMPLEMENTATION pass in which the critic read the REAL DIFF (2,045 lines of shipped code, every question citing a line) found **12 defects in 4 rounds**; two passes over the design BRIEF (21 + 17 rounds) found none of them. Note what it evidences: the critic's READ TARGET, not a thin brief — that pass had a full brief |
| L2 | *ASK WHAT THE SUBSTRATE ALREADY DOES BEFORE DESIGNING THE MECHANISM* (2026-08-30) | Three subsystems died in ONE pass when a fresh critic ASKED the lookup question the primary skipped (`join_progress` already owned the cover; the game already does sub-windows as switcher siblings; Slate clips for free) — the "existing owner" angle the prompt already carries, exercised |
| L3 | *BEFORE A VALUE BECOMES OURS, CENSUS WHAT IT ALREADY IS -- and grep its WRITERS* (2026-08-29) | 13 rounds shipped an identity install that `StartP2P` overwrote 150 lines later; a recorded MISS: *"a `/qf` critic cannot catch this: it interrogates the design's frame, and the frame never contained 'something else already writes this'"* — the motivation for a WRITERS reflex the prompt lacks |
| L4 | *A REFERENCE CAPTURE MUST MATCH THE CONDITIONS OF THE REPORT* (2026-08-31) | A fresh critic caught the wrong reference in its FIRST question, *"purely by listing the folder"* — the one row that documents the critic itself LOOKING |
| L5 | *If the fix GROWS every round, you have not found the root yet* (2026-08-24) + `memory/feedback_a_converged_fix_should_shrink_not_grow.md` (the per-round table) | 9 rounds accreted six mechanisms, all discarded; *"state the fix's SIZE in every `/qf` brief"*; *"four or more discarded mechanisms is the same signal"* — the skill has neither (E12) |
| L6 | *Oscillation on an axis means the axis is not what decided it* (2026-07-28, second occurrence 2026-07-29) | Four reversals on one axis; six rounds with net diff ZERO; *"treat the second reversal on one axis as a stop signal"* — the skill has no such signal (E12) |
| L7 | *A DECLINED PRODUCT QUESTION DOES NOT GO AWAY* (2026-07-30) | The critic raised the product question in rounds 1 AND 2; the primary declined both, ran 13 more rounds, built, and the user reverted it the same session. Its own Look-FIRST: *"before a design pass, answer 'whose complaint is this?'"* |
| L8 | *A pass can measure every MECHANISM and never measure the DELIVERABLE* (2026-07-30) | Nine rounds corrected sixteen claims and none asked what appears on screen; the design was about to ship describing itself as delivering nothing |
| L9 | *A CHAIN-DERIVED DESIGN IS BLIND TO SIDE EFFECTS AUTHORED BEFORE THE CHAIN* (2026-08-31) | Eight rounds + a full bytecode disassembly missed two artifacts that ONE rendered frame showed; *"a design that has never been looked at has only been proven self-consistent"*. The frame that shows them is the game's CURRENT death screen — a before-state, available during a design pass |
| L10 | *Re-anchor on the ORIGINATING ask, not the thread you are currently in* (2026-07-29) | A sub-thread consumed **five passes, 75 rounds** while the ROOT request's NEXT list held a free, measured deliverable (the chain was glyphs → "glyphs in chat too" → a look at chat → "there's no history" → chat history); *"`/qf`'s ANSWERS-THE-ACTUAL-ASK angle did not catch this and structurally cannot"* |
| L11 | *RUN THE `/qf` LOOP TO CONVERGENCE* (2026-07-30) + `memory/feedback_run_the_qf_loop_to_convergence.md` | Rounds 12-22 run back to back each landed a finding; the pass converged at round 22. The rule EARNS its cost and is kept (§5); it is also NOT in the skill text (WP-6) |
| L12 | *A targeted grep is not a census* (2026-08-25) | The same error five times in one session, four caught by a critic — the critic works when the question forces a measurement |

What the rows support, precisely (pass 2b corrected an over-statement here): L1, L4 and the
skill's own `MAY read the repo` (SKILL.md:25) support making the critic LOOK (WP-1); L2 shows the
lookup QUESTION already in the prompt paying off; L3 motivates a new WRITERS reflex; L12 shows a
critic catching a class when its question demands a measurement.

### 2.3 What the evidence says, in three lines

1. The critic's input is PROSE the audited party wrote, and the skill only PERMITS it to look at
   code (SKILL.md:25) — it never requires it. The one pass whose critic read the real diff
   out-found two brief passes twelve to nothing (L1); the one recorded critic-LOOK caught its
   defect in the first question (L4). How often the critic actually looks is unmeasured today (E3)
   — the files do not keep its words.
2. The skill's safeguards are honor-system. The 820 KB ledger read (E10) is unverified and shows up
   in 2.5 % of rounds at most (E4); proof-of-read is recorded in ~5 % (E5); the six capitalised
   angles are labelled fifteen times in six hundred rounds (E7).
3. Four stop signals the project paid for are absent from the skill by grep (E12; L5-L9), the
   largest single waste on record (L10, 75 rounds) is a shape the current ask-anchor cannot see, and
   one standing user rule (L11) never reached the skill text at all.

## 3. The changes, ranked, with the cut (DESIGN)

| Rank | WP | What | Evidence class | Ships |
|---|---|---|---|---|
| 1 | WP-1 | The critic LOOKS: a DIFF phase + one anchored lookup per round, converged rounds included | measured (L1, L4) + the skill's own MAY | **now** |
| 2 | WP-3 | Persist the critic verbatim; structured reply on ONE shared schema; a per-question ledger; convergence as a printed state | measured (E3, E5, E7, E8) | **now** (step 1 is a one-line change) |
| 3 | WP-6 | Run to convergence is the DEFAULT; one stop list; no round cap as a hand-back | a standing user rule absent from the file (E12, L11) | **now** (text only) |
| — | — | **the cut** — below it, benefit is inferred from ledger rows, not measured on this skill | | |
| 4 | WP-4 | Four stop-signal fields in the brief (fix size by KIND, reversals, whose complaint, deliverable + frame) | ledger rows L5-L9 | after the first census |
| 5 | WP-5 | The ORIGINATING-ask chain, to the root, with each NEXT list | one ledger row, L10, 75 rounds | after the first census |
| 6 | WP-2 | Ranked prior art in place of the whole-ledger read | E4/E10 + a flood measurement | after the first census; its ranking needs its own dry run |

**Dependency edges (pass 2b, finding 12):** WP-1(b)'s "say what you opened", WP-4's
`handed-to-user`, and WP-6's stop list all use WP-3's vocabulary — so WP-3 **step 1** (verbatim
persistence, one line) is the precondition for measuring anything, and the WP-3 schema carries
every field from day one (`anchor`, `priorArt`, the status column, `stop` rows); WP-1/2/4/5/6 add
the RULES that make fields required. Nothing below the cut is deleted; each is parked with its
own pass-2 findings folded and waits for the tier-1 census (§9 step 5) to say whether the prompt
paragraphs that carry it are exercised at all — the doc's own §2.3(2) is the reason to doubt that.

Each WP: DEFECT → EVIDENCE → CHANGE (file by file) → MECHANISM → COST → ACCEPTANCE.

### WP-1 — Point the critic at code, not at the primary's prose (ships now)

**Defect.** The critic's input is a brief the audited party wrote, and the skill merely permits it
to open the repo (SKILL.md:25, "MAY"). The ritual's only measured out-performer (L1) was the pass
whose critic READ the diff, and the skill does not contain the phrase `git diff` (E12). The one
recorded critic-LOOK (L4) caught its defect in the first question; the one recorded critic-MISS
(L3) was a fact no brief could contain — who else WRITES the slot.

**Change (a) — a DIFF phase.** `SKILL.md` §"Phase the ritual" gains a fourth phase after
IMPLEMENTATION: **DIFF pass** — run on the diff that will SHIP, after the last edit and BEFORE any
smoke or handoff. It is licensed by the standing `/qf`-before-implementation rule
(`memory/feedback_qf_before_implementation.md`; `feedback_no_design_architect_agents.md` "still
allowed") — a question-only critic, not a review agent. Its brief is thin in ONE sense: no
CLAIMS / HYPOTHESIS prose. It keeps the ask (§0 section as today), the design's claimed invariants
as a numbered list of FALSIFIABLE sentences (e.g. *"3. no client-authored `PropDestroy` reaches
`ApplyDestroy` without passing `arbiter::Validate` first"*), the range with its hash
(`git diff <range> | sha1sum`), and PRIOR ROUNDS as the ledger's rows with status — the section
the skill has so the critic escalates instead of repeating (SKILL.md:116-118); dropping it would
also starve CROSS-ANSWER and SOURCE-CONSISTENCY. Range rule: uncommitted work = `git diff HEAD`
(plus `--cached` if staged); committed work = `<base>..HEAD`; the brief states which. A changed
hash forces a re-read and is recorded on the round. The critic reads the diff itself (read-only)
and the touched files around each hunk.

**Anchors.** Every question in this phase carries an ANCHOR: a `file:line` in the diff or in a
file the diff touches; OR, for an OMISSION (the most valuable class — a missing late-join row, a
missing gate), the symbol named in `targetClaim` + the file or tree the brief says owns it + the
zero-hit command; OR, for an ANSWERS-THE-ACTUAL-ASK or product angle, the verbatim `targetClaim`
quote. A question with no anchor is discarded — that one question, not the reply. **A converged
or read-only-floor reply carries its own anchor: the last lookup the critic made that found nothing
to ask (command + result)** — otherwise the cheapest rule-compliant reply on round 1 is
`converged: true` with nothing opened, and the terminal stays honor-system (pass 2b, finding 3).
`ledger.py append` re-verifies every anchor (a `file:line` exists and is within `wc -l`; a grep is
re-run) and records the result — an anchor is not honor-system either. The pre-deploy checklist in
`CLAUDE.md` is NOT changed by this doc; the DIFF pass sits before it, not inside it.

**Change (b) — one lookup per round, every phase, converged rounds included.**
`QUESTION_FORM_AGENT.md` "Rules of the format" gains: *at least ONE of your questions — or your
converged reply — must rest on something you opened yourself this round: a file:line, a grep
count, a folder listing with sizes, a writer census of a global. The lookup's RESULT appears only
as the premise of a question ("`StartP2P:245` also writes this slot — which write wins?"); you
never state what it means.* That last sentence keeps (b) on the right side of the doc's own
anti-pattern *"doing the primary's research for it and presenting conclusions"*. The critic prompt
carries the same sentence and adds the two lookup shapes the ledger paid for that the prompt does
not already carry: *who else WRITES this value?* (L3) and *does the reference artifact share the
report's conditions?* (L4). (L2's *"does an existing owner already do this?"* is already there.)
**A critic's lookup is a CLAIM**: the primary re-opens it before citing it; `answered-measured`
requires the PRIMARY's own citation, and a critic anchor alone yields `answered-inferred` (the
carried-framing class, `memory/feedback_qf_challenge_carried_framing_not_just_the_frame.md`).

**Mechanism.** (b) turns MAY into MUST for one question and for the terminal; (a) makes the diff
the read target for the phase where prose was measured to be blind. Neither lets the critic
design: a lookup is a read, a question anchored on a line is still a question, and a reported
observation is a premise the primary must re-open.

**Cost.** One extra pass per feature (a); one read per round (b). Both are cheaper than the rounds
they replace: L1's four rounds against 38.

**Acceptance (through the WP-3 ledger; E3 cannot see the critic).** 100 % of rounds — converged
rounds INCLUDED — carry ≥1 verified anchor; in DIFF passes 100 % of questions carry one. Until the
scripts exist, WP-3 step 1 (verbatim persistence) makes the same numbers grep-countable.

### WP-2 — Ranked prior art in place of the whole-ledger read (parked; below the cut)

**Defect.** The critic prompt says: read `docs/LESSONS.md` AND `docs/security/LESSONS_SECURITY.md`
and scan them — *"its domain sections + section 1 'How to work'"* (SKILL.md:133). That is 820 KB
per round (E10). Nothing verifies it — the proof-of-read line quotes only the two 15 KB docs — and
the citation ceiling is 15 mentions in 606 rounds (E4). The prior-art angle is the one the prompt
calls *"the single highest-yield question you can ask"*, and it is the one least exercised.

**Why the first draft of this WP was wrong (pass 2b, finding 2, `[V]`).** A keyword FILTER
floods: over the ledger's rows, `gate` matches 198, `host` 154, `client` 119, `sync` 94, `join` 78,
`mirror` 73, `spawn` 60, `identity` 56, `eid` 22, `arbiter` 8; the OR-union of five ordinary nouns
{identity, eid, gate, arbiter, mirror} is **291 rows ≈ 130 KB** at identity + title + 300 chars
(≥2 of the 5: 58 rows / 26 KB; ≥3: 8). Appendix A's own splitter, which also admits the six
non-lesson bare-`**` index lines, gives 633 rows, `gate` 202 and a five-noun union of 295 rows /
~110 KB — the same flood within a few rows. "≤2 KB of input" was off by ~50x, a zero-hit exit could
never fire, and domain nouns reach only 4 of the 12 L-rows — the other 8 are PROCESS lessons in
section 1 (271 rows / 379 KB), which the deleted instruction explicitly named.

**Change.** `python tools/qf/prior_art.py --nouns <3-8 nouns> --phase <question|design|impl|diff>`
(NEW, ~120 lines). `[V]` Row structure: 620 rows are `- **TITLE**` bullets with two-space-indented
continuation lines, and at least 7 lesson rows are bare `**TITLE**` paragraphs with no dash
(`LESSONS.md:3092`, `:5003`, `:6978-6986`), under 10 `##` sections (0-9) plus one `###`; ~115 rows
carry a `[[slug]]` (9 carry two) and 514 mention a `memory/…` path, mostly as cross-references to
OTHER rows — so neither is a key. A row = a line starting `- **` OR `**` plus its continuation
lines up to the next such line or header; **identity = `§<section number> / <first 60 chars of
the title>`, always**, with slugs and paths printed as extras. **Rank, don't filter:** score =
distinct nouns matched (title hits x2); print the top 12 DOMAIN rows plus a one-line count of the
rest; then a second tier of the top 6 section-1 rows matching the phase word or `/qf` (44 rows in
section 1 contain `/qf`). The empty signal is *"no row with ≥2 distinct nouns"*, not zero hits.
Measured input budget: 18 rows x ~400 chars ≈ **7 KB**, bounded by N, not by the ledger. The
primary pastes the output into a **PRIOR ART** brief section. The critic MUST run the same script
with ITS OWN nouns and cite any row the primary missed; if a listed row's cited symbol or path is
STALE against the tree, it asks about that too (the existing staleness clause, kept, scoped to the
rows in play). The proof-of-read gains a third fragment that **must come from a row NOT in the
brief's list** — `verify_proof.py` takes the brief's row identities as an exclusion list (pass 2b,
finding 7) — and the schema gains `priorArt: [{identity, fragment}]` so critic-found rows are a
column. The "read the whole ledger" instruction is DELETED (RULE 2), and the second tier is what
replaces its "section 1" clause.

**Mechanism.** The 820 KB honor-system read becomes a ~7 KB ranked input, and the critic keeps
its own reach into the ledger so the primary's noun choice is not the frame. The third fragment
proves a row the primary did NOT hand over was opened.

**Cost.** One script; one command per round for each party.

**Acceptance.** Row citation becomes 100 % of rounds BY CONSTRUCTION (the third fragment); the
informative number is the `priorArt` column — critic-found rows not in the brief's list, at least
one per pass. **Before it ships:** a dry run of the ranking over the twelve L-rows' own passes
(the nouns each brief would have carried) must reach the L-row that pass needed; if it does not,
the ranking is wrong, not the ledger.

### WP-3 — Persist the critic, structure its reply, keep a ledger, print the state (ships now)

**Defect.** The critic replies in free text, and the text is not kept: the thread files hold the
primary's paraphrase (E3), so nothing about the critic's own behaviour is measurable after the
fact. The primary is told to verify the proof-of-read line by hand and records having done so in
~5 % of rounds (E5). Sixteen angles in an 8.8 KB prompt (E11) are labelled fifteen times in six
hundred rounds (E7). Convergence is the critic's phrase "that holds" — a mood, not a state — and
`/qf` has no terminal for "everything left needs a runtime probe", which the workflow has
(`readOnlyFloor` in `tools/workflows/qf_root_loop.js`); nearly half of the longest passes carry no
verdict at all (E8).

**Step 1 (one line, ships first).** The guardrail *"Persist the thread"* (SKILL.md:380-382) is
changed to persist the critic's reply VERBATIM under its own heading before the primary's answers.
That alone makes E3 and E7 grep-countable with no scripts, and is the precondition for measuring
WP-1 and WP-6.

**Step 2 — one schema, shared.** The critic answers in ONE fenced JSON block. The schema is the
workflow's `CRITIC_SCHEMA` (`qf_root_loop.js:43-74`) as a SUPERSET — same field names
(`proofOfRead{qfDoc, opusDoc}`, `credit`, `unresolved[{q, whyItMatters, howToMeasure,
runtimeGated}]`, `converged`, `readOnlyFloor`, `convergenceRationale`), moved out of the JS into
`tools/qf/critic_schema.json` and read by BOTH `qf_root_loop.js` and `ledger.py` (one source,
RULE 2), with these additions and constraints:

- per question: `id`, `angle` (a CLOSED enum in the schema file, mirrored in the prompt; `append`
  rejects unknown values), `targetClaim` (a VERBATIM quote of the brief sentence under attack),
  `anchor` (WP-1's rules);
- `proofOfRead.priorArt: [{identity, fragment}]` (WP-2; empty until it ships);
- `q` is a QUESTION; `credit` keeps the workflow's wording (*"one line naming what is now
  GENUINELY measured"*); `howToMeasure` keeps the workflow's constraint (*"a concrete read-only
  probe: a real log grep, bytecode disasm, code read, or IDA site"*) AND carries its command — an
  imperative that is not read-only ("move the check above the role split") is a fix and is
  discarded; `convergenceRationale` is allowed only with `converged` or `readOnlyFloor` and is a
  LIST of `<id>: closed by <citation>`, no prose; `converged`/`readOnlyFloor` carry their own
  `anchor` (WP-1). **The discard rule covers every field**: any field carrying an instruction, a
  plan or a fix voids the reply.
- **The format rule changes with the format (RULE 2).** `QUESTION_FORM_AGENT.md` "Rules of the
  format" (`:12-16`) is amended to enumerate exactly these non-question fields BY NAME with each
  constraint; the prose `read: "..." | "..."` line is DELETED (replaced by `proofOfRead`).

**Step 3 — two scripts under `tools/qf/`.** `verify_proof.py '<qfDoc>' '<opusDoc>' [--prior
'<fragment>' --exclude <identities>]` greps each fragment against its source on text normalised
for whitespace AND markdown (`*`, backticks, `_` stripped on both sides — the docs wrap at ~100
columns and the rules are bold) and exits non-zero on any miss; two failures = re-spawn twice,
then a `format-failed` ledger row and a hand-back. `ledger.py append <round> <json>` writes one
table row per question ABOVE the primary's free-text answers, records the critic's flags, and
re-verifies anchors; `ledger.py set <round> <id> <status> "<citation>"` records the primary's
answer status in the turn it answers; `ledger.py pass <phase>` opens a scope (the phase rule lets
a design pass append to a question thread, SKILL.md:356-358, so state is per PASS);
`ledger.py stop <reason>` writes every stop, normal or not; `ledger.py status` prints the state.
The free text, the archive-and-rename rule and the file name are unchanged.

**Status vocabulary (closed):** `open` (default at append) · `answered-measured` (the PRIMARY's
own citation required) · `answered-inferred` · `runtime-gated` (probe named) ·
`withdrawn-with-reason` · `handed-to-user`.

**Convergence, ONE predicate in ONE place (pass 2b, finding 6):** in the CURRENT pass,
**converged** ⇔ the critic's `converged` flag ∧ zero rows in {`open`, `answered-inferred`,
`runtime-gated`, `handed-to-user`} ∧ a `bars attested <round>` row written by the primary (the six
existing convergence bars — five phrased *"INVALID while"*, one *"only real if"* — are the
primary's own map, which no ledger can see, so the primary attests them as a row);
**read-only floor** ⇔ the critic's `readOnlyFloor` flag ∧ ≥1 `runtime-gated` row ∧ zero rows in
the other three states ∧ the attestation; **capped** ⇔ the safety ceiling (WP-6), abnormal.
`status` prints exactly one of {open set, converged, floor, capped} from those rows and nothing
else. The six bars are re-phrased onto `converged: true`.

**RULE 2 sweep, by site.** `that holds` occurs 16x in `SKILL.md` (the six bars plus `:37, :209,
:224, :241, :264, :333, :347, :390`), 2x in `QUESTION_FORM_QF_SKILL.md` (`:61, :88`), 1x in
`QUESTION_FORM_AGENT.md` (`:53`); acceptance = `grep -c 'that holds'` is 0 across the three files.

**Mechanism.** The critic's words are on disk; verification is a command with an exit code;
angle coverage is a column; convergence is a printed state from one predicate; the workflow and
the skill share one schema file.

**Cost.** One line (step 1); two scripts (~200 lines together) and a fenced block in place of
prose. The critic's questions are still 2-4 and still questions — the JSON is a wrapper, and the
field constraints plus the all-field discard rule are the fence.

**Acceptance.** E5 becomes 100 % by construction (every round carries the verifier's exit line);
E7 becomes a per-pass histogram over a closed enum; E8 drops to zero because every pass ends in a
printed state; every stop is a `stop` row (WP-6's instrument).

### WP-4 — Four stop-signal fields in the brief (parked; below the cut)

**Defect.** Four dated lessons name a stop signal the ritual should have raised and the skill
carries none of them by grep (E12): fix SIZE (L5), oscillation (L6), a product question raised
twice (L7), and the DELIVERABLE never measured / never looked at (L8, L9). Each cost between six
and fifteen rounds in its recorded instance. **Pass 2b found the first draft of three of the four
rules wrong on their own motivating cases** (findings 8-11); what follows is the corrected form,
and the reason this WP is below the cut is that a rule that failed its motivating case once owes a
dry run before it ships.

**Change.** `SKILL.md` step 2 (the brief) gains four fields, and the auto-loop section gains the
matching stop rule for each. No critic-side "reflex questions" are added — those are the
prompt-paragraph class §2.3(2) shows does not fire; the fields are what the critic sees.

- **FIX SIZE, by KIND:** `<lines> / <new constants> / <new state fields> / <new API entries>`, the
  delta against the previous round with each delta item tagged **`planned`** (in round 1's plan,
  or a mandated row such as late-join) or **`reactive`** (answers a critic objection), and the
  running count of mechanisms designed-then-discarded. *Rule:* two consecutive rounds of REACTIVE
  growth, OR four or more discarded mechanisms (L5's own second threshold), = the next brief opens
  with a re-derivation of the defect in mechanism terms, before any design text. Walked on the
  recorded W10 rounds (`memory/feedback_a_converged_fix_should_shrink_not_grow.md`): R1 share cap →
  R3 full per-connection drain (reactive) → R4 pause the shared drain (reactive) → **fires at R4**,
  five rounds before the R9 ship; a skeleton gaining its seams and its late-join row is `planned`
  growth and does not trip it. A "size only" rule fires at R5 or never (the exemption for
  "extending" growth legalises exactly the accretion L5 names), which is why kind, not size,
  discriminates. A sudden SHRINK names which branch disappeared and whether it was load-bearing on
  the FAILURE path (L5's counter-case).
- **REVERSALS:** per axis argued, the count of reversals so far. *Rule:* the second reversal on
  one axis = stop, write (a) the ground the dependent decision was originally rejected on and (b)
  the axis now being argued, side by side, and hand it to the user (L6). Walked: fires at R10 of
  the font pass (R8/R9/R10/R11; the real contradiction was found at R12) and at the second flip of
  the chat-colour pass (six rounds, four reversals, net diff zero). It is an item of the ONE stop
  list (WP-6), the memory's *"a question only the user can answer"*.
- **WHOSE COMPLAINT + PRODUCT QUESTIONS:** a `WHOSE COMPLAINT:` line — *user-reported* with the
  quote, or *found-by-measurement*, in which case one sentence goes to the user in text BEFORE
  round 1 (L7's own Look-FIRST). *Rule:* a product-feel question is `handed-to-user` on its FIRST
  raise — the standing rule already says *"product-feel you hand back … never decided for them"*
  (`QUESTION_FORM_AGENT.md:46-48`) — unless it is answered `answered-measured` with the user's own
  dated words as the citation, which is how a fresh critic re-asking a decided question is
  answered without a second hand-back (the false positive pass 2b named). Walked: fires at round 1
  of L7, not round 2.
- **DELIVERABLE + FRAME, per phase:** the deliverable in the user's units — what they will see or
  do differently — and, for anything visible, `FRAME LOOKED AT: <path> | n-a (<reason>)`. Which
  frame: in a QUESTION or DESIGN pass, a capture of the CURRENT game at the seam, taken BEFORE the
  pass (L9's two artifacts are on vanilla's own death screen — the before-state is what the design
  was blind to); in an IMPLEMENTATION or DIFF pass, a capture of the built result. Frames are
  produced BETWEEN passes, never inside a round (a round does not run the game, SKILL.md:314-315).
  *Rule:* `converged: true` is INVALID while the deliverable sentence is missing or, for a visible
  deliverable, while no frame of the right kind has been looked at (L8, L9) — the seventh
  convergence bar. Walked: L8's *"ships zero new visible glyphs"* becomes the DELIVERABLE line
  and is measured like any claim; L9's design pass cannot converge without the vanilla frame.

**Mechanism.** Each field is a number, a tag or a list the primary must write down, so the signal
is raised by the act of filling the template, not by anyone remembering the lesson.

**Cost.** Four lines per brief and one capture per visible deliverable.

**Acceptance.** Not absence — **fires**: per pass, count each rule's fires and what each produced
(a re-derivation / a hand-back / a false positive withdrawn, with the round). A rule that never
fires in ten passes is re-examined, not celebrated.

### WP-5 — The ORIGINATING-ask chain (parked; below the cut)

**Defect.** The brief's first section quotes the CURRENT thread's request, and the
ANSWERS-THE-ACTUAL-ASK angle holds the design against that. A sub-thread inherits full legitimacy
from its parent and then competes with it for the whole budget. L10's chain was five links deep,
and the free deliverable sat in the ROOT request's NEXT list, not the immediate parent's — so a
"parent ask" line, the first draft here, would not have fired on its own motivating case (pass
2b, finding 10).

**Change.** `SKILL.md` step 2's first section gains an **ORIGINATING ASK** block: the chain of
requests from this one up to the root, each verbatim with its date or session and its NEXT list
as last recorded, and one sentence saying why this sub-thread outranks every ungated + measured +
small item on ANY list in the chain. Where a NEXT list lives: the request's `memory/project-*.md`
entry, the NEXT section of its design doc under `research/findings/`, its entry in the `CLAUDE.md`
reading order; `NEXT: unrecorded (<places checked>)` is a legal value and a ledger-visible fact.
`ORIGINATING ASK: none (root request)` for a root. *Rule:* a DESIGN or IMPLEMENTATION pass on a
sub-thread does not open until the sentence is written; the ANSWERS-THE-ACTUAL-ASK angle is
extended by one clause — *hold the design against the ROOT ask too, and ask whether depth here is
progress on it.*

**Mechanism.** Every NEXT list up the chain becomes live work the brief must look at, so the
comparison the user made by memory in L10 ("we started this when I wanted glyphs, remember?") is
made every pass by construction.

**Cost.** One block per brief; one look up the chain per pass.

**Acceptance.** Every `qf_thread.md` opened after this ships carries the block (grep), and no pass
exceeds fifteen rounds on a sub-thread without the sentence present.

### WP-6 — Run to convergence is the DEFAULT; one stop list; no cap as a hand-back (ships now)

**Defect.** `[V]` `SKILL.md:33-34` defines bare `/qf` as *"ONE exchange ... The user paces the
loop by re-invoking `/qf`"*, step 5 (`:221-223`) tells the primary to *"hand the loop back to the
user"*, and the one-round default is repeated at `:7, :14, :18, :46, :49` and in
`QUESTION_FORM_QF_SKILL.md:12-13, :57`. `[V]` The user's rule of 2026-07-30, verbatim *"Why are
you always stopping running qf, run qf"* (`memory/feedback_run_the_qf_loop_to_convergence.md`;
L11), names three stops — the critic's verdict, a question only the user can answer, a finding
that changes the ask — and its motivating pass converged at round 22; the string `2026-07-30`
does not occur in the skill (E12). The skill's documented default is the behaviour the user
corrected — and the first draft of this WP kept it, as a 15-round clamp that stops with OPEN rows
and asks for a re-invoke (pass 2b, finding 1: the scheduler role by another spelling).

**Change.** Bare `/qf` and `/qf <steer>` = run until a stop, the steer folded into every round's
brief. **The ONE stop list, in one place (the auto-loop section), every item a `ledger.py stop`
row:** (1) `converged`; (2) read-only floor; (3) `handed-to-user` — a question only the user can
answer, including WP-4's reversal and product items; (4) the reframe bar's own trigger (a premise
flips, a primitive turns out to be several, the model is replaced, a suppression dissolves —
SKILL.md:301-310), kept as its own item because a premise can flip while the ask stands; (5) the
SAFETY CEILING, 50 rounds (above the measured maximum of 43, E9), which writes `stop: capped` and
presents the OPEN set as a residual — abnormal by definition, never a request to re-invoke.
`/qf N` survives ONLY as the user's explicit pacing choice and writes `stop: user cap N`. The
"Two paces" paragraph, step 5's hand-back, the `loop/цикл → 15` default (`:233`), the 1..15 clamp
and every one-round-default site listed above go (RULE 2). The memory's non-stops are named as
non-stops: *"this is a good checkpoint"*, *"the findings are getting narrower"*, *"I should confirm
before continuing"*.

**Mechanism.** The loop's terminal is its own state, and every way it ends is a row.

**Cost.** Text only.

**Acceptance.** Zero `stop` rows in the next ten passes with a reason outside the list, and zero
hand-backs without a `stop` row (the transcript is not the instrument; the ledger is).

## 4. Housekeeping fact — the skill file is not in git

`[V]` `.gitignore:103` ignores `.claude/` whole, under the comment *"Claude Code per-machine config
(MCP paths etc.)"*. So `SKILL.md`, carrying its dated patches, exists only on this machine and has
no history (`git log` on it is empty). `[V]` Tested in two throwaway repos (pass 1, reproduced by
pass 2a): under `.claude/` + `!.claude/skills/`, `git check-ignore -v` still attributes
`.claude/skills/x/SKILL.md` to the `.claude/` rule and `git status --ignored` lists it `!!` — git
cannot re-include a path whose PARENT directory is excluded; under `.claude/*` + `!.claude/skills/`,
the same file shows as `??` (untracked, trackable) while `.claude/settings.local.json` stays `!!`.
So the working change is `.claude/*` + `!.claude/skills/` (no `**` needed). `[V]` `.claude/skills/`
holds THREE skills — `documentize`, `qf`, `qf-workflow` — and there is no `.claude/agents/`
directory; all three were swept for local paths, user names and `docs/security/` content and are
clean (the only security mention is `qf/SKILL.md:129` naming `LESSONS_SECURITY.md` by path, within
`DOCS_ARC`'s rule). That three-file sweep is step 0's gate. One-line `.gitignore` change plus
`git add .claude/skills/`; FIRST in §9 so the arc's own edits land in history.

## 5. Kept as-is, and what was looked at and declined

**Kept (earning their cost, by evidence):** the fresh critic per round and the primary answering
in the open (the design's foundation; the blind spots are narrowed by WP-1..WP-3, not by changing
the roles); run-to-convergence (L11 — every one of rounds 12-22 landed; WP-6 makes the skill text
say what the rule already says); the three existing phases (WP-1 adds a fourth, deletes none);
the six convergence bars (five phrased INVALID, one "only real if") and the reframe-surface rule —
they guard the primary's map, which no instrument can see, so WP-3 has the primary ATTEST them as
a row and WP-6 makes the reframe trigger an item of the one stop list; the radical mandate
section; `/qf N` as the user's explicit pacing choice.

**Removed on the audit's finding, not kept:** the 15-round clamp + re-invoke (WP-6, finding 1);
the critic-side "reflex questions" the first draft of WP-4/WP-5 added to the prompt (finding 27).

**Declined (so it is not re-derived):**

- *A persistent critic via `SendMessage`.* The fresh critic is the point: no anchoring. Continuity
  belongs in the ledger (WP-3), not in an agent's memory.
- *Moving the loop into a `Workflow`.* `docs/QUESTION_FORM_QF_SKILL.md` already records why a
  background loop cannot reach the primary; unchanged.
- *A model override for the critic.* No evidence either way; the critic inherits the session's
  model today and nothing measured here is a capability problem.
- *Tracking `.claude/` whole.* It holds per-machine MCP paths (the `.gitignore` comment); only
  `skills/` is re-included (§4).
- *Rewriting the critic prompt from scratch.* The prompt's content is right; its problem is that
  nothing measures whether it is used. WP-3's `angle` column is the cheaper fix, and a rewrite
  would discard patches each born from a real defect.
- *A keyword FILTER for prior art* (WP-2's first draft) — measured to flood; replaced by ranking.
- *Keeping the "who caught what" keyword counts in §2.* Deleted in pass 1 — the buckets matched
  unrelated sentences.

**Deferred (measure first):** a per-round angle ASSIGNMENT (the primary names 1-2 focus angles
per round from the phase). Worth doing only if WP-3's histogram shows angles that never fire —
that histogram does not exist yet.

## 6. Open questions for the user (product / policy, not engineering)

1. `docs/OPUS_48_DISCIPLINE.md` opens *"for the period when the project runs on Opus 4.8 ... you
   are running with less reasoning headroom"*, and the proof-of-read demands a fragment from it
   every round on every model tier. Keep it as the second proof source (its angles are the
   critic's lens regardless of tier), or point the second fragment at `docs/COOP_SYNC_DOCTRINE.md`
   for sync lanes? This doc assumes KEEP; nothing in §3 depends on the answer.
2. WP-1(a) makes the DIFF pass a fourth phase; should it be MANDATORY before any handoff (a
   pre-deploy checklist item 0), or default-on with an explicit skip line? This doc assumes
   default-on with a written skip reason; making it a checklist item edits `CLAUDE.md`, which is
   the user's file.
3. **DECIDED (USER 2026-09-02, verbatim: *"It's fine for the three to be public"*).** The three skill
   files stay committed and public, while their companions `docs/QUESTION_FORM_*` and
   `tools/workflows/` remain on the deliberate never-commit list (`.gitignore:265-266`) — two
   publication states for two classes of content, by the user's call. A public skill therefore
   points at local-only companions on purpose, and `schema_sync.py --check` reporting "absent" on
   another checkout is the expected state, not a defect.

## 7. Risks named in advance

- **WP-3 could turn the critic into a form-filler.** The JSON wrapper carries the same 2-4
  questions; the amended format rule names the only non-question fields and constrains each, and
  the discard rule covers every field, not only `q`. The first two passes after shipping should be
  read for this specifically.
- **WP-2's ranking could miss the row that mattered.** That is why it is below the cut with a dry
  run over the twelve L-rows' own passes as its gate, and why the critic runs the script with its
  own nouns.
- **WP-1(a) adds a pass.** It replaces the rounds that were finding nothing (L1); if a DIFF pass
  ever returns a converged reply in round 1 on a non-trivial diff, its anchor (the last lookup that
  found nothing) is what the primary reads first.
- **The safety ceiling (WP-6) could be read as the old cap.** It is 50, above every pass on record,
  and it writes `stop: capped` — an abnormal end that presents a residual, never a request to
  re-invoke.
- **This doc's own numbers are upper bounds** (E1, E4, E5) and one instrument measures mostly the
  primary (E3). The acceptance tests re-run the same instruments where they are sound and move to
  the ledger where they are not (WP-1, WP-6).

## 8. Audit log

Three passes on 2026-09-02. Two independent agents were first launched and both died on the API
usage limit before reading the doc; the same two lenses were then run in the main session (pass 1),
and the agents were re-launched on the corrected doc (passes 2a and 2b). Every finding is
dispositioned; nothing is OPEN.

### Pass 1 — in-session, both lenses

| id | Claim | Verdict | Disposition |
|---|---|---|---|
| EV-1 | E8 "8 of 25 largest threads have zero `that holds`" | **WRONG** — 11 of 25 by byte size; the 8 was a miscount on a by-round-count list (that list gives 9) | FOLDED |
| EV-2 | E3 "at most one question in ten cites a file:line" bounds the CRITIC | **WRONG PARTY** — the files hold mostly the primary's paraphrase + answer; the 87 citations sit in the answer text | FOLDED (E3 re-labelled; §2.3 and WP-1 re-based; WP-1's acceptance moved to the ledger) |
| EV-3 | E1 "606 rounds, upper bound" | CONFIRMED; exactly one identical file pair (md5), two 3,218-byte archives | FOLDED (quantified) |
| EV-4 | L1-L12 exist with the stated dates | CONFIRMED (`LESSONS.md` lines 155, 159, 163, 294, 742, 810, 833, 935, 952, 2004, 2252, 2433 as of today) | none |
| EV-5 | "who caught what": user 7 / critic 14 / audit 20 | **TOO CRUDE** — buckets match e.g. "user asked" in unrelated sentences | FOLDED (deleted) |
| EV-6 | "six INVALID bars" | **WRONG COUNT** — five use the word INVALID; the map-completeness bar says "only real if"; six total | FOLDED |
| EV-7 | §4 gitignore mechanics | CONFIRMED by throwaway-repo test; `!.claude/skills/` suffices | FOLDED (test recorded) |
| EV-8 | §1 patch dates | **INCOMPLETE** — 07-09 (x2), 07-13 (x5), 07-14 (x4), 07-22 (x2), 08-24 (x4), plus one cited 06-12 RE-doc date; 07-30 absent | FOLDED (the 07-30 absence became WP-6) |
| EV-9 | README registration; OPUS opening sentence; the memory files named in the design lens exist | CONFIRMED | none |
| EV-10 | Appendix A carried the Windows user name in the scratchpad path | **LEAK (minor)** | FOLDED (`$LOCALAPPDATA` form) |
| EV-11 | WP-2's row-splitting feasibility | CONFIRMED in outline; corrected by pass 2a F4 | FOLDED |
| DS-1 | WP-3's JSON fields are non-question content under a rule that forbids declarative sentences | HIGH | FOLDED — format rule amended; fields constrained; sharpened by pass 2b #5 |
| DS-2 | WP-1(a) "discard a question without a `file:line`" would discard OMISSION questions | HIGH | FOLDED — anchor admits the omission form; per question; sharpened by pass 2b #16 |
| DS-3 | WP-2 deletes the ledger read and with it the "stale symbol/path" clause | MEDIUM | FOLDED — clause kept, scoped |
| DS-4 | WP-1(b) "critic MUST look" vs the anti-pattern "doing the primary's research" | MEDIUM | FOLDED — the result appears only as a premise; sharpened by pass 2b #17 |
| DS-5 | RULE 2: the prose `read:` line beside `proofOfRead`; "that holds" beside `converged`; the hand-back beside the run-to-convergence rule | HIGH | FOLDED (WP-3, WP-6); scope widened by pass 2b #14 |
| DS-6 | Acceptance thresholds "≥50 %", "≥30 %" were arbitrary | MEDIUM | FOLDED — restated by construction |
| DS-7 | "changes are independent" contradicted by `priorArt`/`anchor` living in WP-3's schema | HIGH | FOLDED — dependency graph at §3's head; sharpened by pass 2b #12 |
| DS-8 | WP-4 REVERSALS "surface to the user" vs "do not hand it back every round" | LOW | FOLDED — it is an item of the one stop list |
| DS-9 | WP-3 status vocabulary, setter and CLI unspecified | MEDIUM | FOLDED |
| DS-10 | WP-1(a) diff range for uncommitted work unspecified | MEDIUM | FOLDED (+ the hash, pass 2b #23) |
| DS-11 | WP-5 "the parent's NEXT list" — where it lives | MEDIUM | FOLDED (three places; `unrecorded` legal, pass 2b #24) |
| DS-12 | `verify_proof.py` would fail on fragments straddling a wrapped line | MEDIUM | FOLDED (whitespace + markdown normalisation, pass 2b #21) |
| DS-13 | The WP-4 rules walked on their motivating rows | HIGH | **SUPERSEDED by pass 2b #8-#11** — the FIX SIZE walk was asserted, not executed on the recorded rounds; the PRODUCT walk missed L7's first-raise rule; the FRAME bar was unsatisfiable for a design pass; the PARENT walk was against the wrong link. All four re-walked in WP-4/WP-5 as now written |
| DS-14 | Is the doc itself "a fix that grows"? | MEDIUM | FOLDED as "if only one ships, WP-1"; superseded by the ranked cut (pass 2b #27) |
| DS-15 | Public-repo check | LOW | FOLDED |
| DS-16 | The skill's documented default contradicts a standing user rule | HIGH | FOLDED as WP-6; its first draft kept the cap (pass 2b #1) |

### Pass 2a — independent EVIDENCE agent (re-derived every number blind; 18 tool calls)

| id | Finding | Disposition |
|---|---|---|
| F1 | "60 files across 15 sessions" is WRONG: 64 files across 11 sessions (44 in one); E1/E2/E3/E5/E6/E7 reproduce on the 64 | FOLDED |
| F2 | E4 is 15, not 17 (9 files, max 3 in one) | FOLDED (E4, §2.3, WP-2) |
| F3 | E3's "dominant format" wording and its `?` statistic: the `?` deficit is a hard-WRAP artefact; the files mix three formats (27 % topic->answer, ~19 % wrapped verbatim questions, the rest compressed paraphrases); 2 of 15 sampled cites ARE the critic's. "Unmeasured" stays fair, with a non-zero lower bound | FOLDED (E3 re-worded; conclusion unchanged) |
| F4 | LESSONS.md rows are not all `- **` bullets: at least 7 lesson rows are bare `**TITLE**` paragraphs; `##` is 10 + one `###`; slugs ≈115-124 | FOLDED (WP-2 splitter accepts both; identity rule changed with pass 2b #19) |
| F5 | The prompt literal spans `:127-210` = 8,773 chars; two `Born` histories, not five; angles ≥16 | FOLDED |
| F6 | E12's exception clause named `frame` (not in the list; 17x in SKILL.md); the real second exception is `render` (1) | FOLDED |
| F7 | E10 was stale by one commit: another live session's documentize (`504b5c90`, 18:02) grew LESSONS.md to 759,592 B / 7,092 lines | FOLDED (pinned to the commit) |
| F8 | Off-by-ones: SKILL.md is 396 lines; bare-`/qf` text is `:33-34`; CRITIC_SCHEMA is `:43-74` | FOLDED |
| F9 | The dated-marks list omitted the cited 2026-06-12 and 07-09's multiplicity | FOLDED |
| F10 | EV-1's provenance: a by-round-count top 25 gives 9 zero-holds, not 8 | FOLDED |
| F11 | WP-6 said "the memory's stop list verbatim"; the memory says "that holds" and has no read-only floor | FOLDED (the mapping stated) |
| F12 | WP-4's "three discarded mechanisms" silently lowered L5's "four or more" | FOLDED (four) |

Everything else CONFIRMED by the agent's own re-run (E1 + the md5 pair; E2; E5; E6; E7; E8 11 of
25 by bytes; E9; E12; all twelve L-rows; the L1 and L11 memory files; the six bars; no
read-only-floor terminal in the skill; the §4 test in both variants; README registration; the
OPUS sentence; no user name or machine path; every pass-1 FOLDED row present in the body).
UNVERIFIABLE by construction: the account of the first agents dying; the per-round replay in
DS-13 (now superseded).

### Pass 2b — independent DESIGN agent (six lenses; 2 CRITICAL / 9 HIGH / 16 MEDIUM / 5 LOW)

| # | Finding | Disposition |
|---|---|---|
| 1 | CRITICAL — WP-6 kept the hand-back it claimed to delete: the 15 clamp stops with OPEN rows and asks for a re-invoke; `/qf N` for 1<N<15 is the same by another spelling; the user's rule lists three stops and its pass converged at 22 | FOLDED — no clamp; one stop list; safety ceiling 50 = abnormal `stop: capped`; `/qf N` = explicit user pacing only |
| 2 | CRITICAL — WP-2 as specified floods (five nouns → 291 rows / 130 KB), its zero-hit exit can never fire, and deleting the instruction loses section 1, where all twelve L-rows live | FOLDED — rank not filter, top 12 + a section-1 tier of 6, empty = "no row with ≥2 nouns", budget restated (~7 KB); WP-2 moved below the cut with a dry-run gate |
| 3 | HIGH — a converged reply needs no lookup, so the terminal stays honor-system | FOLDED — `converged`/`readOnlyFloor` carry their own anchor (the last lookup that found nothing) |
| 4 | HIGH — the "thin" DIFF brief dropped PRIOR ROUNDS (repeat instead of escalate; starves CROSS-ANSWER); L1 evidences the read target, not a thin brief | FOLDED — thin = no CLAIMS prose; PRIOR ROUNDS kept as ledger rows; L1's row re-worded |
| 5 | HIGH — non-question fields left three channels for a design; the schema silently diverged from the workflow's (renamed `unresolved`, dropped `whyItMatters`) | FOLDED — one shared schema file, workflow names kept as a superset; `credit`/`howToMeasure` keep the workflow's constraints + a command; `convergenceRationale` is a citation list; discard covers every field |
| 6 | HIGH — the convergence state machine contradicted itself (OPEN excluded `runtime-gated`, so floor ≡ converged; the printed state and the bars were two definitions) | FOLDED — one predicate, per pass, with the primary's `bars attested` row; `status` prints from those rows only |
| 7 | HIGH — the third proof fragment proved nothing (a fragment from the brief's own list passes) and no field held the critic-found rows | FOLDED — fragment from a row NOT in the brief's list; exclusion list in the verifier; `priorArt[]` field |
| 8 | HIGH — DS-13's FIX SIZE walk contradicted by the recorded W10 rounds; a size rule fires at R5 or never, and the "extends" exemption legalises accretion | FOLDED — growth by KIND (`planned`/`reactive`); fires at R4 on the recorded table; discards at four |
| 9 | HIGH — the PRODUCT rule was weaker than the standing rule and skipped L7's "whose complaint" line; false positive on a decided question re-asked | FOLDED — `WHOSE COMPLAINT` line; hand back on FIRST raise unless answered with the user's dated words |
| 10 | HIGH — PARENT ASK = the immediate parent; L10's deliverable sat at the ROOT | FOLDED — the chain to the root, each with its NEXT list; renamed ORIGINATING ASK |
| 11 | HIGH — the FRAME bar was unsatisfiable for a design pass, and a round may not run the game | FOLDED — frame per phase (before-state at the seam / the built result), produced between passes |
| 12 | MEDIUM — "WP-4/5/6 independent of everything" false (they use WP-3's vocabulary; WP-1(b)'s "say what you opened" is a declarative line until `anchor` exists) | FOLDED — edges stated; WP-3 step 1 named the precondition |
| 13 | MEDIUM — WP-1 alone had no measurable acceptance; verbatim persistence is a one-line instrument | FOLDED — WP-3 step 1; sequencing stated |
| 14 | MEDIUM — the RULE 2 sweep was scoped too narrowly (`that holds` 16x / 2x / 1x; the one-round default at 7 more sites) | FOLDED — sites listed; acceptance grep = 0 |
| 15 | MEDIUM — two stop lists with different reframe triggers | FOLDED — one list; the reframe trigger its own item |
| 16 | MEDIUM — the omission anchor was undefined; anchors were unverified; ask/product questions had no line | FOLDED — omission anchor defined; `append` re-verifies; ask/product anchor on the `targetClaim` quote |
| 17 | MEDIUM — a critic's lookup is a claim the primary must re-open | FOLDED — `answered-measured` needs the primary's own citation |
| 18 | MEDIUM — "L2, L3, L4, L12 were catches made by LOOKING" overstated (L3 is a MISS; L2 is the critic ASKING; only L4 looked) | FOLDED — §2.2 closing paragraph and WP-1's defect re-worded |
| 19 | MEDIUM — row identity ambiguous (paths are mostly cross-references) | FOLDED — `§section / title` always |
| 20 | MEDIUM — `angle` had no closed vocabulary | FOLDED — enum in the schema file |
| 21 | MEDIUM — `verify_proof.py` failure path unspecified; markdown breaks a literal grep | FOLDED |
| 22 | MEDIUM — `/qf <steer>`, `/qf N` for 1<N<15, dead `loop/цикл` text | FOLDED |
| 23 | MEDIUM — who decides "materially changed diff" | FOLDED — the diff hash |
| 24 | MEDIUM — no NEXT list recorded → the pass could not open | FOLDED — `unrecorded (<places checked>)` is legal |
| 25 | MEDIUM — WP-6's acceptance lived in the transcript, not the ledger | FOLDED — every stop is a row |
| 26 | MEDIUM — WP-4's acceptance measured absence | FOLDED — count fires and outcomes |
| 27 | MEDIUM — the doc is itself the fix that grows; one fallback line is not a ranking | FOLDED — §3 ranks the six and draws the cut; the critic-side reflex questions are removed |
| 28 | LOW — §4 published three skill files but checked one; `.claude/agents/` does not exist | FOLDED — three-file sweep is step 0's gate; §6.3 dropped |
| 29 | LOW — the DIFF pass is a pre-ship review agent; cite the `/qf` mandate | FOLDED |
| 30 | LOW — E4 used as a reach rate in §7 | FOLDED — clause dropped; the E4 row says what it bounds |
| 31 | LOW — "three" vs the ledger's "four or more" | FOLDED (duplicate of F12) |
| 32 | LOW — 10 `##` sections, not 11 | FOLDED (duplicate of F4) |

Measurements pass 2b took that the doc now carries: section 1 = 271 rows / 379 KB with all twelve
L-rows in it; the per-noun row counts; 44 section-1 rows contain `/qf`; 115 slug rows, 514
`memory/` mentions; the W10 round table on disk; three skills and no `agents/` under `.claude/`.

## 9. Build order, with the cut

0. §4 — version the skills (`.gitignore` split + the three-file sweep + `git add .claude/skills/`),
   so every later step has a diff and a history.
1. WP-3 **step 1** (persist the critic verbatim; one line) + WP-6 (text; the user's rule) — one
   commit each. From here E3 and E7 are countable and every stop is visible.
2. WP-3 steps 2-3: `tools/qf/critic_schema.json` (the workflow switched to read it), the JSON
   block, the format-rule amendment, the `that holds` sweep, `verify_proof.py`, `ledger.py`.
3. WP-1 (the DIFF phase + one anchored lookup per round + the terminal's anchor).
4. **The first census** on the next two real `/qf` passes: E4/E5/E7/E8 by Appendix A, the ledger's
   `anchor`, `stop` and angle columns. Recorded in §8 as pass 3.
   — the cut —
5. WP-4, WP-5, WP-2, each gated on that census plus its own dry run (WP-4's rules on the W10 and
   font tables; WP-5's chain on L10; WP-2's ranking on the twelve L-row passes), one commit each.

Each step is one commit, `[docs]`/`[tools]` prefixed; the skill file itself is a commit only after
step 0.

## 10. AS-BUILT — tier 1 (2026-09-02 evening)

| Step | Commit | What shipped |
|---|---|---|
| 0 | `03748f56` | `.gitignore` `.claude/*` + `!.claude/skills/`; the three skill files (documentize, qf, qf-workflow) enter history after a clean sweep |
| 1a | `62825440` | the critic's reply persisted VERBATIM in `qf_thread.md` |
| 1b | `d43888cd` | bare `/qf` and `/qf <steer>` run to a stop condition; THE ONE STOP LIST (converged / read-only floor / handed to the user / reframe / safety ceiling 50); `/qf N` = the user's explicit pacing only; the "Two paces" paragraph, step 5's hand-back, the loop-word default and the 1..15 clamp deleted |
| 2 | `c3960b01` | `tools/qf/critic_schema.json` (ONE source; the workflow's block is generated by `schema_sync.py`, `node --check` clean); `verify_proof.py` (4-12-word fragments, normalised match, exclusion list for prior-art rows); `ledger.py` (`pass` / `append` / `set` / `attest` / `stop` / `status`; anchors re-verified — a `path:line` must exist, a `grep ... = N` must reproduce N); the critic prompt's REPLY FORMAT; the `read:` line and the "that holds" terminal swept (one historical quote kept); the one-anchored-lookup-per-round rule (WP-1(b)) shipped HERE because the schema enforces it |
| 3 | `c2eb4f60` | the DIFF phase (phase 4) with the thin-brief shape, range + hash, per-question discard, the converged reply's own anchor; the two lookup shapes (`writers-census`, `reference-conditions`) in the prompt |

**Drilled before commit (scratchpad `qfdrill/`):** real fragments pass and a fabricated one fails
(`verify_proof.py`); a malformed reply is discarded with five printed reasons; a converged reply
without the primary's attestation prints OPEN, then CONVERGED after `attest`; `stop converged` writes
the `STOP:` line; a `grep ... = 2` anchor whose count is really 3 is refused; `schema_sync.py --check`
detects drift, `--write` repairs it.

**What the build found, not in the design:**

- `[V]` `docs/QUESTION_FORM_*` and `tools/workflows/` are gitignored ON PURPOSE (`.gitignore:265-266`,
  the "prose only, NOT enforced → enforced" list of never-commit rules). So the skill file, now PUBLIC,
  tells the critic to read companions that exist only on this box, and `schema_sync.py --check` exits 0
  with "absent" on any other checkout. Nothing breaks; but a public skill whose companions are private
  is the same class of content in two publication states — the user decides (`§6`, question 3, NEW),
  before any push.
- The one-lookup-per-round rule cannot be separated from the schema: `ledger.py append` refuses a
  round with no anchored question, so WP-1(b) shipped with WP-3 and WP-1's own commit carries only the
  DIFF phase and the two reflex shapes. §3's dependency graph said the opposite order; the graph was
  right about the edge and wrong about which commit owns it.
- The 4-12-word fragment window bit the drill immediately: a 13-word quote from a ledger row is refused.
  The prompt says 4-12; the old skill said ≤10; the workflow said ≤12. One number now, in the schema.

**§9 step 4 — THE FIRST CENSUS, pass 1 of 2 (2026-09-02, the `/documentize` design pass, read
from `qf_ledger.json`; pass 2 is owed on the next real pass):**

| Column | Before (E-rows, 606 rounds) | Pass 1 (5 rounds, 20 questions) |
|---|---|---|
| anchors verified (E3-class) | unmeasurable — the critic's words were not kept | **20 of 20**: 14 command anchors re-run and reproduced, 6 `path:line` anchors resolved; every round carried at least one |
| prior-art rows cited (E4) | ≤15 mentions in 606 rounds | **20 rows in 5 rounds**, every fragment found verbatim by `verify_proof.py` |
| proof-of-read verified (E5) | recorded in 33 of 606 rounds | **5 of 5**, mechanically |
| angle labels (E7) | 15 in 606 rounds | **20 of 20 questions labelled**, 9 distinct angles (cross-answer-contradiction 4, invariant-not-site-list 4, source-consistency 4, prior-art 3, framing-provenance / measure-dont-infer / existing-owner / regression-by-logic / writers-census 1 each) |
| verdict recorded (E8) | 11 of the 25 largest threads had none | the pass prints its state: OPEN, 16 answered-measured, 4 open (round 5 recorded, unanswered when the usage limit hit); no `stop` yet |

What the first real use found in the tooling, each fixed the same day: command anchors ran under
`cmd.exe`, so a pipeline with `xargs` exited 255 and was recorded verified (`7679edab`: `bash -lc`);
an anchor that BEGINS with a command but also contains a `path:line` was classed by the path and
never re-run (`7679edab`: command-first); `tail` was not a legal command anchor, so a real anchor was
classed as a quote (`77268a7b`: the read-only text tools added). What the pass found in the DESIGN it
interrogated: sixteen answered questions, every one a measured correction to a carried noun — see
`docs/DOCUMENTIZE_ARC.md` §8 pass 3. WP-4 / WP-5 / WP-2 stay parked until pass 2 of the census.

---

## Appendix A — the instruments (re-runnable; Git Bash on this box)

```
B=$(cygpath "$LOCALAPPDATA")/Temp/claude/D--Projects-Programming-VOTV-MP
ALL=$(find $B -maxdepth 3 -iname "qf_thread*")
RX='^#+ *(round|r)[ _]?[0-9]+|^#+.*round [0-9]+'

# file / session census
echo "$ALL" | wc -l; echo "$ALL" | awk -F/ '{print $(NF-2)}' | sort -u | wc -l
# E1 rounds (header regex) + the duplicate census
cat $ALL | grep -c -iE "$RX"
md5sum $ALL | awk '{print $1}' | sort | uniq -d | wc -l
# E2 / E3 question records; those with a file:line anywhere on the line; those with a '?' at all
cat $ALL | grep -c -E '^\*?\*?Q[0-9]'
cat $ALL | grep -E '^\*?\*?Q[0-9]' | grep -c -E '\.(cpp|h|py|rs|md):[0-9]+'
cat $ALL | grep -E '^\*?\*?Q[0-9]' | grep -c '?'
# E4 lessons-row mentions (either party)
cat $ALL | grep -c -i 'LESSONS.md\|lesson row\|the project already learned'
# E5 proof-of-read recorded
cat $ALL | grep -c -i 'proof-of-read\|proof of read'
# E6 discards
cat $ALL | grep -c -i 're-spawn\|respawn\|discarded the critic\|format fail'
# E7 angle labels
cat $ALL | grep -o -E 'FRAMING-PROVENANCE|UNDONE-CHEAP-MEASUREMENT|SOURCE-CONSISTENCY|CROSS-ANSWER|IDENTITY-MAP-COMPLETENESS|ANSWERS-THE-ACTUAL-ASK' | sort | uniq -c
# E8 / E9: per-file bytes|rounds|holds, sorted by BYTES desc, top 25
for f in $ALL; do echo "$(stat -c %s "$f")|$(grep -c -iE "$RX" "$f")|$(grep -c -i 'that holds' "$f")|$(basename "$f")"; done | sort -t'|' -k1 -n -r | head -25 > /tmp/top25.txt
awk -F'|' '$3==0' /tmp/top25.txt | wc -l          # zero-holds
awk -F'|' '$2>15' /tmp/top25.txt | wc -l          # over the cap
awk -F'|' '{print $2}' /tmp/top25.txt | sort -n | sed -n '13p'   # median rounds
# E10 ledger size (append-only; pin the commit)
git rev-parse --short HEAD; wc -lc docs/LESSONS.md docs/security/LESSONS_SECURITY.md
# E11 critic prompt size (the string literal opens at :127 and closes at :210)
sed -n '127,210p' .claude/skills/qf/SKILL.md | wc -c
# E12 absent words + dated marks
for k in "git diff" SIZE oscillat reversal product screenshot render "substrate already" writers census 2026-07-30; do printf "%-18s %s\n" "$k" "$(grep -c -i -- "$k" .claude/skills/qf/SKILL.md)"; done
grep -o -E '2026-0[6-9]-[0-9]{2}' .claude/skills/qf/SKILL.md | sort | uniq -c
# WP-2 flood measurement (rows = '- **' or '**' lines + continuation; count rows matching a noun)
python - <<'EOF'
import re,io
t=io.open('docs/LESSONS.md',encoding='utf-8').read().split('\n'); rows=[]; cur=None
for l in t:
    if re.match(r'^(- )?\*\*',l): cur=[l]; rows.append(cur)
    elif l.startswith('#'): cur=None
    elif cur is not None: cur.append(l)
for n in ['gate','host','client','sync','join','mirror','spawn','identity','eid','arbiter']:
    print(n, sum(1 for r in rows if n in '\n'.join(r).lower()))
EOF
# §4 gitignore test: two throwaway repos, variants '.claude/ + !.claude/skills/' and '.claude/* + !.claude/skills/'
#   git init; write .gitignore; touch .claude/skills/x/SKILL.md .claude/settings.local.json;
#   git status --porcelain --ignored --untracked-files=all; git check-ignore -v <both paths>
```
