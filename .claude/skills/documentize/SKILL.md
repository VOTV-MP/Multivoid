---
name: documentize
description: >
  Update ALL project docs + the auto-memory with this session's fresh, VERIFIED findings, correct any
  stale/false claims, and prepare for a clean /compact or next-session handoff. Invoke at the end of a
  work session, before /compact, or whenever the user says "documentize" / "update the docs" / "prepare
  for compact". Project: VOTV_MP (Voices of the Void coop mod).
---

# /documentize — sweep ALL docs, archive stale, write fresh, prep for compact

You are closing out a work session. Capture what is now TRUE into the living docs + the auto-memory so
the next session (or a compacted context) resumes cleanly with zero re-derivation. **Write the truth,
not the hope** — this project has been burned repeatedly by docs that claimed "PROVEN/works" from a
smoke or a static guess. Accuracy beats optimism.

**Inaccuracy cuts BOTH ways — verify, never trust a status label:**
- **False optimism** — a doc says "PROVEN/works/VERIFIED" but the evidence is a smoke or a static guess.
- **False pessimism (stale-open)** — a doc still says OPEN / FUTURE / TODO / PENDING / "not built" /
  "not wired" / "deferred" / "NEXT" for something that has SINCE BEEN SHIPPED. This is just as
  corrosive: the next session re-implements done work, or the user (who knows it's done) loses trust in
  every status line in the tree.
- **The rule: a status label is a CLAIM, not a fact. You may not carry ANY status forward — done OR
  open — without verifying it against the current code/commits THIS run (Step 0.5).** "It was open last
  session" is not evidence it is open now. (Born 2026-06-21: a sweep left a pile of OPEN/FUTURE items
  standing that were already implemented, because it trusted the labels instead of checking the code.)

**SCOPE — this is a COMPREHENSIVE sweep, not an incremental touch-up:**
- **Update ALL docs**, not only the ones this session edited. Read THROUGH the doc tree (`docs/`,
  `docs/piles/`, `research/findings/`, the runbooks, the auto-memory) and hunt for stale, outdated,
  superseded, or now-FALSE info ANYWHERE — not just where you worked.
- **Archive stale stuff** — move every superseded/abandoned/obsolete doc (or section) to the nearest
  `_archive/` and leave a one-line pointer, so it can NEVER be mistaken for the current plan (RULE 2:
  retired info goes, fully — no parallel stale + fresh).
- **Write NEW docs with FRESH info** — where the current truth has no home (a new subsystem, a new
  cross-cutting fact, a redesign, a verified mechanic), CREATE the doc. Don't cram fresh truth into a
  stale doc's margins; give it a clean canonical home and link it in.

Work top-to-bottom. Skip a step only if it genuinely does not apply, and say so.

## 0. Survey what actually changed this session
- `git --no-pager status --short` + `git --no-pager log --oneline -8` + skim `git --no-pager diff --stat`
  to see what was added / changed / committed.
- List, for yourself: what was BUILT, what was VERIFIED (and HOW — real hands-on / matching real log vs
  merely "smoke passed" vs "compiles"), what was RETIRED, what FAILED, what is still OPEN.
- Note the deployed DLL SHA + proto version if a build shipped, and the current HEAD.

## 0.5. MANUAL status reconciliation — verify EVERY open claim against the CODE (mandatory, both directions)

**This is a HAND check, item by item — not a glance, not a trust-the-label pass.** Before you update any
doc, build the authoritative status ledger by reading the CODE, because the docs are guilty-until-proven.

1. **Enumerate every status marker across the whole doc tree.** Grep the tree for the open-status
   vocabulary and list every hit with its file:line:
   `grep -rniE "OPEN|FUTURE|TODO|PENDING|NEXT|not (yet )?(built|wired|implemented|done|verified)|deferred|unverified|\[ \]|\[\?\]|planned|stub|placeholder" docs/ research/ memory/`
   (also scan for the inverse: `DONE|SHIPPED|VERIFIED|PROVEN|WORKS|COMPLETE|AS-BUILT` — those need the
   same code check in the other direction).
2. **For EACH item, open the actual code / commit and decide its TRUE state — manually.** Do not infer
   from the doc. Concretely, per item: grep for the symbol/feature, read the function, check
   `git --no-pager log --oneline` / `git --no-pager log -S"<symbol>"` for the commit that shipped it, and
   if needed build/run-trace. Classify into one of:
   - **STILL OPEN** — confirmed absent/incomplete in the code → keep it open, but re-state the evidence
     ("no such function as of `<HEAD>`") so the next run doesn't have to re-derive it.
   - **ACTUALLY DONE (stale-open)** → flip it to AS-BUILT / VERIFIED (whichever the evidence supports),
     cite the file:line + commit that proves it, and MOVE the planning doc/section to `_archive/` if it
     was a pure roadmap for now-shipped work (RULE 2).
   - **PARTIAL** — name exactly which sub-parts shipped (file:line) and which remain, and split the item.
3. **When code and doc disagree and you cannot resolve it from the code in a reasonable read, ASK THE
   USER** rather than guessing — a one-line targeted question ("docs/X says Y is FUTURE; I see `<symbol>`
   shipped in `<commit>` — is Y done?") beats silently picking wrong. The user has the ground truth for
   what's been hands-on-verified.
4. **Produce the ledger in your Step 5 report**: a table of every reconciled item — claim → code evidence
   → true status → doc action taken. This is the proof you did the manual check, not a label-trust pass.

5. **Reconcile the LESSONS + DIG-records for staleness too — they rot exactly like docs.** A durable
   lesson (`memory/lesson_*.md` / `feedback_*.md`, the `docs/LESSONS.md` ledger, `docs/piles/`) that names
   a file, function, offset, flag, or seam is a CLAIM about the code, and the code moves. Grep each
   lesson's cited symbols/paths against the tree: if a lesson names something that no longer exists or now
   behaves differently, **UPDATE the measured fact (re-cite the new file:line) or ARCHIVE the lesson if the
   whole finding was superseded** — same discipline, both directions. A lesson that points the next
   session at a function that was renamed/deleted sends them on a WORSE dig than no lesson at all. Add
   every lesson you touch to the Step 5 ledger.

Only after this ledger exists do you touch the docs in Step 1. The triage in Step 1 CONSUMES this ledger
(it is the source of truth for every status line you write).

## 1. Sweep + update the LIVING docs in `docs/` (fresh info, honest status)
**ENUMERATE the whole doc tree first** (`ls docs/ docs/piles/ docs/piles/_archive/ research/findings/`)
and read each doc — then TRIAGE every one:
- **CURRENT** → update in place with this session's truth.
- **STALE / superseded / abandoned / now-FALSE** → move to the nearest `_archive/` + leave a one-line
  pointer (never leave a stale doc looking authoritative).
- **MISSING** (fresh truth has no canonical home) → write a NEW doc and link it into the index +
  CLAUDE.md reading order.

Apply the living-knowledge-base discipline ([[feedback-docs-piles-living-knowledge-base]]): every
design/diagnosis/as-built updates the doc; mark each claim **DESIGN** vs **AS-BUILT** vs **VERIFIED**;
**NEVER mark anything "working/VERIFIED" from an autonomous smoke alone** — only a real hands-on or a
matching real log counts, and say which.
- **The two canonical cross-cutting maps** — keep current when a seam/identity/dispatch fact changed:
  `docs/COOP_DISPATCH_VISIBILITY.md` (will my hook fire? observe-vs-drive) and
  `docs/COOP_ENTITY_EXPRESSION_MAP.md` (each entity's spawn→catch→identity→destroy + the dupe matrix).
  Confidence-tag claims `[V]` verified / `[RD]` RE-derived / `[?]` unverified.
- `docs/piles/` (the living pile knowledge base): update the relevant `NN-*.md` to as-built/verified;
  archive a superseded approach to `_archive/`; fix the README index. Do NOT commit generated
  `re-artifacts/` (bytecode JSON — RULE 3, gitignored).
- `docs/ROADMAP.md`, `docs/MULTIPLAYER_UI.md`, `docs/FEASIBILITY.md`, `docs/COOP_SCOPE.md` — bump status
  lines / dates where this session changed them (e.g. "not built" → BUILT, an old date → today).
- **Reconcile contradictions in BOTH directions (consume the Step 0.5 ledger).** If a doc says "PROVEN"
  but the evidence is a static dump or a smoke, downgrade with the date + actual evidence. **If a doc
  says OPEN / FUTURE / TODO for something the Step 0.5 ledger proved is already shipped, flip it to
  AS-BUILT/VERIFIED with the file:line + commit, and archive the now-obsolete roadmap section.** Never
  re-assert a status you did not verify against the code this run. Verify a game-domain or offset claim
  against the SDK CXXHeaderDump / bytecode before re-asserting it.

## 2. Update `research/` (point-in-time log + runbooks)
- If new RE/design was produced, drop a dated finding in `research/findings/` (durable `*-RE-*` vs
  point-in-time `*-DESIGN-*`); archive a definitively-dead approach to `_archive/`. Keep
  `research/findings/README.md` oriented.
- If a hands-on test is pending, update/author `research/handson_runbook_<date>.md` (take N): the deployed
  SHA + proto, what changed, the EXACT steps, what to read in the log, the honest status. Don't leave a
  stale "needs your grab" runbook for something now verified, or a "VERIFIED" runbook for something only
  smoke-tested.

## 3. Update the auto-memory (the cross-session brain)
- **Write/refresh the session TOPIC file** in `memory/` (one fact per file; full session detail —
  what shipped, the as-built, the key decisions/lessons, the deployed SHA, what's OPEN/NEXT, the
  uncommitted state). Convert relative dates to absolute. Link related memories with `[[name]]`.
- **Update `MEMORY.md`** (the index loaded each session): the one-line pointer for the new topic file,
  the **POST-COMPACT READ FIRST** pointer at the top → this session's topic, and the top "Current
  state" entry. Keep every line ≤200 chars (detail lives in the linked file).
- **If `MEMORY.md` is near its size limit, compact it**: one-line the older per-session entries (move
  their detail into their topic files), never delete a still-relevant pointer.
- Record durable **feedback** (a correction/preference, with the why + how-to-apply) and **lessons**
  (e.g. a dispatch/identity fact) as their own files; don't bury them in a session entry.
- Before saving: check for an existing file that already covers it — UPDATE it, don't duplicate. Delete
  a memory that turned out wrong.

## 3.5. Write this session's LESSONS per the DIG-RULE (so the next session never re-digs)

**HARD PAIRING (user rule 2026-07-11): a lesson write is TWO writes — the `memory/` file AND its row
in the registry doc `docs/LESSONS.md`. Never one without the other; a lesson that exists only in
`memory/` is invisible to the browsable ledger, and a ledger row with no memory file has no detail.
Before leaving this step, diff your session's new/updated `memory/lesson_*` / `feedback_*` files
against the rows you touched in `docs/LESSONS.md` — the two sets must match 1:1.**

This is a MANDATORY sub-sweep, not optional. The DIG-RULE
([[feedback-map-all-wire-events-before-fixing-missing-sync]] is its named instance; the general principle
is broader): **when a dig produces a hard-won measured fact, record it as a durable lesson so a future
session reads it instead of re-excavating the same hole.** This project has literally dug the same place
twice (rock F2, 2026-07-08/09); the user made it a rule so it never happens a third time.

1. **Extract every dig from THIS session.** Walk your own trail: what did you have to MEASURE, disasm,
   probe, or reason out that was NOT obvious from the code at a glance? What surprised you? What did an
   audit/agent surface? What failed once before it worked, and WHY? Each of those is a candidate lesson.
   Distinguish a *dig* (a non-obvious fact you had to excavate) from routine work (no lesson needed).
2. **Write each as its own durable file** in `memory/` — `lesson_<slug>.md` (a measured
   engine/identity/dispatch fact) or `feedback_<slug>.md` (a working-agreement/how-to-work correction).
   Use the DIG-RULE shape: **(a) the measured fact** (tagged `measured` with its file:line / log /
   disasm citation — never launder an inference as measured), **(b) why it was non-obvious / the trap
   that cost the dig**, **(c) "where to look FIRST next time"** — the pointer that lets the next session
   skip the excavation. Link related memories with `[[name]]`. Add the one-line pointer to `MEMORY.md`
   (Standing RULES or the lessons list) — an unindexed lesson is an un-findable lesson.
3. **Prefer UPDATE over duplicate.** If a lesson file already covers the area, sharpen it with the new
   measurement rather than adding a near-twin.
4. **A lesson earns its file only if it saves a FUTURE dig.** Don't manufacture lessons from trivial
   work; a thin session legitimately adds none. But a real dig with no lesson written is the exact
   failure the DIG-RULE exists to prevent — if you dug, you owe the note.
5. **Update the canonical LESSONS LEDGER — `docs/LESSONS.md`.** This is the single browsable document that
   holds the running list of every learned lesson + DIG-RULE lesson (grouped by domain; one row each:
   the takeaway, the "look here FIRST next time" pointer, and a link to the full `memory/` file). Every
   `/documentize`:
   - **ADD** a row for each lesson written in sub-steps 1–4 this session.
   - **RECONCILE** the existing rows for staleness (consume the Step 0.5 lesson check): fix a row whose
     cited symbol/path moved; strike/relocate a row whose lesson was archived. `docs/LESSONS.md` is a
     living doc under the Step 1 sweep like any other — it must never point at a dead symbol.
   - If `docs/LESSONS.md` does not exist yet, CREATE it (seed it from the current `memory/` lessons +
     the Standing RULES in `MEMORY.md`) and add it to the CLAUDE.md reading order.
   It complements `MEMORY.md` (the terse auto-memory index loaded each session), it does not replace it:
   `MEMORY.md` is the machine index; `docs/LESSONS.md` is the human-readable, categorized digest.

## 4. Prepare for compact / handoff (clean resume)
Verify a fresh context can pick up with no re-derivation:
- The **READ-FIRST pointer** in `MEMORY.md` points at this session's topic file, and that file's NEXT
  section names the concrete next step(s).
- Nothing is mid-edit; the tree builds (don't claim "builds" unless you built it). State the deployed
  SHA, HEAD, and the uncommitted set (or that it's committed).
- If a background job / workflow / hands-on is in flight, record its ID + what its result will mean.
- The CLAUDE.md "reading order after a session reset" still resolves (add a new canonical doc to it if
  one was created this session).

## 5. Report
Give the user a tight summary: what got documented/updated, the honest status of the work (verified vs
pending), the deployed/committed state, and the exact NEXT step. No forbidden hand-off phrases ("should
work", "build clean — ready") — state evidence. **Lead with the Step 0.5 reconciliation ledger** (claim →
code evidence → true status → action) so the user can see every OPEN/DONE flip you made and why — that
ledger is the deliverable that proves you checked the code, not the labels.

---
**Guardrails:** Every lesson written to `memory/` MUST land its row in `docs/LESSONS.md` in the same
run (the Step 3.5 hard pairing) — an unpaired lesson is a skill violation. Do NOT mark work "VERIFIED"
without real hands-on or a matching real log. **Do NOT carry a
status label forward in EITHER direction without verifying it against the code this run** — false-open
(re-asserting OPEN/FUTURE/TODO for shipped work) is as bad as false-PROVEN. Do NOT invent status. Honor
RULE 2 (retired info goes, fully — archive, don't leave parallel stale + fresh). Commit the doc updates
per [[feedback-commit-autonomously]] (coherent commit, to `main`; ask before push). No emojis in files
unless requested.
