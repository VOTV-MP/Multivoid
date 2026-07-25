# CI autobuild + dev-release design (PR #2 basis) — CONVERGED ("that holds" at R29)

> 2026-07-25. **DESIGN, CONVERGED — not built.** Pass 1 (/qf 15) hit its cap NOT converged; the
> confirmation pass (R16-R29, fresh critic each round) closed 13 more rounds of material spec holes
> and ended in a genuine "that holds" at R29 (the critic independently re-walked the predicate and
> found no cross-answer contradiction). Remaining unknowns are NAMED and assertion-gated at first
> real use (i1 vcpkg drift, i2 reusable-checkout semantics, i6 ref-dispatch source, the D8
> fork-push probe) — none blocks §5 step 1. **NEXT = build per §5.** Thread: session scratchpad
> `qf_thread.md` (R1-15 + injection + R16-29); THIS DOC carries the substance, §6 the round digest.

## 1. What the user actually asked for

GitHub user **huoyan1231** opened PR #2 ("add autobuild", +129-line `.github/workflows/build.yml`:
nightly cron on windows-latest + vcpkg, DLLs as login-gated Actions artifacts, Chinese comments).
The owner asked (RU, compressed): do we need it; will we go into GitHub debt (org repo, "как mtablue");
"Я тоже бы хотел билды типа dev experimental выпускать в релизах, но это только dev experimental, это
неполноценный релиз. Как назвать его тоже надо подумать ... или может короче".

USER DECISIONS (all 2026-07-25, in-session):
- Billing fear resolved: public repos = free unlimited Actions (billing is private-repo only). No debt possible.
- Nightly cron rejected ("нам не надо билдить каждую ночь ведь это тупо").
- Name = **"dev"** (approved from the recommendation set). Tag format `v<game>-b<N>-dev` + title
  "Multivoid <game> b<N>-dev" — user-approved.
- Moving-target CI-rebuilt bytes for dev releases — user-approved.
- The human-only consume ritual (R6 reframe) — user-approved.
- **MANUAL-ONLY builds**: "можно сами будем когда захотим запускать билд процесс — так лучше".
- PR #2: **accept + hard-edit INSIDE it** ("мы примем его и свою редактуру жесткую проведем").

## 2. Measured fact base

- **PR #2 diff read in full**: nightly cron + dispatch; selective submodule init `src/votv-coop/third_party`
  only (reference/* never fetched); opus clone fallback citing BUILDING.md; vcpkg C:\vcpkg clone+bootstrap+
  unshallow, skip-if-exists; static cache key; vcpkg_installed keyed on hashFiles(vcpkg.json); vswhere
  generator detect; Release build; upload Release/*.dll; no secrets; only github.com clones. Author
  demonstrably built the project post-cc0ab911 [inferred from real-error citations in his comments].
- **ParseBuildNumber** (`src/votv-coop/src/loader/xinput_proxy.cpp:47-58`): requires trailing `-<digits>.dll`
  else -1/never loaded; scan pattern `multivoid-*.dll` (:88); highest build wins (:100-105); reads ONLY the
  trailing digits => build numbers collide ACROSS game targets; a `-dev` filename suffix would never load.
- **CMake identity** (`src/votv-coop/CMakeLists.txt:16-37,566-570`): OUTPUT_NAME `multivoid-<game>-<build>`;
  kProtocolVersion regex-parsed from protocol.h; configure re-runs on protocol.h change. kProtocolVersion=125.
- **docs/RELEASE.md**: every release bumps proto; publish DLL + xinput1_3.dll + SHA256; step 5 = master env
  op (`COOP_LATEST_PROTO/MOD`) — STABLE-only by design; ZERO releases shipped yet.
- **master.rs:64-74,774-780**: `/v1/latest` serves env constants; the master NEVER scrapes GitHub.
- **session_manager.cpp:210-240**: client verdicts — proto<=0 silent; ==ours "(latest)"; >ours amber UPDATE;
  <ours **"(dev; latest released bN)"** — an in-game dev surface ALREADY EXISTS, computed relationally,
  never stored.
- **STALE LITERALS FOUND (fix owed)**: pre-rebrand `github.com/pelmentor/VOTV_MP/releases` at
  `session_manager.cpp:230` AND `:463` (s29b sweep miss; only these two repo-wide — verified by grep).
- **.gitmodules complete**: 4 build gitlinks (GNS fa489fd/v1.5.1, imgui, minhook, opus) all registered
  (opus since cc0ab911, on origin/main); freetype/miniaudio = vendored trees (040000), no init needed.
- **gh auth status**: token scopes include `workflow`. **gh pr view 2**: `maintainerCanModify: true`.
- Platform facts: public Actions free; `releases/latest` excludes prereleases/drafts; GITHUB_TOKEN pushes
  suppress workflow triggers; workflow_dispatch targets base-repo refs only; on push/tag events the
  event's commit supplies the workflow YAML; tags/releases are deletable objects; scheduled workflows
  auto-disable after 60 idle days (moot — no cron). **R28 MEASURED (GitHub docs, verbatim) — i5 and i3 promoted from inferred: "Each workflow run will
  use the version of the workflow that is present in the associated commit SHA or Git ref of the
  event" (the trampoline's entire justification, now measured); caches: "Workflow runs can restore
  caches created in either the current branch or the default branch", "cannot restore caches
  created for child branches or sibling branches" (contrib-branch isolation confirmed; a contrib
  branch CAN read main's caches — read-only, harmless; releases are cacheless regardless).**
  **R24 MEASURED (GitHub docs, verbatim):
  workflow_dispatch "will only trigger a workflow run if the workflow file exists on the default
  branch" -> build.yml must be on main before any dispatch (R25: it arrives there via the PR #2
  merge, §5 step 4). i6 INFERRED (R25 reword): a ref-dispatch builds the REF's SOURCE — asserted
  via the artifact ZIP's recorded commit sha == the mirror branch head (an echo marker would be
  vacuous: sanitized workflows are byte-identical to main's).**
- vcpkg.json: builtin-baseline 9b965a11; protobuf 3.21.12 override (double-abseil avoidance).
  BUILDING.md: MSVC match between vcpkg-protobuf and project generator is load-bearing
  (`__std_*_trivial_N` link errors).

## 3. The design (v15)

### D1. Triggers
- MTA divergence note (R28, per the 2026-05-28 rule): mtasa-blue ships continuous CI NIGHTLIES; we
  deliberately diverge — MANUAL-ONLY builds by the user's decision ("сами будем когда захотим
  запускать билд процесс"); the project's release cadence is human-paced, and the nightly channel
  was explicitly rejected ("билдить каждую ночь тупо").
- CI builds: **workflow_dispatch ONLY** (user decision). Contribution CI (R24 rework — the old
  "review before DISPATCHING" gated the wrong moment: the mirror fetch-push is ITSELF a push event,
  and a contributor-added `on: push` workflow would execute at that instant, before any review):
  **`tools/release/mirror_pr.ps1` with SANITIZE-BY-DEFAULT** — fetch `pull/N/head`, REPLACE
  `.github/workflows/` with main's copy, push the mirror branch (dispatch can't see fork refs). A
  contributor `on: push` workflow then never EXISTS on the pushed branch — capability-level, not
  intention-level (the R10 principle). Explicit `-KeepWorkflows` flag = only for maintainer-authored
  workflow content after line-by-line review (checklist includes "check the `permissions:` key" —
  R21); the script REFUSES to push a workflow delta without the flag. Full-PR-diff review happens
  BEFORE the mirror-push. Branch creator deletes the branch after the verdict (cache scope dies
  with the ref).
- Releases: tag-push `v<game>-b<N>[-dev]` -> a THIN TRAMPOLINE YAML that calls the reusable
  **release-core @main** (refusal logic always executes from main HEAD — the tag's own YAML would judge
  retags with OLD logic). Second entry point: `workflow_dispatch(tag)` for no-run recovery.

### D2. CI build job (PR #2 steps as basis, reworked)
English comments; vcpkg at OUR OWN path (never C:\vcpkg — the runner preinstalls a drifting copy there
[inferred, confirm at acceptance]) pinned to an explicit vcpkg release tag; builtin-baseline stays the
package pin; full-history clone (versions DB needs it); cache keys include ImageVersion + detected VS
toolset (closes the stale-cache MSVC trap; the MSVC match is INTERNAL to one run — green does not require
VS18 specifically); submodule init `src/votv-coop/third_party` only; artifact ZIP name carries the commit
sha (DLL inside stays canonical); artifacts = CI evidence, NOT distribution (documented unsupported);
payload DLL + xinput1_3.dll; 30-day retention; the LEDGER LINT (see D3) runs here too.
Acceptance gates (R24/R25 rework — MEASURED platform fact: "This event will only trigger a workflow
run if the workflow file exists on the default branch" (GitHub docs, workflow_dispatch), so "green
dispatch BEFORE landing on main" was physically impossible as written and is RETIRED in §5 step 4):
(1) build.yml reaches main via the PR #2 merge; the gate = one green dispatch of build.yml@main.
i6 (R25 reword — the echo marker was vacuous under sanitize, whose workflows are byte-identical to
main's): what matters is "a ref-dispatch builds the REF's SOURCE"; asserted via the artifact ZIP's
recorded commit sha == the mirror branch head, at first contrib use (or trivially at acceptance).
(2) one-time CI-bytes smoke (download artifact -> deploy to install folders -> standing LAN smoke)
on a CACHELESS run, before the FIRST dev release.

### D3. Release-core (@main)
- **CACHELESS cold build** (kills fork-branch cache poisoning + cleaner provenance; releases are rare).
- **SELF-CHECK every run**: checkout#1 HEAD == tag sha; checkout#2 HEAD == origin/main HEAD (checkout
  semantics in reusable workflows = inferred; first drill measures it, every run re-asserts).
- **Early fingerprint refusal**: runner's MSVC toolset + Windows SDK (vswhere/env dump) **+ the
  sha256 of build-core.yml at main HEAD (R23 — the build PATH is part of "what was proven
  runnable"; a human edit to build-core between the smoked run and a release refuses with "build
  path changed — re-smoke + re-commit fingerprint")** vs the committed fingerprint file read from
  MAIN HEAD (semantics: "the runner toolchain + build path whose CI bytes were last proven
  runnable" — a property of NOW; a mid-ritual image roll -> refusal -> re-smoke -> fingerprint
  commit -> plain re-run; numbers never burn from rolls). Edits to release-core/trampoline (which
  produce no bytes) stay under the human-sole-owner residual, NAMED in D7.
- **Refuse-to-publish preconditions** (R16 EXACT PREDICATE, R17-refined — supersedes the earlier
  "unique vs union" wording, which on a literal read self-refused every legitimate release):
  `state(N)` = fold of the ledger rows carrying number N, in file order: `consume {sha,game}` ->
  EXPECTED(sha,game); `published` -> PUBLISHED (closed, API-free); `burn` / `retracted` -> TERMINAL;
  a second consume over an unclosed one = lint FAIL (ambiguous mint). Publish allowed IFF
  `state(N) == EXPECTED` AND tag sha == row sha AND tag game == row game AND no OTHER tag/release
  carries N besides the triggering tag + this run's own draft. The matching consume row is the
  EXPECTED set, not the blocking set; blocking = any terminal/PUBLISHED row, a mismatched consume, a
  foreign tag/release. Plus: tag-commit proto == N == tag number; main HEAD proto > N; retag allowed
  only TOWARD the row's sha; absent ledger = valid empty start. **The judge evaluates ALL
  preconditions with NO short-circuit and emits the full verdict vector** (one labeled PASS/FAIL line
  per check) — so a drill asserts its named line and an early failure can never mask the branch under
  test (R17). **Idempotent completion (R18 rewrite — the SHA256-match version was structurally dead: D6's
  moving-target bytes mean a re-run's rebuild never matches the published asset)**: completion is
  judged by tag-ASSOCIATION, not bytes — if a LIVE release already sits on the triggering tag and the
  source sha recorded in its body == tag sha, the run emits `ALREADY_PUBLISHED` and no-op succeeds
  (assets never touched); if the recorded sha mismatches, `RELEASE_TAG_MISMATCH` = FAIL reconcile
  (human). The body is written by the publish step with FIXED machine keys (`source: <sha>` +
  `sha256: <hash>` — one format shared by the publish step, the completion check, and the
  RELEASE.md template); a live release on the triggering tag whose body has NO parseable key ->
  `RELEASE_BODY_UNPARSEABLE` = FAIL reconcile — fail-closed, NEVER a fall-through to a fresh
  publish over a live release (R22). The release-body SHA256 is the byte-binding written AT publish
  time; it is not re-derivable and not re-checked. PRINCIPLE (R18, per one-cache-per-question): the ledger owns "MAY
  b<N> be published" (the uniqueness invariant); the release object owns "has THIS tag's publish
  COMPLETED" — reading the run's own work product is not gating the invariant on the API.
- **LEDGER** (file on main; the single mint authority for dev AND stable; append-only, HUMAN-written):
  grammar `consume | published | burn | retracted` (the `drill` class was REMOVED in R17 — drills use
  real numbers via the real ritual, so no writer remained; RULE 2). **ROW SCHEMA (R19 — restores the
  R10 tuple the R16 compression lost): every row = `{kind, N, game, tagName, sourceSha, date}`;
  tagName carries the dev/stable axis (bare vs -dev). verify_latest.ps1 reads the newest `published`
  row with a BARE tag (closure, not expectation); a RELEASE_TAG_MISMATCH reconcile reads "what was
  published" from the same row.** consume = the mint expectation
  (in-flight until the release page is live); **published** = the HUMAN-appended closure row written
  at the ritual's final step after watching the run green + verifying the release page — state(N)
  closes API-free (R17; the GitHub API is a DRIFT DETECTOR only, per the ledger principle; the row
  may ride the next leak-audited push — until then the lone-consume WARN covers the window);
  **burn / retracted = TERMINAL forever** — a retracted number never republishes (bytes were pulled
  for a reason; fixed bytes take a NEW number via a new consume), and dispatch-recovery on a tag
  whose N is terminal refuses with `N is terminal (<class>)` (R16). burn = never-published number
  killed; retracted = published-then-deleted (bytes were public). **TERMINAL rows are PUSH-IMMEDIATE
  (R19)**: until the row is on origin, the invariant lives only in the deletable API — exactly the
  hole the ledger exists to close. Retraction procedure = delete the release + delete the tag
  (admin; defence in depth — without the tag, dispatch-recovery has no target) + append the row +
  push NOW (a one-line ledger push is trivially leak-auditable; the batch-cadence exception is
  justified). Same for burning a consumed N whose tag exists. **STABLE retraction extra (R23):
  roll the master env constants back to the previous stable's values (or clear if none) + re-run
  verify_latest — and verify_latest's query is FOLD-AWARE: "the newest bare-tag row whose
  state(N) == PUBLISHED" (a retracted N has a published row too; the terminal closes it).** The `published` row alone keeps the
  batch cadence — R21 invariant analysis: the consume row + tag reach origin ATOMICALLY at ritual
  start, so uniqueness already holds on origin (EXPECTED blocks any foreign mint via sha-mismatch +
  foreign-tag; a re-run lands on ALREADY_PUBLISHED no-op); a clone lost pre-push costs only a
  liveness nag (lone-consume WARN + verify_latest complaining), never safety — and the
  "release-deleted + row-unpushed" case IS retraction, which is push-immediate. The ritual
  RECOMMENDS pushing the published row right away while watching the green run (zero extra cost);
  it is REQUIRED only for terminals.
  **LINT** (every dispatch build + every release run): burn x live-release = FAIL reconcile; published
  release without matching consume row (sha) = FAIL; aged lone consume = WARN "annotate". Uniqueness rides
  ledger-RECORDED history, never the deletable API.
- **DRAFT-FIRST publish**: draft -> assets -> SHA256 verify -> flip (prerelease for -dev, full for bare).
  READ-BACK asserts after flip: release.prerelease == tag shape; `releases/latest` != this dev tag —
  TRI-STATE with labeled vacuity (LATEST_404 = no stable yet, logged, never silent-green; LATEST_OK_DIFFERENT;
  LATEST_IS_THIS = FAIL). Re-runs delete stale DRAFTS only; published releases are never workflow-deleted.
- **Global concurrency group "release"** (cancel-in-progress: false) — full serialization.
- **Where the judge lives (R16)**: ALL refusal/lint/predicate logic is in `tools/release/*.ps1`
  executed from checkout#2 (main HEAD); the release-core YAML is a THIN orchestrator (checkouts,
  script invocations, publish steps — no branching logic). This makes the existing self-check
  (checkout#2 HEAD == origin/main HEAD) guard the JUDGE, not just the evidence. The run additionally
  logs `github.workflow_ref` + `github.workflow_sha` as evidence (not a gate — a gate cannot guard
  itself; that residual is D7(a), accepted).
- Regex selftest fixture: MUST-MATCH (v0.9.0n-b122, v0.9.0n-b123-dev) + MUST-REFUSE near-twins
  (v0.9.0n-b12-devx, v0.9.0nb12, b12dev, bare b122, leading-zero).
- Release body: source commit + SHA256 (+ "development build, not hands-on verified" for dev).
- STABLE extra: workflow summary prints the exact master-env values to paste; the rewritten RELEASE.md
  checklist ends with `tools/release/verify_latest.ps1` (curl /v1/latest, compare proto+mod to the newest
  `published` row with a BARE tag — the R19 schema; the old "newest STABLE consume row" wording is
  superseded (R20); drilled to FAIL pre-env-step and PASS post-env-step).

### D4. The ritual (human consumes, robot verifies — R6 reframe, user-approved)
Tag HEAD (proto N) -> commit "consume b<N>" (bump N->N+1 + ledger row) -> `git push --atomic origin main <tag>`
(one leak-audited push) -> WATCH the run to green; the checklist is not done until the release page shows
the asset + SHA256 -> **append the `published b<N>` ledger row** (closes state(N) API-free; rides the next
leak-audited push — R17). **Failure surface (R11, spec'd R17): watching-to-green IS the primary surface
(the ritual is synchronous; the checklist stays open until the page is verified); GitHub's default Actions
failure email to the admin is the backstop; no custom notification machinery is built.** Refusals are
stateless (plain re-run); wrong-commit tag -> retag toward the row's sha pre-publish; wrongly chosen
number -> BURN row (append-only never reversed; numbers are cheap). No robot writes to main, ever (the
R3-R5 auto-bump machinery was DISSOLVED by the reframe — do not build it).

### D5. Naming (user-approved)
dev is OUTSIDE the identity: the (game, build) pair + ledger uniqueness maps b<N> 1:1 to one release page.
DLL filename stays canonical `multivoid-<game>-<build>.dll` [measured: loader rejects suffixes]. No new
in-game dev marker (a second version axis = the retired mod-semver mistake); the existing relational
"(dev; latest released bN)" line covers the surface. COOP_LATEST_PROTO stays stable-only.

### D6. Bytes (user-approved tradeoff)
Published bytes = CI rebuild of the tagged source (public Actions provenance; moving target accepted).
Human gate: dev tag only after the standing local pre-handoff checklist on the same commit. Fingerprint
updates are human-committed after re-smoking CI bytes.

### D7. Security posture
No repo secrets anywhere. Release job `permissions: contents: write` (Release API only). **v* TAG RULESET**:
creation/move restricted to the repo admin, github-actions excluded (robot capability-restricted, not just
intention-restricted). Main protection: force-push OFF, deletion OFF, require-PR OFF (the daily direct-push
flow survives). All third-party actions PINNED by full commit SHA. NAMED RESIDUALS (accepted, documented):
(a) the release token could push main = rewrite its own refusal authority — mitigations: push-restriction
ruleset attempted at acceptance (dropped if it frictions the daily flow), SHA-pinned actions kill the
supply-chain vector, any such push is a visible github-actions commit caught by the daily pull;
(b) contrib-branch caches are branch-scoped [inferred] and releases are cacheless anyway;
(c) R23: human edits to release-core/trampoline (no bytes produced) are ungated by the fingerprint —
covered by "human sole owner of main" (any robot edit would be a visible github-actions commit);
build-core edits ARE gated (its sha256 is part of the fingerprint);
(d) R24: "the human reviews a -KeepWorkflows mirror correctly" — bounded by sanitize-by-default
(the unreviewed path cannot execute contributor workflows at all);
(e) R27, DELIBERATE: the judge scripts (`tools/release/*.ps1`) are NOT fingerprint-gated — gating
the judge by its own hash is self-referential theater (the refusal is executed BY the judge; a
malicious edit skips the hash check too — same class as (a): a gate cannot guard itself). Coverage
= human-sole-owner of main + robot edits are visible github-actions commits + SHA-pinned actions.

### D8. PR #2 handling (user-decided)
ACCEPT + hard-edit INSIDE the PR: our rework commits pushed onto huoyan1231's branch. R18 provenance
correction: the FLAGS are measured (token has `workflow` scope; maintainerCanModify true) but the PUSH
itself is UNVERIFIED — pushing workflow-file edits onto a fork branch is historically its own
permission surface. The FIRST rework push IS the viability probe (it is the first build step anyway;
no throwaway test commit is sent to the contributor's branch). On refusal the fallback engages:
cherry-pick his commit (authorship preserved), supersede + close #2 with thanks — build.yml then
lands on main as a HUMAN commit and §5 step 4 proceeds identically (R27). Merge ordering is owned by
R25/§5 step 4 (merge FIRST, then dispatch@main — the old "merge only after green" wording here is
superseded); the author keeps contributor credit; main never carries the cron version.

### D9. Stale-literal fix owed (R17: the "or" is RESOLVED by measurement)
Measured: :230 is the fallback when the master's `info.url` is empty (keep the fallback — dropping it
leaves the UPDATE line with nowhere to point); :463 is a hardcoded URL in the join-mismatch verdict (no
master URL in scope there). Root-cause form: ONE compiled constant `kReleasesUrl` (protocol.h, next to
the master hostname) used at BOTH sites — the s29b sweep miss happened precisely because the literal
was duplicated. Ship alongside the workflow build.

### D10. Repo settings
Main branch protection (force-push/deletion OFF) + the v* tag ruleset (D7). **Default workflow token
permissions = read-only — MEASURED already set** (R21: `gh api .../actions/permissions/workflow` ->
`{"default_workflow_permissions":"read","can_approve_pull_request_reviews":false}`); acceptance
re-asserts it. HONEST LIMIT: the setting controls only the DEFAULT — an explicit `permissions:` key
in a workflow can still elevate to write on a base-repo branch, so the REAL contrib-lane gate is the
D1 review-before-dispatch, whose checklist now EXPLICITLY includes "check the permissions key"; the
v* tag ruleset independently blocks robot tags. Residual named, same class as D7(a).

### D11. build-core (R21 — one build implementation, two entries)
ONE reusable `build-core.yml` (workflow_call) on MAIN owns the vcpkg/CMake build steps: `build.yml`
(PR #2) = a thin dispatch entry calling build-core with cache ON; release-core calls the SAME
build-core with cache OFF (cacheless, D3). **The `uses:` FORM is pinned (R22): the REMOTE form
`VOTV-MP/Multivoid/.github/workflows/build-core.yml@main` in BOTH callers** — a local-path `uses:`
resolves from the triggering branch, letting a contrib branch shadow build-core while build.yml's
own diff reads clean; remote@main closes that (same logic as the trampoline). build-core changes
land only as human commits on main. The D1 review checklist explicitly covers EVERY workflow file
changed on the branch, not just build.yml (R22). The fingerprint and the CI-bytes smoke are thereby
proven against the SAME implementation that produces release bytes — no two-build drift. The
contributor's steps migrate into build-core with attribution; his authorship stays in the PR's
build.yml commits (noted in D8). MUST-PASS drills never depend on the PR: build-core + release-core
are both release-lane files on main (§5 step 2a). **R26/R27: build-core carries a CALLER-AGNOSTIC checkout self-check — it takes an explicit
`source-sha` INPUT and asserts `git rev-parse HEAD == source-sha` (no `github.sha` dependency:
on the recovery dispatch `github.sha` is main's HEAD while the tree is the TAG's, so the naive
assert fails by construction; `github.sha` is logged as evidence only). Walked across all four run
shapes: tag-push passes the tag sha; recovery-dispatch resolves the tag input to a sha; a
contrib-mirror or plain dispatch passes `github.sha` as the input. The artifact ZIP name records
the rev-parse value (the tree's own truth). The ledger LINT in the CI lane is ADVISORY-ONLY (R28): its DATA is
fetched from origin/main, but the script itself executes from the dispatched ref's tree
(sanitize replaces only workflows, never `tools/`), so NO invariant rides it — every release
invariant is enforced by the judge running from checkout#2 on the release lane.**

## 4. Drill matrix (before the gate is trusted)

R17 REWRITE — the b9000+ namespace was WRONG (the proto==N precondition can never hold at proto 9000,
so every drill would refuse on the proto branch and mask its target; the MUST-PASS drills could never
pass; class=drill was terminal so the publish drill self-refused). Drills now run on **REAL numbers via
the REAL ritual**: refusal drills are constructed from real history (e.g. tag the PRIOR commit, whose
proto is N-1, without a consume row — proto==N holds at the tag commit, main proto > N holds, and the
refusal arrives on exactly the ledger branch); the full no-short-circuit verdict vector (D3) means each
drill asserts its NAMED line, so ordering can never mask a branch. Numbers consumed by drills get
burn/retracted rows afterwards ("numbers are cheap", D4). The publish drill publishes a real number
with body "DRILL", then the admin deletes it + appends the retracted row — exercising retraction too.
No drill-scoped bypass exists in the gate.

R19 additions: **each drill states + CHECKS its PRECONDITION** (the prior-commit refusal drill
requires "N-1 carries no ledger row" — true exactly in the pristine pre-first-release window where
the drill campaign runs, and the drill script verifies it rather than assuming);
R28: **campaign cleanup is the drill matrix's FINAL step** — the admin deletes every drill tag and
verifies via `git ls-remote --tags` against the expected live set; a missed leftover tag on a
future legitimate N produces an EXPLICIT refusal (the foreign-tag branch of the predicate) —
fail-closed annoyance, never silent corruption.
R21 refinement: **refusal drills ride the dispatch-recovery entry** (`workflow_dispatch(tag)` on
release-core@main — executes MAIN's YAML, so no dependency on the tagged commit containing the
trampoline, and every refusal drill doubles as proof of the recovery path); the tag-push path is
proven by the MUST-PASS ritual drill + the publish drill, whose tags point at post-step-2a commits
(precondition "tag commit CONTAINS the trampoline" — stated + checked); **mint churn is
priced and user-visible**: the campaign consumes ~3 real build numbers (burned/retracted afterwards),
so the public sequence will show gaps — accepted per D4 "numbers are cheap" (user-approved). A bump
WITHOUT a wire change is MEASURED harmless across every kProtocolVersion consumer (R20 grep): all
sites are equality/compare-for-display — dllmain banner :71, server_browser tint :222,
session_manager verdicts :219/:459-462/:490, session.cpp header backstop :290-291, lobby_announcer
:39; the join gate is EQUALITY so a bump splits cohorts BY DESIGN (the s29 Minecraft rule), and
nothing indexes or derives wire semantics from the value.

| Drill | Expected |
|---|---|
| proto != N (tag at wrong commit) | refusal log line |
| missing ledger row (tag without consume) | refusal |
| fingerprint mismatch (poison+revert on main) | refusal pre-build |
| duplicate number (re-tag a consumed N) | refusal |
| robot tag creation from a workflow (R26 vehicle: admin-authored throwaway `on: push` branch workflow, direct-pushed like the positive control, attempts `git push origin <v-tag>` with GITHUB_TOKEN; R27: the workflow EXPLICITLY declares `permissions: contents: write` — otherwise the read-only default refuses BEFORE the ruleset is consulted (fused-guard) and the drill discriminates nothing; the same drill thereby also MEASURES the D10 explicit-key elevation limit; precondition: v* ruleset active; branch deleted after) | platform ruleset rejection |
| mirror-branch dispatch with a CORRUPTED fork ledger copy (R26; R28: the CI-lane lint is advisory-only — the drill proves data-anchoring, not an invariant) | lint verdict unaffected (data read from origin/main) |
| ritual atomic push (the drill's own setup) | MUST-PASS |
| main advanced past consume (next bump landed) | MUST-PASS |
| dispatch-recovery on an existing tag | MUST-PASS publish path |
| dispatch-recovery on a terminal N (retracted/burn row) | refusal "N is terminal (<class>)" |
| kill mid-upload -> re-run | stale draft deleted, resumed |
| second concurrent run (same tag) | queued (concurrency group), then `ALREADY_PUBLISHED` no-op (R18) |
| ONE-TIME PUBLISH DRILL (real b<N>-dev; body = the REAL machine keys `source:`/`sha256:` + a `DRILL` marker line, R23 — the drill exercises the real parser + real retraction, never manufacturing the RELEASE_BODY_UNPARSEABLE state) | read-back asserts + page shows asset+SHA256 + latest unchanged; then admin deletes, RETRACTED row |
| verify_latest.ps1 pre-env / post-env | FAIL then PASS — **DEFERRED to the FIRST real STABLE ritual** (R22: the campaign window publishes -dev only, so no `published` bare-tag row can exist yet; the first stable ritual's checklist carries this as its own acceptance) |
| checkout self-check assert | both HEADs logged correct |
| POSITIVE CONTROL for the sanitize drill (R25, per the negative-grep lesson): admin pushes a throwaway branch DIRECTLY (not via the script) carrying a benign self-authored echo `on: push` workflow | the run FIRES (proves the instrument can show non-zero); branch deleted after |
| SANITIZE drill (R24): branch with a benign self-authored `on: push` workflow, mirrored via mirror_pr.ps1 default | ZERO workflow runs triggered by the mirror push + workflows dir == main's (meaningful only after the positive control) |
| -KeepWorkflows flag drill (R24): mirror a branch carrying a workflow delta WITHOUT the flag | script refuses to push |

## 5. Build plan (after convergence; R20 REWRITE — the two lanes ship to DIFFERENT places, because a
tag-push executes the TAG-COMMIT's YAML and the trampoline calls release-core @main: drill tags must
point at MAIN commits that already CONTAIN the trampoline, with release-core + scripts + ledger seed
on main — so the release lane cannot live inside the PR)

1. D9 literal fix (ONE kReleasesUrl constant) + `tools/release/` scripts (verify_latest.ps1, ledger
   lint, regex fixture selftest, judge/predicate scripts).
2a. RELEASE LANE -> straight to MAIN (human commits; the human is the sole owner of main anyway):
   release-trampoline.yml + release-core.yml + build-core.yml (D11 — the ONE build implementation
   both entries call) + mirror_pr.ps1 + ledger file seed (empty = valid start). NO build.yml here —
   R25: build.yml arrives on main THROUGH the PR #2 merge, so the merge diff is REAL (his file, his
   authorship + our rework commits; the user's "accept + hard-edit INSIDE it" stays substantive).
   **NO fingerprint file is seeded (R19)** — release-core treats a missing fingerprint as refusal
   (fail-closed), which enforces the ordering by the gate itself.
2b. CI LANE -> rework inside PR #2 (the contributor's own topic): build.yml as the THIN dispatch
   entry calling build-core@main (D11), English comments, credit note. The FIRST rework push is the
   D8 viability probe.
3. Repo settings: main protection + v* tag ruleset (+ push-restriction ruleset attempt).
4. Merge PR #2 (build.yml lands on main via the PR; R24's default-branch requirement satisfied) ->
   acceptance: green dispatch of build.yml@main. **R25: the old "green dispatch BEFORE the workflow
   lands on main" clause is RETIRED deliberately — its protected invariant was never "main is
   always green" but "no RELEASE until proven runnable", which the fingerprint fail-closed gate
   already holds mechanically; a red CI workflow on main is fixed forward, it is not a release
   surface.** Then: CI-bytes smoke **on a CACHELESS run (R23: build.yml gains a `cacheless`
   dispatch input passing cache OFF to build-core — the smoked path IS the release path)** -> the
   human commits the fingerprint file from THAT cacheless run's own toolchain dump + build-core
   hash (R19/R23; "proven runnable" now has the right referent) -> drill matrix ON MAIN
   (MUST-PASS drills only possible after the fingerprint lands).
5. Rewrite docs/RELEASE.md onto the ledger ritual (both classes; one mint authority).
6. First real dev release when the user wants one.

## 6. Round digest (PRIOR QF ROUNDS — for the confirmation pass's brief)

R1 cross-release uniqueness -> CI enumeration; "proven" laundering conceded -> acceptance gates; bytes
tradeoff stated; dev outside identity. R2 vcpkg tool pin = wrong knob -> own path + explicit tag; submodules
measured complete; bump-before leaves window-after -> number consumed; naming provenance clarified.
R3 artifact lane = no new class (wire rule governs; sha in ZIP); always-red main rejected -> auto-bump
(later dissolved); one-time CI-bytes smoke; bare-number key + tags consume + fixture. R4 token-push trigger
suppression; fingerprint gate wired; master/latest MEASURED (existing dev line; pelmentor literals found);
moving-target on veto; D8 accept+edit (user aside). R5 COOP_LATEST_PROTO stable-only measured; idempotent
bump script (later dissolved); append-only ledger born (deletable tags); fingerprint on toolset only.
**R6 REFRAME (user-approved): robot writer -> VERIFIER; one owner of main = human; R3-R5 machinery
dissolved; branch protection; ledger = owning authority. User injected MANUAL-ONLY builds.**
R7 dispatch = base refs only; triggering-tag exclusion + empty-ledger start; --atomic + burned numbers;
fingerprint += SDK. R8 ONE workflow both shapes (single mint); cacheless releases; early fingerprint step;
D8 measured viable (workflow scope + maintainerCanModify). R9 fingerprint from main HEAD (numbers never
burn from image rolls); verify_latest.ps1 named step; MUST-REFUSE fixture rows; contrib-run writables
enumerated + branch deletion owner. R10 ledger binds sha+game; burn rows recorded; refusal drill matrix;
v* tag ruleset (capability not intention). R11 refs read at run time from origin/main; draft-first publish
+ resume drill; same-day surface = ritual final step + failure email; verify_latest drilled both ways.
R12 trampoline @main (event's commit supplies YAML); burns in union forever; public-repo drills b9000+.
R13 main protection sans require-PR (daily flow); read-back asserts (prerelease + latest); one-time publish
drill; ledger lint robot-checked. R14 dispatch-recovery entry point; checkout self-check; RETRACTED row
class (grammar completed). R15 (cap) global concurrency group; tri-state labeled read-back vacuity;
ledger-rewrite residual NAMED + SHA-pinned actions.

## 6b. Confirmation-pass digest (R16-R29, 2026-07-25 session 2 — genuine "that holds" at R29)

R16 exact state(N) fold predicate (the union wording self-refused); retracted TERMINAL; judge in
tools/release/*.ps1 from checkout#2. R17 b9000 drills deleted -> real numbers; no-short-circuit
verdict vector; `drill` class removed; `published` closure row; failure surface; D9 kReleasesUrl.
R18 completion by tag-association + body sha (moving target killed byte-rematch); ledger owns MAY /
release owns COMPLETE; D8 push demoted to unverified (first push = probe). R19 row schema restored;
TERMINAL rows push-immediate; no fingerprint seed (fail-closed ordering); drill preconditions;
churn priced. R20 two-lane §5 (tag-YAML fact); published-bare-row wording fix; proto-bump grep.
R21 D10 perms measured; D11 build-core single implementation; refusal drills via recovery entry;
published-row batch safety analysis. R22 uses: remote@main pin; verify_latest drill deferred to
first stable; machine body keys + RELEASE_BODY_UNPARSEABLE. R23 stable retraction env rollback +
fold-aware verify_latest; cacheless smoke; fingerprint += build_core_sha256; D7(c). R24 mirror
sanitize-by-default (push event = execution moment); dispatch-requires-default-branch measured;
contrib drills; D7(d). R25 build.yml via PR merge (real diff); "green BEFORE main" retired
(invariant on fingerprint fail-closed); sanitize positive control. R26 lint data from main;
robot-tag drill vehicle. R27 caller-agnostic source-sha self-check (recovery-lane assert bug);
robot-tag drill declares contents:write (fused-guard); D7(e) judge deliberately unfingerprinted.
R28 CI-lint advisory-only; drill-tag cleanup step; i5+i3 measured verbatim; MTA divergence note.
R29 critic's independent predicate walk -> **that holds**.

## 7. Status

**BUILT + DRILLED 2026-07-25 (session 3, same day as convergence).** §5 steps 1-5 are LIVE on main:
D9 `kReleasesUrl` (`49f10515`); the release lane (`47b88116`: tools/release/ scripts + LEDGER.tsv +
trampoline/release-core/build-core, actions SHA-pinned); D8 probe PASSED first try (rework
`4a980ecf` onto the fork; PR #2 reply posted, MERGED `ac729ca1` — merge diff = build.yml only);
rulesets 19728696/19728697/19728708 (PR merges need `--admin`; direct pushes auto-bypass — daily
flow intact; robot pushes/tags blocked); RELEASE.md rewritten (`fb3c39bc`); cacheless acceptance
run 30153795340 GREEN → CI-bytes LAN smoke PASS → fingerprint committed (`97cc02e1`:
MSVC 14.51.36231 / SDK 10.0.26100.0 / build-core 45c1ac9c).

**§4 drill matrix: EVERY row ran and PASSED** (b125 consumed→published→retracted for it; ledger =
3 rows; campaign cleanup verified zero v-tags/releases/drill-branches on origin). Highlights:
wrong-proto + no-consume refusals on their NAMED lines (the old-commit tag push fired NOTHING —
the event-YAML fact live); fingerprint-poison refused PRE-BUILD after a genuine PUBLISH verdict;
robot tag → GH013 ruleset rejection WITH explicit contents:write (D10 limit measured); sanitize
mirror = zero runs + workflows==main with the positive control FIRED first; advisory lint on a
corrupted-ledger mirror read origin/main's rows (i2/i6 self-check evidence in the same run);
publish drill: stale-draft deletion, sha256 re-download verify, prerelease read-back, labeled
LATEST_404, DRILL-marked body, retraction + push-immediate terminal row, terminal refusal ×2
(recovery AND trampoline). **The matrix caught one real bug live**: the ALREADY_PUBLISHED no-op
left the judge step red (child exit code fell through) — fixed `e4c5e503`, re-drilled green.
verify_latest pre/post stays DEFERRED to the first stable ritual (R22, by design).

**Remaining: §5 step 6 — the first REAL dev release; USER DECISION 2026-07-25: after the
ini/config work (arcs 1+2) is finished.** Proto is now 126 (the drill consume); the next release
consumes 126+.
