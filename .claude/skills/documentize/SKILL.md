---
name: documentize
description: >
  Close a work session so the next one resumes with zero re-derivation. The status reconciliation is a
  CENSUS, not a tree-wide hand sweep: `tools/docs/status_census.py census` computes the bounded set of
  docs (the session's blast radius + an amortised sweep) and their status rows; the hand fills ONE
  verdict token per row; the living docs, research/, the memory and the lessons are updated as before;
  then `status_census.py close` makes the three close commits itself, with the machine-written
  `Docs-Census:` trailer CI reads back. A close without the trailer is a run that did not close.
  (Rewritten 2026-09-03 from its own measured output -- docs/DOCUMENTIZE_ARC.md.)
---

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
  (main, the inner `research/` repo, and the trees no repository tracks — `CLAUDE.md`, the memory
  directory, the ignored docs incl. `docs/security/`), (ii) every doc citing a SPECIFIC symbol or path
  of the session's code diff, (iii) the K = 40 docs whose last census is oldest, so every doc reaches a
  hand verdict within ~39 closes. `--sweep` censuses the whole read set, on the user's request.
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

It prints: the read set per tree; the bases (the previous close's trailer commit in main and in
`research/`); radius (i) touched / (ii) cited (with the generic symbols it DROPPED, and why) / (iii) the
sweep; the row count (labels / dead citations); the table path
(`~/.claude/projects/<slug>/history/census/pending.md`); the ratchet values against their targets; the
accretion count. Read those lines — "radius: N docs" is the size of the manual check you now owe.

The table has one row per STATUS LABEL found in the radius (a tag like `[?]`, a `Status:` field, a
table cell, a checkbox, a heading carrying Open questions / OPEN / TODO / NEXT, a bold or capitalised
status word leading the line) plus one row per prose line whose CITATION no longer resolves. Each row
carries the label, the **sub-state** clause (the parenthetical or trailing "(commit pending)" /
"uncommitted" / "hands-on-pending" on the line or the next — where §2.3 measured most of the rot), every
token on the line with its resolve state (`ok` / `gone` / `past-eof` / `external`), the newest date on
the line, a running-total flag, and an EMPTY verdict column.

If you edit a doc AFTER the census, re-run `census`: verdicts already typed carry forward into the
re-census by (path, line content); the close refuses a stale census, so this is never optional.

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
  violations, and the ratchet refuses a close that ADDS one).
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
  carrying `USER` + `verbatim` never move.

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
missing attribution trailer. On green it makes THREE commits — the private history (snapshot + state +
the verdict table), the inner `research/` repo, and main — each from a PRIVATE index (nothing another
session staged is swallowed or discarded), each with the `Docs-Census:` trailer git appends, the
subject prefixed `[docs] close:`. It prints the three shas and the trailer line.

**Never `git commit` a close by hand.** A close without the trailer is a run that did not close, and
`tools/docs/docs_census_gate.py` fails the push on a `[docs] close:` subject without it, a trailer
without the prefix, the retired `[docs] documentize` subject form, a verdict sum that misses `rows`, a
`base=` that does not tile onto the previous close, a repeated `census=`, a grown ratchet column, or a
missing `Co-Authored-By:`.

## 5. Report
Give the user a tight summary that **leads with the verdict table** — the census's rows with your
verdicts and the action taken per row (the same table the script filed under
`~/.claude/projects/<slug>/history/census/`) — and the trailer line the script printed; then the honest
status of the work (verified vs pending), the committed state (three shas), and the exact NEXT step. No
forbidden hand-off phrases ("should work", "build clean — ready"); state evidence.

---
**Guardrails:** Every lesson written to `memory/` MUST land its row in `docs/LESSONS.md` in the same run
(the Step 3.5 hard pairing). Do NOT mark work "VERIFIED" without real hands-on or a matching real log.
Do NOT carry a status label forward in EITHER direction without a verdict against the code this run —
the close refuses an unverdicted row for exactly this reason, and a `STILL TRUE` is a verdict you must
have earned by reading the code, not a default. Do NOT invent status. Honor RULE 2 (retired info goes,
fully — archive, don't leave parallel stale + fresh). The close commit is the script's ([[feedback-commit-autonomously]]
still governs WHEN: coherent close, to `main`; ask before push). No emojis in files unless requested.
