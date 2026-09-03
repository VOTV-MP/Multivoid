---
name: documentize
description: >
  Close a work session so the next one resumes with zero re-derivation. The status reconciliation is a
  CENSUS, not a tree-wide hand sweep: `tools/docs/status_census.py census` computes the bounded set of
  docs (the session's blast radius + an amortised sweep) and their status rows; the hand fills ONE
  verdict token per row; the living docs, research/, the memory and the lessons are updated as before;
  then `status_census.py close` makes the close commits itself, one per tree, with the machine-written
  `Docs-Census:` trailer CI reads back. A close without the trailer is a run that did not close.
  (Rewritten 2026-09-03 from its own measured output -- docs/DOCUMENTIZE_ARC.md.)
---
<!-- corr-vocabulary: quoted-doc -->

# /documentize — sweep by census, verdict by hand, commit by script, prep for compact

You are closing out a work session. Capture what is now TRUE into the living docs + the auto-memory so
the next session (or a compacted context) resumes cleanly with zero re-derivation. **Write the truth,
not the hope** — this project has been burned repeatedly by docs that claimed "PROVEN/works" from a
smoke or a static guess. Accuracy beats optimism.

**Inaccuracy cuts BOTH ways — verify, never trust a status label:**
- **False optimism** — a doc says "PROVEN/works/VERIFIED" but the evidence is a smoke or a static guess.
- **False pessimism (stale-open)** — a doc still says OPEN / FUTURE / TODO / PENDING / "not built" /
  "deferred" / "NEXT" for something that has SINCE BEEN SHIPPED: the next session re-implements done
  work, or the user loses trust in every status line in the tree.
- **The rule: a status label is a CLAIM, not a fact.** You may not carry ANY status forward — done OR
  open — without verifying it against the current code/commits THIS run. What changed on 2026-09-03 is
  WHICH labels you verify: **the census's rows**, computed and bounded, not every hit in the tree. The
  old text ordered a hand check of every status marker in `docs/ research/ memory/` — 11,291 grep hits
  per run, mostly not labels, at ~8 closes per active day — and no run ever did it while 148 of 281 close
  commits said "reconciled" (`docs/DOCUMENTIZE_ARC.md` §2). A mandate nothing observes is satisfied by
  assertion; the census is what observes this one.

**SCOPE — comprehensive by CENSUS, not by reading the tree:**
- **The docs you reconcile** = the census's radius: (i) every doc the session touched, in every tree
  (main, every OWNED inner repo — ownership is the local git identity, so `site/` counts as well as
  `research/` — and the trees no repository tracks: `CLAUDE.md`, the memory directory, the ignored docs
  incl. `docs/security/`), (ii) every doc citing a SPECIFIC symbol or path of the session's code diff,
  (iii) the K = 40 OLDEST-censused docs by the age ladder, so every doc reaches a hand verdict within
  ~70 closes. A doc the session TOUCHED is still a sweep candidate — that is the only way `MEMORY.md`,
  `LESSONS.md` and `CLAUDE.md`, touched by steps 3/3.5/4 of every close, are ever read whole.
  `--sweep` censuses the whole read set, on the user's request.
- **The hand phase is on probation, with a stated bar (D8), and the bar counts DOCS.** Over the first
  100 AGEING-lane DOCS, if fewer than 5 of them yielded any correction (`actually-done` / `stale-done`
  / `partial`), the true rot rate is under ~5 % of docs and this step is DELETED rather than defended.
  **The unit is the doc, not the row, because rows inside one doc are not independent events**: `[V]`
  2026-09-04 the first full close drew 78 ageing rows from 25 docs with ONE contributing 18, and the
  single real finding it made was one superseded plan worth 21 rows -- so a bar counting rows is
  decided by which docs the sweep happened to reach, in both directions. Every trailer carries
  `ageing-docs=` and `ageing-corr-docs=` (the denominator and numerator) plus `ageing-rows=` /
  `ageing-corr=` as texture, so the bar is evaluated by summing trailers and never by recollection.
  It also carries **`ageing-lost=`**: rows that left the table with an EMPTY verdict, which is what a
  run scores when it acts on a doc before verdicting it. A close with a non-zero `ageing-lost` has a
  compromised sample and its `ageing-corr` understates the rot -- read the two together. (The bar
  used to name a fourth term, `sub-state-stale`. Nothing recorded it and nothing needed to: a stale
  sub-state clause under a true label IS a claim, and the hand verdicts that row `STALE DONE`, so the
  term was already inside the third one.)
- **READ THE ZERO HONESTLY: D8 has no valid data points yet, which is not the same as a clean
  corpus.** Across the three closes on record, 315 rows were answered entirely by `STILL TRUE` (273)
  and `NOT A LABEL` (42), with every correction column at **0**. That reads like a corpus with no rot
  -- and it is not evidence, because the ONE real stale-open the sweep has ever produced left the
  table UNVERDICTED (the operator acted before verdicting) and so never entered the numerator. The
  counter that would have caught it, `ageing-lost=`, did not exist until 2026-09-04 and was itself
  broken on its founding case until the same day. So the honest state is: **the numerator has never
  been trustworthy, and D8 is at 25 of its 100 ageing docs.** Do not retire the hand phase on this
  zero, and do not defend it on this zero either -- the first trustworthy closes start now.
- **Archive stale stuff** — a superseded/abandoned doc or section goes to the nearest `_archive/` with a
  one-line pointer (RULE 2: retired info goes, fully — no parallel stale + fresh).
- **Write NEW docs with FRESH info** where the current truth has no home; link them in. A NEW doc in
  main is PUBLISHED by an explicit `--new` at the close, or gitignored — the close refuses a new
  unignored doc it was not told about.

Work top-to-bottom. Skip a step only if it genuinely does not apply, and say so.

## 0. Survey what actually changed this session
- `git --no-pager status --short` + `git --no-pager log --oneline -8` + skim `git --no-pager diff --stat`.
  **Anything STAGED in the shared index that you did not stage is another session's** (this box runs
  two sessions on one working tree — `docs/CROSS_SESSION.md`): leave it alone; the close excludes a
  whole-file foreign staging and refuses a partial one.
- List, for yourself: what was BUILT, what was VERIFIED (and HOW — real hands-on / matching real log vs
  merely "smoke passed" vs "compiles"), what was RETIRED, what FAILED, what is still OPEN.
- Note the deployed DLL SHA + proto version if a build shipped, and the current HEAD.

## 0.5. The CENSUS — run it (replaces the tree-wide enumeration)

```
python tools/docs/status_census.py census            # first run on a fresh box: --since <date>
```

It prints: the read set per tree; the bases (the previous close's trailer commit in main and in each
owned inner repo); radius (i) touched / (ii) cited (with the generic symbols it DROPPED, and why) /
(iii) the sweep; the row count (labels / dead citations / symbol drift); the table path
(`~/.claude/projects/<slug>/history/census/pending.md`); the ratchet values against their targets; the
accretion count. Read those lines — "radius: N docs" is the size of the manual check you now owe.

The table has one row per STATUS LABEL found in the radius (a tag like `[?]`, a `Status:` field, a
table cell, a checkbox, a heading carrying Open questions / OPEN / TODO / NEXT, a bold or capitalised
status word leading the line) plus one row per prose line whose CITATION no longer resolves. Each row
carries its LANE (below), the label, the **sub-state** clause (the parenthetical or trailing "(commit
pending)" / "uncommitted" / "hands-on-pending" on the line or the next — where §2.3 measured most of
the rot), every token on the line with its resolve state, the newest date on the line, a running-total
flag, and an EMPTY verdict column.

**Two LANES, because a row's age decides which question it can even be asked.** A row the session
introduced or changed cannot have AGED — nothing about it was ever true and then stopped being — so it
is asked the AUTHORING question (*is this claim true as I write it?*) and is only raised at all when
it asserts something falsifiable. A standing claim is asked the AGEING question (*was this true once
and is it still?*), oldest first by the clock ladder, with the RUNG printed so a date resting on
`mtime` is visibly weaker than one from a commit.

**Resolve states.** `ok` / `gone` / `past-eof` / `external` / `ambiguous` are about whether the path
resolves. Two more are about CONTENT, at different strengths: a citation whose doc QUOTES the cited
line (`` `f.cpp:12` says "…" ``) and whose words are not there is `moved` / `content-gone`, DEAD like a
vanished path; a citation next to a backticked SYMBOL found ELSEWHERE in the cited file is `drift`,
which raises its own row kind, names the true line (`session_lanes.h:179->223`), and never refuses —
that pairing is inferred, not stated.

If you edit a doc AFTER the census, re-run `census --force`: verdicts already typed carry forward by
(path, line content), and the close refuses a stale census, so this is never optional. **A verdict
whose LINE you fixed does not carry — that is the point, and it is recorded rather than lost:** the
re-census appends it to the resolved ledger (`status_census.py resolved` reads it back) and the
close's `resolved=` / `flips=` carry the correction the verdict columns cannot, because those describe
the text being committed.

## 0.6. The HAND VERDICTS — one token per row, against the CODE (mandatory)

**This is the manual check, item by item — the census bounded it, it did not do it.** For EVERY row in
`pending.md`: open the doc at that line, grep the symbol/feature, read the function, check
`git --no-pager log -S"<symbol>"` for the commit that shipped or removed it, and write exactly one token
in the VERDICT column:

| verdict | meaning | action, bounded by the row's `kind` |
|---|---|---|
| `STILL OPEN` | confirmed absent/incomplete in the code | keep it open; RE-STATE the evidence on the line ("no such function as of `<HEAD>`") so the next run does not re-derive it |
| `ACTUALLY DONE` | a stale-open: it shipped | flip to AS-BUILT / VERIFIED (whichever the evidence supports) with file:line + commit; a LIVING planning doc whose open rows are ALL actually-done is MOVED to `_archive/` with a pointer (the verdict-driven archive) |
| `STALE DONE` | false optimism, or a dead citation under a live label | downgrade the tag, date it, cite the evidence, stamp |
| `PARTIAL` | some sub-parts shipped | name exactly which (file:line) and which remain; split the item |
| `STILL TRUE` | the label and the line hold | nothing — but a LABEL row whose citation resolved `gone` / `past-eof` cannot be STILL TRUE: the close refuses it, fix the pointer |
| `NOT A LABEL` | the grammar mis-flagged the line: it states no status about anything (a vocabulary table, a legend, a sentence containing a status word) | nothing to the doc — the row is the CENSUS's defect, and the trailer records it against the RUNG that raised the row, so `not-a-label=` really is the label grammar's measured false-positive rate. Do NOT use it to dismiss a claim you simply did not check |


**THE FOUR STATUS VERDICTS BELONG TO LABEL ROWS ONLY.** `STILL OPEN` / `ACTUALLY DONE` / `STALE DONE`
/ `PARTIAL` say something about a STATUS, so the close REFUSES them on a row of kind `cite`, `drift`
or `loose` — those rows exist because a citation resolved dead, a symbol moved, or the loose regex
fired, and none of them carries a label to be stale about. On such a row only `STILL TRUE` (nothing to
do — on a `cite` row it means the citation is dead ON PURPOSE) and `NOT A LABEL` (this rung mis-fired)
can answer. Without that rule a `cite` row answered `STALE DONE` lands in the number D8 reads.

A row of kind `drift` makes no status claim: it says the SYMBOL beside a citation now lives elsewhere
in the cited file. Check it — the true line is printed — and either fix the number (then the row
returns corrected, and the verdict you gave is recorded in the resolved ledger) or verdict it
`NOT A LABEL`. **You write ONE rejection token whatever the row's kind; the machine attributes it**
to the rung that produced the row — `not-a-label=` (the label grammar), `not-a-cite=` (the citation
resolver), `drift-ok=` (the symbol rung), `not-loose=` — so each instrument's false-positive rate is
measured on its own and a single counter is never three of them added together. If a whole DOC quotes the status
vocabulary rather than using it (a legend, a skill text, this file), mark it once with
`<!-- corr-vocabulary: quoted-doc -->` and a reason beside it, rather than verdicting its rows one by
one forever.

**VERDICT THE WHOLE TABLE BEFORE YOU ACT ON ANY ROW.** The order is not stylistic. Acting first
EDITS the doc, which makes it `touched`, which makes the re-census read it DIFF-SCOPED — so the rows
you corrected are no longer in the table, and the resolved ledger never sees the verdict that
motivated the correction, because you never typed one. `[V]` 2026-09-04: a sweep found a 2026-05-25
plan whose 21 checkboxes had all shipped as `docs/signals/`; the doc was stamped before its rows were
verdicted, and that close reports `ageing-corr=0` for a run that corrected 21 rows. That is the exact
defect the resolved ledger exists to prevent, reintroduced by doing the steps out of order. Fill every
verdict, THEN act, THEN re-census.

The action's FORM depends on the doc's kind (path pattern): a LIVING doc (undated filename outside
`_archive/`) is REWRITTEN in place with one `[corr YYYY-MM-DD: was …; measured …; <cite>]` stamp — never
a paragraph of correction prose accreted beside the old claim; a DURABLE RECORD (`-RE-<date>`, dated
findings, runbooks) gets a per-claim refresh (`[V]` with the instrument named) or a supersede-stamp and
stays; a `-DESIGN-<date>` doc is stamped only, never rewritten; a MEMORY TOPIC is updated in place or
deleted if wrong; a MEMORY LESSON is sharpened; an `_archive/` row is left alone.

**When code and doc disagree and you cannot resolve it from the code in a reasonable read, ASK THE
USER** with a one-line targeted question. That row stays unverdicted — and the close refuses until it
is answered. That is correct: the run has not closed.

## 1. Sweep + update the LIVING docs the census pointed at (fresh info, honest status)
The triage — CURRENT (update in place) / STALE (archive + pointer) / MISSING (write the new doc, link it
into `docs/README.md` and the CLAUDE.md reading order) — runs over the census's docs plus the docs this
session wrote. Apply the living-knowledge-base discipline ([[feedback-docs-piles-living-knowledge-base]]):
mark each claim **DESIGN** vs **AS-BUILT** vs **VERIFIED**; **NEVER mark anything "working/VERIFIED"
from an autonomous smoke alone** — only a real hands-on or a matching real log counts, and say which.
- **The two canonical cross-cutting maps** — keep current when a seam/identity/dispatch fact changed:
  `docs/COOP_DISPATCH_VISIBILITY.md` and `docs/COOP_ENTITY_EXPRESSION_MAP.md`. Confidence-tag claims
  `[V]` verified / `[RD]` RE-derived / `[?]` unverified.
- `docs/piles/` (the living pile knowledge base): update the relevant `NN-*.md`; archive a superseded
  approach; fix the README index. Do NOT commit generated `re-artifacts/` (gitignored).
- `docs/ROADMAP.md`, `docs/MULTIPLAYER_UI.md`, `docs/FEASIBILITY.md`, `docs/COOP_SCOPE.md` — bump status
  lines / dates where this session changed them.
- Verify a game-domain or offset claim against the SDK CXXHeaderDump / bytecode before re-asserting it.

## 2. Update `research/` (point-in-time log + runbooks)
- New RE/design → a dated finding in `research/findings/` (durable `*-RE-*` vs point-in-time
  `*-DESIGN-*`); a definitively-dead approach → `_archive/`. Keep `research/findings/README.md` oriented.
  The close ADDS new findings docs to the inner repo itself (it has no remote; ownership is location +
  ignore rules).
- A pending hands-on test → update/author `research/handson_runbook_<date>.md` (take N): the deployed
  SHA + proto, what changed, the EXACT steps, what to read in the log, the honest status.

## 3. Update the auto-memory (the cross-session brain)
- **Write/refresh the session TOPIC file** in `memory/` (one fact per file; what shipped, the as-built,
  the decisions/lessons, the deployed SHA, what's OPEN/NEXT, the uncommitted state). Absolute dates.
  Link related memories with `[[name]]`.
- **Update `MEMORY.md`**: the pointer for the new topic file, the **POST-COMPACT READ FIRST** pointer at
  the top → this session's topic. Every line ≤ 200 chars (`mem-over200=` in the trailer counts the
  violations, and the ratchet refuses a close that ADDS one). **Every pointer it makes must RESOLVE** —
  `memref-dead=` is ratcheted at 0 over its markdown links and its `memory/<glob>` patterns, and over
  CLAUDE.md's backticked repo paths. Do NOT compact by inventing a filename glob: lesson and feedback
  files carry NO date in their names (0 of 705), so `memory/lesson-*<date>*` matches nothing. Address a
  day through `memory/INDEX_BY_DATE.md`, which the CENSUS regenerates from the age ladder (the close
  only checks it, so no path the content pin already checked is rewritten afterwards).
- Record durable **feedback** and **lessons** as their own files; UPDATE an existing file rather than
  duplicating; delete a memory that turned out wrong.

## 3.5. Write this session's LESSONS per the DIG-RULE (so the next session never re-digs)

**HARD PAIRING (user rule 2026-07-11): a lesson write is TWO writes — the `memory/` file AND its row in
`docs/LESSONS.md`.** Before leaving this step, diff your session's new/updated `memory/lesson_*` /
`feedback_*` files against the rows you touched in `docs/LESSONS.md` — the two sets must match 1:1
— run `python tools/docs/lessons_gate.py --pairing`, which prints both halves (memory files with no
row / rows pointing at no file). Then run `python tools/docs/lessons_gate.py` — it must PASS before
the close: besides the citations and symbols it now fails on a dead `[[wikilink]]` and warns on every
row carrying a running total. The close ratchets its three counts, so a new lesson file without its
row makes `pairing-unref` grow and the close refuses.

1. **Extract every dig from THIS session**: what you had to MEASURE, disasm, probe, or reason out that
   was NOT obvious from the code at a glance; what surprised you; what an audit surfaced; what failed
   once before it worked, and WHY. A dig, not routine work.
2. **Write each as its own durable file** (`lesson_<slug>.md` / `feedback_<slug>.md`), DIG-RULE shape:
   **(a) the measured fact** (tagged `measured` with its file:line / log / disasm citation — never an
   inference laundered as measured), **(b) why it was non-obvious**, **(c) "where to look FIRST next
   time"**. Link related memories. Add the pointer to `MEMORY.md`.
3. **Prefer UPDATE over duplicate.** 4. **A lesson earns its file only if it saves a FUTURE dig.**
5. **Update `docs/LESSONS.md`**: ADD a row per lesson written; RECONCILE existing rows the census
   flagged (a row citing a moved symbol/path is a census row like any other).

## 4. Prepare for compact / handoff (clean resume)
- The **READ-FIRST pointer** in `MEMORY.md` points at this session's topic file, and that file's NEXT
  section names the concrete next step(s).
- Nothing is mid-edit; the tree builds (don't claim "builds" unless you built it). State the deployed
  SHA, HEAD, and the uncommitted set (or that it's committed).
- A background job / workflow / hands-on in flight: record its ID + what its result will mean.
- The CLAUDE.md "reading order after a session reset" still resolves. Do NOT grow it: the trailer's
  `ro-bytes` / `ro-longest` are ratcheted (red only on growth; target 58 KB, no entry over 15 lines,
  printed until reached). Shorten by MOVE-THEN-CUT — grep the destination for the fact first; lines
  naming the USER (case-insensitively) beside a quotation never move -- and the close REFUSES if one
  has left, rather than reporting it after the fact.

## 4.5. THE CLOSE — the script commits, never you

```
python tools/docs/status_census.py close -m "<subject, no prefix>" \
    --trailer "Co-Authored-By: <model name> <noreply@anthropic.com>" \
    --trailer "Claude-Session: <this session's URL>" \
    [--new docs/NEW_DOC.md]... [--also-comment src/x.h]...
```

It REFUSES, saying why, on: an unverdicted row; `STILL TRUE` / `ACTUALLY DONE` on a label row whose
citation is dead; a doc changed since the census (re-run `census`); a new unignored doc in main that is
neither `--new` nor gitignored; a doc another session staged PARTIALLY (a same-file collision — resolve
per `docs/CROSS_SESSION.md`); a non-doc path that is not the close's own tooling and not a
comment-only change (commit it FIRST, on its own, with its own subject); a ratchet column that grew; a
cumulative column that SHRANK (the resolved ledger is append-only, so a drop means the private history
was replaced); a hand-edited row count; a STATUS verdict on a row that carries no label; a line
recording what the USER said that has LEFT the reading
order and is findable in no doc; a stale `memory/INDEX_BY_DATE.md` (re-run `census --force`, which regenerates it); a missing
attribution trailer. On green it makes ONE COMMIT PER TREE — the private history (snapshot + state +
the verdict table + the ledger), EVERY owned inner repo that this session touched (ownership is the local git identity, so `site/`
counts as well as `research/`), and main — each from a PRIVATE index (nothing another session staged
is swallowed or discarded), each with the `Docs-Census:` trailer git appends, the subject prefixed
`[docs] close:`. It prints the shas, the trailer line, and how many verdicts this close resolved.

**Never `git commit` a close by hand.** A close without the trailer is a run that did not close, and
`tools/docs/docs_census_gate.py` fails the push on a `[docs] close:` subject without it, a trailer
without the prefix, the retired `[docs] documentize` subject form, a verdict sum that misses `rows`, a
`base=` that does not tile onto the previous close, a repeated `census=`, a grown ratchet column, a
shrunken cumulative one, an UNDECLARED trailer column (every column's kind lives in
`tools/docs/trailer_schema.py`, imported by both sides), or a missing `Co-Authored-By:`.

## 5. Report
Give the user a tight summary that **leads with the verdict table** — the census's rows with your
verdicts and the action taken per row (the same table the script filed under
`~/.claude/projects/<slug>/history/census/`) — and the trailer line the script printed; then the honest
status of the work (verified vs pending), the committed state (every sha the close printed), and the exact NEXT step. No
forbidden hand-off phrases ("should work", "build clean — ready"); state evidence.

**Read `resolved=` and `flips=` before you write "nothing needed correcting".** The verdict columns
describe the text being COMMITTED, so a run that fixed three stale claims commits three corrected
lines and reports `stale-done=0`. What the run corrected is the ledger's delta, which the close prints
on its own line.

---
**Guardrails:** Every lesson written to `memory/` MUST land its row in `docs/LESSONS.md` in the same run
(the Step 3.5 hard pairing). Do NOT mark work "VERIFIED" without real hands-on or a matching real log.
Do NOT carry a status label forward in EITHER direction without a verdict against the code this run —
the close refuses an unverdicted row for exactly this reason, and a `STILL TRUE` is a verdict you must
have earned by reading the code, not a default. Do NOT invent status. Honor RULE 2 (retired info goes,
fully — archive, don't leave parallel stale + fresh). The close commit is the script's ([[feedback-commit-autonomously]]
still governs WHEN: coherent close, to `main`; ask before push). No emojis in files unless requested.
