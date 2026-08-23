# Docs arc — the public-repo documentation audit (opened 2026-08-23)

> **Canonical LIVING doc for the arc.** The repo went PUBLIC on 2026-07-19 and the doc tree was
> never audited against that fact. This tracks WHAT the arc is, the work-package breakdown, and the
> current state of each WP. Keep it current when a WP moves; do not let a status label rot
> (the `/documentize` rule).
>
> Status tags: **DECIDED** (ratified by the user) · **AS-BUILT** (shipped + in tree) ·
> **IN PROGRESS** · **OPEN** (not started) · **DEFERRED** (later, by design).
> Evidence tags: `[V]` measured personally with a citation · `[A]` reported by an audit agent, not
> personally re-verified · `[?]` unverified.

---

## 0. What the arc IS (DECIDED 2026-08-23)

The user reversed an earlier inclination to pull `docs/` out of the public repo: **`docs/` STAYS
public.** Instead the tree is audited file-by-file for what does not belong in public, and the
reader-facing shape is fixed. Two halves:

1. **Subtraction** — the live exploit register (`docs/security/`) leaves the public tree, and the
   ~27 files that cite it stop advertising how many holes are open.
2. **Addition** — the tree gains the entry point a public repo needs (`docs/README.md`,
   `SECURITY.md`), because today it is written 100% inside-out with no reader path.

**Three user decisions, all DECIDED 2026-08-23, verbatim in substance:**

- **History is NOT rewritten.** *"Публично уже что на то пофиг, историю не меняем."* No
  `filter-repo`, no force-push. Snapshot removal only.
- **`docs/security/` leaves public.** *"уберем security из паблика."*
- **The holes get closed afterwards.** *"и просто далее дыры безопасности закроем."* That is the
  real fix; this arc is the wrapper, not the cure — see §5.
- Some WPs may want a `/qf` pass before implementation. Marked per WP below.

---

## 1. The audit finding (measured 2026-08-23)

Swept all **131 tracked files** under `docs/` (~3 MB).

**No credential or infrastructure leak** `[V]`: the only IP-shaped strings in tracked docs are
`1.1.1.1` (LESSONS.md:727), `1.2.3.4` (a PLAN_01 example) and `4.27.2.0` (the engine version). The
2026-07-24 A11 fix (origin VPS IP purged in favour of `<coop-vps>`) still holds. `re-artifacts/` is
untracked (`git ls-files docs/piles/re-artifacts` → 0). No key material, no `.pem`, no real host
except `master.multivoid.dev`, which is public by necessity.

**The finding is `docs/security/`** — 14 files, ~130 KB, a working exploit map for a SHIPPING mod.
`TRACKER.md` carries 16 OPEN rows with exact `file:line` and an explicit "attacker → gain" column:

| Row | What a stranger gets, per our own register |
|---|---|
| `A3` | irreversibly wipes another player's persistent world — and the forged destroy is **fanned out to other clients before the host validates it** |
| `A4` | drives another player's desk mid-session, deletes their email, wipes DB rows, unlocks doors |
| `A1` | **evicts the real host and receives every joiner** (signaling identity is self-asserted) |
| `W3` | remote OOM grind — still OPEN on purpose (a sized window would be a RULE-1 crutch) |

The register is exactly the ammunition `docs/DEVS_GAUNTLET.md` warns about: the VOTV devs named
security as the make-or-break — *"you can have the greatest intentions but if you do it wrong, you
put people in harms way and your game is DOA."*

**Three facts that shaped the plan, all `[V]`:**

1. **It has been public for ~34 days.** Added `d95683cc` (2026-07-20 16:13), last touched
   2026-07-30, every commit on `origin/main`. Removing it from HEAD does not unpublish it; the user
   has accepted that and declined a history rewrite.
2. **It bleeds through citations.** 27 tracked files outside the directory reference
   `docs/security/`, and several **restate the severity in public prose** — `ROADMAP.md:516`
   ("kill a joining client with one packet — see the ranked list"), `MULTIPLAYER_UI.md:427`
   ("TRACKER = 20 OPEN findings"), plus rows in `LESSONS.md` and two `research/findings/*` designs.
   Cutting the directory alone would leave 27 dangling pointers that advertise the hole count.
3. **12 of those 27 are SOURCE files** carrying `// docs/security/TRACKER.md W1`-style comments over
   a shipped fix. **Those stay.** Attribution of a CLOSED finding is good practice and reveals
   nothing; only OPEN-count advertising is the problem.

**Honest framing (RULE 1).** Removing the document is a filter, not a root fix. The attack surface
does not shrink by one byte. It is still correct — a ready-made map is not the same as the surface —
but this arc may never be described as "security fixed". §5 is the cure.

### 1b. Minor scrubs found in the same sweep

| Where | What | Why |
|---|---|---|
| `docs/piles/findings/votv-snapshot-adoption-root-causes-2026-06-10.md:618` | an absolute `C:\Users\<login>\AppData\Local\Temp\...` path | leaks the local account name **and** is a dead pointer (that temp file is long gone) |
| `docs/piles/findings/voidtogether-adoption-plan-2026-05-25.md:4` | calls another VOTV multiplayer mod a **"rival"**, next to a quoted "we can take CHAT, PERMISSIONS" | reads publicly as lifting a neighbour's work. **Verified `[V]` no code was taken** — `src/` contains exactly three attribution comments (`player_handshake_nick.cpp:80,106`, `engine_widget.cpp:128`) and nothing else |

### 1d. `research/` — a RECORDED user decision that was never executed, and a live leak inside it

While sweeping the citations, the trail led to a **decision already taken and never carried out**.
`research/findings/tooling/votv-ue4ss-f2-migration-DESIGN-2026-08-21.md:520-524` records, verbatim:

> **USER 2026-08-21:** *"research folder also get fully offline from public repo, what else?"*

The census answering it concluded `research/` (then 345 md files) goes **private in place** — its own
git repo at the same disk path, the public repo `.gitignore`s it and drops it from the index, exactly
the `site/` pattern, so **zero reading-order pointers break**. That was written, ratified, and then
not done: on 2026-08-23 the corpus was still fully tracked at **420 files / 24 MB** `[V]`.

**What the delay cost — measured 2026-08-23 `[V]`:**

| Exposure | Where |
|---|---|
| **The origin VPS IP, in the clear** | `research/crash_2026-07-03_rehost_wispkill/host_votv-coop_2026-07-04_rehost_plain.log:11085-11088` (four lines: `stun:`, `turn:`, `signaling: resolved`, `net: P2P host listening ... via signaling`) and `research/findings/computers-devices/votv-connection-selector-events-pinecone-2026-06-11.md:225` |
| **3 × `UE4Minidump.dmp`** | `research/crash_2026-07-03_rehost_wispkill/UE4CC-Windows-*/` — process memory dumps of the maintainer's own machine, contents never audited |

The IP is **the same class as finding A11**, which purged that exact address from four tracked docs
on 2026-07-24 — and whose sweep never crossed out of `docs/`. Publishing the origin defeats the root
domain's Cloudflare proxying outright. *No literal secrets were found alongside it:* grepping the
tracked logs for `signalingToken` values, TURN credentials and `Authorization:` headers returns
nothing `[V]`.

**Decision (2026-08-23, autonomous under the user's standing instruction to decide and use `/qf` when
needed — no fork here, the decision already existed):** execute the 2026-08-21 plan as written.
Recorded in this arc rather than in the migration design, because that doc is a point-in-time record.

**This supersedes one part of that census and not the rest.** The same 2026-08-21 pass also proposed
moving **92 of 129 `docs/` files** into the private corpus and unpublishing the AI-process exhaust
(`docs/LESSONS.md`, `docs/OPUS_48_DISCIPLINE.md`). Both are **CANCELLED**:

- the docs move is cancelled by the user's 2026-08-23 reversal (*"Я передумал убирать папку docs из
  публичного репозитория. Оставляем"*) — this arc replaces a wholesale move with a content audit;
- unpublishing the AI-process docs is cancelled by a standing user rule — the user does not hide
  working with Claude (`[[feedback-commits-and-pushes-only-as-pelmentor]]`; the README credits it,
  the commit trailer keeps it). Hiding the process docs would contradict a position held on purpose.

### 1c. What STAYS, and why it is an asset

The RE docs, the two cross-cutting maps, `piles/`, `kerfur/`, `items/`, `events/`, `LESSONS.md`,
`DEVS_GAUNTLET.md`, the process docs. A public mod repo that shows its measurements is the
credibility argument; nothing here helps an attacker who does not already have the binary.

---

## 2. What readme.com's guidance actually changes for us (assessed 2026-08-23)

The user asked whether <https://docs.readme.com/main/docs/about-readme> teaches anything our docs
lack. Read `about-readme`, `structuring-your-docs`, and their best-practices blog post.

**Caveat first:** readme.com is a SaaS for **API reference** hubs. Half their rules (multi-language
snippets, endpoint/error sections, traffic metrics, their AI editor) do not map onto an engineering
doc tree for a game mod. Do not import those.

**Where we already exceed their advice** — no action:

| Their rule | Our practice |
|---|---|
| #7 docs-as-code ("the further docs live from code, the faster they rot") | docs ship in the same repo and usually the same commit as the code |
| #9 "quarterly audits of stale content" | `/documentize` Step 0.5 verifies **every status label against the code in both directions**, per session |
| #6 docs in the release workflow | `docs/RELEASE.md` step 0 + `tools/release/tripwires.ps1` machine-checks it |
| #13 changelog discipline | `tools/release/notes/b<N>.md` + `ledger_lint` in CI |
| #12 structure for LLM consumption | `CLAUDE.md` reading order + `MEMORY.md` + `docs/LESSONS.md` |

**Where they land a real hit — one rule, and it is the important one:**

> *"The biggest mistake documentation teams make is describing it from the inside out, starting with
> how it was built rather than how developers need to use it."*

Our tree is **100% inside-out**. 131 files, 3 MB, and **no `docs/README.md`** — no entry point at
all. The only navigation is the reading order in `CLAUDE.md`, which is addressed **to Claude**, not
to a human. Nine of thirteen subdirectories have their own README; the root does not.

There is also **no audience separation**: player-facing (`INSTALL.md`), contributor-facing
(`ARCHITECTURE.md`, `RE_WORKFLOW.md`, `COOP_SCOPE.md`) and maintainer-only material
(`LESSONS.md` at 371 KB, `piles/`, session logs) sit in one flat pile. That was fine while the repo
was private. Publicly it means a visitor's first click is a 371 KB engineering ledger.

**Adopt exactly two things** (WP-5): a root map with three reader lanes, and upward backlinks from
the subdirectory READMEs. Everything else from that source is declined on the record so it is not
re-derived.

---

## 3. Work packages

| WP | What | Status | `/qf`? |
|---|---|---|---|
| **WP-1** | `docs/security/` out of the public tree: `git rm --cached -r`, enforced `.gitignore` block, files stay on disk so reading-order 4f keeps working locally | **AS-BUILT** | no — mechanical |
| **WP-1b** | `research/` private in place (the §1d decision): untrack + `.gitignore` + its own inner git repo, no remote | **AS-BUILT** | no — the decision pre-existed |
| **WP-2** | The citation bleed: public prose keeps the architectural statements, drops the open-hole advertising. **Source-file citations NOT touched** | **AS-BUILT** for living docs; dated records deliberately untouched (see below) | no |
| **WP-3** | Root `SECURITY.md` — the public vulnerability-reporting policy. This is the correct public replacement for a private register | **AS-BUILT** | no |
| **WP-4** | The §1b scrubs (local path, "rival" wording + an explicit no-code-taken line) | **AS-BUILT** | no |
| **WP-5** | `docs/README.md` — the three-lane reader map (I play / I want to help / I maintain this) + upward backlinks from the subdir READMEs | **OPEN** | maybe — the lane split is a product decision about who we expect |
| **WP-6** | Close the OPEN security findings themselves | **DEFERRED to its own arc** — see §5 | yes, per finding |

Ordering ran WP-1 → WP-1b (promoted the moment the leak was measured) → WP-4 → WP-3 → WP-2 → WP-5.

### WP-2 — the keep/cut rule that was applied

Decided autonomously under the user's standing instruction; recorded so the next sweep does not
re-litigate it.

- **CUT** from public prose: how many findings are open, a specific *unfixed* attack and its effect,
  and any "see the ranked list" pointer. Worst two fixed: `docs/ROADMAP.md` (a five-clause
  mini-tracker inside a dated entry) and `docs/MULTIPLAYER_UI.md` ("TRACKER = 20 OPEN findings").
- **KEEP**: architectural statements (they are the engineering value), honest non-actionable posture
  ("this does not authenticate *who* a peer is" — the same thing `SECURITY.md` now says out loud),
  and finding IDs cited **over a shipped fix**.
- **NOT TOUCHED**: the 12 source files carrying a `// docs/security/TRACKER.md W<n>` comment above a
  fix, and the **dated point-in-time records** under `research/` — rewriting a dated record to look
  better in hindsight is the opposite of this project's own convention (the voidtogether doc says so
  in its own header). WP-1b unpublishes them anyway, which is what actually resolves those.
- Where an unpublished path is still the right *local* pointer it stays, and the doc gets **one
  local-only banner** instead of a scrub on every line: `docs/LESSONS.md` §9, `docs/RELEASE.md`,
  `CLAUDE.md` 4f.

---

## 4. Execution log

| Date | WP | What happened | Commit |
|---|---|---|---|
| 2026-08-23 | — | Arc opened; audit measured; user decisions recorded in §0 | this doc |
| 2026-08-23 | WP-1 | `docs/security/` untracked (13 files) + enforced `.gitignore` block; `CLAUDE.md` 4f marked local-only | see §4 commit |
| 2026-08-23 | WP-1b | `research/` untracked (433 index entries, 24 MB) + `.gitignore` block + inner repo `f69be11` (8,138 files, **no remote**). A stray Windows-reserved `research/pak_re/nul` blocked `git add` and was deleted (0 bytes, junk from a `> nul` redirect) | see §4 commit |
| 2026-08-23 | WP-4 | Local-account path scrubbed from a piles finding; "rival" softened + a measured no-code-taken statement added to the VoidTogether doc | see §4 commit |
| 2026-08-23 | WP-3 | Root `SECURITY.md` written: private-advisory reporting, scope in/out, and an honest "what holds / what does not" section | see §4 commit |
| 2026-08-23 | WP-2 | Citation bleed swept per the rule above across `ROADMAP`, `MULTIPLAYER_UI`, `COOP_SYNCER_MODEL`, `COOP_SERVER_MODEL`, `COOP_EVENT_JOIN`, `LESSONS` §9; two now-dead `research/` links removed from `README.md` | see §4 commit |

---

## 5. What this arc does NOT do

**It does not close a single security hole.** As of this writing `docs/security/TRACKER.md` stands
at 16 OPEN rows, four of them CRITICAL. The user's decision is explicit: pull the register out of
public *and then* close the holes. That second half is a separate arc with its own `/qf` per finding
and the fix order the `PLAN_*` files already rank (P1 peer auth → A1 signaling identity → A3/A4
authority-on-receive → W3 net-thread Begin latch).

Until that arc lands, the honest statement about Multivoid's security is the one already written in
`docs/security/THREAT_MODEL.md`, not an absence of documentation.
