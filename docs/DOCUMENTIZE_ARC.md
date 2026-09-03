# /documentize arc — revising the session-close skill on its own evidence (LIVE doc)

> **Canonical LIVING doc for the `/documentize` skill revision.** Opened 2026-09-02 on the user's
> instruction (§0), the second skill measured the way `docs/QF_ARC.md` measured `/qf`: the skill's own
> output, censused with re-runnable commands, against what the skill's text mandates. The skill is
> `.claude/skills/documentize/SKILL.md` (207 lines, versioned since `03748f56`).
>
> Status tags as in the other arcs: **DECIDED** · **AS-BUILT** · **PENDING** · **DESIGN** · **`[V]`**
> measured (instrument named beside the number) · **`[A]`** taken from the lessons ledger · **`[?]`**
> unverified. **§3 is the ORIGINAL design; most of it is now BUILT** `[corr 2026-09-03: was "Everything
> in §3 is DESIGN. Nothing is built."; measured — WP-1 steps 0/1/2 shipped `43ad649a`+`2e0e591d`, the
> seven defects a 20-round /qf pass found shipped `34c354ba`, the two lanes `f2d07176`]`. **§10 is the
> current state and supersedes §3 wherever they disagree**; §8 is the audit log (three passes, 47
> findings, all dispositioned, 2026-09-02) and §8's pass 4 the post-ship audits; §9 the build order.

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

### WP-1 — One census, one trailer, one private history (step 0 BUILT 2026-09-03, `43ad649a`; the skill text and WP-4 / WP-2 / WP-3 open)

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
(298), the memory directory (1,063) and `CLAUDE.md`. Per path, by OWNERSHIP — the repository whose working tree holds the path and whose ignore
rules do not exclude it, a function of location and `.gitignore`, not of whether anyone has run
`git add` yet (round 6, Q2: `[V]` `research/` has 10 modified and 47 untracked paths, ten of them
new `findings/*.md` dated 08-25..09-02 that `git -C research check-ignore` does not match — they
are the inner repo's, and its close ADDS and commits them; the other 37 are PNGs and scratch
dirs, not `*.md`, printed as a count and never staged): owned by main → `git diff <base>`; owned
by the inner `research/` repo (`[V]` nested and ignored, `.gitignore:283`; own HEAD, no remote) →
`git -C research diff <its base>`, and it gets its own close commit, see (c); owned by NO
repository — ignored by the one whose tree holds it, or outside every tree → the diff of the
private history (d), whose snapshot set is therefore COMPUTED — today `CLAUDE.md` (ignored, `:113`), the whole memory
directory (`[V]` 6.10 MB of text, 2.50 MB gzipped, delta-compressed after the first snapshot), and
**20 ignored `*.md` under `docs/`: the 14 of `docs/security/` (`.gitignore:301`; the four security
lines §2.3 sampled, #13, #14, #27, #28, live there) and six more — `AGENT_SPAWNING.md`, `DOCS_ARC.md`,
`SERVER_BROWSER_ARC.md`, the three `QUESTION_FORM_*`** — which round 3's list of three trees would have
left with no history at all. Ignored `*.md` OUTSIDE `docs/` (`[V]` three: `SUPPORT.md`, two
`reference_*_vps.md`) are outside the read set and get none; the census prints the read set's size
per tree every run. A NEW unignored `*.md` in main's read set is the one path the close does not
decide alone, because main is PUBLIC: `close --new <path>` (repeatable) publishes it deliberately
and the trailer counts `new=`; while such a file exists and is neither named nor ignored, `close`
REFUSES and prints it — a new doc is published on purpose or gitignored on purpose, never left for
the next `git add` (`[V]` today: none). (ii)
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

**Change (b) — the hand check, bounded, and its EXISTENCE machine-checked (round 6, Q3).** Step 0.5
is rewritten: the verdict column is filled BY HAND for the census's rows — tens, not thousands —
with the skill's existing spelling `STILL OPEN` / `ACTUALLY DONE` / `PARTIAL` plus `STALE DONE`
(false optimism) **plus `STILL TRUE`**, the verdict `[V]` 21 of §2.3's 28 rows needed and the first
four spellings could not express — without it `rows` could never equal the verdict sum and an
UNVERDICTED row was arithmetically invisible, which put the hand check back in the assertion class
this doc names. So the close is TWO PHASES: `census` writes the row table with an EMPTY verdict
column to the working file the private history will commit; the hand fills it; `close` re-reads it
and REFUSES until every row carries exactly one token of the closed five, so `rows = still-open +
actually-done + stale-done + partial + still-true` holds by construction and CI checks the identity
on every trailer. The hand's judgment stays the hand's, but `close` refuses the one contradiction a
machine can see: `STILL TRUE` or `ACTUALLY DONE` on a row whose own citation the mechanical column
resolved `gone` or `drifted-content` — a claim cannot be certified true by a citation that no longer
exists. (The brief's larger option — force the verdict through the script — is adopted; what stays
prose is HOW to judge, not WHETHER a judgment was recorded.) An action per verdict is bounded by
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
             still-true=e cited-dead=f accretion=g ro-bytes=h ro-longest=i mem-over200=j
             sweep-cursor=k sweep-cycle=l census=<history sha> research-base=<sha> new=m foreign=n
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
commit in the range and fails on a missing trailer, a ratchet column that grew against the
previous trailer, **and — round 6, Q3 — the checks against WHAT THE TRAILER COUNTS, since CI
cannot recompute a census over trees it never sees: the verdict-sum identity `rows = still-open +
actually-done + stale-done + partial + still-true`; `base=` equal to the previous close commit's
sha in the range (or the boundary), so the censuses TILE; `census=` distinct from every earlier
trailer's, so a trailer pasted from the previous close is caught; `Co-Authored-By:` present** —
the `tools/qf/ledger.py:215` shape, recompute what can be recomputed, at CI's reach. History is
all it needs, so the untracked files' numbers are checked by a machine that never sees the files. **Registration, not mention (round 3, Q4):** the gate must know
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
box (round 4, Q3; CORRECTED by round 6, Q1 — round 4 cited half the ledger):** the ledger holds
TWO rows about one shared repository and they are two AXES, not a contradiction. `[V]`
`LESSONS.md:6982` and `docs/CROSS_SESSION.md:148-162` (2026-08-30): a BARE commit commits the
INDEX, which is shared, so it swallowed ten paths another session had staged — *"skip the index:
`git commit -F - -- <paths>`"*. `[V]` `LESSONS.md:145` (2026-09-01): a PATHSPEC commit commits the
WORKTREE of the named paths, which is also shared, so it carried the other session's unstaged
hunks of the SAME file. No commit form separates two sessions' hunks inside one file — that is
what `CROSS_SESSION`'s split-by-hand protocol is for — and each form is safe on exactly the axis
the other is not. So `close` commits from a PRIVATE INDEX and touches the shared one only to
align it afterwards: `GIT_INDEX_FILE=<tmp> git read-tree HEAD` → `git add <its paths>` →
`git commit`, then `git reset -q -- <its paths>` on the shared index (the 2026-09-01 pattern; the
reset is what clears the phantom revert a private-index commit otherwise leaves there). `[V]`
drilled in a scratch repository: a neighbour's whole-file staged `B.md` survived the close, was
absent from the close commit, and landed in the neighbour's own bare commit; the close carried
exactly `A.md` + a new `N.md`. **Its paths** = the census's radius (i): docs modified in the
worktree, plus `--new` paths — MINUS every path that is STAGED in the shared index, because the
close stages nothing there, so every staged entry is another session's in-flight commit by
construction; that is the discriminator the shared index lacks (nobody can see WHO, the close
needs only NOT ME). A staged entry whose blob equals the worktree is EXCLUDED and counted
(`foreign=`); one whose blob differs from the worktree is a partial staging or a same-file
collision and REFUSES the close with the path printed. The census therefore reads the
WORKTREE of its paths — what the commit will carry — and round 3 Q2's *"reads the INDEX"* is
retired with the refusal it justified. **What a close owns is not two directories, it is
CLAIMS (round 5, Q2):** `[V]` of the 76 closes since 2026-08-24, 24 carried a non-markdown path
outside `docs/` — `.h`/`.cpp` headers whose COMMENTS held a stale claim (the skill's Step 1 reaches
a claim wherever it lives: `multiplayer_menu.h`, `ko_respawn.h`, `world_identity.h` twice), `tools/`
and `reference/` READMEs, and eight that carried real code (`dead_api_census.py` 151 lines,
`compare_zips.ps1` 111, `mp.py`, `trash_proxy.cpp`, `.gitignore` …). So the owned set is computed
per staged path: (1) any `*.md` the repository tracks, anywhere (`[V]` 31 of 166 live outside
`docs/`); (2) the close's own instruments, `tools/docs/**` and `.claude/skills/**`; (3) any other
path whose change is COMMENT-ONLY — decided on the WHOLE FILE, not per diff line, by a
STRING-AWARE lexer per language (round 6, Q4: the round-5 measurement stripped `//…` and `#…`
by regex, blind to a `#` or `//` inside a string literal, so a code change behind `s.find("://")`
or `re.compile(r'(<FONT COLOR="#…')` would have passed): the old blob and the new blob each have
their comment tokens removed — Python by `tokenize` (COMMENT tokens), C-family and `.inc` by a
state machine over `"…"`/`'…'`/`//`/`/* */`, PowerShell `#` and `<# #>` outside quotes, YAML `#`
outside quotes — whitespace collapsed, and the two residues must be EQUAL; a file type with no
comment grammar (`.json`, `.txt`, `.gitignore`, a new or deleted file) is code by construction.
`[V]` the lexer re-run over the same 24 closes partitions them IDENTICALLY, 16 green / 8 refuse,
the 8 being the code cases — a 151-line tool shipped inside a close, which the ledger already names
as the failure — and its drill fixture holds the two quoted-delimiter lines the critic found plus a
trailing-comment rewrite (CODE / CODE / comment-only). A
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

**Mechanism.** The set to check is computed, bounded and printed; "reconciled" means every row
carries a verdict token or the close refuses; the numbers live in commit trailers that CI checks
against each other, and in a private history.

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

Three passes on 2026-09-02, all on the committed draft `3363f970`..`f440ad3d`: an independent
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
| 6 | Q1 | prior-art | a neighbour's staged doc hunks in the ONE shared index — which refusal can see WHO staged a doc, when `CROSS_SESSION.md:162` says the pathspec on COMMIT is the only protection and `LESSONS.md:6982` records ten staged paths swallowed by a bare commit | measured: the two ledger rows are two axes (bare commit = shared index; pathspec commit = shared worktree, `LESSONS.md:145`); scratch-repo drill of a private-index close: the neighbour's staged `B.md` survived and landed in its own commit | WP-1(c): `close` commits from a PRIVATE index, then `git reset -q -- <its paths>`; its paths exclude everything staged in the shared index (not-me by construction: excluded if blob == worktree, refused if not); the census reads the worktree; round 3 Q2's index read retired |
| 6 | Q2 | framing-provenance | "57 uncommitted paths" is 10 modified + 47 untracked, ten of them new findings docs — does "tracked by neither" send them to the private history instead of the repo that owns their directory | measured: 10 M / 47 ??; the 10 `findings/*.md` are not ignored by `research/` (0 `check-ignore` hits); the 37 others are PNGs and dirs; main has 0 untracked docs today | WP-1(a): ownership = location + ignore rules, not `git add` state; the research close ADDS its new findings; a new unignored doc in PUBLIC main needs `--new` or the close refuses; the private history holds only what no repo can |
| 6 | Q3 | name-the-class | no trailer column is checked against what it counts; 21 of 28 sampled rows are `still-true`, a verdict with no token, so an unverdicted row is invisible and a pasted previous trailer passes CI | measured: §2.3 table 9 + 12 still-true; four spellings at (b); `ledger.py:215` re-runs a command and compares digits | WP-1(b)/(c): a fifth token `STILL TRUE`; two phases `census` → hand → `close`, which refuses until every row carries one token; CI checks the verdict-sum identity, `base=` tiling, a distinct `census=`, `Co-Authored-By:`; a `STILL TRUE` on a dead citation refuses |
| 6 | Q4 | undone-cheap-measurement | the 16/8 measurement's comment grammar is not string-aware — a code change behind a quoted `//` or `#` would pass as comment-only | measured: re-run with a whole-file string-aware lexer (Python `tokenize`, a C state machine, ps1, yaml): the same 24 partition identically 16/8; the two quoted-delimiter fixtures classify CODE, a trailing-comment rewrite comment-only | WP-1(c): the grammar is named per language, whole-file, string-aware; no grammar = code; the three fixture lines are in the drill |

**Round 5 was answered the next morning** (2026-09-03; the reply had landed and been gated at the
usage limit the evening before, its four anchors verified). Fifth reactive round; all four changes
are specifications of the one mechanism — three of them replace a LIST with a COMPUTED set (the
trees that owe a history source, the paths a close owns, the gate's boundary), the fourth turns two
hand-typed values into REQUIRED inputs.

**Round 6 was the user's cap** (2026-09-03: *"Это последний раунд что сейчас придет"*, then *"И затем
собираем"*). All four answered-measured; the critic did not return `converged`, so the ledger's STOP
is `user-cap 6`, not convergence, and this doc says so: the pass is UNSETTLED by the ritual's own
rule and §9 opens on the USER's word, not on a CONVERGED line. What round 6 changed is not small:
round 4's *"never a pathspec commit"* rested on half the ledger and is replaced by a private-index
commit drilled in a scratch repo; the hand verdict, the doc's one remaining assertion, is forced
through the script; the comment grammar became a string-aware lexer; the per-path rule became
ownership. A next critic, if the user wants one, starts from these four.

Round 4 found the ledger's third first-use defect: `tail` was not a legal command anchor, so the
critic's `tail ... | grep -c = 25` was classed as a quote and not re-run — fixed (`a10af029`), re-run,
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

### Pass 4 — the post-ship audits of the BUILT census (2026-09-03, two lenses, `sonnet`)

Run on `43ad649a` + `2e0e591d` (the first pair died on a model usage limit and were re-spawned).
**EVIDENCE lens: 3 CRITICAL, 2 HIGH, 3 MEDIUM, 3 LOW. DESIGN lens: 1 HIGH, 7 MEDIUM, 6 LOW** — and it
scored ~30 of 42 design claims IMPLEMENTED-as-written. Every CRITICAL and HIGH below was
**re-measured by the primary before being accepted**; each fix is drilled RED.

| # | finding | my re-measurement | disposition |
|---|---|---|---|
| C1 | a neighbour's `git add -N` (intent-to-add) doc is invisible to BOTH guards at once, so a stranger's close publishes it | `[V]` in a scratch repo: `ls-files -- '*.md'` LISTS it, `ls-files --others` does NOT, `diff --cached --name-only` is EMPTY — so it was owned as a tracked doc and never reached the `--new` refusal | **FIXED**: `intent_to_add()` reads `status --porcelain=v2` for `1 .A `, subtracts those from the tracked set and adds them to the new set, so the `--new` refusal fires. Drilled |
| C2 | `staged_entries()` reads the INDEX, so another session's plain UNSTAGED edit to a doc in our radius rides into our close | `[V]` true by construction (round 6 replaced the index read with a worktree read and the unstaged refusal went with it) | **FIXED by an invariant, not a heuristic**: the census PINS each radius doc's content hash and the close refuses to commit any doc whose bytes changed since. Escape = re-run `census`; an edited line then arrives as a NEW row needing its own verdict, so the trailer's counts describe the text being committed. Drilled |
| C3 | the private history repo is as shared as main, yet its commit used a bare `git add -A`; and `census/pending.md` is one fixed path, so a second census destroys the first hand's verdicts | `[V]` the history path is a pure function of the repo path; the commit did use `add -A` | **FIXED**: that commit now goes through a PRIVATE index too (all three do), and a census that would overwrite held verdicts REFUSES, naming their age, unless `--force`. The guard runs FIRST, before the 35 s scan. Drilled |
| H4 | the gate's tiling check passes a vacuously EMPTY `base=` (every string starts with `""`) | `[V]` `'abc123'.startswith('') == True`; the shipped drill only ever tried a wrong-but-nonempty base | **FIXED** (`if not base or …`) + a 13th gate arm |
| H5 | `STATUS_RE` captures `NOT FIXED` as `FIXED` — the label records the OPPOSITE of the line's claim | `[V]` `label_of()` on the audit's real example returns `('lead','FIXED')`; also `NOT VERIFIED` → `VERIFIED` | **FIXED**: an optional `NOT ` prefix in both `STATUS_RE` and `CELL_RE`; `[V]` the next real census carries **28** `NOT …` labels that were being recorded inverted. Drilled with four cases |
| D-H | the design text's resolve-state vocabulary (`exists / moved / gone / drifted-content`) never matched what shipped (`ok / gone / past-eof / ambiguous / external`), and no content-DRIFT check exists in the census Resolver | `[V]` correct; drift detection exists only in `lessons_gate.check_quoted_cites`, scoped to the ledger | **OPEN, recorded**: the vocabulary in WP-1(a) above is now the shipped one. Extending drift detection to every doc that QUOTES a cited line is a real gap — `check_quoted_cites` is already general and reusable, so it is the next census change, not a redesign |
| M/L | the kind→action table is prose only; `--snapshot` is a `snapshot` subcommand; `external`/`--loose`/`show`/the accretion exclusion list are unstated; `SYMBOL_RE`/`CITE_RE` are re-forked from `lessons_gate`'s and have drifted; `_strip_hash` lacks escape-awareness; the sweep does not skip `_archive/`; multi-line bold spans and legend lines produce spurious rows; a close can be TWO commits when `research/` has nothing in radius | not individually re-derived | **RECORDED, not fixed.** None changes a verdict or commits a wrong byte; the two grammar ones add noise rows a hand verdict absorbs. The regex fork is the one to fix first (one concept, two implementations — RULE 2) |
| N | the AS-BUILT note's row counts (1,101 / 879) no longer match the live table | `[V]` a later census re-run; now 1,111 / 885 after the NOT-label fix | **noted**; the numbers are re-stamped at the first real close, which is where they become a trailer |

The audits' own residual, stated: **a shared working tree cannot be made safe by refusals alone** — every
guard here is "detect and refuse", and two sessions running `/documentize` concurrently still interleave.
The project already has the shape of the answer (`tools/game_lock.py`, `docs/CROSS_SESSION.md`); whether
a close takes a lock over the whole census→hand→close window is a decision for the user, not a fix to
slip in.

### Staleness measurement — the agent's own summary

28 labels, 14 open-ish / 14 done-ish; 5 stale-open of 14 (4 of 8 true labels), 0 stale-done, 1
undecidable; 8 of 28 lines rot in a subordinate fact; false-open clusters in 2026-06/07
point-in-time docs, every 2026-08 line accurate, one cross-doc miss; 29 % of hits are not labels.
Scratch: `stale_sample/` in this session's scratchpad.

## 9. Build order

0. ONE commit: `tools/docs/status_census.py` with its drill (RED on the §2.3 fixture first): the
   label grammar, the computed read set and per-path OWNERSHIP, the subordinate-fact column, the
   trailer, the two phases `census` → hand → `close` (three commits from a PRIVATE index, the
   owned-path refusal with the string-aware lexer and its three fixture lines, the foreign-staged
   exclusion/refusal, `--new`, the five verdict tokens with the refuse-until-verdicted and the
   dead-citation contradiction, the required attribution trailers), `--snapshot` (identity copied
   from main), `--sweep`, `--loose`; AND `tools/docs/docs_census_gate.py` +
   `.github/workflows/docs-census.yml` with the gate's own drill on a synthetic history (an old-form
   close before the boundary ignored; after it a prefixed close without a trailer, a trailer without
   the prefix, a retired-prefix subject, a missing `Co-Authored-By:`, a verdict sum that misses
   `rows`, a `base=` that does not tile, a repeated `census=`, each RED). The gate's boundary is the
   add-commit of the workflow file, so the first `[docs] close:` can never precede the gate (round
   5, Q3).
1. WP-1 text: Step 0.5, Step 1's enumeration, Step 0.5(5), the scope statement, the frontmatter, the
   Step 5 ledger; the memory file + `MEMORY.md` line for the 2026-06-21 rule rewritten (after §5.1).
2. WP-4: the three checks in `lessons_gate.py`, drilled RED first; Step 3.5 points at `--pairing`.
3. WP-2 text + the `accretion=` column; the five debts, one per run.
4. WP-3: the three size columns + the ratchet; the first shrink (`4e`) by move-then-cut.
5. **The first census**: ten close commits later, the trailers are read back (`ro-bytes`,
   `accretion`, `sweep-cursor`, verdict counts), `/context` is re-read by hand, and a `--sweep` census
   replaces §2.3's sample. Recorded here.

Each step is one commit, `[docs]`/`[tools]` prefixed; nothing ships before the user answers §5.

**Step 0 BUILT 2026-09-03 (`43ad649a`), NOT hands-on: the first real census ran, no real close yet.**
`tools/docs/status_census.py` (725 LOC) + `status_grammar.py` (280) + `comment_lexer.py` (105) —
three modules because the first cut was ~1,000 lines holding three concepts; `docs_census_gate.py`
(156) + `.github/workflows/docs-census.yml`; drills `status_census_drill.py` (recall on §2.3's
stale-open lines and precision on its eight vocabulary false positives; the three lexer fixtures; a
scratch two-session close with every refusal shown RED and the ratchet RED) and
`docs_census_gate_drill.py` (10 arms, 9 RED for their own reason). `[V]` the first census on this
tree: 35 s; read set 1,548 (main 166 / research 298 / private 1,084); radius 43 touched + 49 cited +
40 sweep = 112 docs; 1,101 rows (879 labels, 225 dead citations); ratchet `ro-bytes=119116
ro-longest=275 mem-over200=37`; accretion 275; the history baseline holds 1,084 files. `[V]` the gate
computes its boundary (`43ad649a`) and judges 0 closes. **Two first-run defects, fixed before the
commit and recorded here because the drill did not catch them:** 2,400 session-UUID fragments in
memory frontmatter read as commit hashes (the hash lookaround now excludes `-`), and the ledger
gate's per-citation tree walk ran the census past ten minutes over ~5,000 tokens (one basename
index now, 0.1 s to build). Three decisions made while building, each a specification the design
text did not carry: a verdict does NOT carry across closes (an unchanged line is re-judged at its
next census; it carries only from a pending table into its own re-census); the `STILL TRUE`
contradiction refuses on LABEL rows only (on a prose line a dead pointer can be dead on purpose —
this doc naming its retracted `ledger_rot.py`); a backticked commit hash is a hash, and a foreign
hash (`e31aaaa6`, UE4SS) is informational, never a refusal. The first REAL close is the next
`/documentize`; steps 2-5 are open.

**THE FIRST REAL CLOSE RAN 2026-09-03, and it measured the design's own promise false.** WP-1(b) says
the hand check is *"tens, not thousands"*. `[V]` the first run: radius 67 docs -> **714 rows**, because
the radius is bounded in DOCS while the work is paid per ROW and a doc's row count is unbounded
(`docs/LESSONS.md` 85 status lines, `CLAUDE.md` 71, one research finding 63) -- so editing one line of a
doc owed a verdict on every status line in it. Two fixes, each measured: a TOUCHED doc now contributes
only the rows whose line-hash is absent from its BASELINE version (`git show <base>:<path>`), 714 -> 119;
and the amortised sweep spends its OWN row budget rather than what is left of a shared one -- shared,
the session's 119 rows ate it and the sweep took **1 doc of 1,521 candidates** (~1,521 closes to cross
the tree), separate it takes 12 (~127). A third correction followed: a diff-scoped doc is NOT stamped
"censused", or it reaches the back of the queue having had only its new lines read. Lesson +
ledger row written.

**The first close also added a SIXTH verdict token, `NOT A LABEL`.** `[V]` 23 of this close's 146 rows
are the grammar catching VOCABULARY, not a claim -- the two skills' own token tables and tag legends,
a quoted regex, "QUESTION -> DESIGN -> IMPL". The five status verdicts had no value for "this line is
not a claim", so a false positive had to be verdicted `STILL TRUE`, laundering noise into the counts
and hiding the grammar's precision. `not-a-label=` now MEASURES that precision every close instead of
asserting it, and the gate's identity check counts it.

**Three instrument defects the first close found and fixed, all false-DEAD citations:** `.github` was
missing from `CITE_ROOTS`, so every citation to `build-core.yml:57` -- a line that exists -- resolved
`gone` (the same tuple that had `include`, which never existed: one list, two silent failure modes);
eight external sources cited by research findings (GNS `udp.cpp`/`p2p.cpp`/`certs.cpp`/`certstore.h`,
four CXXHeaderDump headers, `WindowsD3D12Viewport.cpp`) were not in `lessons_gate_allow_files.txt`;
and a SECTION reference (`§6c.c`) matched the path grammar as a C file. Dead citations 24 -> 9, and the
nine that remain are all deliberate (a retracted `ledger_rot.py`, a retired `ko_respawn.h`, the drill's
own sentinel, a lesson quoting pointers that rotted on purpose).

**What the close found in the DOCS**, both of the classes §2.3 measured, both fixed with a `[corr]`
stamp: `AUTHORITATIVE_INTERACTABLE_MIGRATION.md:3` claimed `coop::Door` + `coop::Keypad` "IMPLEMENTED
2026-06-06 (uncommitted; hands-on-pending)" -- `[V]` neither symbol exists anywhere in the tree and
`git log -S'coop::Door' -- src` returns no commit, while the door/keypad sync HAS been committed since
June under other names, so the label was a stale-open riding a false sub-state; and its `:96` cited
`interactable_sync.cpp:652-656` in a 567-line file (`[V]` the passage is at :314-317).
`COOP_CLIENT_MODEL.md:159` still said the skins re-cook was "hands-on pending" -- `[V]` skins reached
real players and produced a FIELD defect report on 2026-09-01.

**An OPEN defect in the flow, recorded not fixed:** fixing a row erases the verdict that motivated the
fix. The close pins the census's content, so a doc edited after the census forces a re-census; the
corrected line then arrives as a NEW row and is verdicted `STILL TRUE`, while the `ACTUALLY DONE` /
`STALE DONE` that CAUSED the edit is gone from the final table. So this close's trailer reads
`actually-done=0 stale-done=0` although it fixed three stale claims -- the counts under-report found
rot by construction, and the finding survives only in the `[corr]` stamps and this section. The fix
shape is a resolved-row carry (a verdict whose row disappears because it was acted on is recorded, not
dropped); it is the next census change.

**Step 2 BUILT 2026-09-03 (WP-4), and it found a hole in the gate it extended.** `lessons_gate.py`
gains C (`check_wikilinks`), D (`check_pairing`, exposed as `--pairing`, which returns before the 22 s
corpus build) and E (`check_running_totals`); C and D fail the gate, E is a WARN that is printed and
never silent; all three need the memory corpus, so `[V]` in a simulated CI run
(`MULTIVOID_MEMORY_DIR=/nonexistent`) they print **UNVERIFIABLE**, not SKIPPED-as-green, exactly as
WP-4 required. Their counts ride three new trailer columns (`wikilinks-dead` / `pairing-unref` /
`pairing-dead`), ratcheted by the close and by the CI gate; `[V]` today 0 / 40 / 0, and 5 rows carry a
running total. The drill gained two RED arms (a dead `[[wikilink]]`, a dead `memory/<slug>.md`
reference) and a WARN-visibility assertion on E. `[V]` C found one real dead wikilink on its first run
(`[[lesson-a-cannot-in-a-comment]]`, fixed to the full slug).

**The hole (a PRE-EXISTING defect, not a regression of this work).** Running the ledger drill — for the
first time since 2026-09-01 — reported `dead file  exit=0`: the gate could not fail on a citation to a
file that does not exist. Root, measured: check A files a bare-basename citation that resolves nowhere
as `UNVERIFIABLE` instead of `DEAD` whenever `absent_cite_roots()` is non-empty (a correct CI
accommodation — `research/` is gitignored there, `reference/` unfetched), guarded only by the comment
*"with every root present this branch cannot be taken"*. But `CITE_ROOTS` listed **`include`**, a
top-level directory this repository has **never** had (`[V]` `git log --oneline -- 'include/*'` = 0
commits; our headers live under `src/votv-coop/include/`). So the branch fired on EVERY run, on every
machine, and that half of check A had never worked. `[V]` after removing `include`: the arm is
`DEAD CITATIONS (1) … exit=1`, and the real ledger still PASSES (229 citations, 0 dead) — the hole was
latent, not hiding rot. The drill now asserts the branch's PREMISE (`absent_cite_roots() == []`) before
running any arm, so the class cannot return silently. Lesson row + memory file written.

**A correction to WP-3's first shrink, measured before any line was cut.** The design says the `4e`
entry's session digests *"already live in `memory/project_s2*.md`"*. `[V]` `4e` names **21** digests
(s17..s30b); files matching `project_s2*` exist for **11** of them, and the other ten (s17, s18, s19,
s23c, s24, s24b, s25, s26, s29b, s30b) appear in memory only under other filenames — an s-number
mentioned in a file is not proof its digest is there. So the shrink is not a pure cut: it owes a
per-fact grep of the destination (the move-then-cut rule) and, where the fact is absent, a destination
written first. `[V]` the reading order is 55 entries, longest `4e` at 275 lines, then `4e-browser` 152,
`4a-identity` 87, `1a-veh` 73. Step 4 is NOT done.

**Step 1 BUILT 2026-09-03 (the commit after `04964c3f`):** `.claude/skills/documentize/SKILL.md` rewritten
whole around the census — Step 0.5 = run `census`, Step 0.6 = the hand verdicts (the five tokens, the
action per kind, the ask-the-user row that keeps the close refusing), Step 4.5 = the close is the
script's (the refusal list, the three commits, never a hand `git commit`), Step 5 leads with the verdict
table; the frontmatter no longer says "Update ALL project docs" (L-2); the tree-wide grep is gone from
the text. `memory/feedback_documentize_manual_status_reconciliation.md` rewritten in the same run (RULE
2 — one text of the 2026-06-21 rule, its form change recorded with the measurement that forced it). The
ratchet's day-one baseline is minted by the FIRST real close, which is the next `/documentize`.

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

---

## 10. Pass 5 — the 20-round `/qf` design pass, and what it did to this document (2026-09-03)

The user asked for the loop to run to convergence (*"I thought qf runs until convergence?"*), made the
criterion explicit (*"What would be the best for the project - thats my decision"*) and lifted the
reframe stop (*"If reframe needed - reframe autonomously and run"*). It ran rounds 7-20 on top of the
6 that preceded the build, stopped on a user-paced cap when the finding severity decayed, and the
critic never returned `converged` — recorded here as a **user-paced stop, not convergence**.

**80 questions, all answered-measured. Every measurement has a re-runnable script.**

### 10.1 What it falsified — including two of my own load-bearing claims

| # | the claim | how it died |
|---|---|---|
| 1 | *"the radius bounds the hand check"* | the radius is bounded in DOCS, the work is paid per ROW: 67 docs -> **714 rows**. Fixed by diff-scoping (714 -> 119) before this pass began |
| 2 | **the round-13 keystone: "the hash carry erases verdicts on edit"** | over **1,393 rows across 37 real commit pairs on five docs**, a claim-shaped key carries where the hash key loses on **ONE row (0.07%)**. The ordinal component churned 0.5-7% for nothing. The erasure is a RECORDING problem, not a key problem |
| 3 | **"DATEDNESS is the label-rot risk key"** | measured directly: open-ish rows naming a symbol that resolves today — **undated 20.8% (95/456)**, dated <2026-08 9.4%, dated >=2026-08 11.3%. UNDATED is twice the densest, the opposite of the assumption. Dead citations rot in OLD DATED docs (dated-RE 73.5%, living 14.3%); stale-OPEN labels rot in UNDATED LIVING docs. **Two rot forms, opposite distributions** |
| 4 | *"`reversals=` measures the hand's precision"* | a row is keyed on `sha1(line)`, so a second verdict needs the line unchanged AND the doc's sweep turn (cycle 109). A flip that far out is the WORLD moving and the sweep CATCHING it — the arc's success, not the hand's error. Renamed `flips=`, reported, never a metric |
| 5 | a ratchet on `corpus-dead-cites=` | proposed in round 18, dead in round 19: one rename touches 5 tracked docs against a close radius of ~50 — an ordinary extraction would refuse a session over work it did not do. **A ratchet may only cover a number the closing session can move** |
| 6 | **a quote of the user's own ask** | rounds 7-10 quoted a Russian sentence about *"convenience of keeping documentation"* as verbatim. It appears **0 times in `docs/`, 0 in `memory/`, and 4 times in the qf thread — all four my own briefs.** Introduced in round 7, hardened by repetition, on the ASK, which is the one thing a brief's first section exists to anchor. RETRACTED. Whether ergonomics is also wanted is **an open question for the user** |
| 7 | a citation of mine, in a brief about citations | `docs/LESSONS.md:5008` cited for the positional-table lesson, which lives at **`:6548`** — `:5008` is the K2Node/ActionMappings row, and both end with "a failed resolve is loud while a resolve to the WRONG function is silent". At 7,101 lines `:5008` resolves LIVE: a position check passing on wrong content. `LESSONS.md:1896` had already measured 3 of 4 live `LESSONS.md:<N>` citations wrong at rest; mine is the fourth |
| 8 | *"cadence ~8 closes/day"* | that pattern counts this arc's own doc commits. Real closes of this mechanism in all of history: **ONE**. Three patterns give 1 / 110 / 284 — three scopes, and I published one without naming its unit |
| 9 | *"12 rows/close"* | `ROW_BUDGET = 40` was introduced BY the close commit, so 12 is the PRE-FIX value and every "12 -> 507 closes" figure of rounds 8-10 described code that no longer existed |

### 10.2 The corpus, measured (scripts in the session scratchpad)

- **6,081 rows over 1,552 read-set docs**; 42.7% of docs yield ZERO rows; mean 3.92, median 1, max 141.
- **37.7% of the corpus (2,292 rows) carries a DEAD citation** — dated-RE 73.5%, dated-other 56.1%,
  `_archive/` 52.4%, lesson files 35.4%, dated-DESIGN 24.4%, memory ~18.5%, **living 14.3%**. Of those,
  **2,245 are kind `cite` (no label) and 47 are LABEL rows**, so the contradiction refusal reaches 2%
  of the class and D4 removes the other 98% from the hand entirely.
- **70.8% of rows sit in docs untouched 30d+**, but LIVING is the FRESHEST kind (876 of 1,262 rows
  under 7 days) — the doc is fresh because we keep editing it, while its OLD lines are what
  diff-scoping never reaches.
- **THE FILENAME PARTITION IS DEAD**: 20 of the 22 dated `-DESIGN-` docs CLAUDE.md's reading order
  names carry AS-BUILT / SHIPPED / "design of record". Authority is inbound citation, not the name —
  but authority does NOT discriminate rot (dated-uncited 46.1% vs dated-CITED 48.5%), so it decides
  RELEVANCE while datedness decides which rot form.
- **The corpus contains duplicates**: 38 dated documents at two read-set paths, 33 byte-identical and
  **5 drifted apart** — four real cross-tree pairs plus one legitimate archive snapshot.
- **The date ladder** over 1,067 memory files: frontmatter `modified:` 645, filename date 124, a BODY
  date 291, mtime-only **SEVEN** (named). mtime alone is invalid there (228 share the 2026-07-28
  compaction day), and `git blame` in the private history returns ONE date for every line, so of 456
  undated open-ish rows only **248 (54.4%) have a real line age**.
- **`MEMORY.md`'s retrieval index is partly dead**: six of its eleven `memory/<glob>` pointers resolve
  to ZERO, because **0 of 597 lesson files carry a filename date** — the compaction replaced lists with
  greps that return silence.
- **The reading order**: 55 entries, 1,270 lines, **117,945 bytes**; the first two lines of every entry
  total 9,166 B (7.8%), the movable body 108,779 B (92.2%). Destination coverage per entry is 68.5%
  overall but **`4e` — the longest, and the one the design named first — is 15%**, against
  `4e-browser` 94%, `4e-sbarc` 92%, `4d-death` 87%. The shrink order is measured coverage, not length.
- **Corpus repair, sized**: of the 2,292 dead-citation rows, **678 (29.6%) also name a backticked
  symbol and 573 (25.0%) name one that RESOLVES today**, against ~8 rows for the quoted
  `file:line says "..."` form. **~1,711 rows have no mechanical correction and this design does not
  repair them.**

### 10.3 The seven shipped defects (all fixed, `34c354ba`)

1. **The sweep could not reach the docs the reading order lists FIRST.** `candidates` dropped `touched`
   BEFORE any ordering while steps 3/3.5/4 touch `MEMORY.md`, `LESSONS.md`, `CLAUDE.md` every close:
   those four hold **194 label rows**, the one real close surfaced **17**, and the state file had never
   stamped `CLAUDE.md` once.
2. **The sweep was ALPHABETICAL.** `never` held 1,521 of 1,552 docs and was `sorted()`, so the utc
   order could not take effect for ~150 closes; it swept README/SECURITY/BUILDING and **zero** research
   findings, against the 185 dated 2026-06/07 where §2.3 measured the rot. Now oldest-first on a clock
   ladder; **the cycle fell 109 -> 70**.
3. **The pending table was POSITIONAL** — `hashes[n-1]` by printed row number, the shape `f74d05dc`
   retired once already; **four hash collisions in 6,081 rows became zero**.
4. **The read set omitted 77 tracked docs and a whole repo** — `research/findings` only, so the
   `handson_runbook_*` files (cited by name in 17 docs) could never receive a row and `site/` was
   invisible. Owner repos are now discovered by the LOCAL GIT IDENTITY, the invariant CLAUDE.md states
   and `history_repo` already enforced (11 inner repos, separated perfectly). `_archive/` left the set.
5. **The content pin covered one of three commit sites.**
6. **The trailer's vocabulary was four hand-written lists** — 23 columns written, 15 read.
7. **`--force` said the opposite of what it does** — on the operator's only forward path.

### 10.4 The design as it stands (D0-D11) and what is BUILT

**BUILT 2026-09-03:** `34c354ba` the seven defects + `trailer_schema.py` (every column declares its
KIND: IDENTITY / VERDICT / RATCHETED / MONOTONE / GATED / REPORTED, and the gate fails an undeclared
one); `f2d07176` the two lanes (a fresh row cannot have AGED, so it is asked the AUTHORING question and
only when it asserts something falsifiable — 17 authoring rows against 166 ageing on this tree) plus the
two vocabulary-quote scopes; **D0 the resolved ledger** (below).

**D0 AS-BUILT — the resolved ledger.** The defect is not that a verdict is wrong. It is that ACTING on
one erases it: the fix rewrites the line the verdict names, the row's hash changes, the carry-forward
drops the verdict, and the corrected line returns as a fresh row verdicted `STILL TRUE`. That is why
the close of 2026-09-03, on a run that corrected two memory topics, wrote `actually-done=0
stale-done=0`. **The verdict columns are not the bug and were not changed** — they describe the text
being committed, which is exactly what the content pin exists to guarantee. What was missing is a
record of the verdict that MOTIVATED the fix.

So the verdict is appended to `census/resolved.jsonl` in the private history *at re-census time* —
the moment it is lost, not the moment of the close, which would have to reconstruct it. The record
carries no close sha, because none exists yet and none is needed: the ledger is committed by the
history commit, so the commit that first contains a record IS the close that published it. The
discriminator between "acted on" and "merely gone" is the RADIUS: a prior verdict whose doc is not
being scanned this time left the frame and is NOT recorded (drilled as an explicit control, or the
ledger would inflate on every change of sweep queue). The trailer carries the CUMULATIVE totals
`resolved=` / `flips=` (a flip = a verdict that named something wrong: `STILL OPEN` / `ACTUALLY DONE`
/ `STALE DONE` / `PARTIAL`), because CI never sees the private history — a running total is the only
property available to it, and it checks the one that matters: a close may not un-record what an
earlier close recorded. That is a NEW schema kind, `MONOTONE`, the opposite direction to `RATCHETED`
and distinct for that reason. `status_census.py resolved` reads it back, so it is not write-only.

**This is also the instrument D8 needs.** D8's falsifier counts corrections over 300 ageing-lane rows
— and until now those corrections were precisely the ones the trailer could not see. Each record
carries its row's LANE for that reason.

**D6 AS-BUILT — the citation content rung, and the detector that never had an input.** A line number
is a POSITION; the claim is about CONTENT. `lessons_gate.check_quoted_cites` (check A2) says exactly
that and has run on every gate invocation since 2026-08-30, when an extraction moved five cited facts
and the positional check passed all five in the run that created the rot. **`[V]` 2026-09-03: the
form it requires — `` `file:line` says/reads/states "…" `` — occurs FIVE times in the whole 1,613-doc
read set and ZERO times in `docs/LESSONS.md`, the ledger it guards.** The check built for that defect
has never had an input. Nothing was broken; nothing was ever checked.

What this corpus writes instead is a citation beside a BACKTICKED SYMBOL — **1,816 of the 5,302
resolving citations**. So the rung reads there too, at a deliberately different strength:

| | pairing | on a miss | why |
|---|---|---|---|
| QUOTE | explicit (the verb names it) | `moved` / `content-gone`, **dead** — refuses `STILL TRUE` | unambiguous by construction |
| SYMBOL | inferred from adjacency | `drift` — its own row kind, **never refuses** | a mostly-right gate is one people learn to ignore |

The strength split is not caution for its own sake: hand-checking the first heuristic found **one
false pair in four** (`docs/LESSONS.md:1590` cites `config.cpp:508` for `resize(255)`; `ToUtf8`
belongs to a later citation in the same sentence) and one RANGE read as stale for pointing inside
itself (`docs/PERF_ARC.md:366` cites `reflection.cpp:576-677` for `CountObjectsByClass`, at 647).
Both are closed, as are three more shapes: a symbol occurring more than once gives no unambiguous
repair, a symbol absent from the cited file is no evidence at all, and a symbol AT the line is fine.
**And the quote rung's only corpus hit was itself a false positive** — `nick_color.h:3` carries "The
COLOR AXIS has ONE owner: this module" across lines 3 and 4, so a per-line matcher missed it, and
joining alone still missed it because line 4 begins with its own `//`. Fixed in the shared owner, so
`lessons_gate` gains the fix for the day it does get an input.

Final numbers on this tree: **31 `drift` rows across 25 docs, 0 quote-rung dead.** A drift row names
its own repair (`session_lanes.h:179->223`). `cite-drift=` rides the trailer, REPORTED.

**Two columns were DROPPED, as decisions rather than omissions.** `corpus-dead-cites=` would cost a
24-second whole-corpus walk on every close for a number no session can move — which is the same
reason round 19 refused to ratchet it — and `cite-unquoted=` was meant to measure the corpus
converting to the quoted form, a target the measurement above says is the wrong one.

**D9 AS-BUILT — the index that is loaded into every session, and the pointers in it that went
nowhere.** `MEMORY.md` is compacted by replacing lists with GREPS ("08-31 — грепом
`memory/lesson-*2026-08-31*`"). **`[V]` 2026-09-03: SIX of its eleven such patterns match ZERO files,
because 0 of 705 lesson and feedback files carry a date in their FILENAME while every `project*` file
does** — so every `project*` glob worked and every `lesson*` glob was a dead end, in the one file
loaded into every session's context. Nothing gated it: `lessons_gate` fixes its ledger to
`docs/LESSONS.md` and never looks at `MEMORY.md` or at CLAUDE.md's reading order.

Two candidate roots were measured and both rejected before the third was built. Renaming 705 files is
out: the `name:` slug is the `[[wikilink]]` key, so a rename breaks every link pointing at them.
Grepping the body is out on precision: `[V]` 702 of 705 carry a date somewhere in the text, but a
lesson MENTIONING a day is not a lesson written that day — `2026-09-01` matches 39 files where the
frontmatter `modified:` matches 35. What shipped instead PUBLISHES the date the age ladder already
computes: `tools/docs/memory_index.py` writes `memory/INDEX_BY_DATE.md` — **1,073 entries over 84
days, 7 of them on the weak `mtime` rung, and the rung is printed beside each** so a guessed date is
visibly weaker than a stated one. It is regenerated by every close (the config catalog's discipline:
rewritten every run, never read back) and carries the doc-scope vocabulary marker, because an index
reprinting other files' descriptions is not making their claims.

`memref-dead=` is RATCHETED at **0** and covers what `wikilinks-dead` does not: MEMORY.md's markdown
links and date globs, and CLAUDE.md's backticked repo paths. Getting there also corrected three
CLAUDE.md pointers that named ANOTHER repo's paths as if they were ours (`docs/ARC.md`,
`docs/design/`, `tools/blender/votvio/` — all VotvIO's) — they now say which tree they are in, which
is better prose as well as a resolvable pointer. Three shapes that only LOOK dead are controls in the
drill: a directory (`docs/events/`) is a resolved pointer, a path written from the SOURCE root
(`include/coop/thing.h`) resolves, and the same glob shape over dated `project_*` files is live.

**D10 AS-BUILT — and the handed-down coverage number was measuring the wrong thing.** The build plan
ordered the reading-order cut by "MEASURED destination coverage (4e-browser 94 %, 4e-sbarc 92 %,
4d-death 87 %; 4e at 15 %)". Re-derived before building on it, per the re-base rule: **those are
SYMBOL coverage** — do the backticked identifiers in the entry also appear in the doc it points at —
which reproduces at 94 / 100 / 90 % and is close to useless as a cut criterion, because two texts
about the same subsystem name the same things by construction. **CLAUSE coverage — is this CLAIM
already in the destination — is 0-11 % for every entry in the reading order, `4e-browser` included at
6 %.** Acting on the handed-down ranking would have cut `4e-browser` first: 14.5 KB whose ~136 claims
exist nowhere else.

So the finding inverts the job. The reading order is **not a redundant index that can be trimmed** —
it is the ONLY copy of most of what it says, and the ~37 KB still owed against the 58 KB target is a
WRITING job, one move at a time. `tools/docs/reading_order.py` reports both readings (the misleading
one is named as such), and `ro-moved=` at the close compares the reading order against the private
history's previous `CLAUDE.md`: a clause that LEFT and is findable somewhere MOVED; one findable
nowhere was **CUT**, and each is printed in full, because a claim being destroyed is not a number.

The one move this pass made is the one the measurement most supported: **`4e.` was 271 lines of
session-by-session build log (s17-s30b) inside an index entry** — 24 KB, a fifth of the reading order,
and the single entry that set `ro-longest` to 275 against a target of 15. It moved VERBATIM to
`docs/signals/HISTORY.md` (fidelity checked character-by-character, whitespace-normalised) and the
entry became a pointer. **`ro-bytes` 119,076 -> 95,206; `ro-longest` 275 -> 152.**

The `USER`+`verbatim` exemption was **stated and not enforced**: the sentence splitter cuts
`USER, verbatim: "…"` at its colon, so the fragment carrying the quote no longer carried the word
`USER` and the guard protected ZERO lines. It is evaluated per SOURCE LINE now, and protects 8 in the
top twenty entries.

### 10.6 The DIFF pass (phase 4) — nine defects in the code that had just shipped

The pass the user approved after the build (*"Ok go with the recommendation"*). Round 1 of the critic
plus a parallel self-read found **nine defects in a diff that was one hour old and had passed every
drill**. Five were mine, found while the critic read; four were the critic's, and none overlapped.

**The two that were live gates doing nothing:**

- **`memref-dead` was a hardcoded 0.** The commit that shipped it described a ratchet at target 0; the
  compute block had silently failed to apply (a `t.replace()` whose anchor did not match, with the
  replacement count never checked). `dead_refs()` saw 3 dead pointers while `ratchet_values()`
  reported 0. **The drill passed it** because it asserted `== 0` in the GREEN state, which a
  never-computed field satisfies by construction — the arm exercised the detector and never the
  wiring. Drill arm H now asserts every declared trailer column has a producer, and was shown RED
  against this exact defect.
- **`running-totals` had been declared in the trailer schema since it was written and produced by
  nothing, ever.** No close has emitted it in its life. The schema's `REPORTED` kind says "printed and
  never enforced", which describes a column nothing READS — it has no word for one nothing WRITES.

**The two that corrupted the numbers the arc is measured by:**

- **`retired_verdicts` recorded verdicts nothing had acted on.** Reproduced: census 1 sweeps a doc
  WHOLE and its rows are verdicted; an edit adds one line; the re-census reads the now-touched doc
  DIFF-SCOPED, so every whole-scan row vanishes while the doc is still in the radius — "resolved: 3
  verdict(s) retired", none of them touched. It inflates `flips=`, **the input to D8's falsifier**, in
  the direction that keeps the hand phase alive on false evidence. The first fix (suppress every scope
  change) would have suppressed the genuine case too, which has the same shape; the right one asks the
  FILE — `whole_hashes(key)` — instead of asking this census's row set.
- **A `drift` row had no token but `NOT A LABEL`.** The skill said so in writing, and
  `docs_census_gate` declares `not-a-label=` to be the LABEL GRAMMAR's measured precision, so 31
  corpus-wide drift rows would have flowed into another instrument's error rate. A seventh token,
  `DRIFT OK`, now carries the symbol rung's own false-positive rate, refused in both directions.

**Three more, smaller:** `memref-dead` never looked at `[[wikilinks]]`, the dominant pointer form in
the files it guards (68 in MEMORY.md, 15 in CLAUDE.md; all resolve today, so the column read 0 by luck
rather than by construction); the `USER`+verbatim exemption existed only in `coverage()`, which the
close never calls, so at the one moment it could bind, nothing consulted it — and its window missed
the 3 entries where the quotation wraps to the next line, including the half that carries the user's
actual words; and `moved_and_cut` counted a clause relocated into `_archive/` as MOVED, when archiving
is retirement.

**One answered without a code change:** `accretion` is a ratchet a MOVE can satisfy — relocating the
271 lines took it 275 → 274 because the single hit inside them left the living scope. Honest here (that
text was always a dated build log), but nothing in the number distinguishes relocation from folding, so
`accretion_count`'s docstring now states the scope rule and says a falling close should say which it did.

**The method note worth keeping:** the critic's four and my five did not overlap at all. Reading my own
diff for defect CLASSES — a declared column with no producer, a drill arm whose green state a broken
field satisfies — found what a critic reading the same diff did not, and vice versa.

### 10.7 Round 2 — and the reframe that made round 1's fix one instance of a rule

Round 2 read `fff9ffeb` (round 1's fix) as hard as the original, which was right: **two of its four
questions were about the fix itself.**

- **The scope fix asked the wrong witness.** `whole_hashes` returned the hashes the GRAMMAR emits, and
  a `cite` row exists ONLY while its citation resolves dead — so RESTORING a cited file makes the row
  vanish while the doc line is byte-identical, and the verdict is recorded as acted-on for a line
  nobody touched. That is the same bias the scope fix had just removed, one layer down. Reproduced,
  then fixed by hashing every LINE (`status_grammar.line_hashes`) — **which also closed a latent bug
  nobody had asked about**: the occurrence ordinal advanced per ROW, so two identical lines were 1 and
  2, and if the first stopped producing a row the second silently became 1, changing its hash, losing
  its verdict AND recording it as retired.
- **A print where a refusal belongs.** `cut` and `lost` had no trailer column and no gate, while
  `ro-bytes` is RATCHETED and had just fallen 23,870 — so a close could earn the ratchet by deleting
  the user's own words, with the evidence printed after the fact. The ledger's own row says a detector
  that cannot prevent what it names is a post-mortem. **A lost USER record now REFUSES the close**
  (`ro-lost`, GATED); `ro-cut` is REPORTED, because a deliberate deletion is legitimate and losing the
  user's words is not.
- **The exemption measured an uppercase token, not a speaker.** Of the 60 lines in the reading order
  carrying a quotation, 8 were exempt; 14 are under a case-insensitive speaker test. The six it adds
  include the user's own Russian rejection of the browser design and their hands-on verdict *"obs
  issue is gone, imgui gets captured in all modes possible"* — both introduced by a lowercase "the
  user". Two of the six are collateral, and that is the right way to be wrong: over-exempting costs an
  explicit act, under-exempting deletes the user's words silently. Exempt clauses **15 → 25**.

**THE REFRAME (Q4).** Round 1 gave the drift rung its own token because answering drift with
`NOT A LABEL` polluted the label grammar's precision measure. Round 2 asked whether that was one
token's mistake or **the missing rule that every verdict token belongs to the rung that produced its
row** — and it is the rule: rows arrive from FOUR instruments, and `[V]` 16 of one census's 87 rows
were citation-rung rows, so `not-a-label=` was already three instruments' errors added together. The
hand still writes ONE rejection token; the MACHINE now attributes it (`not-a-label` / `not-a-cite` /
`drift-ok` / `not-loose`). **The seventh token is RETIRED whole (RULE 2)** — for a drift row, "the
pairing was a coincidence" and "this row is not a claim" are the same sentence, so it was a second
mechanism for one concept, alive for about three hours.

**A near-miss worth recording:** the edit that retired the token deleted the shipped `contra` refusal
along with it (they were adjacent, and the removal was made by index rather than by reading). The
drill caught it immediately — `STILL TRUE on a dead citation refuses` went red. That is the drill
paying for itself on the same day it was extended.

**NOT BUILT, in order:** D11 symbol-first corpus repair (the 31 drift rows are now surfaced, but
nothing proposes their fix in bulk); the remaining ~37 KB of reading-order MOVES, now instrumented but
not done. WP-E (convenience, §10.5) owes its own `/qf`. The DIFF pass continues — round 1 did not
converge.

**D8 — the falsifier standing behind "keep the hand phase":** over the first **300 ageing-lane rows**
(NOT closes — 251 retired-form closes peaked at 14 in ONE day, so a close is not a unit of evidence),
if `actually-done + stale-done + partial` totals fewer than **5**, the true rot rate is under ~3% at
95% and the hand phase is deleted. **`ageing-rows=` and `ageing-corr=` ride every trailer since
2026-09-03** — before that the counts mixed both lanes and no trailer carried the lane at all, so the
bar could not be evaluated from the record it is evaluated from. The fourth term it used to name,
`substate-stale`, had no recorder and needed none: a stale sub-state under a true label is a claim,
and its row is verdicted `STALE DONE`. §2.3's 36% predicts ~108; the round-15 proxy's 15%
predicts ~45. The one filed close contributes ZERO to that 300: its rows were drawn under the old
alphabetical selector.

### 10.5 CONVENIENCE is a real requirement (USER 2026-09-03) — and the design has no item for it

Round 10 retracted a Russian sentence about *"convenience of keeping documentation"* that rounds 7-10
had quoted as the user's verbatim ask: it exists only in my own briefs. When the retraction was put to
them the user answered **"that quote is nice, I'm not against it"** — so **the QUOTE stays retracted
(I wrote it, it is not a citation) while the REQUIREMENT is endorsed and dated 2026-09-03.** D0-D11
are all about HONESTY; not one of them is about the cost of using the thing.

**Measured on the first run under the new code (2026-09-03), from what I actually did rather than what
the skill says):**

| the cost | the evidence |
|---|---|
| the verdict column is not hand-fillable | I did not fill it by hand. I wrote `scratchpad/verdict.py` and ran it TWICE — editing 66 markdown cells by hand was not the path anyone took, including its author |
| the table is not readable as filed | throwaway Python written **six times** in one run to group rows by doc and print the LINE TEXT — the one thing needed at every row. `show` exists and prints a flat dump instead |
| the census is re-run per doc edit | three full runs this session, each ~30 s, of which ~26 s is a resolver index + corpora build over a tree that had not changed |
| the hand pass has no order | 66 rows arrived in table order; the reading order I actually used was BY DOC, which the table does not group |

**WP-E, not yet designed:** a `verdict` subcommand (by row, range, or predicate — the shape the ad-hoc
script took); `show` grouped by doc WITH the line text and the lane; a resolver cache keyed on the
tree's state; the pending table ordered by doc. Each is a measured cost, not a preference. It owes its
own `/qf` before it is built, and it does NOT displace D0 (the resolved ledger), which is a
correctness defect and stays first.

### 10.8 The move lane publishes, and it is audited for fidelity (2026-09-04)

D10 moved 271 lines out of `CLAUDE.md`'s entry `4e.` into the new `docs/signals/HISTORY.md`. `CLAUDE.md`
is UNPUBLISHED; `docs/signals/HISTORY.md` is tracked. **The move was therefore a publication, and
nothing in the lane knows that.**

`moved_and_cut` gates on `ro-lost` -- a clause that left the reading order and is findable in no doc --
because LOSS is the only failure mode a move is imagined to have. The mirror question (*did this clause
ARRIVE somewhere it may not be?*) is asked by no instrument, and the file's own header answered it on the
wrong axis: it noticed three sessions were off-topic and argued to keep them because *"moving them
elsewhere would have meant re-filing claims rather than relocating them"* -- a fidelity argument, correct
for a move, silent about publication.

Two of those three were the 2026-07-20 SECURITY sessions. Caught by the pre-push leak audit's axis 4
before any push, and fixed by rebuilding the 16 commits from the introducing one (5 doc'd SHAs rewritten)
rather than scrubbing the tip, because the blob lives in the intermediate commit. The cut material was
already filed and kept current in the local security tree -- so it was a DUPLICATE as well as a leak, and
one of its claims had been superseded there in the six weeks since, making the public copy stale in a way
the original was not.

The general check is cheap and is the proper fix: for a commit's added lines in TRACKED files, do any
also appear in an UNPUBLISHED tree (`CLAUDE.md`, the memory directory, `docs/security/`, `research/`)?
If so the close owes an explicit publication acknowledgement, exactly as it already owes `--new` for a
new doc. Rule: `[[feedback-push-leak-audit-service-ties-and-sha-rewrite]]` head 6.

### 10.9 DIFF pass round 4 -- four confirmed, and the fourth instance of the pattern (2026-09-04)

The pass's own pattern held again: **every round found the PREVIOUS round's fix incomplete in the same
way -- a rule applied to one DIRECTION, one SCOPE or one INSTRUMENT.** Round 4 found the biggest one.

| # | what the critic asked | measured | fix |
|---|---|---|---|
| Q1 | what stops the witness answering for an UNVERDICTED row? | `census_history.py:194` returns one line BEFORE `whole_hashes` is consulted, and the symbol `unverdicted` names no counter anywhere in `tools/docs/` | `lost_unverdicted()` asks the same witness for the empty-verdict case; the census PRINTS the docs; `ageing-lost=` rides the trailer |
| Q2 | does `not-a-cite=0` measure the rung, or which token the hand reached for? | 3 of the close's 10 `cite` rows were instrument error verdicted `STILL TRUE`. `git ls-files '*.hpp'` = **0** while the corpus cites dozens, so a four-file allowlist could never cover it; and `lessons_gate.py:379` matched by exact string, so `engine.hpp` could not reach its own `Engine.hpp` | the allowlist is consulted only AFTER resolution fails (so a pattern can never mask a file we have), case-insensitively, with fnmatch patterns; `*.hpp` is guarded by `hpp_premise_holds()` |
| Q3 | `ro-lost` guards CLAUDE.md -- what refuses when a USER line leaves MEMORY.md? | nothing. `mem-over200` is RATCHETED at target 0 and stands at 37, **11** of them USER-attributed; `memref-dead` falls the same way when a pointer is deleted rather than repaired | `moved_and_cut` takes a `select` region; MEMORY.md comes under the SAME refusal; `memory/*.md` joins both haystacks so a legitimate compaction still reads as MOVED |
| Q4 | is D8's independent unit the row or the doc? | 78 ageing rows over **25 docs**, top contributor 18 (23 %); the run's single finding was ONE doc worth 21 rows | `ageing-docs=` / `ageing-corr-docs=`; the bar restated over **100 DOCS, fewer than 5 corrected**, with row counts kept as texture |

**Q3 is the fourth instance, and the clearest statement of the pattern so far.** `ro-lost` was built in
round 2 for a real property -- a RATCHETED number may not be earned by deleting the user's own words --
and then left attached to the one FILE that prompted it, while the property belongs to every
deletion-earnable ratchet. Writing a rule and scoping it to its first instance is the same motion as
fixing the rejection side and leaving the acceptance side open (round 3), and as asking the grammar's
row set instead of the file (round 2).

**A near-miss caught by reading, not by a drill:** the widened MEMORY.md guard was first written
against `env.memory_dir`, and the attribute is `env.memory`. Behind `getattr(env, "memory_dir", None)`
the whole refusal would have been silently unreachable -- a gate that can never fire, the exact class
rounds 1-3 spent themselves on. Both new gates therefore ship with arms asserting them in their RED
state (`drill_mem_user_lost_refuses`, `drill_lost_unverdicted_is_counted`), never at a green value.

**MODULAR RULE -- two files are over the soft cap and owe an extraction (flagged 2026-09-04, not yet
done).** `status_census.py` is **1,273** LOC and `status_census_drill.py` is **1,225**, against the
800-line soft cap. Neither of round 4's fixes is a distinct subsystem (they are conditions wired into
existing flows), so the extract-first trigger did not fire -- but the flag is owed at the moment of
touching, not deferred to a catalog:

- `status_census.py` -> the natural seam is the one the file already has, **compute the table** vs
  **make the commits**. `run_close` and its refusals are a coherent second module (`census_close.py`),
  and the `e5b69aa1` extraction already proved the pattern by moving the private history out.
- `status_census_drill.py` -> split by what the arm DRIVES: the unit/fixture arms (A-F, the grammar,
  the resolver, the reading order) from the arms that run a REAL close end to end (G-I plus the two
  added here). The second group is where the runtime goes, and it is the group that keeps growing.

**AND THE GATE WAS TAKING 66 SECONDS, WHICH IS ITS OWN DEFECT.** Found while (wrongly) suspecting the
Q2 fix of a regression: the baseline was already 1m06s, so the reorder cost ~4s and the attribution was
mine, not the code's. The real cause is `resolve_cite`, which did a full `os.walk` of `CITE_ROOTS` for
EVERY basename citation -- `[V]` those roots hold ~53,000 files (research 24,433 / reference 12,433 /
src 11,226 / tools 4,962) and the ledger carries ~1,500 citations. That is exactly the shape
`docs/PERF_ARC.md` records for `FindFunction` walking `GUObjectArray` per lookup, in a dev tool instead
of a hot path. One walk into a basename index and a dict lookup: **66s -> 24s**, and the gate's full
output is BYTE-IDENTICAL before and after (control run with only the index reverted), so the index buys
speed and changes no verdict. It matters because a gate the operator is told to run at step 3.5 and CI
runs per push is one that gets skipped at a minute and run at twenty seconds.

### 10.10 DIFF pass round 5 -- the fifth instance, and it was mine twice over (2026-09-04)

| # | measured | fix |
|---|---|---|
| Q1 | `Resolver.external` matched by exact string while `lessons_gate.allow_match` matched by fnmatch+casefold -- `[V]` `trashBitsPile.hpp` / `engine.hpp` / `Engine.hpp` all census-**False**, gate-**True**. Worse: round 4's allowlist edit REPLACED the four explicit CXX names with `*.hpp`, so the census matched FEWER than before | `Resolver.external` delegates to `allow_match`. 7/7 agree; the retired predicate disagrees on 4, so the arm can fail |
| Q2 | `lost_unverdicted` copied `retired_verdicts`' "the line is still in the file" skip, whose justification INVERTS here. `[V]` the real stamp PREPENDED and left every line byte-identical (0 hashes unique to the pre-stamp text, 336 common) -- **the counter would have read 0 on its own founding incident** | the skip removed with its now-dead parameter; the arm rebuilt around the real scenario |
| Q3 | `hpp_premise_holds` asked `git ls-files` (**0**) while `_basename_index` walks the filesystem (**297** `.hpp`, all vendored). A submodule is never in the index, so the check was green by construction, forever | the premise walks the trees we OWN (`src`, `tools`); the renamed-vendored-header residual is stated rather than hidden |
| Q4 | `lessons_gate_drill.py` had **0** arms for the three predicates round 4 added, and `[V]` nothing in `.github` ran it at all | `drill_allowlist` (6 arms, RED controls), wired into `docs-census.yml`, plus a corpus-conditional SKIP so it passes where CI has no memory corpus |

**Q1 and Q4 are the fifth instance, and both are mine.** Round 4's Q2 was *"one counter, three instruments"*
and I fixed the matcher in ONE of the two modules that read the list -- while `status_grammar`'s own
docstring said *"one list for both instruments"*, which is exactly the claim that was false. Round 4's Q4
was *"CI never ran this drill"* and I added the census drill to CI while adding three refusal paths to a
sibling gate whose drill had no arms and no runner. **Sharing the DATA and forking the PREDICATE is the
same defect as two copies of the data**, and applying a lesson to the instrument that prompted it is the
same motion as scoping a rule to the file that prompted it (round 4 Q3).

**Two fixtures that tested a defect nobody had.** The lost-unverdicted arm passed while the counter was
broken, because it REWROTE the rows instead of prepending above them; and once that was fixed it still
proved nothing, because with 5 candidate docs the sweep RE-SELECTS everything every close, so the rows
came back renumbered rather than vanishing -- which the counter correctly does not report. The real
corpus's ordinary state is K=40 against ~1,600 docs, where a swept doc is not re-selected for tens of
closes; the arm now says so with `-k 0`. A fixture that does not reproduce the real edit is not a weaker
test, it is a test of something else.

**And the instrument refused a true anchor.** `tools/qf/ledger.py`'s `_LOC` had no `yml`, so a
`.github/workflows/build-core.yml:188` anchor was rejected as prose -- on the round whose finding was
that nothing in CI ran a drill. A gate that cannot express where a defect lives pushes the anchor
somewhere weaker; `yml`/`yaml` added.
