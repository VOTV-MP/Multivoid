# Release checklist — Multivoid (the ledger ritual)

**Cadence policy (USER, 2026-07-26): dev releases through this lane are a RARE, END-OF-SESSION
act.** The CI cacheless build is ~40 min vs ~1 min locally — never block a session waiting on it.
Fire the workflows as the session's last action and let them finish unattended; the `published`
ledger row may ride the next session's first leak-audited push. All iteration (smokes, hands-on)
runs on LOCAL builds.

> Rewritten 2026-07-25 onto the CI release lane. Design of record:
> `research/findings/tooling/votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md`.
> The identity model is the Paper pair (game target + build number); the
> version-identity design is
> `research/findings/architecture-audits/votv-version-identity-v122-DESIGN-2026-07-19.md`.
>
> **`research/` is LOCAL-ONLY since 2026-08-23** (untracked + `.gitignore`d, files on disk in
> their own inner repo — the local-only docs-arc note). Every `research/...` pointer in this tree
> resolves in a working clone and will not resolve on GitHub. This is deliberate.

A RELEASE is: a tag `v<game>-b<N>` (stable) or `v<game>-b<N>-dev` (dev prerelease)
whose page carries the ONE package zip (`Multivoid-Multivoid-<version>.zip`,
assembled by `tools/release/package.ps1` from the tagged build's `main.dll`)
+ its SHA256.
The body (ONE writer: `New-ReleaseBody`, used by publish, retro regeneration,
and recovery alike) is: dev disclaimer -> `## What's new` (the content of
`tools/release/notes/b<N>.md` — the changelog authority, see
`tools/release/notes/README.md`) -> `## Install` (minimal steps + the
`docs/INSTALL.md` link) -> `## Build provenance` (machine keys:
`source: <sha>` / `sha256: <hash>  <file>`). Bytes are
the CI rebuild of the tagged source (cacheless), published by the release lane
(`release-trampoline.yml` -> `release-core.yml@main` -> `build-core.yml@main`).

**The single mint authority for build numbers is `tools/release/LEDGER.tsv`**
(append-only, HUMAN-written; dev AND stable). Tags and release pages are
deletable platform objects — they are drift detectors, never the invariant.
The robot never writes main; the workflow VERIFIES and publishes, the human
consumes numbers.

## The ritual (every release; human consumes, robot verifies)

0. **Human gate** (dev included): the standing local pre-handoff checklist has
   passed on the commit being released (build + smoke discipline — a dev tag is
   not a way around it). For a stable: hands-on verified.
   **Named requirement (ini arc 4):** at least one smoke ran with
   `VOTVCOOP_RUN_CONFIG_SELFTEST=1` and its host log carries
   `config-selftest: DONE fail=0` — mp.py's smoke verdict machine-asserts this
   line whenever the env gate is set (a config/catalog regression fails the
   smoke itself, exit 8).
   **Named requirement (arc D2, 2026-07-28):** the same smoke's log carries
   `repertoire selftest: PASS` AND `font selftest: PASS` on every peer. The font
   one asserts the PHENOMENON — a known emoji glyph is flagged `Colored` and its
   atlas box holds non-greyscale texels — because "the donor resource loaded"
   goes GREEN on a build compiled without `ImGuiFreeTypeBuilderFlags_LoadColor`,
   which bakes every emoji INVISIBLE rather than missing. A release that shipped
   that would look fine to every other check. Both lines are printed at boot, so
   this costs a grep.
   **Trip-wires (2026-07-26):** run `tools/release/tripwires.ps1` and paste its
   output into the handoff. ADVISORY — a FIRED wire re-opens the
   `docs/VERSION_MIGRATION.md` §11 decision ledger, it never blocks the
   release. (The UE4SS-switch fork those wires were minted for was TAKEN — F2,
   2026-08-21 — and shipped at WP-2 commit 3; the wires stay as drift watches
   on the ledger's premises.) On FIRED or a 2nd consecutive CHECK-UNREACHABLE: append
   the dated `TRIPWIRE-DECISION` line + re-freeze in the same commit (§11's
   no-wallpaper rule; the script detects an overdue disposition mechanically).
   Commit the refreshed `tripwires_state.json` with the release flow.
0.5. **Author the changelog + show it to the user** (2026-07-26): write
   `tools/release/notes/b<N>.md` (format rules in `tools/release/notes/README.md`:
   plain bullets, no heading, verbs are status claims anchored to the consume
   comment + the git range) and SHOW its text to the user before the tag push —
   the judge's NOTES_OK check refuses a tag whose notes file is missing or
   malformed, but only a human gates the prose's truth. The file is the
   changelog AUTHORITY; the release page's `## What's new` is a publish-time
   copy (ledger_lint NOTES_DRIFT keeps them equal forever after).
1. **Tag HEAD** (its `kProtocolVersion` IS the number N being released):
   `git tag v<game>-b<N>[-dev]`. Game = `VOTVCOOP_GAME_TARGET` in
   `src/votv-coop/CMakeLists.txt`, no dashes ("0.9.0n" style).
2. **Consume commit**: bump `kProtocolVersion` N -> N+1 in
   `src/votv-coop/include/coop/net/protocol.h` AND append the consume row to
   `tools/release/LEDGER.tsv`:
   `consume<TAB>N<TAB><game><TAB>v<game>-b<N>[-dev]<TAB><tag commit sha><TAB>YYYY-MM-DD`
3. **One atomic, leak-audited push**: `git push --atomic origin main v<game>-b<N>[-dev]`
   (the consume row + tag reach origin together; uniqueness holds on origin from
   this moment).
4. **WATCH the run to green** (Actions -> "release"). The judge refuses with a
   labeled verdict vector on any precondition miss; refusals are STATELESS —
   fix the cause, re-run (recovery: dispatch `release-core` with the tag).
   The checklist is NOT done until the release page shows the assets + SHA256.
   GitHub's failure email to the admin is the backstop, nothing else is.
5. **Append the `published` row** (same N/game/tag/sha, today's date) — this
   closes state(N) API-free. It may ride the next leak-audited push;
   RECOMMENDED: push it right away while watching the green run.

STABLE extra (dev releases skip this — `COOP_LATEST_*` is stable-only):
6. On the master box, edit the master service's env file (the path is in the local-only deploy notes):
   `COOP_LATEST_PROTO=<N>`, `COOP_LATEST_MOD=<game> b<N>`, then
   `systemctl restart coop-master`. (Informational toast only — never gates a join.)
7. `tools/release/verify_latest.ps1` — must PASS (it FAILs before step 6 by
   design; fold-aware: reads the newest bare-tag published row).

## When something goes wrong

- **Judge refusal** — read the `CHECK <name>: FAIL` line; every branch is
  labeled. Fix the cause, plain re-run. No state to clean up.
- **Fingerprint refusal** ("build path changed / no fingerprint") — the runner
  toolchain or `build-core.yml` moved since the last proven-runnable smoke:
  dispatch `build.yml` with `cacheless=true`, smoke the CI bytes locally
  (deploy + LAN smoke), commit the run's `fingerprint-dump.json` as
  `tools/release/fingerprint.json`, re-run the release. Numbers never burn
  from image rolls.
- **Wrong-commit tag** (pre-publish) — retag only TOWARD the ledger row's sha:
  `git tag -f <tag> <row sha> && git push -f origin <tag>`.
- **Wrongly chosen number** (never published) — append a `burn` row and PUSH
  IMMEDIATELY (terminal rows must not sit local). Numbers are cheap; the
  public sequence keeps gaps.
- **RETRACTION** (published bytes must go): delete the release page, delete the
  tag, append a `retracted` row, push NOW. A retracted N NEVER republishes —
  fixed bytes take a NEW number via a new consume. STABLE retraction also:
  roll the master service's env file back to the previous stable value (or clear it), restart,
  re-run `verify_latest.ps1`.
- **Re-run on a completed tag** — lands on `ALREADY_PUBLISHED` (no-op, assets
  untouched). `RELEASE_TAG_MISMATCH` / `RELEASE_BODY_UNPARSEABLE` = reconcile
  by hand; the workflow never overwrites a live release.

## Ledger grammar (tools/release/LEDGER.tsv)

Row = `kind<TAB>N<TAB>game<TAB>tagName<TAB>sourceSha<TAB>date`; kinds:
`consume` (mint expectation) | `published` (human closure) | `burn` /
`retracted` (TERMINAL forever). Lint runs advisory in every CI build and
ENFORCING in every release run; `tools/release/ledger_lint.ps1` local anytime.

## Invariants the code enforces (do not re-implement per release)

- Join gate = byte-equality on (game target, build): browser pre-flight popup,
  Join-seam wire gate, header backstop. Old cohorts keep playing among
  themselves (per-lobby equality, never latest-only — the Minecraft rule,
  user directive 2026-07-19).
- UE4SS loads the fixed contract name `Mods/Multivoid/dlls/main.dll`;
  `cppmod_entry` REFUSES to start beside a leftover pre-mod-folder install
  (`multivoid-*.dll` / `votv-coop.dll` next to the exe) with a removal dialog.
  Artifact identity rides the DLL's own VERSIONINFO pair — `deploy-mod.ps1`
  and `publish.ps1` both fail closed on a tree/tag mismatch. (The in-game
  boot-warning modal's live feeder is `server_browser_native`'s missing-donor
  warning.)
- `releases/latest` never surfaces a dev prerelease (read-back asserted at
  publish); the in-game "(dev; latest released bN)" line is computed
  relationally, no dev axis exists in the identity.
- The judge + fingerprint + ledger predicates all live in `tools/release/*.ps1`
  executed from main HEAD — editing them is a human-only act (rulesets:
  `main-push-admin-only`, `v-tags-admin-only`, force-push/deletion off).
