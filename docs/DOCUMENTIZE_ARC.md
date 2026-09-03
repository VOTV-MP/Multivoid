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
from the previous memory topic file. **Symbols of the diff (corrected by round 2, Q2):** touched
paths as PATH citations (`session_runtime.cpp`, never the bare basename — `harness` alone is cited
by 117 docs); function/class names from `git diff -U0` hunk headers and from definitions added or
removed, **kept only if SPECIFIC — cited by at most 5 docs**; commit hashes in the range. Generic
symbols are printed as dropped, with their counts, so the cut is visible. `[V]` measured on the
newest source commit (`fff4032b`): 15 hunk-header symbols; `OnDisconnect` is cited by 70 docs,
`Snapshot` 47, `Complete` 34, `Reset` 31; radius (ii) is **148 docs uncut and 6 docs with the cut**
— "tens, not thousands" is true only under the cut, and the census prints the radius size every
run so the claim is measured, not carried. A docs-only diff has radius (i) only and prints
`radius: docs-only`. **Radius:** (i) the docs touched by the session — **enumerated PER TREE
(round 3, Q1), and the tree list is not a list (round 5, Q1): every path the census READS owes a
history source, and the source is decided PER PATH by git's own tracking state, never by a
directory name.** The READ SET is computed: every `*.md` the main repo tracks (`[V]` 166 — 135
under `docs/`, 31 outside it: `tools/` 14, `reference/` 6, `.claude/` 3, `assets/` 2, five at the
root, one under `src/`), every `*.md` under `docs/` whether tracked or not, `research/findings/`
(298), the memory directory (1,063) and `CLAUDE.md`. Per path: tracked by main → `git diff <base>`;
tracked by the inner `research/` repo (`[V]` nested and ignored, `.gitignore:283`; own HEAD, no
remote) → `git -C research diff <its base>`, and it gets its own close commit, see (c); tracked by
NEITHER → the diff of the private history (d), whose snapshot set is therefore COMPUTED as the read
set minus what any repository tracks — today `CLAUDE.md` (ignored, `:113`), the whole memory
directory (`[V]` 6.10 MB of text, 2.50 MB gzipped, delta-compressed after the first snapshot), and
**20 ignored `*.md` under `docs/`: the 14 of `docs/security/` (`.gitignore:301`; the four security
lines §2.3 sampled, #13, #14, #27, #28, live there) and six more — `AGENT_SPAWNING.md`, `DOCS_ARC.md`,
`SERVER_BROWSER_ARC.md`, the three `QUESTION_FORM_*`** — which round 3's list of three trees would have
left with no history at all. Ignored `*.md` OUTSIDE `docs/` (`[V]` three: `SUPPORT.md`, two
`reference_*_vps.md`) are outside the read set and get none; the census prints the read set's size
per tree every run. (ii)
every doc citing a specific symbol or path of the diff; (iii) **the amortised sweep** — the K docs
whose last census is oldest. **The arithmetic, restated on the computed read set (rounds 3 and 5):**
`[V]` 166 + 20 + 1 + 298 + 1,063 = 1,548 files; at K = 10 a full cycle is 155 closes (~19 active
days at 8.1 closes a day); **K = 40, so a full cycle is 39 closes (~5 active days)**, and the trailer
prints `sweep-cycle=` beside `sweep-cursor=`. Each doc's last-census commit is per-doc STATE, which a scalar trailer cannot hold:
it lives in `docs_census_state.json` inside the private history repo, committed with every
snapshot. `--sweep` = the full pass, on the user's request.
**Label grammar (M-1, corrected by the `/qf` pass round 1 — §8 pass 3):** a LABEL is a STATUS
tag or token — `[?]`, `[SUPERSEDED ...]`, a bold or capitalised status word at line or table-cell
start (`**OPEN**`, `| OPEN |`, `DESIGN`, `AS-BUILT`, `PENDING`, `NOT BUILT`, `DONE`), a `Status:`
field, a checkbox, **or a heading carrying `Open questions` / `OPEN` / `TODO` / `NEXT` / `Pending`**
(§2.3 #9, #12 were headings); case-sensitive; the skill's loose regex survives only under
`--loose`. **`[V]`, `[A]`, `[RD]` are PROVENANCE tags, not status, and are NOT labels** — `[V]`
`docs/LESSONS.md` alone carries 184 of them and is in the radius of 181 of 283 close commits, so
counting them would flood every run with rows the ledger gate already owns. **A SUB-STATE column
is mandatory:** the parenthetical or trailing clause on the label line AND the next line, matching
`pending|uncommitted|not yet|hands-on|TODO|commit pending` — `[V]` in §2.3 both label rows the
first grammar would have produced (#1 `**Status:** ... IMPLEMENTED` and #20 `**A2 — DONE**`) rotted
in exactly that clause (`(uncommitted; ... hands-on-pending)`, `(commit pending)`), outside the label
and outside every other column. **Per row it prints:** `file:line | kind | label | sub-state | every
backticked token on the line with its own resolve state (exists / moved / gone / drifted-content) |
date on the line, if any | running-total? (a count pattern)`. The subordinate-fact column is the
mechanical half: a `path:line` beyond `wc -l`, a symbol with zero hits, a hash `git cat-file -e`
cannot find, a `N of M` — the class §2.3 found rotting under true labels. **The drill asserts RECALL
and PRECISION on a FIXTURE of real lines, not a synthetic RED alone** (the ledger: *"an instrument's
self-test must assert precision as well as recall"*): the six stale-open lines and the eight
vocabulary false positives of §2.3 are the fixture; the grammar as first drafted scored 2 of 6 on
recall and 0 of 2 on the sub-state, which is the number the drill exists to keep from regressing.

**Change (b) — the hand check, bounded.** Step 0.5 is rewritten: the verdict column is filled BY
HAND for the census's rows — tens, not thousands — with the skill's existing spelling `STILL OPEN` /
`ACTUALLY DONE` / `PARTIAL` plus `STALE DONE` (false optimism), and an action per verdict bounded by
the row's `kind`: a LIVING doc is rewritten (WP-2), a POINT-IN-TIME doc is stamped and never
rewritten (`docs/README.md`'s convention), an ARCHIVE row is left. **`kind` is decided by the path
pattern, and each kind has ONE action (corrected twice by the `/qf` pass — round 1 Q3 replaced a
banner list with a dated-filename invariant; round 2 Q4 showed that invariant would have frozen 484
files the conventions say to update):**

| kind | path pattern | count today `[V]` | action on a stale row |
|---|---|---|---|
| LIVING | undated filename outside `_archive/` — `CLAUDE.md`, `MEMORY.md`, every `*_ARC.md`, the maps, the trackers | 99 in `docs/` | REWRITE the claim + one `[corr ...]` stamp; a DATED SECTION inside it (`§N (date)`) is stamped and kept |
| DURABLE RECORD | `-RE-<date>` and the other dated findings, runbooks, FACTS, STUDY files under `docs/` and `research/findings/` | 105 + 147 | the user's 2026-09-02 per-claim rule (`CLAUDE.md`, RULE 2026-09-02): a fact CONFIRMED is REFRESHED in place — `[V]` with the instrument named; a fact REFUTED gets a supersede-stamp and stays |
| DESIGN | `-DESIGN-<date>` | 69 | STAMP only — *"deliberately never rewritten"* (`docs/README.md:161-163`) |
| MEMORY TOPIC | `memory/project_*` and `project-*`, dated OR undated (the date is the session, not a freeze) | 356 (298 + 58; `[V]` 174 dated / 124 undated among the underscored) | UPDATE in place, delete if wrong — the skill's own Step 3 (`SKILL.md:136`) |
| MEMORY LESSON | `memory/lesson_*`, `feedback_*`, `reference_*` | 588 + 102 + 10 | SHARPEN in place (Step 3.5(3)), never `[corr]`-stamped (out of WP-2's scope); their citations are `lessons_gate`'s |
| ARCHIVE | `_archive/` | 32 | none |

**And one action is VERDICT-driven, layered on the kind (round 3, Q3):** when every open row of a
LIVING planning doc is verdicted `ACTUALLY DONE`, the action is not a rewrite — it is the skill's own
*"MOVE the planning doc/section to `_archive/`"* with a one-line pointer (`SKILL.md:65`), the action
D8 measured at 8 moves in 279 commits. `[V]` §2.3 #1 (`AUTHORITATIVE_INTERACTABLE_MIGRATION.md`, a
planning doc whose phases shipped in June) is exactly that case, and a kind table alone would have
sent it to a rewrite. `[V]` the banner census would have classed 101 of 136 non-archive docs as
neither living nor point-in-time, the three maps among them; the six stale-open lines of §2.3 fall
into LIVING (4, one of them the archive case) and DESIGN / DURABLE (2), the right action in every
case. `STALE DONE` = downgrade the tag,
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
carry them. **Who writes it (corrected by round 2, Q3):** `[V]` nothing exists today — no hook,
`core.hooksPath` unset, no CI step reads `git log`, `Docs-Census` appears in no commit — so as
first drafted the trailer would have been READ off the script by the agent and PASTED, the
honor-system shape (*"a check whose output you do not read is not a check"*). Instead **the script
performs the close commit**: `status_census.py close -m "<subject>" --trailer "Co-Authored-By: …"
--trailer "Claude-Session: …"` runs the census, evaluates the ratchet, and on green runs
`git commit` itself with the trailers appended through `git interpret-trailers` (git 2.52 on this
box; `[V]` it appends three trailers in the order given); red = exit 1 = no commit. **The
attribution trailers are REQUIRED inputs, never minted (round 5, Q4):** `[V]` 40 of the last 40
commits carry both `Co-Authored-By:` and `Claude-Session:` by hand, 614 commits carry the session
URL, and the model name has ELEVEN spellings in history (`Claude Fable 5`, `Fable 5.1`, `Opus 4.7`
… `Pelmentor`), so neither value is the script's to know — it REFUSES to commit without both, the
user's *"the trailer STAYS"* rule enforced as a refusal, and writes the same three on all three
commits of a close (main, `research/`, the private history). The CI gate checks `Co-Authored-By:`
beside `Docs-Census:` on every close commit. And **a CI gate observes
it outside the run**: `tools/docs/docs_census_gate.py` reads `git log --format=%B` for every close
commit in the push and fails on a missing trailer or a ratchet column that grew against the
previous trailer — history is all it needs, so the untracked files' numbers are checked by a
machine that never sees the files. **Registration, not mention (round 3, Q4):** the gate must know
which commits OWE a trailer, and "documentize in the subject" is the MENTION this doc's own audit
rejected (H-7; `[V]` 12 of the last 40 subjects carry the word). The script writes every close
commit with the fixed subject prefix **`[docs] close:`** — the registration — and the gate fails
three shapes: a prefixed commit without a trailer, a trailer without the prefix, and **a subject
that STARTS with the retired close form `[docs] documentize`** (one close path, RULE 2) — never "the
word documentize anywhere", which would make an arc-doc edit like `[docs] DOCUMENTIZE_ARC:` a
false close. **The gate's range starts at a REGISTERED boundary (round 4, Q1), and the boundary is COMPUTED,
not written (round 5, Q3):** a commit cannot carry its own hash, so a `since` sha in the workflow
would be either the parent, minted by hand, or a second commit — the shape the ledger's *"a
file-hash gate can only be minted where it is checked"* row forbids. Instead the gate finds the
commit that ADDED its own workflow file, `git log --diff-filter=A --format=%H --
.github/workflows/docs-census.yml | tail -1`, judges `<that>..HEAD`, and prints the boundary sha it
computed on every run (`[V]` the same command on `build-core.yml` returns `47b88116`, 2026-07-25;
the workflow checks out with `fetch-depth: 0` as `build-core.yml:57` already does). History before it
is not judged — `[V]` the unpushed range alone holds 10 subjects with the word (7 arc-doc edits + 3
old-form closes) and all history holds 249 old-form closes and 0 prefixed ones, so a gate with no
boundary would be red by construction on the push that lands it, the L5 shape this doc cites
against its own first draft. A later move of the workflow file moves the boundary with it, on
purpose and visibly. **The inner repository gets its own close (round 4, Q2):** `research/`
is a repository the main commit cannot carry (`[V]` 57 uncommitted paths there today, 16 under
`findings/`, 54 commits, no remote), so `close` also runs `git -C research commit` with the same
prefix and the same trailer, censuses `research/`'s INDEX like main's, refuses unstaged docs in
radius there too, and the main trailer records `research-base=<sha>`. **Index hygiene on a shared
box (round 4, Q3):** `close` never commits with a pathspec (`[V]` `LESSONS.md:145` — the pathspec
form discards the index) and never blindly (`[V]` `:6978` — a no-pathspec commit once swallowed
another session's ten staged paths): before committing it lists the index and REFUSES, printing
them, any staged path the close does not own. **What a close owns is not two directories, it is
CLAIMS (round 5, Q2):** `[V]` of the 76 closes since 2026-08-24, 24 carried a non-markdown path
outside `docs/` — `.h`/`.cpp` headers whose COMMENTS held a stale claim (the skill's Step 1 reaches
a claim wherever it lives: `multiplayer_menu.h`, `ko_respawn.h`, `world_identity.h` twice), `tools/`
and `reference/` READMEs, and eight that carried real code (`dead_api_census.py` 151 lines,
`compare_zips.ps1` 111, `mp.py`, `trash_proxy.cpp`, `.gitignore` …). So the owned set is computed
per staged path: (1) any `*.md` the repository tracks, anywhere (`[V]` 31 of 166 live outside
`docs/`); (2) the close's own instruments, `tools/docs/**` and `.claude/skills/**`; (3) any other
path whose staged hunks are COMMENT-ONLY — the code residue of the `-` lines (comments stripped,
whitespace collapsed) equals that of the `+` lines, which admits a trailing `// …` rewrite on an
unchanged declaration (`[V]` the rule re-run over the 24: 16 green, 8 refuse, and all 8 are the
code cases — a tool shipped inside a close, which the ledger already names as the failure). A
refused path is committed FIRST in its own `[tools]`/`[src]` commit; there is no `--also` flag,
because an override recorded as a count is still an override nobody reads back. The other
session's hunks stay theirs, per `docs/CROSS_SESSION.md`'s `git apply --cached` protocol. **Where the gate runs:** in its OWN
workflow file, `.github/workflows/docs-census.yml`, on push — `build-core.yml` is untouched, because
`[V]` `tools/release/fingerprint.json:4` pins `build_core_sha256` and the ledger row *"Editing
`build-core.yml` = do the fingerprint re-commit ritual in the SAME workstream"* records a release run
refused for exactly an added gate step; if the user wants it inside `build-core.yml`, the ritual is
in the same commit. **What the census READS (round 3, Q2):** for tracked docs, the INDEX
(`git show :path`) — what the commit will carry — and `close` REFUSES when a tracked doc in radius
(i) has unstaged changes, printing them (the ledger's *"a green verdict can rest on code that is not
in the commit"* and *"`git commit -- <paths>` ... DISCARDS YOUR INDEX"*, both paid for on this
two-session box); the untracked trees are censused from the snapshot (d), which is what their
history will hold.

**Change (d) — the private history, and where the hand verdicts live.** `status_census.py
--snapshot` copies the COMPUTED snapshot set of (a) — the whole memory directory, `CLAUDE.md`, and
every read-set `*.md` no repository tracks (today the 20 ignored files under `docs/`, `docs/security/`
among them; local-only material stays local: this repository is under the user's profile, never
inside the project) under their relative paths into a LOCAL-ONLY repository
(`~/.claude/projects/<slug>/history/`, `git init`, no remote — the `research/` pattern from the
docs-arc note) and commits them. **Its identity is COPIED, not typed (round 5, Q4):** `git init`
copies `user.name` / `user.email` from the main repo's LOCAL config (`[V]` `pelmentor
<pelmentr@gmail.com>` in main, `research/` and `site/` alike — the *"set the same in any NEW repo"*
rule is a copy by definition) and refuses to initialise when main has none; every later snapshot
commit re-compares the two and refuses on a mismatch. **The per-row HAND VERDICTS have no home with history
today** (`[V]` one verdict word in the last 40 commit bodies; the Step 5 table lives in a chat
report), so `close` writes the full census + verdict table to that repository too
(`census/<utc>-<sha>.md`), commits it with the snapshot and `docs_census_state.json`, and the
trailer carries **`census=<history sha>`** — the counts in the trailer point at the rows they
count. The Step 5 report pastes the same table for the human. Nothing is published, nothing
changes how `CLAUDE.md` loads, and the next census can diff every file that holds the rot (D17).
This is what the first draft's WP-5 was for.

**What is dropped, named.** The user's 2026-06-21 rule (`memory/feedback_documentize_manual_
status_reconciliation.md`) says *"Enumerate every status marker (grep the tree ...)"* per run. The
PER-RUN tree-wide enumeration is replaced by blast radius + amortised sweep; the MANUAL item-by-item
verdict survives, on a bounded list; the tree still reaches a verdict within N runs. The memory file
and `MEMORY.md`'s Standing RULES line are rewritten in the same commit (RULE 2 — no two texts of one
rule). **§5, question 1 puts this to the user.**

**Mechanism.** The set to check is computed, bounded and printed; "reconciled" means every row has a
verdict, checkable by reading the list; the numbers live in commit trailers and a private history.

**Cost.** One ~400-line script and a ~100-line gate with a workflow, each with a drill (the
`lessons_gate_drill.py` pattern: shown RED before trusted green); a census per run instead of a claim.

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

**Where each check can run (corrected by the `/qf` pass round 1 — §8 pass 3, Q4).** `[V]` the gate
finds the memory corpus only at the hardcoded `~/.claude/projects/<slug>/memory` or
`$MULTIVOID_MEMORY_DIR` (`lessons_gate.py:55-59`), `.github/` sets neither, and the gate already
prints *"UNVERIFIABLE here -- absent search root(s)"* (`:329`) and *"CHECK SKIPPED -- corpus absent"*
(`:332`) in that case. So checks C and D can run ONLY at the local close; in CI they print
UNVERIFIABLE, never green. Their results therefore go where the other local numbers go — the
`Docs-Census:` trailer gains `wikilinks-dead=`, `pairing-unref=`, `pairing-dead=` — and the history
of those three columns is how the pairing is observed outside the run that owes it.

**Acceptance.** A/A2/B green in CI as today; C/D/E printed UNVERIFIABLE in CI (not SKIPPED-as-green);
at every local close the three pairing columns are in the trailer, and `pairing-unref` / `pairing-dead`
/ `wikilinks-dead` are non-increasing across consecutive trailers, each remaining entry a named
exception in `lessons_gate_allow.txt`.

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
stamped; a DATED log section (`§N (date)`) is STAMPED and kept. LIVING is the WP-1 invariant (an
undated filename outside `_archive/`), which covers `CLAUDE.md`, `MEMORY.md`, every `*_ARC.md` and
the maps; a dated filename is point-in-time and is only ever stamped. The WHY of a correction — the
lesson — goes to a `LESSONS.md` row / memory file through the Step 3.5 pairing, and the stamp links
it (`[[slug]]`), so RULE 2 deletes no evidence. **The detector (corrected by the `/qf` pass round 1
— §8 pass 3, Q2):** `[V]` no measurement ever showed a compliant "CORRECTED <date>" rewrite and an
accretion "CORRECTED <date>" beside a standing claim to be grep-distinct today, because they are
the same string; of the five accretion sites D3 named, only two carry a phrase on the first draft's
"legacy" list and the other three carry `NO LONGER` / `CORRECTED`, the vocabulary that list
excluded — so `accretion=` would have read 2 and paying three of the five would have moved it by
nothing. The distinction is STRUCTURAL, and it exists only after the token does: the column counts
**every line carrying correction vocabulary** (`corrected`, `superseded`, `is false`, `was wrong`,
`stood here`, `said the opposite`, `no longer`) **that is not in `[corr YYYY-MM-DD: ...]` form**, plus
any `[corr ...]` whose `was` clause exceeds 120 characters. **Its SCOPE (corrected by round 2, Q1):**
`CLAUDE.md`, `MEMORY.md`, and the lines under UNDATED headings of undated non-archive docs — a line
under a heading that carries a date is a dated log section, stamped and kept, and is not counted.
EXCLUDED: `memory/lesson_*` and `feedback_*` files (their subject is wrongness — a new lesson row
that says "was wrong" must never move the count), `docs/LESSONS.md`, `_archive/`, dated filenames,
and any doc carrying the marker `<!-- corr-vocabulary: quoted -->` (the two skill arcs and the
README's legend, which quote the vocabulary — L9). `[V]` "reads ~25" was `CLAUDE.md` alone: the
scope as first written held 367 vocabulary lines in 96 living docs (117 under dated headings, 250
under undated) plus 216 in lesson files; under the corrected scope today's count is about 275, and
**the number the doc carries is the one the script's first run PRINTS, never a prose estimate.** It
falls as each line is CONVERTED — rewritten into the token with the old sentence gone, or the old
sentence deleted because the correction is itself current — and whether a given line was accretion
or an in-place rewrite is the HAND's call at conversion, not the grep's. The trailer's `accretion=`
must not rise; a new sentence in scope that says "X was wrong" without the token raises it, and
that refusal is the rule working.

**Mechanism.** The compliant form is a token and the debt form is its absence, so the count means
one thing once the token exists; the WHY has a destination, so replacing loses nothing.

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
EOF), `ro-longest=` (lines of the longest entry, entries delimited INSIDE the section by the
hyphen-admitting pattern `^[0-9]+[a-z-]*\. ` — Appendix A's, which gives 54 entries and a longest of
275; `[V]` a pattern without the hyphen gives 25 entries and a longest of 596 because `4e.` then
swallows `4e-style`..`4e-coexist` — round 4, Q4, a notation slip in this doc's first wording) and
`mem-over200=`. **The gate is a RATCHET: red only if any of the three GREW
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

## 5. Questions put to the user — DECIDED (USER 2026-09-02, verbatim: *"Yes to all"*)

1. **WP-1 changes the FORM of the 2026-06-21 rule — AGREED.** The per-run tree-wide enumeration
   becomes "the session's blast radius + the K oldest-censused docs every run", so every doc still
   reaches a hand verdict within N runs, and the item-by-item MANUAL verdict survives on a bounded
   list. The memory file that records the rule is rewritten to say so (§9 step 1).
2. **WP-3 runs a script on `CLAUDE.md` — AGREED, and "half of today" is the target.** The script
   measures and reports each run and refuses a close only on GROWTH of the reading order; the shrink
   target (117 KB → 58 KB; no entry over 15 lines) is a printed line until first reached.

The green light opens §9. Per the standing rule, the build begins with a `/qf` pass on this design
under the revised skill — the first pass on the new ledger, and the first census `docs/QF_ARC.md`
§9 step 4 owes.

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

### Pass 3 — the `/qf` design pass under the revised skill (the first pass on the new ledger)

Ledger pass 1 (`design`), this session's scratchpad. Every reply gated by `verify_proof.py` and
`ledger.py append`; every question's status set by the primary with its own citation.

| Round | id | Angle | Question (abridged) | Answer | Design change |
|---|---|---|---|---|---|
| 1 | Q1 | framing-provenance | the label grammar's measured recall on §2.3's rot, and its per-run row count on docs every close touches | measured: 2 of 6 stale-open lines caught, 0 of 2 sub-states; `LESSONS.md` has 184 `[V]/[A]/[RD]/[?]` lines and is in 181 of 283 close commits | WP-1: sub-state column; headings as labels; provenance tags are not labels; the drill's fixture is §2.3's real lines |
| 1 | Q2 | cross-answer-contradiction | which measurement showed the compliant `CORRECTED <date>` and the accretion `CORRECTED <date>` grep-distinct | none; of D3's five sites only 2 hit the first draft's legacy list | WP-2: count every correction-vocabulary line not in `[corr]` form; the hand decides at conversion |
| 1 | Q3 | invariant-not-site-list | what `kind` the census prints for the 101 unbannered docs incl. the three maps, and what invariant decides rewritability | measured: 101 of 136; by dated filename 37 / 99; the six stale-open files split 4 / 2 correctly | WP-1(b)/WP-2: kind = dated filename or `_archive/` → point-in-time, else LIVING |
| 1 | Q4 | prior-art | can WP-4's checks C/D run in CI when the gate's corpus is absent there | no: `lessons_gate.py:55-59`, `.github/` sets no corpus, the gate prints UNVERIFIABLE (`:329`) | WP-4: C/D local, results as trailer columns; CI claims A/A2/B only |

| 2 | Q1 | cross-answer-contradiction | on which scope was "reads ~25" measured, and what does the ratchet read on a close whose new lesson row says "was wrong" | measured: `CLAUDE.md` alone; the scope as written held 367 lines in 96 docs (117 under dated headings) + 216 in lesson files | WP-2: scope = `CLAUDE.md` + `MEMORY.md` + undated-heading lines of undated docs; lesson files, the ledger, dated files and marker-carrying docs excluded; the day-one number is what the script prints |
| 2 | Q2 | measure-dont-infer | what row count a REAL diff's radius produces when hunk-header symbols are generic | measured on `fff4032b`: 148 docs uncut, 6 with a ≤5-docs specificity cut; basenames like `harness` cite 117 | WP-1(a): specific symbols only, basenames as path citations, generic ones printed as dropped, the radius size printed every run |
| 2 | Q3 | prior-art | which mechanism puts the trailer into the commit and refuses a close — a hook, the script, or the agent pasting the script's output | measured: no hook, no `hooksPath`, no CI step reads `git log`, `Docs-Census` in no commit | WP-1(c): the script performs the close commit with the trailer via `interpret-trailers`; a CI gate over commit bodies checks presence and non-increase |
| 2 | Q4 | source-consistency | which files "dated filename → never rewritten" misclassifies, and was the ACTION checked beyond six files | measured: 105 `-RE-` (durable), 147 other dated, 232 `memory/project_*` (Step 3: update) — 484 files; only 69 `-DESIGN-` are never rewritten | WP-1(b): a five-row kind→action table keyed on path pattern, with the user's per-claim refresh/refute rule |

| 3 | Q1 | source-consistency | what enumerates "docs touched" in the three trees a main-repo diff cannot see, and what N the user agreed to at K = 10 | measured: `research/` a nested ignored repo, `CLAUDE.md` ignored, `memory/` outside any repo; 1,510 files → 151 closes at K = 10 | WP-1(a): per-tree enumeration; the snapshot covers the whole memory dir; K = 40 → 38 closes; per-doc state in the private history |
| 3 | Q2 | cross-answer-contradiction | where the per-row HAND verdict gets history when the trailer carries counts, and what the counts describe when verdict edits are unstaged | measured: 1 verdict word in the last 40 commit bodies | WP-1(c)/(d): the census+verdict table is committed to the private history, `census=` in the trailer; `close` reads the index and refuses unstaged docs in radius |
| 3 | Q3 | invariant-not-site-list | which table row yields the skill's own archive action, and where 814 memory files fall | measured: `SKILL.md:65`; memory 298 + 58 project, 588 lesson, 102 feedback, 10 reference | WP-1(b): a verdict-driven ARCHIVE action; memory rows with real counts |
| 3 | Q4 | prior-art | how the CI gate knows a close commit — a mention or a registration — and whether §9 owes the fingerprint ritual | measured: 12 of 40 subjects mention documentize; `fingerprint.json:4` pins `build_core_sha256` | WP-1(c): the `[docs] close:` prefix; the gate in its own workflow file |

| 4 | Q1 | cross-answer-contradiction | does the gate refuse the very push that lands it, and what distinguishes an arc-doc edit from an unregistered close | measured: 10 "documentize" subjects in the unpushed range (7 arc-doc + 3 old-form closes), 249 old-form closes in history, 0 prefixed | WP-1(c): a registered boundary (`since` = the landing commit); the third shape keys on the retired `[docs] documentize` prefix only |
| 4 | Q2 | invariant-not-site-list | what `research/`'s census reads when no commit, base or registration is named for that repo | measured: 57 uncommitted paths (16 under `findings/`), 54 commits, no remote | WP-1(c): `close` commits the inner repo too, same prefix and trailer; index-read; `research-base=` in the main trailer |
| 4 | Q3 | existing-owner | what `close` does with a staged path outside the doc trees on a shared index | measured: `LESSONS.md:145` and `:6978`; `CROSS_SESSION.md:142-143` | WP-1(c): list the index; refuse any staged path outside the trees a close owns; never a pathspec commit |
| 4 | Q4 | source-consistency | which entry delimiter the ratchet implements — the doc's `<n><letters>` (25 entries, longest 596) or Appendix A's hyphen pattern (54 / 275) | measured: both | WP-3: the hyphen pattern; the notation slip corrected |
| 5 | Q1 | invariant-not-site-list | `docs/security/` is ignored (14 files, 0 tracked) and in no snapshot — which enumeration reaches the four security lines §2.3 sampled, and what invariant decides which trees owe a diff source | measured: 20 ignored `*.md` under `docs/` (14 security + 6), 166 tracked `*.md` (31 outside `docs/`), 3 ignored outside; read set 1,548 | WP-1(a)/(d): the read set is computed, the history source is decided per path by tracking state, the snapshot set = read set minus what any repo tracks; K = 40 → 39 closes |
| 5 | Q2 | regression-by-logic | 32 of 82 closes since 08-24 committed a path outside `docs/` + `.claude/skills/` — which would the index refusal have been green on, and what decides the trees a close owns | measured: 76 closes, 24 with a non-md path outside own tooling; under a comment-residue rule 16 green / 8 refuse, the 8 all real code (151- and 111-line tools among them) | WP-1(c): a close owns CLAIMS — tracked `*.md` anywhere, `tools/docs/` + `.claude/skills/`, comment-only hunks elsewhere; code refuses and goes in its own commit; no `--also` |
| 5 | Q3 | source-consistency | which §9 step lands the gate, and whose sha `since` carries when a commit cannot contain its own hash | measured: §9 named neither file; `git log --diff-filter=A -- build-core.yml` returns `47b88116`; `fetch-depth: 0` at `build-core.yml:57` | WP-1(c)/§9.0: the boundary is COMPUTED as the add-commit of the workflow file and printed per run; step 0 lands script + gate + workflow + both drills in one commit |
| 5 | Q4 | writers-census | who writes `Co-Authored-By` / `Claude-Session` (40 of 40 recent commits, by hand) and who sets the `pelmentor` identity in the new history repo | measured: 40/40 both; 614 session URLs; 11 model-name spellings; identity `pelmentor` in main, `research/`, `site/` | WP-1(c)/(d): both trailers are REQUIRED `--trailer` inputs, refused if absent, written on all three commits; the gate checks `Co-Authored-By:`; the history repo copies main's local identity and refuses without one |

**Round 5 was answered the next morning** (2026-09-03; the reply had landed and been gated at the
usage limit the evening before, its four anchors verified). Fifth reactive round; all four changes
are specifications of the one mechanism — three of them replace a LIST with a COMPUTED set (the
trees that owe a history source, the paths a close owns, the gate's boundary), the fourth turns two
hand-typed values into REQUIRED inputs. §9 does not open until the ledger prints CONVERGED.

Round 4 found the ledger's third first-use defect: `tail` was not a legal command anchor, so the
critic's `tail ... | grep -c = 25` was classed as a quote and not re-run — fixed (`77268a7b`), re-run,
reproduced. Two consecutive rounds of reactive growth, so the defect is re-derived in mechanism
terms before round 3 (and rounds 3-4's changes are each a specification of the one mechanism — the
script owns the whole close as three commits, main / research / private history, and CI reads
main's stream from a registered boundary — none a new one): *a mandate nothing observes is satisfied by assertion; every fix must therefore be a
computed set or a recorded number, produced and read by a machine — which is why the trailer must
be written by the script and read by CI, or the fix is prose too.* Every round-2 change serves that
one sentence; none replaces a round-1 mechanism.

Round 1 also found a tooling defect in the ledger on first use: a command anchor that also contains
a `path:line` was classed by the path and never re-run, and command anchors ran under `cmd.exe`
rather than Git Bash (a pipeline with `xargs` exited 255 and was recorded as verified). Both fixed
before round 2 (`ledger.py`: command-first classification; `bash -lc` runner; the FIRST `= N` is the
claim; a non-zero exit with no output is a failure). The verified anchors: Q1 `docs/AUTHORITATIVE_
INTERACTABLE_MIGRATION.md:3`, Q2 `CLAUDE.md:724`, Q3 the `find ... | wc -l = 101` pipeline re-run,
Q4 `tools/docs/lessons_gate.py:57`.

### Staleness measurement — the agent's own summary

28 labels, 14 open-ish / 14 done-ish; 5 stale-open of 14 (4 of 8 true labels), 0 stale-done, 1
undecidable; 8 of 28 lines rot in a subordinate fact; false-open clusters in 2026-06/07
point-in-time docs, every 2026-08 line accurate, one cross-doc miss; 29 % of hits are not labels.
Scratch: `stale_sample/` in this session's scratchpad.

## 9. Build order

0. ONE commit: `tools/docs/status_census.py` with its drill (RED on the §2.3 fixture first): the
   label grammar, the computed read set and per-path history source, the subordinate-fact column,
   the trailer, `close` (three commits, the owned-path refusal, the required attribution trailers),
   `--snapshot` (identity copied from main), `--sweep`, `--loose`; AND `tools/docs/docs_census_gate.py`
   + `.github/workflows/docs-census.yml` with the gate's own drill on a synthetic history (an
   old-form close before the boundary ignored; a prefixed close without a trailer, a trailer without
   the prefix, a retired-prefix subject and a missing `Co-Authored-By:` after it, each RED). The
   gate's boundary is the add-commit of the workflow file, so the first `[docs] close:` can never
   precede the gate (round 5, Q3).
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
