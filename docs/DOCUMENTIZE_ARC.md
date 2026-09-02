# /documentize arc — revising the session-close skill on its own evidence (LIVE doc)

> **Canonical LIVING doc for the `/documentize` skill revision.** Opened 2026-09-02 on the user's
> instruction (§0), the second skill measured the way `docs/QF_ARC.md` measured `/qf`: the skill's own
> output, censused with re-runnable commands, against what the skill's text mandates. The skill is
> `.claude/skills/documentize/SKILL.md` (207 lines, versioned since `03748f56`).
>
> Status tags as in the other arcs: **DECIDED** · **AS-BUILT** · **PENDING** · **DESIGN** · **`[V]`**
> measured (instrument named beside the number) · **`[A]`** taken from the lessons ledger · **`[?]`**
> unverified. **Everything in §3 is DESIGN. Nothing is built.** §8 is the audit log; §9 the build order
> with the cut.

---

## 0. THE ASK (USER 2026-09-02, verbatim)

> *"Ok I green light it. The next skill we improve in the same way is /documentize"*

"The same way" = measure the skill against what it actually produced, name the changes with the largest
measured benefit, rank them with a cut, write the hot doc, audit the doc. Same evidence discipline as
`docs/QF_ARC.md`: every number carries its command (Appendix A), every instrument names what it
actually counts.

## 1. What the skill is, in one paragraph

`/documentize` closes a work session: survey what changed (Step 0), **hand-reconcile every status
label in the whole doc tree against the code, both directions** (Step 0.5 — "a status label is a
CLAIM, not a fact"), sweep and update ALL living docs and archive the stale ones (Step 1), update
`research/` (Step 2), write the session's memory file + index line (Step 3), write every lesson TWICE —
a `memory/` file and its `docs/LESSONS.md` row, and diff the two sets (Step 3.5), prepare the handoff
(Step 4), and report with the reconciliation ledger as the proof (Step 5). Its scope statement is
explicit: *"this is a COMPREHENSIVE sweep, not an incremental touch-up ... Update ALL docs, not only
the ones this session edited ... read each doc."* It is invoked *"at the end of a work session, before
/compact"*.

## 2. Evidence base

Two sources. (a) The skill's own output: `[V]` **279 commits whose message mentions documentize**
(2026-06-21 .. 2026-09-02, `git log -i --grep`), the artifacts it maintains (`CLAUDE.md`, `MEMORY.md`,
the `memory/` directory, `docs/LESSONS.md`, the doc tree), and the session transcripts. (b) The
lessons ledger's own rows about documentation rot.

### 2.1 What the skill's text mandates vs what a run can do `[V]`

| # | Measurement | Value | What it counts / limitation |
|---|---|---|---|
| D1 | Status markers Step 0.5 says to hand-check "item by item" per run, using the skill's own regex | **11,291** = 2,912 in `docs/*.md` (153 files) + 3,907 in `research/findings/*.md` (298) + 4,472 in `memory/*.md` (1,055); plus 2,590 inverse (DONE/SHIPPED/...) hits in `docs/` alone | The skill's own grep, markdown only. `NEXT` and `OPEN` match ordinary prose too, so this over-counts labels — but the skill's instruction is to check every HIT |
| D20 | Bytes the skill says to "read each doc" of, per run | **17.4 MB** = docs 4.76 MB + findings 6.58 MB + memory 6.09 MB | `cat | wc -c` over the three trees the skill names |
| D9 | Runs, and their real breadth | **279 commits; mean 6.0 files, 102 insertions per commit; ~10 commits/day** over the last 14 days (1, 4, 9, 7, 8, 9, 1, 13, 15, 12, 10, 6) | `--shortstat`; a commit is the lower bound of a run. The mandated COMPREHENSIVE sweep is measured INCREMENTAL in every run on record |
| D6b | Runs whose commit body says "reconcil" | **203 of 279** | The WORD is present; D1 says the check it names cannot have been done in full |
| D4 | The hard pairing (`memory/` lesson file ↔ `LESSONS.md` row), the one step with a mechanical diff | **40 of 690 files unreferenced (5.8 %); 1 dead reference of 631** | slug match either naming convention; this safeguard HOLDS |
| D5b | `CLAUDE.md` pointers that do not resolve | **2 of 95** backticked repo paths (one a `b<N>` template, one a pointer into the VotvIO repo) | after allowing the `src/votv-coop/` prefix and brace expansion; Step 4's "reading order still resolves" HOLDS |
| D8 | Archiving (Step 1: "archive stale stuff", every run) | **8 moves into `_archive/` across 279 commits**; `_archive/` holds 26 files | `--diff-filter=RA`; the 433 deletes in those commits are the 2026-08-23 untracking of `research/`, not archiving |

### 2.2 What the skill's output costs the next session `[V]`

| # | Measurement | Value | What it counts / limitation |
|---|---|---|---|
| D2 | What every session loads before doing anything | **`CLAUDE.md` 1,723 lines / 147.5 KB + `MEMORY.md` 75 lines / 23.7 KB = 171 KB**; `/context` at this session's start: **68.6k tokens "Memory files", 6.9 % of the window** | `wc`; the `/context` figure is the harness's own |
| D13 | The reading order inside `CLAUDE.md` | lines 461-1723 = **73 % of the file; 68 entries; the longest 275 lines** (`4e. docs/signals/`), then 152 (`4e-browser`), 87 (`4a-identity`), 73 (`1a-veh`) | awk over `^<n><letters>. ` entries |
| D2b | `MEMORY.md` against the skill's own rule "keep every line ≤200 chars" | **35 of 75 lines over; the longest 1,466 chars** | `awk length>200` |
| D3 | Inline correction markers in `CLAUDE.md` — rot handled by ACCRETION beside the wrong claim | **27 lines**: corrected 8, superseded 6, no longer 5, is FALSE 5, was wrong 2, stood here 2, "said the opposite" 1 | grep; the skill's guardrail says *"retired info goes, fully — archive, don't leave parallel stale + fresh"* |
| D6 | `docs/LESSONS.md` growth | **318 KB (Aug 1) → 688 KB (Sep 1) → 760 KB (Sep 2)**; documentize commits added **+4,838 / −268 lines** (18:1), mean +27, max +124 per touch | `git show <rev>:` + `--numstat`; append-only in practice; no consumer reads it whole (`docs/QF_ARC.md` E10; this skill's own Step 0.5(5)) |
| D7 | The memory corpus | **1,055 files / 6.09 MB** (588 lesson, 355 project, 102 feedback, 9 reference); `MEMORY.md` links **69** of them | ls/wc; the index's own header says "pointers only; lesson lists live as GREPS" |
| D12 | Docs carrying a LIVE / LIVING banner | 19; **2 untouched since July** (`MODULARIZATION_PLAN.md` 07-19, `COOP_METHODOLOGY.md` 07-20) | `git log -1` per file |
| D17 | Are the two largest outputs versioned? | **No.** `CLAUDE.md` is gitignored (`.gitignore:113`, user decision 2026-05-25 "per-user dev notebook"); `memory/` is outside any repository | `git check-ignore -v`; no diff can show which run wrote a wrong claim, and no growth curve of either exists |
| D19 | Invocations from the transcripts | **not measurable this way** — the tag string is also in tool output and skill listings, so a grep counts thousands | 279 commits is the floor |

### 2.3 Sampled staleness of the labels themselves `[V]` (independent agent, 2026-09-02; 28 hits, 14 open-ish + 14 done-ish, every k-th sorted line, ≤2 per file, code fences and legend lines skipped)

Population in `docs/*.md`: **1,407 open-ish hits, 2,711 done-ish hits.** Each sampled line was
checked against the code and git; the agent's own classification, with its evidence:

| # | file:line | label (abridged) | class | evidence (abridged) |
|---|---|---|---|---|
| 1 | AUTHORITATIVE_INTERACTABLE_MIGRATION.md:4 | PHASE A+B IMPLEMENTED (uncommitted; hands-on-pending) | **STALE-OPEN** | committed `43e2a843` (06-05) + `f8185847` (07-04); keypads W/HO in SYNC_PROFILES:153 |
| 2 | COOP_MIRROR_IDENTITY_WINDOW_RACE.md:100 | own-key != pending-key -> never steal | still-true (vocabulary FP: "pending" is a name) | quiescence_drain.h:73, pile_spawn_bind.cpp:5 |
| 3 | COOP_SYNC_PROFILES.md:159 | Power panel · U · snapshot + pending | still-true (FP: a lane column) | power_sync.cpp:139/:261; U is still the honest verdict after the 09-02 RE |
| 4 | DEATH_ARC.md:1018 | blast radius is uncensused (open windows...) | still-true (FP: "open windows") | death_revive.cpp has only RemoveFromParent |
| 5 | LESSONS.md:4571 | kills the NEXT open's fade-in | still-true; **two citations rotted** | `:359` is now `:470`, `:214` is `:262` in chat_view.cpp |
| 6 | MULTIPLAYER_UI.md:2003 | still NOT hands-on ... current is b125 | **STALE-OPEN** (partial) | build is 151 (protocol.h:710); the label discussed with the user 08-31, never folded; running total b125 |
| 7 | SERVER_BROWSER_ARC.md:434 | P3 cyan STAYS; UI_STYLE §6's question now CLOSED | still-true; **sibling doc stale** | VOTV_UI_STYLE.md:209-222 §6 still reads "still open" |
| 8 | items/container.md:349 | THE ONE OPEN GATE: is slot OnClicked cancelable | still-true (name collision) | container_take_probe.cpp measures something else |
| 9 | piles/08-HOST-AUTH-TRASH-CHANNEL.md:34 | L1 + L2: the open functional bugs | **STALE-OPEN** | the doc's own 06-23 banner marks it historical; L2 seam shipped `03d38d2b`; the line is unstamped |
| 10 | piles/_archive/07-MORPH-V2...:213 | Init-POST observer does NOT fire for a BP-deferred clump | still-true (FP: "deferred" is a UE term) | host_spawn_watcher.cpp:130-133 |
| 11 | piles/findings/thin-client...IMPL-SPEC-2026-06-20.md:62 | pile-reconcile core (P5 pending-remove) in prop_adoption.cpp | **STALE-OPEN** (location) | no prop_adoption.cpp in HEAD; split 06-30 into quiescence_drain + pile_spawn_bind |
| 12 | piles/findings/votv-held-pose-stream-design-2026-05-27.md:873 | Open questions — Q1 hand bone socket (held_entity_sync) | **STALE-OPEN** (dissolved) | held_entity_sync never existed; hand_item.cpp:35-50 welds into the viewmodel (07-10) |
| 13 | (local security tree) | a dated ledger row recording a batch of findings opened that day | still-true (dated ledger row) | append-only by its own rule, so a later count never falsifies it. **Row REDACTED 2026-09-04:** the original quoted the register's own OPEN counts, which `DOCS_ARC` WP-2 scrubs from public docs |
| 14 | (local security tree) | an OPEN register row | still-true | **Row REDACTED 2026-09-04:** the original named the finding, stated its mechanism, and cited the code range confirming it still live — a complete pointer, and the exact thing `DOCS_ARC` WP-2 exists to keep out of a public doc |
| 15 | ARCHITECTURE.md:94 | D-3 slim contract shipped; zero imports; abi_gate in CI | still-true | cppmod_entry.cpp:8-21; build-core.yml:313 |
| 16 | COOP_EVENT_JOIN.md:122 | event_cue AS-BUILT (code-verified 07-04) | still-true | event_cue_sync.h:59 |
| 17 | COOP_SYNCER_MODEL.md:301 | mirror_manager.h:357 DrainMirrorsForSlot — Built | still-true (the line number still holds) | mirror_manager.h:357 |
| 18 | CRUTCHES.md:314 | the proper fix: intercept OpenLevel when dead (fail CLOSED) | still-true (FP: "fail CLOSED") | C3 CLOSED `33008d87` |
| 19 | LESSONS.md:2441 | switcher_widgets censused in ui_menu only; ui_stats/ui_settings reach it | **UNDECIDABLE** | the CXX dump has no `switcher_widgets`; needs a runtime probe |
| 20 | MODULARIZATION_PLAN.md:94 | A2 — DONE (commit pending); git rm'd the two .gitkeep | **STALE-OPEN** (partial) | committed `91f31c6e` (07-07); "commit pending" never flipped |
| 21 | OVERLAY_CAPTURE_COEXIST.md:562 | Fail-CLOSED ... (6 sigs as of 26674b21, 7 once DX12) | still-true; **count rotted** | sdk_profile.h now has 10 kSig* |
| 22 | SERVER_BROWSER_ARC.md:250 | the banner "nothing built" is false; status in §9 | still-true | §9 AS-BUILT at :550 |
| 23 | VERSION_MIGRATION.md:583 | fix compose VERIFIED 08-22 (0c14a931) | still-true; **next sentence rotted** | :588 "proxy stays until commit 3" — `1912d229` landed 08-28 |
| 24 | piles/08-HOST-AUTH-TRASH-CHANNEL.md:122 | dropGrabObject thunk RETIRED fb490e36; option 1 (8bc797ef) BUILT+FAILED | still-true; **dangling hash** | `8bc797ef` is not a commit in this repo (history squashed) |
| 25 | piles/_archive/06-AS-BUILT-sync-mirror.md:64 | files changed (as-built): pile_morph, proto 80->81 | still-true (archived, bannered) | pile_morph deleted `1fc67aed`; the doc's own banner says REVERTED |
| 26 | piles/findings/votv-client-world-divergence...06-09.md:104 | FALSIFIED 06-10 ... (4) Deferred remote_prop 896>800 | still-true; **sub-item (4) stale, LOC rotted** | remote_prop.cpp is 774 LOC since s28; the deferral shipped unflipped |
| 27 | (local security tree) | a lesson about a SHIPPED gate, cited by file:line | still-true; **citation moved** | the constant moved to `intent_authority.cpp:35` in `7de9228c` — the drift rung's exact case. Subject redacted 2026-09-04 |
| 28 | security/TRACKER.md:627 | B4 IS BUILT (41c19d02 + 3f357ecd) CurrentWorldKind() | still-true | both commits 08-25; registry_reaper.cpp calls it |

**Rates.**

| half | stale-open | stale-done | still-true | undecidable |
|---|---|---|---|---|
| open-ish, all 14 hits | **5** (#1, #6, #9, #11, #12) | 0 | 9 | 0 |
| open-ish, true status labels only (8 — the six vocabulary false positives #2-5, #10, #11 dropped) | **4 of 8 = 50 %** | 0 | 4 | 0 |
| done-ish, all 14 hits | 1 (#20, "commit pending") | **0** | 12 | 1 (#19) |
| done-ish, true status labels only (12) | 1 | 0 | 10 | 1 |

**Secondary rot inside lines whose headline label still holds: 8 of 28** — two line-number
citations (#5), a build number (#6), a sibling doc left open (#7), a signature count 6 -> 10 (#21),
a next sentence (#23), a dangling commit hash (#24), a deferred item that shipped (#26), a citation
file that moved (#27). None of these contain a status word, so the skill's grep never lands on them.

**The pattern, in the agent's words.** (1) False-DONE is essentially absent (0 of 14); what rots is
the subordinate fact riding on a true label — line numbers, running counts, parenthetical
sub-states ("uncommitted", "commit pending", "Deferred (4)"), a hash that stopped resolving after a
squash. (2) False-OPEN clusters in point-in-time docs from 2026-06/07 that were never re-stamped
when the work landed (#1, #9, #11, #12, #20 are all 05-27..07-07 material); every 2026-08 line
sampled was accurate; the one recent miss is CROSS-DOC (#7: one arc declares another doc's question
closed, the other doc still says open), which a per-line check cannot see. (3) The skill's
vocabulary over-counts: 8 of 28 hits (29 %) are not status labels at all ("pending-key",
"pending-remove", "BP-deferred", "open windows", "fail CLOSED", "NEXT open's").

### 2.4 The lessons ledger on documentation rot `[A]` (titles in `docs/LESSONS.md`)

| # | Row | What it says about this skill |
|---|---|---|
| L1 | *A correction in a NEW SUBSECTION leaves the headline stale — and the headline is what gets quoted* | Accretion (D3) is the documented failure shape, not a style choice |
| L2 | *A running total stated in prose inside an APPEND-ONLY register is stale by construction* (2026-08-24) | Counts written into `LESSONS.md` / `CLAUDE.md` rot the moment the next row lands |
| L3 | *A line number is a POSITION; the claim is about CONTENT, and only content can check content* (2026-08-30) | Citations by `file:line` are the rot the skill's Step 0.5(5) hunts — by hand, over 620+ rows |
| L4 | *Validate WHERE YOU READ, not against a mirror — or staleness fails OPEN* | A status checked against another doc instead of the code is not checked |
| L5 | *A GATE LEFT RED ON PURPOSE CARRIES NO SIGNAL — it cannot tell "waiting" from "forgotten"* | A mandated step nothing enforces is the same: it cannot tell "done" from "asserted" (D6b) |
| L6 | *A FIX IN THE TREE IS NOT A FIX IN THE FIELD* (2026-08-31) | A status owes two states; a single label cannot carry both |
| L7 | *A comment citing a DEPENDENCY's line number rots silently, and the confident ones rot worst* | Same class as L3, one level down |
| L8 | `.gitignore:250-266` (the "CLAIMED-ENFORCED, ACTUALLY MISSING" block) + `docs/DOCS_ARC.md` | *"A prose rule nothing enforces is not a rule — and an index that ASSERTS enforcement is worse than silence, because it stops anyone checking"*; and readme.com's one real hit on this tree: docs written inside-out |

### 2.5 What the evidence says, in three lines

1. **The skill mandates by prose what no run can do at the cadence it runs.** A hand check of 11,291
   markers (D1) and a read of 17.4 MB (D20), ten times a day (D9), satisfied in the report by the word
   "reconciled" (D6b). The ONE step with a mechanical check — the memory↔ledger pairing diff (D4) — is
   the one that holds. This is the `/qf` finding again (`QF_ARC` §2.3): safeguards are honor-system
   wherever nothing makes them observable.
2. **Its output is append-only, corrected by accretion, and loaded whole.** The ledger grows 18:1 (D6),
   `CLAUDE.md` carries 27 corrections beside the claims they correct (D3, L1), archiving is rare (D8),
   and every session pays 68.6k tokens for the result before reading a single source file (D2).
3. **Its two largest outputs have no history** (D17), so the rot it exists to prevent cannot be
   measured where most of it lives — the sampled stale rate (§2.3) is the first number anyone has.

## 3. The changes, ranked, with the cut (DESIGN)

| Rank | WP | What | Evidence class | Ships |
|---|---|---|---|---|
| 1 | WP-1 | Reconcile by INSTRUMENT over the session's blast radius, not by hand over the tree; the report's ledger is the instrument's output plus a verdict column | D1, D20, D9, D6b, D4 (mechanical holds) | **now** |
| 2 | WP-3 | A context budget for what a session loads: the reading order and the index get a gate, the skill's own ≤200-char rule gets enforced | D2, D13, D2b | **now** |
| 3 | WP-2 | Corrections in LIVING docs REPLACE, with a one-line dated note; a detector for accretion vocabulary | D3, L1, the skill's own guardrail | **now** |
| — | — | **the cut** | | |
| 4 | WP-4 | The ledger's citations are checked by instrument, rows with dead citations are quarantined, the pairing diff becomes a script | D6, L2, L3, D4's 40 | after WP-1's first census |
| 5 | WP-5 | Give the reading order a history: move it out of the untracked `CLAUDE.md` into a tracked file the session reads first | D17, D13 | the user's call (§6) |

**Dependency edges.** WP-1's script (`tools/docs/status_census.py`) is the substrate WP-4 extends;
WP-3's gate (`tools/docs/context_budget.py`) is independent; WP-2 is a text rule plus a detector mode
of WP-1's script. Nothing below the cut is deleted; WP-4 waits for WP-1's first census so its row
census is sized on a measurement, and WP-5 touches the user's own file.

### WP-1 — Reconcile by instrument over the blast radius (ships now)

**Defect.** Step 0.5 orders a hand check of every status marker in the tree; there are 11,291 hits
(D1) and the skill runs ~10 times a day (D9). No run has done it, every run's report says it has
(D6b), and the one mechanical step in the skill is the one that holds (D4). The mandate makes the
report a claim about the checker, not about the docs.

**Change.** (a) NEW `tools/docs/status_census.py [--since <rev>|--staged] [--sweep]`: from the
session's diff (`git diff --name-only <since>..HEAD` + the working tree), list every status marker
in (i) the docs touched by the session and (ii) every doc that cites a symbol, path or commit the
diff touched (grep the diff's symbols across `docs/`, `research/findings/`, `memory/`); for each hit
print `file:line | marker | the cited symbol/path | resolves? (exists / moved / gone)`. The cited-
symbol column is the mechanical half — a marker beside a symbol that no longer exists is rot by
construction. `--sweep` runs the same over the whole tree and is a SEPARATE, rare, explicit act (the
user asks for it), never part of a session close. (b) Step 0.5 is rewritten: the hand check applies
to the census's rows — tens, not thousands — and each row gets a verdict (`STILL-OPEN` /
`ACTUALLY-DONE` / `STALE-DONE` / `PARTIAL`) with its evidence; the Step 5 report's ledger IS the
census output with the verdict column filled, pasted, never written from memory. (c) The scope
statement changes to what is true: a session close reconciles the session's blast radius; the tree
is swept on demand.

**Mechanism.** The set to check is computed, bounded and printed; "reconciled" can then mean
"every row of this list has a verdict" and be checked by reading the list.

**Cost.** One ~150-line script; a census per run instead of a claim.

**Acceptance.** Every documentize commit body after this ships contains the census's row count and
the verdict counts (grep-able); the sampled stale rate (§2.3) is re-taken after ten runs.

### WP-2 — Corrections in living docs REPLACE (ships now)

**Defect.** 27 lines in `CLAUDE.md` correct a claim by writing the correction next to it (D3): *"the
previous line here said the opposite and was wrong"*, *"that stood here is FALSE"*. The ledger
already names the failure (L1) and the skill's own guardrail forbids it — in prose nothing enforces.
The point-in-time docs under `research/` are a different case: there the supersede stamp is the
documented convention (`docs/README.md`) and stays.

**Change.** Skill text: in a LIVING doc (`CLAUDE.md`, `MEMORY.md`, any `*_ARC.md`, the maps, the
trackers) a correction REWRITES the claim in place and leaves at most one dated line — *"(corrected
2026-09-02: was X, measured Y, `<citation>`)"* — never the old sentence beside the new. The detector:
`status_census.py --accretion` greps the living docs for the accretion vocabulary (`CORRECTED`,
`SUPERSEDED`, `is FALSE`, `was wrong`, `stood here`, `said the opposite`, `no longer`) and lists the
hits as RULE-2 debt; Step 1 consumes the list. The 27 existing lines are the first debt, paid down
one entry per run, not in one sweep.

**Mechanism.** The vocabulary that marks accretion is finite and grep-able; the rule becomes a count
that should fall.

**Cost.** Text plus a detector mode of WP-1's script.

**Acceptance.** D3 re-measured after ten runs: falling, not rising; zero NEW accretion lines in the
diffs of those runs.

### WP-3 — A context budget for what a session loads (ships now)

**Defect.** Every session starts by loading 171 KB — 68.6k tokens, 6.9 % of the window — of which
73 % is a reading order whose longest entry is 275 lines (D2, D13) and an index that breaks its own
≤200-char rule on 35 of 75 lines (D2b). Step 4 tells every run to ADD to the reading order; nothing
in the skill ever shortens it. An entry that digests a doc in 275 lines is a second copy of the doc,
loaded whether or not the session needs it.

**Change.** NEW `tools/docs/context_budget.py`: measures the reading-order section of `CLAUDE.md`
(bytes, entries, the longest entry) and `MEMORY.md` (bytes, lines over 200 chars) and exits 1 over
budget. First budget = **half of today**: reading order ≤ 60 KB, no entry over 15 lines, `MEMORY.md`
≤ 12 KB with every line ≤ 200 chars — the numbers are a policy, stated as such, to be re-set from
the first census. The skill's Step 4 runs the gate; over budget, the run must SHORTEN before it adds
(an entry is a pointer plus the two or three facts a session must know before opening the doc; the
rest lives in the doc it points at). The digest-sized entries (signals 275, browser 152, identity 87,
vehicles 73) are the first four to shrink, one per run.

**Mechanism.** A budget with a gate turns "add a pointer" into a zero-sum edit; the cost every
session pays becomes a number a run can be refused on.

**Cost.** One ~60-line script; a few minutes per run while the debt is paid down.

**Acceptance.** `/context` "Memory files" at session start halves within ten runs; the gate is green
on every documentize commit after the debt is paid.

### WP-4 — Ledger citations checked by instrument; rows quarantined, not carried (parked)

**Defect.** `docs/LESSONS.md` is 760 KB and grows 18:1 (D6); Step 0.5(5) orders every lesson's cited
symbols and paths be grepped against the tree every run — 620+ rows, by hand. Rows cite line numbers
(L3, L7) and running totals (L2) that rot by construction.

**Change.** `tools/docs/ledger_rot.py`: for each row, extract `path:line`, `path`, and backticked
symbols; test each against the tree (exists / line beyond `wc -l` / symbol has zero hits); print the
rows with dead citations. Step 3.5's RECONCILE consumes that list: a row with a dead citation is
re-cited or moved to a `## Quarantine` section with the failing citation named — never silently
carried. The pairing diff (D4) becomes the same script's `--pairing` mode, so the 40 unpaired files
are a printed list. **Parked** because its row census must be sized on WP-1's first census and a dry
run (how many of 620 rows have a checkable citation at all).

### WP-5 — Give the reading order a history (parked; the user's call)

**Defect.** `CLAUDE.md` is gitignored by the user's decision and `memory/` is outside any repository
(D17). The skill's largest outputs cannot be diffed, so a wrong claim written by a run has no
commit to be found in, and no growth curve exists.

**Change (proposed, not decided).** Move the reading-order section (project knowledge, not
per-machine config) into a tracked `docs/READING_ORDER.md`; `CLAUDE.md` keeps one line — *"read
`docs/READING_ORDER.md` first"*. The section then has a diff per run, the WP-3 gate applies to a
tracked file, and a session that does not need it does not pay for it. Cost: one Read at session
start when it is needed. This is the user's file; §6 asks.

## 4. Kept as-is, and what was looked at and declined

**Kept:** the two-write lesson rule and its pairing diff (the one mechanical step; D4); the
`[V]`/`[RD]`/`[?]` tags; "never VERIFIED from a smoke alone"; the memory topic file + index line;
Step 4's handoff checks (D5b shows the pointer check holds); the ~10/day cadence — a session close
should be cheap and frequent, which is exactly why its mandate must be small.

**Declined:** splitting the skill in two (a close-out skill and a sweep skill) — WP-1's `--sweep` flag
is the same thing without a second file; auto-compacting `MEMORY.md` by script — the index is prose
the user reads, a gate is enough; tracking `CLAUDE.md` whole — the user decided it is a per-user
notebook.

## 5. Open questions for the user

1. WP-5: move the reading order into a tracked `docs/READING_ORDER.md` that `CLAUDE.md` points at?
   Costs one Read at session start; gains history and a gate on a tracked file.
2. WP-3's budget numbers are a policy (half of today). Set a different number, or let the first
   census set it?

## 6. Risks named in advance

- **WP-1 could narrow the check to what a session touched and miss cross-doc rot.** That is what the
  cited-symbol column and the `--sweep` mode are for; and the sampled stale rate (§2.3) is re-taken
  after ten runs to see whether tree-wide rot moves.
- **WP-3 could push facts out of the reading order that a session needed.** The entry keeps the two or
  three facts a session must know BEFORE opening the doc; the rest is one Read away in the doc the
  entry points at — which is where the skill's Step 1 says the truth lives.
- **The stale rate (§2.3) is a 28-item sample.** It bounds the defect; it does not size it.

## 7. Risks to the doc's own numbers

- D1 over-counts labels (`NEXT`, `OPEN` in prose); the skill's instruction is per HIT, so the load
  claim stands.
- D9's "~10 runs/day" counts commits; a run that committed twice or not at all moves it either way.
- D3's vocabulary is the accretion phrasing found in `CLAUDE.md` today; other phrasings exist.

## 8. Audit log

_Pending: two independent read-only agents (evidence re-derivation; design consistency) on this
version, then a `/qf` pass under the revised skill (its first ledger rows). Findings and dispositions
go here._

## 9. Build order, with the cut

0. `tools/docs/status_census.py` (+ `--accretion`, `--sweep`) — WP-1's substrate; drilled on this
   session's own diff before it is trusted.
1. WP-1 text: Step 0.5 and the scope statement rewritten; the Step 5 ledger = the census + verdicts.
2. WP-3: `tools/docs/context_budget.py` + Step 4 runs it; the first four entries shrink one per run.
3. WP-2 text + the detector; the 27 debts paid one per run.
4. **The first census**: ten runs later, re-take D2, D3, §2.3, and the row counts in the commit
   bodies. Record here.
   — the cut —
5. WP-4 (gated on step 4 + a dry run); WP-5 (gated on §5.1).

---

## Appendix A — the instruments (re-runnable; Git Bash on this box)

```
M=$(cygpath "$LOCALAPPDATA")/../../.claude/projects/D--Projects-Programming-VOTV-MP/memory   # ~/.claude/...
RX='OPEN|FUTURE|TODO|PENDING|NEXT|not (yet )?(built|wired|implemented|done|verified)|deferred|unverified|\[ \]|\[\?\]|planned|stub|placeholder'
# D1 (markdown only; research/ holds non-markdown bulk that times a raw grep out)
grep -rniE --include='*.md' "$RX" docs/ | wc -l; grep -rniE --include='*.md' "$RX" research/findings/ | wc -l; grep -niE "$RX" $M/*.md | wc -l
# D20
cat $(find docs -name '*.md') | wc -c; cat $(find research/findings -name '*.md') | wc -c; cat $M/*.md | wc -c
# D9 / D6b
git log -i --grep=documentize --shortstat --format= | awk '/files? changed/{f+=$1;n++;i+=$4} END{print n, f/n, i/n}'
git log -i --grep=documentize --format='%ad' --date=short --since=2026-08-19 | sort | uniq -c
git log -i --grep=documentize --format=%B | grep -c -iE 'reconcil'
# D4 pairing, D5b pointers: the two python snippets in the session log (memory slug either convention; src/votv-coop prefix + brace expansion)
# D8
git log -i --grep=documentize --diff-filter=RA --name-status --format= | grep -c _archive
# D2 / D13 / D2b / D3
wc -lc CLAUDE.md $M/MEMORY.md; awk 'length>200' $M/MEMORY.md | wc -l
awk '/^[0-9]+[a-z-]*\. /{if(lbl!="")print n, lbl; lbl=substr($0,1,60); n=0} {n++} END{print n, lbl}' CLAUDE.md | sort -rn | head -6
grep -o -iE "CORRECTED|was wrong|is FALSE|SUPERSEDED|no longer|said the opposite|stood here" CLAUDE.md | tr 'A-Z' 'a-z' | sort | uniq -c
# D6
for d in 2026-08-01 2026-09-01; do r=$(git rev-list -1 --before="$d 00:00" main -- docs/LESSONS.md); git show $r:docs/LESSONS.md | wc -c; done
git log -i --grep=documentize --numstat --format= -- docs/LESSONS.md | awk -F'\t' '$1 ~ /^[0-9]+$/ {a+=$1;d+=$2;n++} END{print a,d,n}'
# D7 / D12 / D17
ls $M/*.md | wc -l; cat $M/*.md | wc -c; grep -o -E '\[\[[^]]+\]\]|\([a-z0-9_.-]+\.md\)' $M/MEMORY.md | sort -u | wc -l
for f in $(grep -l -iE "LIVE doc|LIVING doc|living arc|living document" docs/*.md docs/*/*.md); do echo "$(git log -1 --format=%ad --date=short -- $f) $f"; done | sort
git check-ignore -v CLAUDE.md
```
