# Release notes + install-doc pipeline — design of record (2026-07-26)

**Status: AS-BUILT** (this session; 15-round /qf "holds"; all drills below measured PASS).
Parent lane design: `votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md`.

## The ask (user, 2026-07-26, verbatim-ish)

"Судя по неравному количеству скачиваний у нашей пары dll, надо загонять инфу туда вниз
по типу как установить и ссылку на док по установке, который мы создадим для людей. И еще
туда надо changelogs заводить уже и подумать как, мб автоматически, мб сами будем писать.
Вот следующий релиз я хочу чтобы были changelogs и релиз делаем потому что мы добавили
поддержку dx12 в imgui."

## Founding measurement (round 1 — killed the zip fork)

Per-asset downloads: b126 payload 13 / proxy 12; b127 payload 11 / proxy 7. The b127 gap
is fully explained by b126 updaters (the proxy does not change between builds; a correct
update downloads the payload only). "Broken installs in the wild" NOT proven -> no zip
bundle; the asset shape is unchanged. Revisit only on real field reports.

## The architecture (what was built)

- **`docs/INSTALL.md`** — the SINGLE OWNER of install/update/uninstall/troubleshooting
  prose. README = quickstart + link; site Download = one sentence + link (site/NOTES.md
  rule row; rides the next site redeploy); release bodies = a minimal template block +
  link. No per-build data by lint (below). Loader dup behavior code-anchored
  (xinput_proxy.cpp:19/88; boot_warning_dialog.cpp:69 "MOD INSTALL PROBLEM").
- **Changelog = hand-written `tools/release/notes/b<N>.md`** (auto-generation from commit
  subjects REJECTED: commits are internal jargon; a public changelog is a claim surface
  under `lesson_public_claim_surfaces_carry_verdict_discipline`). The git-tracked file is
  the AUTHORITY; the release page's `## What's new` is a publish-time copy, written once
  (ALREADY_PUBLISHED = never machine-rewritten). Write-once after publish is enforced
  MECHANICALLY by NOTES_DRIFT (below). Cumulative CHANGELOG.md skipped — GitHub's
  /releases page renders every body scrolled, and a third copy drifts (surfaced to the
  user as the primary's call).
- **ONE body writer** — `New-ReleaseBody` (ledger_lib.ps1) serves publish, retro
  regeneration (`notes_regen.ps1`), and recovery republish. Layout: dev disclaimer ->
  `## What's new` (notes content) -> `## Install` (literal payload filename of THIS
  release + xinput1_3.dll + folder + delete-old rule + blob/main INSTALL.md link) ->
  `## Build provenance` (machine keys, grammar byte-unchanged).
- **Gates** (all script-only; no workflow file touched -> fingerprint untouched, measured:
  the pinned set is toolset+SDK+build-core.yml only):
  - judge `NOTES_OK` (pre-build refusal): notes file for tag.N exists on the MAIN
    checkout + format lint (non-empty; no leading heading; no machine-grammar lines).
    Same `ConvertFrom-ReleaseTag` object — no second tag parser.
  - publish backstops: exactly ONE `^source:`-grammar line; sha256-grammar line count ==
    asset count; `Test-Path docs/INSTALL.md`.
  - ledger_lint `INSTALL_STALENESS`: INSTALL.md + README.md carry no 40/64-hex, no
    literal `multivoid-<target>-\d+\.dll` (placeholders pass — the naive `multivoid-\d`
    ban would kill them: '0' of the target is a digit, /qf R12 catch); every
    `multivoid-<x.y.z?>-` filename context names the CURRENT target, parsed by
    `Get-GameTargetFromCMake` — THE one PS-side parser of `VOTVCOOP_GAME_TARGET`,
    throws labeled UNREADABLE on parser-miss (tri-state lesson). A retarget mechanically
    fails the docs until updated (VERSION_MIGRATION step 5 says so).
  - ledger_lint `INSTALL_CONSISTENT`: the template's anchor phrases (folder path,
    delete-old rule — shared constants in ledger_lib.ps1) must appear verbatim (ordinal)
    in INSTALL.md. The machine diff between the two surfaces that share install prose.
  - ledger_lint `NOTES_DRIFT`: for every LIVE release, the body's `## What's new`
    section == `notes/b<N>.md`, ordinal after CRLF/trailing-ws normalization. Labeled
    tri-state: NOTES_SECTION_ABSENT / notes-file-missing / mismatch — never a silent
    pass. Deleted pages are the existing retraction machinery's concern (live-only
    iteration; no eternal block on a deleted platform object).
- **Retro** (the correction path's first real execution): `notes_regen.ps1` rebuilds a
  live body from the notes file via the ONE writer, with source sha + sha256 map PARSED
  from the live body's own machine lines (the API serves no asset hashes); pre-flight +
  fetch-back asserts: every machine line verbatim, completion parser resolves the same
  sha, payload filename present in the preserved sha256 lines, What's-new == notes file.

## Drill evidence (all run this session, real gates, real identifiers)

- Fixture selftest (tag grammar + notes format + body invariants + staleness patterns +
  target parser tri-state + case-sensitivity + 40-hex known-positive): **31/31 ok**
  (`tools/release/tag_regex_selftest.ps1`; runs in-lane at release-core.yml:77 too).
- Judge NOTES_OK on a local temp tag v0.9.0n-b999-dev: FAIL(missing) -> FAIL(leading
  heading) -> PASS(clean bullets); VERDICT REFUSE observed on the FAIL path.
- Full `ledger_lint.ps1` against the live release set: **exactly 2 predicted FAILs**
  (NOTES_SECTION_ABSENT on the pre-retro b126/b127 bodies — the gate catching real
  drift), zero INSTALL failures on the demoted README + new INSTALL.md, zero ledger
  faults. This is the live proof the new gates fire on real objects.
- Retro dry-runs for b126 + b127: constructed bodies verified (machine lines byte-exact,
  parser roundtrip, literal payload filenames interpolated).

## As-built deviations (from the /qf letter)

- R9 said NOTES_DRIFT fetch-fail = FAIL in enforcing mode. As built it rides the lane's
  existing labeled-WARN "API unreachable" branch (ledger_lint has no mode axis; advisory
  vs enforcing is continue-on-error at the workflow layer). Faithful to the intent — no
  silent green — and in the release lane an unreachable API already kills the run at the
  judge's own API-dependent checks; a transient lint-then-judge API flap can skip one
  NOTES_DRIFT pass for OLD releases only (labeled WARN), never the new release's own
  backstops.
- b126's note describes the whole mod ("first public development build"), not the b126
  git range — the range (ini arcs 1+2) is meaningless to a first-time downloader.

## Sequencing / residuals

- Public acts (retro `gh release edit` x2, tag push) are behind ONE user green light,
  with all three notes texts shown first (RELEASE.md step 0.5).
- b127-dead-Tidy trap: b127 shipped with the DEAD Tidy button (fix a05b14e5 is
  post-tag) — b127's note deliberately does not mention Tidy; the fix is credited in
  b128's note.
- Semantic prose truth is human-gated (no lint judges prose); the machine gates cover
  format, drift, and staleness only.
- Quote-stripping (someone quoting a changelog line without the dev disclaimer) is
  unclosable and accepted; the page is triple-marked (title -dev, Pre-release badge,
  disclaimer line 1 adjacent to the section).
