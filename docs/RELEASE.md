# Releasing

## What a release is

A release is a tag, `v<game>-b<N>` for a stable or `v<game>-b<N>-dev` for a prerelease, whose
page carries one package zip, `Pelmentor-Multivoid-<major>.<minor>.<build>.zip`, and its
SHA256. The zip is assembled by `tools/release/package.ps1` from the tagged build's `main.dll`,
and the bytes are CI's cacheless rebuild of the tagged source, published by the release lane
(`.github/workflows/release-trampoline.yml` into `release-core.yml`). The page body has one
writer: a dev disclaimer, then what is new (the notes file for that build), then the install
steps with a link to [INSTALL.md](INSTALL.md), then the build provenance (the source commit and
the checksum).

Build numbers are minted in one place, the append-only, human-written ledger
`tools/release/LEDGER.tsv`. A tag or a release page is a deletable platform object and a drift
detector, never the authority. The identity a release carries is the version pair on
[versioning.md](versioning.md).

Dev releases are rare, end-of-session acts: the cacheless CI build takes most of an hour, so a
release is fired as the last thing and left to finish, while every iteration runs on local
builds.

## The procedure

1. **The gate.** The pre-handoff checklist has passed on the commit being released: a Release
   build, deployed, and a two-peer smoke of at least thirty seconds with clean logs; for a stable,
   a hands-on run by a person. The smoke's host log carries `config-selftest: DONE fail=0` (the
   smoke run with `VOTVCOOP_RUN_CONFIG_SELFTEST=1`), and every peer's log carries
   `repertoire selftest: PASS` and `font selftest: DONE fail=0`. Run `tools/release/tripwires.ps1`
   and keep its output; it advises and never blocks.

   **The relay gate blocks.** Run

   ```
   python tools/sig_gate.py --remote <deployed relay> --token <its token>
   ```

   and it must pass. The mod fails closed on a signaling relay that does not challenge it, so a
   build published against an old relay loses peer-to-peer for every install at once. If the
   services need a redeploy, redeploy the signaling relay and the master together (the recipe is
   in `tools/coop-server-rs/README.md`), run the gate, then publish. Also confirm a join request
   for a direct lobby returns the host's identity; without it a password-locked direct lobby
   cannot be joined from the browser.
2. **The changelog.** Write `tools/release/notes/b<N>.md` by the rules in
   `tools/release/notes/README.md`: plain bullets, no heading, verbs that are status claims,
   anchored to the protocol header's consume comment and the git range. Show it to the
   maintainer before the tag is pushed; the judge refuses a tag whose notes file is missing or
   malformed, and only a person judges whether the prose is true.
3. **Tag HEAD.** `git tag v<game>-b<N>[-dev]`, where HEAD's build number is N and the game is
   the target in `src/votv-coop/CMakeLists.txt`, written without dashes.
4. **The consume commit.** Bump the build number from N to N+1 in `coop/net/protocol.h` and
   append the consume row to the ledger:

   ```
   consume<TAB>N<TAB><game><TAB>v<game>-b<N>[-dev]<TAB><tag commit sha><TAB>YYYY-MM-DD
   ```
5. **One atomic push.** `git push --atomic origin main v<game>-b<N>[-dev]`, so the row and the
   tag reach the remote together and the number is unique there from that moment.
6. **Watch the run to green.** The judge refuses with a labelled verdict on any missed
   precondition; a refusal is stateless, so fix the cause and re-run (dispatch `release-core`
   with the tag). The release is done when the page shows the asset and its SHA256; the
   failure email is the only backstop.
7. **The published row.** Append `published` with the same N, game, tag, commit and today's
   date, and push it while watching the green run.
8. **The update check.** For a stable, set `COOP_LATEST_PROTO=<N>` and
   `COOP_LATEST_MOD=<game> b<N>` in the master's environment and restart it; the in-game line
   informs and never gates a join. A dev release skips this unless it is retiring an older
   cohort, since the client has no dev-or-stable axis and compares the build number alone. Then
   `tools/release/verify_latest.ps1` must pass (`-AllowDev` when the master was pointed at a
   prerelease on purpose).
9. **The mod store.** Upload the same zip to the package listing; never delete a listed version,
   deprecate it.

## When something goes wrong

- **A judge refusal** names its check. Fix the cause and re-run; nothing to clean up.
- **A fingerprint refusal** means the runner toolchain or the build workflow moved since the last
  proven-runnable smoke: dispatch the build workflow cacheless, smoke its bytes locally, commit
  the run's fingerprint dump as `tools/release/fingerprint.json`, re-run the release.
- **A tag on the wrong commit**, before publish: retag only toward the ledger row's commit and
  force-push the tag.
- **A wrongly chosen number**, never published: append a `burn` row and push it at once; numbers
  are cheap, and the public sequence keeps its gaps.
- **A retraction**, when published bytes must go: delete the release page and the tag, append a
  `retracted` row, push now. A retracted number never republishes; fixed bytes take a new number.
  For a stable, roll the master's update-check values back and restart it, then run the verify
  script again.
- **A re-run on a completed tag** is a no-op that touches no asset. A tag or body mismatch is
  reconciled by hand; the workflow never overwrites a live release.

## The ledger

A row is `kind<TAB>N<TAB>game<TAB>tagName<TAB>sourceSha<TAB>date`. The kinds are `consume` (the
number is expected), `published` (a person closed it), and the terminal `burn` and `retracted`.
`tools/release/ledger_lint.ps1` runs advisory in every CI build and enforcing in every release run,
and locally at any time.

## What the code enforces, so no release re-implements it

- Two peers play together only on a byte-equal version pair: the browser's warning, the join
  handshake's refusal, and the packet header's backstop. Old cohorts keep playing among
  themselves.
- The loader loads the fixed contract name `Mods/Multivoid/dlls/main.dll`, and the entry refuses
  to start beside a leftover install of the old standalone loader, with a dialog. The pair rides
  the DLL's own version resource, and both the deploy script and the publish step fail closed on
  a mismatch with the tree or the tag.
- The latest-release pointer never surfaces a prerelease, asserted at publish; the in-game "dev"
  wording is computed from the numbers, and no dev axis exists in the identity.
- The judge, the fingerprint and the ledger predicates live in `tools/release/` and execute from
  the main branch's head, where editing them is a protected act. The publish job checks out the
  tag for the release content and identity and overlays only those predicates from main, so
  the refuse-to-publish logic is never readable from a tag.
