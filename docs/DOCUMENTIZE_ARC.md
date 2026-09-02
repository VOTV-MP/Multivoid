# /documentize arc — revising the session-close skill on its own evidence (LIVE doc)

> **Canonical LIVING doc for the `/documentize` skill revision.** Opened 2026-09-02 on the user's
> instruction (§0), the second skill measured the way `docs/QF_ARC.md` measured `/qf`: the skill's own
> output, censused with re-runnable commands, against what the skill's text mandates. The skill is
> `.claude/skills/documentize/SKILL.md` (207 lines, versioned since `03748f56`).
>
> Status tags as in the other arcs: **DECIDED** · **AS-BUILT** · **PENDING** · **DESIGN** · **`[V]`**
> measured (instrument named beside the number) · **`[A]`** taken from the lessons ledger · **`[?]`**
> unverified. **Everything in §3 is DESIGN. Nothing is built.** §8 is the audit log (three passes, 47
> findings, all dispositioned, 2026-09-02); §9 the build order.

---

## 0. THE ASK (USER 2026-09-02, verbatim)

> *"Ok I green light it. The next skill we improve in the same way is /documentize"*

"The same way" = measure the skill against what it actually produced, name the changes with the largest
measured benefit, rank them, write the hot doc, audit the doc. Same evidence discipline as
`docs/QF_ARC.md`: every number carries its command (Appendix A), every instrument names what it
actually counts. The doc's own first draft failed that discipline in one place that matters — it
proposed as NEW an instrument that already runs in CI (§8, F1) — and the audit's corrections are kept
visible rather than smoothed over.

## 1. What the skill is, in one paragraph

`/documentize` closes a work session: survey what changed (Step 0); reconcile status labels against
the code, **"MANUAL status reconciliation — verify EVERY open claim against the CODE ... a HAND check,
item by item"** over the whole tree (Step 0.5; the preamble frames it: *"a status label is a CLAIM, not
a fact"*); sweep the living docs — *"ENUMERATE the whole doc tree first ... and read each doc — then
TRIAGE every one"* — and archive the stale ones (Step 1); update `research/` (Step 2); write the
session's memory file + index line, with *"Keep every line ≤200 chars"* and a `MEMORY.md` compaction
rule (Step 3); write every lesson TWICE — a `memory/` file and its `docs/LESSONS.md` row — and diff the
two sets (Step 3.5); prepare the handoff, adding a new canonical doc to the `CLAUDE.md` reading order
*"if one was created this session"* (Step 4); report with the reconciliation ledger as the proof
(Step 5). Its scope statement: *"this is a COMPREHENSIVE sweep, not an incremental touch-up ... Update
ALL docs, not only the ones this session edited."* It is invoked *"at the end of a work session,
before /compact"*. The user's rule behind Step 0.5 is recorded in
`memory/feedback_documentize_manual_status_reconciliation.md` (2026-06-21: a sweep trusted the labels
and left a pile of OPEN items standing for shipped work).

## 2. Evidence base

Two sources. (a) The skill's own output: `[V]` **279 commits whose message mentions documentize**
(2026-06-21 .. 2026-09-02, `git log -i --grep=documentize`, counted BEFORE this arc's own commits,
which the instrument would otherwise count — Appendix A excludes `DOCUMENTIZE_ARC`), the artifacts it
maintains (`CLAUDE.md`, `MEMORY.md`, the `memory/` directory, `docs/LESSONS.md`, the doc tree), and a
sampled staleness check. (b) The lessons ledger's rows about documentation rot. Every number below was
re-derived blind by the pass-2a agent; where it drifted or was wrong, the corrected value stands here
and §8 records the change.

### 2.1 What the skill's text mandates vs what a run can do `[V]`

| # | Measurement | Value | What it counts / limitation |
|---|---|---|---|
| D1 | Status-marker HITS Step 0.5 says to hand-check "item by item" per run, using the skill's own regex | **11,291** = 2,912 in `docs/*.md` (153 files) + 3,907 in `research/findings/*.md` (298) + 4,472 in `memory/*.md` (1,055); plus 2,590 inverse (DONE/SHIPPED/...) hits in `docs/` alone | The skill's own grep (`SKILL.md:55`), markdown only. It counts HITS, not labels: two fixed-seed samples put true status LABELS at **8 of 28** (§2.3, fences and legends skipped) and **5-7 of 30** (pass 2b, over all hits — `OpenLevel`, `doorOpen`, `deferred-spawn`, `g_pendingAttaches`...). The true label population is several times smaller; the instruction is per HIT, so the load claim stands, and a census built on the raw regex is mostly noise (M-1) |
| D20 | Bytes Step 1 says to "read each doc" of, per run | **17.4 MB** = docs 4.76 MB + findings 6.58 MB + memory 6.09 MB | `cat | wc -c` over the three trees the skill names |
| D9 | Runs, and their real breadth | **279 commits; mean 6.0 files, 102 insertions per commit; 8.1 commits per ACTIVE day, 6.9 per calendar day** since 2026-08-19 (1, 4, 9, 7, 8, 9, 1, 13, 15, 12, 10, 6; three zero days) | `--shortstat`; a commit is the lower bound of a run. ONE sweep-shaped commit exists on record (`40b1512e`, 2026-07-12, 56 files, all modifications); every other run is incremental against a mandate that says comprehensive |
| D6b | Commits whose body mentions "reconcil" | **148 of 281 (53 %)** | Per commit (`git log -1 --format=%B <h> | grep -q`); the first draft's "203" counted LINES. The WORD is present in half the runs; D1 says the check it names cannot have been done in full |
| D4 | The hard pairing (`memory/` lesson file ↔ `LESSONS.md` row), a step with a mechanical diff | **40 of 690 files unreferenced (5.8 %); 1 dead reference** (`[[lesson-a-cannot-in-a-comment]]`, a truncated slug — a wikilink, which the CI gate below does not check) | slug match either naming convention; the reference denominator is pattern-dependent (631-669); the safeguard HOLDS |
| D4b | The ledger's citations are ALREADY machine-checked in CI | **`tools/docs/lessons_gate.py` (412 LOC, `99445efb`, 2026-08-29; `build-core.yml:188`): check A = `file:line` exists and is within the file; A2 = a QUOTED citation must still find its quote near the line (the CONTENT check); B = backticked symbols must exist in a code corpus.** Today: PASS | The first draft proposed exactly this as a new script and called the pairing diff "the ONE mechanical step". The L3 row the draft quoted names the gate in its second sentence (§8, F1). NOT covered by the gate: wikilinks, the pairing, running totals |
| D5b | `CLAUDE.md` pointers that do not resolve | with a broad extraction: **144 backticked paths, 10 unresolved** — a `b<N>` template, TWO pointers into the VotvIO repo (`docs/ARC.md`, `docs/design`), two historical names stated as such, an ellipsis path, a generated header, a lock file that exists only while held, one DESIGN seam not built | `src/votv-coop/` prefix + brace expansion + `.h/.cpp/.py/.md` fallback; the first draft's "2 of 95" was a narrower pattern. Either way Step 4's "reading order still resolves" HOLDS |
| D8 | Archiving (Step 1: "archive stale stuff", every run) | **8 moves into `_archive/` across 279 commits** (7 renames + 1 add), five of them into `research/_archive`; the four `_archive` dirs hold 32 files (19 tracked) | `--diff-filter=RA`; the 433 deletes in those commits are one commit, `cf3780d2` (2026-08-23), untracking `research/` (420) and `docs/security/` (13) — not archiving |

### 2.2 What the skill's output costs the next session `[V]`

| # | Measurement | Value | What it counts / limitation |
|---|---|---|---|
| D2 | What every session loads before doing anything | **`CLAUDE.md` 1,723 lines / 147.5 KB + `MEMORY.md` 75 lines / 23.7 KB = 171 KB**; `/context` at this session's start: **68.6k tokens "Memory files", 6.9 % of the window** | `wc`; the `/context` figure is the harness's own and not re-derivable by an agent |
| D13 | The reading order inside `CLAUDE.md` | from the heading at line 461 to EOF: **1,263 of 1,723 lines (73 %), 117,233 of 147,533 bytes (79.5 %); 54 entries; the longest 275 lines** (`4e. docs/signals/`), then 152 (`4e-browser`), 87 (`4a-identity`), 73 (`1a-veh`), 57 (`4e-imgui`), 57 (`4d-death`) | awk delimited by the heading (the first draft's whole-file pattern gave 68 by adding 8 principle lines and 6 checklist items). `4e` is a pointer paragraph plus 25 SESSION digests (s17..s30b) whose real home is `memory/project_s2*.md` |
| D2b | `MEMORY.md` against the skill's own rule "keep every line ≤200 chars" | **35 of 75 lines over; the longest 1,466 chars** | `awk length>200`; the same skill's Step 3 compaction rule HAS run (the index header says x11) — it shortens the index, never the reading order |
| D3 | Correction vocabulary in `CLAUDE.md` | **29 hits on 25 lines** (corrected 8, superseded 6, no longer 5, is FALSE 5, was wrong 2, stood here 2, "said the opposite" 1). Inspected: **~5 are accretion** — the old claim still standing beside its correction (the `0x45` substrate at :689/:692, "receive boundary STRICT" at :721/:724, "A2 never passed driven" at :508 vs :543, :1421/:1427, the pin at :81 vs :95); **5 are already in-place rewrites with a dated note** (the shape WP-2 prescribes); 6 narrate corrections made in OTHER docs; 3 are vocabulary false positives ("is corrected" of a physics corrector, a boolean "is FALSE", the README convention) | grep + reading; the first draft called all 27 "accretion" and would have counted the remedy as the debt (§8, C-1). The ledger names the failure shape (L1) and the skill's guardrail forbids it — in prose nothing enforces |
| D6 | `docs/LESSONS.md` growth | **318 KB (2026-08-01) → 688 KB (2026-09-01) → 760 KB (2026-09-02)**; documentize commits added **+4,838 / −268 lines** (18:1), mean +27, max +124 per touch | `git show <rev>:` + `--numstat`; append-only in practice. No consumer is VERIFIED to read it whole (`docs/QF_ARC.md` E10 shows the `/qf` critic is told to and nothing checks it); its citations ARE machine-checked (D4b) |
| D7 | The memory corpus | **1,055 files / 6.09 MB** (588 lesson, 355 project, 102 feedback, 9 reference); `MEMORY.md` links **69** of them | ls/wc; the index's own header says "pointers only; lesson lists live as GREPS" |
| D12 | Docs carrying a LIVE / LIVING banner | 19; **2 untouched since July** (`MODULARIZATION_PLAN.md` 07-19, `COOP_METHODOLOGY.md` 07-20) | `git log -1` per file |
| D17 | Are the two largest outputs versioned? | **No.** `CLAUDE.md` is gitignored (`.gitignore:113`; user decision 2026-05-25 NIGHT: *"CLAUDE.md is a per-user dev notebook"*); `memory/` is outside any repository | `git check-ignore -v`; no diff can show which run wrote a wrong claim, and no growth curve of either exists. And `CLAUDE.md` is private ON PURPOSE: `:588` *"this file is addressed to Claude, that one to humans, and only one of them is on GitHub"* — its reading order carries security material `docs/DOCS_ARC.md` scrubbed from the public docs on 2026-08-24 (§8, C-3) |
| D19 | Invocations from the transcripts | **not measurable this way** — 109,437 case-insensitive hits across 14 transcripts (3.9 GB): the string is in tool output and skill listings | 279 commits is the floor |

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
"pending-remove", "BP-deferred", "open windows", "fail CLOSED", "NEXT open's"). **A 28-item sample
bounds the defect; it does not size it** (§8, H-7): the re-take is a census, not a sample.

### 2.4 The lessons ledger on documentation rot `[A]` (titles in `docs/LESSONS.md`; all re-found by pass 2a)

| # | Row | What it says about this skill |
|---|---|---|
| L1 | *A correction in a NEW SUBSECTION leaves the headline stale — and the headline is what gets quoted* | Accretion (D3) is the documented failure shape, not a style choice |
| L2 | *A running total stated in prose inside an APPEND-ONLY register is stale by construction* (2026-08-24) | Counts written into `LESSONS.md` / `CLAUDE.md` rot the moment the next row lands (§2.3 #6, #21, #26) |
| L3 | *A line number is a POSITION; the claim is about CONTENT, and only content can check it* (2026-08-30) | Its own body: `lessons_gate` printed PASS in the session that created the rot, because in-range lines had drifted — which is why check A2 (content) exists. **The first draft quoted this title and missed the instrument named in its second sentence** |
| L4 | *Validate WHERE YOU READ, not against a mirror — or staleness fails OPEN* | The row is about a runtime mirror; the transfer to "a status checked against another doc instead of the code" is THIS doc's analogy (§2.3 #7 is the instance), not the row's claim |
| L5 | *A GATE LEFT RED ON PURPOSE CARRIES NO SIGNAL — it cannot tell "waiting" from "forgotten"* (2026-09-02) | A mandated step nothing enforces is the same class (D6b); and the first draft's own size gate would have been red by design for ten runs (§8, C-2) |
| L6 | *A FIX IN THE TREE IS NOT A FIX IN THE FIELD* (2026-08-31) | A status owes two states; a single label cannot carry both |
| L7 | *A comment citing a DEPENDENCY's line number rots silently, and the confident ones rot worst* | Same class as L3, one level down |
| L8 | `.gitignore`, the block headed *"CLAIMED-ENFORCED, ACTUALLY MISSING (measured 2026-08-23)"*, and the docs-arc note | *"A prose rule nothing enforces is not a rule — and an index that ASSERTS enforcement is worse than silence, because it stops anyone checking"*; and readme.com's one real hit on this tree: docs written inside-out. `LESSONS.md` carries the first sentence as a row of its own too |
| L9 | *A text-scanning gate reads prose as code — including the prose that documents the gate* | Found by pass 2b: any detector over correction vocabulary must exclude the ledger, the archives and the docs that define the vocabulary — this one included |

### 2.5 What the evidence says, in four lines

1. **The skill mandates by prose what no run can do at the cadence it runs.** A hand check of 11,291
   hits (D1) and a read of 17.4 MB (D20), eight times an active day (D9), satisfied in half the reports
   by the word "reconciled" (D6b). The two steps with a mechanical check — the pairing diff (D4) and
   the CI ledger gate (D4b) — are the ones that hold. This is the `/qf` finding again
   (`QF_ARC` §2.3): safeguards are honor-system wherever nothing makes them observable, and
   mechanical where something does.
2. **The rot that exists is not where the skill looks.** In the sample, half the true open labels in
   2026-06/07 point-in-time docs are stale and no done label is false (§2.3); the dominant rot is the
   subordinate fact under a true label — a line number, a count, a hash, a sub-state — which carries no
   status word, and the one recent miss is cross-doc. The raw vocabulary over-counts by a factor of
   several (D1), so a per-hit hand check would drown the labels that matter in prose that does not.
3. **Its output is append-only, corrected by accretion in a few places, and loaded whole.** The ledger
   grows 18:1 (D6), `CLAUDE.md` carries ~5 true accretion sites among 29 correction hits (D3),
   archiving is rare (D8), and every session pays 68.6k tokens for the result before reading a single
   source file (D2) — a measured COST, with no measured harm on record.
4. **Its two largest outputs have no history and are private on purpose** (D17), so their rot cannot
   be diffed and their history cannot be published; any instrument for them must WRITE its numbers
   somewhere that has history — the close commit itself.

## 3. The changes, ranked (DESIGN)

**The cut criterion (stated, per `QF_ARC` §3):** above the cut, the benefit is MEASURED on this
skill's output; below it, inferred. After the audits all four remaining changes sit above it — each
answers a measured row — and they collapse onto ONE new script, ONE extension of an existing gate,
and ONE machine trailer, because the audits showed the first draft's three scripts, a gate, a
quarantine section and a new file were the shape this doc diagnoses (per-run chores nothing
observes). The first draft's WP-5 (a tracked reading order) is DISSOLVED: it would have published
material scrubbed from the public docs (§8, C-3), and its real goal — a history for the untracked
outputs — is met inside WP-1 with no publication.

| Rank | WP | What | Measured on |
|---|---|---|---|
| 1 | WP-1 | ONE census script + ONE machine trailer: reconciliation over the session's blast radius plus an amortised tree sweep, a label grammar, and a private history for the two untracked outputs | D1, D20, D9, D6b, D17, §2.3 (50 % stale-open in old docs; cross-doc misses) |
| 2 | WP-4 | Extend `lessons_gate.py` (exists, CI) to what it does not cover: wikilinks, the pairing, running totals | D4, D4b, §2.3's subordinate-rot class (8 of 28), L2 |
| 3 | WP-2 | Corrections in living docs REPLACE, with ONE machine-distinct stamp; the accretion count is a trailer column that must not rise | D3 (~5 sites), L1, L9, the user's same-day supersede-stamp rule |
| 4 | WP-3 | A RATCHET, not a threshold, on the reading order and the index: red only on GROWTH against the previous trailer; the "half of today" number is a printed target until reached | D2, D13, D2b, L5 (a red-by-design gate has no signal) |

**Dependency edges.** WP-1's script and trailer are the substrate; WP-2's detector and WP-3's ratchet
are columns of that trailer; WP-4 is a separate extension of an existing file and depends on nothing.

### WP-1 — One census, one trailer, one private history (ships on the user's green light)

**Defect.** Step 0.5 orders a hand check of every status hit in the tree — 11,291 (D1), mostly not
labels — and Step 1 orders every doc read (D20), at eight runs an active day (D9). No run has done
it and half the reports say "reconciled" (D6b). The rot that exists (§2.3) sits in old point-in-time
docs the session never touched, in subordinate facts that carry no status word, and across docs.
And the two files most of it lives in have no history (D17).

**Change (a) — `tools/docs/status_census.py`, the ONE script.** Its inputs: the diff base and the
working tree. **Base rule (H-2):** the newest commit whose body carries the `Docs-Census:` trailer
(below), so consecutive censuses TILE the history with no gap; on the first run `--since=<date>`
from the previous memory topic file. **Symbols of the diff:** touched paths and basenames;
function/class names from `git diff -U0` hunk headers and from definitions added or removed; commit
hashes in the range. A docs-only diff has radius (i) only and prints `radius: docs-only`.
**Radius:** (i) the docs touched by the session; (ii) every doc citing a symbol of the diff; (iii)
**the amortised sweep** — the K docs (default 10) whose last census is oldest, by a `sweep-cursor`
carried in the trailer, so EVERY doc in `docs/`, `research/findings/` and `memory/` reaches a verdict
within N runs without any run reading the tree (`--sweep` = the full pass, on the user's request).
**Label grammar (M-1):** a LABEL is a bracket tag (`[?]`, `[V]`, `[A]`, `[RD]`, `[SUPERSEDED ...]`), a
bold or capitalised status token at line or table-cell start (`**OPEN**`, `| OPEN |`, `DESIGN`,
`AS-BUILT`, `PENDING`, `NOT BUILT`), a `Status:` field, or a checkbox; case-sensitive; the skill's
loose regex survives only under `--loose`. **Per row it prints:** `file:line | kind (LIVING /
POINT-IN-TIME / ARCHIVE, from the filename convention and the banner) | label | every backticked
token on the line with its own resolve state (exists / moved / gone / drifted-content) | date on the
line, if any | running-total? (a count pattern)`. The subordinate-fact column is the mechanical
half: a `path:line` beyond `wc -l`, a symbol with zero hits, a hash `git cat-file -e` cannot find,
a `N of M` — the class §2.3 found rotting under true labels.

**Change (b) — the hand check, bounded.** Step 0.5 is rewritten: the verdict column is filled BY
HAND for the census's rows — tens, not thousands — with the skill's existing spelling `STILL OPEN` /
`ACTUALLY DONE` / `PARTIAL` plus `STALE DONE` (false optimism), and an action per verdict bounded by
the row's `kind`: a LIVING doc is rewritten (WP-2), a POINT-IN-TIME doc is stamped and never
rewritten (`docs/README.md`'s convention), an ARCHIVE row is left. `STALE DONE` = downgrade the tag,
date it, cite the evidence, stamp. Step 1's *"read each doc"* and Step 0.5(5)'s per-row lesson grep
are rewritten to the census's rows too (H-3) — one regime, not two — and the frontmatter
description drops *"Update ALL project docs"* (L-2). The Step 5 ledger IS the census output with the
verdict column, pasted.

**Change (c) — the machine trailer.** The script writes ONE line into the documentize commit body:

```
Docs-Census: base=<sha> rows=N labels=L still-open=a actually-done=b stale-done=c partial=d
             cited-dead=e accretion=f ro-bytes=g ro-longest=h mem-over200=i sweep-cursor=j
```

Every acceptance in this doc is a grep over that trailer, so a run that did not write it is a run
that did not close (H-7), and the untracked files' numbers get a history through the commits that
carry them.

**Change (d) — the private history.** `status_census.py --snapshot` copies `CLAUDE.md` and
`MEMORY.md` into a LOCAL-ONLY repository (`~/.claude/projects/<slug>/history/`, `git init`, no
remote — the `research/` pattern from `docs/DOCS_ARC.md`) and commits them with the same trailer.
Nothing is published, nothing changes how `CLAUDE.md` loads, and the next census can diff the two
files that hold most of the rot (D17). This is what the first draft's WP-5 was for.

**What is dropped, named.** The user's 2026-06-21 rule (`memory/feedback_documentize_manual_
status_reconciliation.md`) says *"Enumerate every status marker (grep the tree ...)"* per run. The
PER-RUN tree-wide enumeration is replaced by blast radius + amortised sweep; the MANUAL item-by-item
verdict survives, on a bounded list; the tree still reaches a verdict within N runs. The memory file
and `MEMORY.md`'s Standing RULES line are rewritten in the same commit (RULE 2 — no two texts of one
rule). **§5, question 1 puts this to the user.**

**Mechanism.** The set to check is computed, bounded and printed; "reconciled" means every row has a
verdict, checkable by reading the list; the numbers live in commit trailers and a private history.

**Cost.** One ~300-line script with a drill (the `lessons_gate_drill.py` pattern: shown RED on a
synthetic tree before trusted green); a census per run instead of a claim.

**Acceptance.** Every close commit carries the trailer (grep); `sweep-cursor` advances every run;
the re-take of §2.3 at run 10 is a `--sweep` CENSUS, not a sample.

### WP-4 — Extend the ledger gate to what it does not cover (ships on the green light)

**Defect.** `tools/docs/lessons_gate.py` already proves every `file:line`, every quoted citation and
every backticked symbol in `docs/LESSONS.md` (D4b) — but not `[[wikilinks]]` (D4's one dead ref is
one), not the memory↔ledger pairing (40 unpaired files, by eye), not running totals (L2). `[V]` Over
the 620 rows: 125 (20 %) carry a `path:line`, 277 (45 %) a code path, 444 (72 %) a backticked
identifier, 500 a `memory/` link, 126 (20 %) nothing checkable (pass 2b).

**Change.** Three checks added to the existing gate, drilled RED first in `lessons_gate_drill.py`:
C = every `[[slug]]` resolves to a memory file under either naming convention; D = `--pairing`, the
two-set diff of Step 3.5 printed as lists (unreferenced files / dead references); E = a running-total
pattern (`\b\d+ (rows|files|findings|of \d+)\b` in a row body) is listed as a WARN with the row's
date, never PASS-silent. Step 3.5's "diff the two sets" becomes `lessons_gate.py --pairing`.

**Cost.** ~80 lines in an existing file; no new mandate.

**Acceptance.** The gate stays green in CI with the three checks on; the pairing list is empty or
each entry is a named exception in `lessons_gate_allow.txt`.

### WP-2 — Corrections in living docs replace, with one machine-distinct stamp (ships on the green light)

**Defect.** Five sites in `CLAUDE.md` keep the wrong claim standing beside its correction (D3), the
ledger names the shape (L1), and the skill's guardrail forbids it in prose nothing enforces. The
first draft's detector would have counted its own remedy as debt (§8, C-1), and it collided with the
user's same-day rule *"refute via supersede-stamp"* (`CLAUDE.md`, RULE 2026-09-02) and with living
docs that carry DATED sections (`docs/vehicles/ATV.md` §16, `DEATH_ARC`, `MULTIPLAYER_UI` §8a/§8b)
where the stamp is right (H-5).

**Change.** ONE vocabulary: the supersede stamp IS the correction line, in a machine-distinct form —
`[corr YYYY-MM-DD: was <≤120 chars>; measured <...>; <cite>]` — on the same line as the rewritten
claim. Two section kinds inside a living doc: a HEADLINE / current-state section is REWRITTEN and
stamped; a DATED log section (`§N (date)`) is STAMPED and kept. LIVING = the D12 banner census +
`CLAUDE.md` + `MEMORY.md` + `*_ARC.md`; `-DESIGN-` and `_archive/` never. The WHY of a correction —
the lesson — goes to a `LESSONS.md` row / memory file through the Step 3.5 pairing, and the stamp
links it (`[[slug]]`), so RULE 2 deletes no evidence. **The detector** is a column of WP-1's script
(`accretion=`): it counts the LEGACY phrasings (`is FALSE`, `was wrong`, `stood here`, `said the
opposite`, `the previous line here`, `no longer says`) and any `[corr ...]` whose `was` clause exceeds
120 characters (a sentence kept, not a note) — excluding `LESSONS.md`, `_archive/`, and any doc whose
banner says it defines the vocabulary (L9; this doc included); `no longer` alone is ordinary English
(`"A2 IS NO LONGER OPEN"` is a status) and is not counted. The five existing sites are the first
debt; the trailer's `accretion=` must not rise, and it falls as they are paid.

**Mechanism.** The compliant form and the debt form are grep-distinct, so the count means one thing;
the WHY has a destination, so replacing loses nothing.

**Acceptance.** `accretion=` in consecutive trailers is non-increasing; zero legacy phrasings in
the diff of any close commit.

### WP-3 — A ratchet on what a session loads (ships on the green light)

**Defect.** Every session starts by loading 171 KB — 68.6k tokens, 6.9 % of the window — of which
117 KB is a reading order of 54 entries whose longest is 275 lines (D2, D13), plus an index that
breaks its own ≤200-char rule on 35 of 75 lines (D2b). Step 4 adds to the reading order when a doc
is created; nothing ever shortens it. The first draft proposed a threshold gate at half of today —
which would have been RED BY DESIGN for the whole pay-down, the state the ledger's own L5 says carries
no signal, and a refusal on the user's file (§8, C-2, M-7).

**Change.** The trailer carries `ro-bytes=` (the reading order from its heading at `CLAUDE.md:461` to
EOF), `ro-longest=` (lines of the longest entry, entries delimited by the `^<n><letters>. ` pattern
INSIDE the section) and `mem-over200=`. **The gate is a RATCHET: red only if any of the three GREW
since the previous trailer.** Green on day one, red exactly when the mandate is violated, no policy
number needed to ship. The target — reading order ≤ 58 KB (half of 117), no entry over 15 lines,
`mem-over200=0` — is a printed line until first reached, after which it becomes the ceiling.
**Shorten = MOVE-THEN-CUT (M-6):** before a line leaves an entry, the script greps the destination
for the fact's key token and records `moved-to:` in the trailer; lines carrying `USER` + `verbatim`
are exempt from any cut. The first four shrinks, with destinations: `4e` (275 lines → the pointer,
the pipeline, the mixed-ownership rule, the current front, the two product questions, NEXT; the 25
session digests s17..s30b → `memory/project_s2*.md`, where they already live), `4e-browser` (152 →
`docs/SERVER_BROWSER_ARC.md` §9), `4a-identity` (87 → `docs/security/PLAN_01_PEER_AUTH.md` §0a),
`1a-veh` (73 → `docs/vehicles/ATV.md` §17). One per run, by trailer delta.

**Mechanism.** A ratchet refuses only growth, so it never blocks a session close for a debt it did
not create, and the target is visible every run without being a red light.

**Cost.** Three trailer columns and one comparison.

**Acceptance.** No close commit's trailer shows a larger `ro-bytes` / `ro-longest` / `mem-over200`
than its predecessor; `/context` "Memory files" at session start is re-read at run 10 (the harness's
figure, recorded in §8 by hand).

## 4. Kept as-is, and what was looked at and declined

**Kept:** the two-write lesson rule and its pairing diff (D4 — now a script mode, WP-4); the CI ledger
gate (D4b) and its drill pattern, which WP-1's script copies; the `[V]`/`[RD]`/`[?]` tags; "never
VERIFIED from a smoke alone"; the memory topic file + index line; the `MEMORY.md` compaction rule
(it has run x11); Step 4's pointer check (D5b holds); the ~8-runs-per-active-day cadence — a session
close should be cheap and frequent, which is exactly why its mandate must be small; `CLAUDE.md`
private and untracked (the user's decision, and `DOCS_ARC`'s scrub depends on it).

**Declined (so it is not re-derived):** a tracked public `docs/READING_ORDER.md` (C-3: it would
re-publish scrubbed security material; and a prose pointer loads nothing while an `@` import saves
nothing — H-6); a threshold size gate (C-2: red by design); a separate `ledger_rot.py` (F1: the gate
exists); a correction detector on the raw vocabulary (C-1: it counts its own remedy); splitting the
skill into a close-out skill and a sweep skill (the amortised sweep is the same thing without a
second file); auto-compacting `MEMORY.md` by script (the index is prose the user reads; the ratchet
is enough); tracking `CLAUDE.md` whole in the public repo.

## 5. Open questions for the user

1. **WP-1 changes the FORM of your 2026-06-21 rule.** The per-run tree-wide enumeration becomes
   "the session's blast radius + the K oldest-censused docs every run", so every doc still reaches a
   hand verdict within N runs, and the item-by-item MANUAL verdict survives on a bounded list. The
   memory file that records your rule would be rewritten to say so. Agree?
2. **WP-3 runs a script on your file** (`CLAUDE.md` is your per-user notebook by your decision). It
   MEASURES and REPORTS each run and refuses a close only on GROWTH of the reading order; the shrink
   target (half of today) is a printed line, not a red light. Agree, and is half the right target?

## 6. Risks named in advance

- **WP-1 could still miss cross-doc rot** (§2.3 #7). The amortised sweep reaches the sibling doc
  within N runs; a cited-symbol column catches the case where the two docs share a token; a pure
  prose contradiction between two docs is caught by no per-line instrument and is said so here.
- **The label grammar could miss a label written in prose.** `--loose` keeps the old regex for a
  `--sweep`; the census prints both counts (`rows=` hits, `labels=` labels).
- **WP-3 could push a fact out of the reading order that a session needed.** Move-then-cut greps the
  destination first; user-verbatim lines never move; the entry keeps the facts a session must know
  before opening the doc.
- **The private history (WP-1d) is one more repository on this box.** No remote, no publication;
  its only consumer is the next census's diff.

## 7. Risks to the doc's own numbers

- D1 counts hits; the label population is several times smaller (two samples disagree on the
  factor: 29 % vs ~80 % prose, different strata).
- D9's cadence counts commits; the arc's own commits are excluded by name, and a run that committed
  twice or not at all moves it either way.
- D3's "~5 accretion sites" is one reader's classification of 25 lines; the trailer column will
  count with the grammar in WP-2, and the two numbers need not agree.
- The `/context` figure (D2) is the harness's and is re-read by hand, not re-derived.

## 8. Audit log

Three passes on 2026-09-02, all on the committed draft `bf55de69`..`757b1dba`: an independent
STALENESS measurement (28 sampled labels → §2.3), an independent EVIDENCE re-derivation (pass 2a, 14
findings) and an independent DESIGN audit (pass 2b, 19 findings: 3 CRITICAL / 7 HIGH / 7 MEDIUM /
2 LOW). Every finding is dispositioned; nothing is OPEN. The three critical findings and F1 each
caught a defect in the doc's own method, and they are kept in the text on purpose.

### Pass 2a — evidence

| id | Finding | Disposition |
|---|---|---|
| F1 | WP-4 proposed `ledger_rot.py` for checks that `tools/docs/lessons_gate.py` (412 LOC, CI since `99445efb`) already performs — file:line, QUOTED-content drift (A2), symbols; the L3 row the doc quoted names the gate in its second sentence; "the ONE mechanical step" is two | FOLDED — D4b added; WP-4 is now an EXTENSION (wikilinks, pairing, running totals); §2.5 corrected; the miss is recorded in §0 and L3 |
| F2 | D6b "203 of 279" counted LINES; per commit it is 148 of 281 (53 %) | FOLDED |
| F3 | D13 "68 entries" was the whole-file count (8 principles + 6 checklist items); the reading order has 54; by bytes it is 79.5 % | FOLDED |
| F4 | D3 "27 lines of accretion": 25 lines / 29 hits; ~5 accretion, 5 already the dated-rewrite shape, 6 narrations of other docs' corrections, 3 false positives; the detector would count its own remedy | FOLDED (D3 rewritten; WP-2 redesigned with C-1) |
| F5 | D9 "~10/day" holds only for the last five days; 8.1 per active day, 6.9 per calendar day; one 56-file sweep commit exists (`40b1512e`) | FOLDED |
| F6 | The `--grep=documentize` instrument counts this arc's own commits (`DOCUMENTIZE_ARC` in the message) | FOLDED — 279 stated as pre-arc; Appendix A excludes the name |
| F7 | `.gitignore:250-266` is `:253-266` | FOLDED — cited by the block's header string (L-1) |
| F8 | D5b "2 of 95" was pattern-dependent; a broad extraction gives 144 / 10, mostly by design; two VotvIO pointers, not one | FOLDED |
| F9 | `_archive` = 32 files over four dirs (26 excluded `research/_archive`); the 433 deletes = `cf3780d2` | FOLDED |
| F10 | §1 attribution slips: the CLAIM sentence is the preamble; "read each doc" is Step 1; Step 4 adds "if one was created"; `MEMORY.md` compaction IS ordered | FOLDED (§1 rewritten with the quotes in their places) |
| F11 | "no consumer reads it whole (E10)" — E10 says the read is unverified | FOLDED |
| F12 | L4 is the doc's analogy, not the row's claim | FOLDED (marked as such) |
| F13 | The project slug in Appendix A is not a user name and is already public in `QF_ARC` | none (placeholder used anyway, L-1) |
| F14 | Drift values (D1 11,315; D20 17.46 MB; D2 172.1 KB; D2b 36/76; D7 1,056 / 70 links) — the instruments count the arc doc itself | noted in §2 (the arc's own lines) |

### Pass 2b — design

| # | Finding | Disposition |
|---|---|---|
| C-1 | WP-2's detector counts its own compliant form as debt; "zero new lines in the diffs" is undefined for an untracked file | FOLDED — machine-distinct `[corr ...]` stamp; legacy-phrasing count; exclusions (L9); the count lives in the commit trailer |
| C-2 | WP-3's threshold gate is red by design through the pay-down (L5) and a blocking refusal on the user's file | FOLDED — a ratchet on growth; the target is a printed line; §5.2 asks about the file |
| C-3 | WP-5 would publish reading-order content that `DOCS_ARC` scrubbed from public docs (exploit-map counts, the GNS default, the plaintext leg) | FOLDED — WP-5 dissolved into WP-1(d), a LOCAL-ONLY history; declined in §4 |
| H-1 | WP-1 drops the user's tree-wide per-run requirement without naming it; the memory file keeps the old text | FOLDED — amortised sweep keeps tree-wide coverage; the change of form is named and asked (§5.1); the memory file is rewritten in the same commit |
| H-2 | The blast radius had no diff base, no symbol rule, no docs-only case | FOLDED — trailer-tiled base; symbol rule; `radius: docs-only`; `--staged` deleted |
| H-3 | Step 1's "read each doc" and Step 0.5(5)'s per-row lesson grep would survive beside the census | FOLDED — both rewritten to the census's rows; acceptance `grep -c 'read each doc' = 0` |
| H-4 | WP-3's arithmetic (half of today was 59 %); D13's 68 | FOLDED — bytes stated (117,233 → target 58 KB); 54 entries |
| H-5 | WP-2 vs the user's supersede-stamp rule and dated sections; LIVING undefined; the WHY had no destination | FOLDED — one vocabulary; two section kinds; LIVING defined; the WHY goes to the ledger through the pairing |
| H-6 | A prose pointer loads nothing; an `@` import saves nothing | FOLDED — WP-5 no longer touches loading |
| H-7 | Acceptances rested on `--grep=documentize` commits and a 28-item sample | FOLDED — the machine trailer; the re-take is a `--sweep` census |
| M-1 | ~80 % of raw hits are prose; a label grammar is needed | FOLDED |
| M-2 | WP-4's dry run supplied (20 % / 45 % / 72 % / 20 %); content drift is A2's job | FOLDED |
| M-3 | No cut criterion; ranking preceded §2.3; "ten times a day" | FOLDED — criterion stated; re-ranked on §2.3; measured cadence |
| M-4 | "Ships now" was conditional; per-run prose chores | FOLDED — one script, one trailer; deltas, not chores |
| M-5 | Verdict spelling vs the skill; `STALE DONE` had no action; `-DESIGN-` files never rewritten | FOLDED — the skill's spelling; action per verdict; `kind` column |
| M-6 | "Shorten" undefined; user-verbatim lines unprotected | FOLDED — move-then-cut; exemptions; the four shrinks with destinations |
| M-7 | A refusal on the user's file was never asked | FOLDED — §5.2 |
| L-1 | Slug placeholder; the docs-arc note is cited by name in five tracked files after a de-link; `.gitignore` range | FOLDED — placeholder; "the docs-arc note"; header string |
| L-2 | "every run's report" (it was 53 %); "nothing shortens it" (the index IS compacted); the frontmatter description | FOLDED |

### Staleness measurement — the agent's own summary

28 labels, 14 open-ish / 14 done-ish; 5 stale-open of 14 (4 of 8 true labels), 0 stale-done, 1
undecidable; 8 of 28 lines rot in a subordinate fact; false-open clusters in 2026-06/07
point-in-time docs, every 2026-08 line accurate, one cross-doc miss; 29 % of hits are not labels.
Scratch: `stale_sample/` in this session's scratchpad.

## 9. Build order

0. `tools/docs/status_census.py` with its drill (RED on a synthetic tree first): the label grammar,
   the radius, the subordinate-fact column, the trailer, `--snapshot`, `--sweep`, `--loose`.
1. WP-1 text: Step 0.5, Step 1's enumeration, Step 0.5(5), the scope statement, the frontmatter, the
   Step 5 ledger; the memory file + `MEMORY.md` line for the 2026-06-21 rule rewritten (after §5.1).
2. WP-4: the three checks in `lessons_gate.py`, drilled RED first; Step 3.5 points at `--pairing`.
3. WP-2 text + the `accretion=` column; the five debts, one per run.
4. WP-3: the three size columns + the ratchet; the first shrink (`4e`) by move-then-cut.
5. **The first census**: ten close commits later, the trailers are read back (`ro-bytes`,
   `accretion`, `sweep-cursor`, verdict counts), `/context` is re-read by hand, and a `--sweep` census
   replaces §2.3's sample. Recorded here.

Each step is one commit, `[docs]`/`[tools]` prefixed; nothing ships before the user answers §5.

---

## Appendix A — the instruments (re-runnable; Git Bash on this box)

```
M=$HOME/.claude/projects/<project-slug>/memory
RX='OPEN|FUTURE|TODO|PENDING|NEXT|not (yet )?(built|wired|implemented|done|verified)|deferred|unverified|\[ \]|\[\?\]|planned|stub|placeholder'
# D1 (markdown only; research/ holds non-markdown bulk that times a raw grep out)
grep -rniE --include='*.md' "$RX" docs/ | wc -l; grep -rniE --include='*.md' "$RX" research/findings/ | wc -l; grep -niE "$RX" $M/*.md | wc -l
# D20
cat $(find docs -name '*.md') | wc -c; cat $(find research/findings -name '*.md') | wc -c; cat $M/*.md | wc -c
# D9 / D6b (exclude this arc's own commits by name; per-commit body test)
git log -i --grep=documentize --format=%H | while read h; do git log -1 --format=%s $h | grep -q DOCUMENTIZE_ARC || echo $h; done > /tmp/runs.txt; wc -l < /tmp/runs.txt
git log -i --grep=documentize --shortstat --format= | awk '/files? changed/{f+=$1;n++;i+=$4} END{print n, f/n, i/n}'
git log -i --grep=documentize --format='%ad' --date=short --since=2026-08-19 | sort | uniq -c
n=0; while read h; do git log -1 --format=%B $h | grep -qiE reconcil && n=$((n+1)); done < /tmp/runs.txt; echo $n
# D4 pairing (either slug convention) -- becomes lessons_gate.py --pairing under WP-4
python - "$M" <<'PY'
import os,re,sys,io
M=sys.argv[1]; led=io.open('docs/LESSONS.md',encoding='utf-8').read().replace('_','-')
files=[f[:-3] for f in os.listdir(M) if re.match(r'^(lesson|feedback)[_-]',f)]
print(len(files), sum(1 for f in files if f.replace('_','-') not in led))
PY
# D4b
python tools/docs/lessons_gate.py; git log --format='%h %ad' --date=short -- tools/docs/lessons_gate.py | tail -1
# D8
git log -i --grep=documentize --diff-filter=RA --name-status --format= | grep -c _archive; find docs research -path '*_archive*' -name '*.md' | wc -l
# D2 / D13 (delimited by the heading) / D2b / D3
wc -lc CLAUDE.md $M/MEMORY.md; awk 'length>200' $M/MEMORY.md | wc -l
S=$(grep -n 'Reading order after a session reset' CLAUDE.md | cut -d: -f1); tail -n +$S CLAUDE.md | wc -lc
tail -n +$S CLAUDE.md | awk '/^[0-9]+[a-z-]*\. /{if(lbl!="")print n, lbl; lbl=substr($0,1,60); n=0} {n++} END{print n, lbl}' | sort -rn | head -6
grep -o -iE "CORRECTED|was wrong|is FALSE|SUPERSEDED|no longer|said the opposite|stood here" CLAUDE.md | tr 'A-Z' 'a-z' | sort | uniq -c
# D6
for d in 2026-08-01 2026-09-01; do r=$(git rev-list -1 --before="$d 00:00" main -- docs/LESSONS.md); git show $r:docs/LESSONS.md | wc -c; done
git log -i --grep=documentize --numstat --format= -- docs/LESSONS.md | awk -F'\t' '$1 ~ /^[0-9]+$/ {a+=$1;d+=$2;n++} END{print a,d,n}'
# D7 / D12 / D17
ls $M/*.md | wc -l; cat $M/*.md | wc -c; grep -o -E '\[\[[^]]+\]\]|\([a-z0-9_.-]+\.md\)' $M/MEMORY.md | sort -u | wc -l
for f in $(grep -l -iE "LIVE doc|LIVING doc|living arc|living document" docs/*.md docs/*/*.md); do echo "$(git log -1 --format=%ad --date=short -- $f) $f"; done | sort
git check-ignore -v CLAUDE.md
```
