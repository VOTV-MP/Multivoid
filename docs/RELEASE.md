# Release checklist — Multivoid (the ledger ritual)

> Rewritten 2026-07-25 onto the CI release lane. Design of record:
> `research/findings/tooling/votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md`.
> The identity model is the Paper pair (game target + build number); the
> version-identity design is
> `research/findings/architecture-audits/votv-version-identity-v122-DESIGN-2026-07-19.md`.

A RELEASE is: a tag `v<game>-b<N>` (stable) or `v<game>-b<N>-dev` (dev prerelease)
whose page carries `multivoid-<game>-<N>.dll` + `xinput1_3.dll` + their SHA256
(machine keys in the body: `source: <sha>` / `sha256: <hash>  <file>`). Bytes are
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
6. On the coop box edit `/etc/coop-master.env`:
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
  roll `/etc/coop-master.env` back to the previous stable (or clear), restart,
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
- The loader scans `multivoid-*.dll`, loads the highest build, and pops the
  in-game "MOD INSTALL PROBLEM" dialog when several version files coexist.
- `releases/latest` never surfaces a dev prerelease (read-back asserted at
  publish); the in-game "(dev; latest released bN)" line is computed
  relationally, no dev axis exists in the identity.
- The judge + fingerprint + ledger predicates all live in `tools/release/*.ps1`
  executed from main HEAD — editing them is a human-only act (rulesets:
  `main-push-admin-only`, `v-tags-admin-only`, force-push/deletion off).
