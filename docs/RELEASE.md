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
whose page carries the ONE package zip (`Pelmentor-Multivoid-<version>.zip`,
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

## THE NEXT RELEASE IS A FLAG DAY — the one-time list (written 2026-08-31)

**Read this before the numbered ritual below.** It does not replace the ritual; it says which of
its steps are load-bearing *this once*, what has no step at all, and in what order the pieces have
to land so we do not break the only cohort we have. **Delete this whole section once the tag is
published** (RULE 2) — everything durable in it lives in the ritual, `docs/THUNDERSTORE.md`, or
`site/NOTES.md`.

**Identity.** `N` = whatever `kProtocolVersion` reads at tag time — **149** as of writing. Game
target `0.9.0n`, so: tag `v0.9.0n-b<N>`, GitHub asset + Thunderstore zip
`Pelmentor-Multivoid-0.9.<N>.zip`, Thunderstore `version_number` `0.9.<N>`. The last published
row is **b133-dev (2026-07-31)**; 134-148 were never released and the sequence keeps the gap.

### Why this one is not an ordinary release — five firsts

1. **First UE4SS-lane release.** b133 shipped the xinput-proxy loader. The artifact is now
   `Mods/Multivoid/dlls/main.dll` inside a Thunderstore-shaped zip, and `cppmod_entry` **refuses
   to start** beside a leftover `multivoid-*.dll` / `votv-coop.dll` next to the exe. So a b133
   tester must **uninstall, not overlay** — that sentence belongs in `tools/release/notes/b<N>.md`
   and in `docs/INSTALL.md`'s update path, or the first thing an existing player meets is a
   removal dialog nobody warned them about.
2. **First release whose VPS services must move — and moving them RETIRES b133.** See below; this
   is the item with real blast radius.
3. **First Thunderstore upload.** Three things become irreversible at that moment
   (`docs/THUNDERSTORE.md` §5): a published version is **immutable** (a README typo costs a whole
   new number), the Team+name pair **is** the namespace (changing either silently creates a
   SECOND package), and an author **cannot delete** a package, only deprecate it.
4. **First site deploy.** `site/NOTES.md:74` gates it: do not deploy until `releases_url` carries a
   PUBLISHED (non-draft) release with exactly one zip. So the site goes out **after** the GitHub
   release, never with it.
5. **First support-rail decision.** The Boosty buttons are pulled (`7ebc2554`) until the page
   exists. The store README is immutable after upload, so the badge is **in or out before
   `package.ps1` runs** — restoring it afterwards costs a version number. Checklist: the local
   `SUPPORT.md` top box.

### The VPS work — the part with no step of its own until now

**Two services, both stale, both already built and proven** (2026-08-31 — see step 6c):

| service | deployed | what the new one changes |
|---|---|---|
| `coop-signaling` | **Jul 20** binary, pre-A59 | challenges every registration (Ed25519). `sig_gate` against the deployed one is **FAIL C — it does not challenge at all**, so A59 is unfixed in production and any b145+ client fails closed on P2P there |
| `coop-master` | **Aug 28** binary (b143-era) | `/v1/host` requires the host's own `gen:` key; `/v1/join` returns `hostIdentity` on DIRECT lobbies (without it a password-locked DIRECT lobby is unjoinable) |

Binaries built on the box from `HEAD:tools/coop-server-rs` (`cargo test --release` **15/15**),
staged on 10010/10011 and measured there: `sig_gate` **PASS 14/14**, `/v1/join` direct carries
`hostIdentity`, an identity-less b≤133 host gets the named 400. **Only the install + restart is
left** — and it must be redone from the *tag's* source if the crate moved since.

**What breaks the instant both restart:** every b≤133 host is de-listed and its re-registration
answered *"this build is too old to host — update Multivoid"*. That is the intended cohort
retirement (user decision 2026-08-29) — the point is only that it must not happen while the
update it names does not exist. Three lobbies were live when this was measured.

**A trap in our own gate:** `sig_gate --remote` over TLS fails on this dev box with
`CERTIFICATE_VERIFY_FAILED: certificate has expired` — the served chain is valid to Oct 18; the
Windows trust store carries an **expired cross-signed `ISRG Root X2`**. The script has no
`--cafile`, so today the only way to run the BLOCKING gate here is `--plaintext` against port
10000. Fix the script or the store before the day; do not discover this at the tag.

**The update notice is stable-only, and that collides with the cutoff.** `COOP_LATEST_*` is
commented out in `/etc/coop-master.env`, so `/v1/latest` serves `proto 0` and the in-game check
stays silent. Ritual step 6 sets it *for stable releases only*. If this ships as a **dev**
prerelease, a b133 player is refused with "update Multivoid" and the game never points them
anywhere. **Recommendation: set `COOP_LATEST_PROTO` / `COOP_LATEST_MOD` for this release
regardless of dev/stable** — the rule exists to stop dev builds nagging stable users, and on the
one day we cut a cohort off, telling them where to go is the whole job. User's call.

### Before the day — free, and worth doing

- **Re-test the three open field defects on the STAGED pair, without touching production.** Defect
  #1 (client never sees the host) was `COOP_MAX_BUILD`, retired in `24418b66`; #2 "No players" on
  tilde, #3 F1 skin not applying, and #4 silent host failure were all parked on "re-test once the
  master is redeployed". Stage as in 6c, then point a test client at it with
  `net.master.custom=1` + `net.master` / `net.signaling` (env: `VOTVCOOP_MASTER_URL`,
  `VOTVCOOP_NET_SIGNALING`). Shipping #2 or #3 into the first Thunderstore package would be
  shipping them immutably. Requires the staged ports to be reachable from outside — verify, do
  not assume.
- **Push.** 33 commits sat unpushed when this was written; the tag must be reachable on origin.
- **Author `tools/release/notes/b<N>.md`** and show the user (ritual 0.5). It is the changelog
  authority and the release body copies it.

### The day, in order

Ritual steps in brackets.

1. Human gate [0] — smoke with `VOTVCOOP_RUN_CONFIG_SELFTEST=1`, the three named log lines,
   `tripwires.ps1`, `ledger_lint.ps1`. **The "for a stable: hands-on verified" clause is
   superseded** by the user's standing position that autonomous evidence is the ceiling
   (`[[feedback-autonomous-evidence-is-the-ceiling]]`); do not park the release on it.
2. Decide the three forks: dev vs stable · Boosty badge in or out · `COOP_LATEST_*` yes/no.
3. Tag + consume row + one atomic leak-audited push [1-3].
4. **While CI builds (~40 min):** rebuild the two Rust binaries on the box from the tagged source,
   back up the live ones, but **do not restart yet**.
5. Watch the run green; confirm the release page shows the zip + SHA256 [4]. Append `published` [5].
6. **Now restart both services** — install, `systemctl restart coop-master coop-signaling`.
7. `sig_gate --remote` → must be **PASS** [0's blocking gate] · `/v1/join` on a DIRECT lobby →
   `hostIdentity` [6c] · if stable-or-decided: `COOP_LATEST_*` + restart [6] →
   `verify_latest.ps1` [7].
8. Thunderstore upload — `docs/THUNDERSTORE.md`, pre-flight checklist first. Irreversible.
9. Site deploy — `zola build` → `npx wrangler pages deploy public --project-name multivoid-site`.

**Why the restart is step 6 and not step 0, against the letter of ritual step 0.** That step says
redeploy *then* publish, and its reason is real: a new build against an old relay loses P2P for
everyone at once. But at publish time the new cohort is **empty** and the old cohort is **live**.
Restarting first kills three real sessions for the ~40 minutes of the CI build with nothing to
download; restarting immediately after the page goes live costs the new cohort a few minutes of
P2P while it has no members. The gate's *evidence* already exists either way — `sig_gate` passed
14/14 on the staged binaries built from this source — so the production run at step 7 is the
confirmation, not the first look. **They remain ONE SITTING; only the order inside it moves.**

### Rollback

The previous binaries are kept on the box (`coop-master.bak-*`, `*.prev`). Restore + restart is
seconds and restores the b133 cohort. What a rollback does **not** undo: a Thunderstore upload
(never delete — deprecate), a published GitHub release (retract per "When something goes wrong",
and a retracted N never republishes), or a site deploy (redeploy the previous build).

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

   **BLOCKING requirement — the signaling relay (b145+, security A59, 2026-08-29):**
   run `python tools/sig_gate.py --remote <deployed-relay> --token <its token>`
   and paste the verdict. It must be **PASS**. This one BLOCKS, where the
   trip-wires only advise, and the reason is a flag day: since b145 the mod's
   signaling client **fails closed** on a relay that does not challenge it, and
   the relay refuses any name its holder cannot sign for. So the relay redeploy
   and the release are **one step, not two** — publish the build against an old
   relay and every install loses P2P at once, with the diagnosis only in a log
   the player cannot see. The same script is the A59 drill against a locally
   built relay (no arguments), so a green release gate and a green drill are the
   same instrument, not two that can disagree.

   Order on the box: redeploy `coop-signaling` **and** `coop-master` (the master
   now requires a host to publish its own `gen:` key — the `h<16hex>`/`c<16hex>`
   mints are retired with the b<=133 cohort), run `sig_gate --remote`, THEN
   publish. A pre-b145 host gets a named 400 from `/v1/host` rather than a silent
   rendezvous failure, which is the whole point of doing it in that order.
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

EVERY build that reaches players (dev drops to testers INCLUDED — this one is
NOT stable-only):
6b. ~~Bump `COOP_MAX_BUILD`~~ **RETIRED 2026-08-31 -- this step no longer exists.**
   The master stopped adjudicating which builds may host, on the user's call: *"Coop max
   build плохая идея, если она не дает другим тестерам на свежих билдах играть, о которых
   мастер не знает."* The ceiling denied every tester running a build newer than the
   deployed value, and since `kProtocolVersion` moves on every wire change that meant a
   coordinated master redeploy was required BEFORE anyone could host a new build. What it
   bought was already conceded as unattributable in A58's own residual, and the pollution
   it aimed at is now handled by the client (red mismatch mark, "which side must update",
   and a `JoinLobby` refusal before any connection). A release no longer needs a master
   restart for version reasons; `COOP_MAX_BUILD`/`COOP_ALLOWED_BUILDS` left in an env file
   are simply ignored.

6c. **A MASTER REDEPLOY IS OWED FOR A FEATURE REASON, WHICH IS NOT THE SAME THING (added
   2026-08-31, still OUTSTANDING as of proto 149).** Step 6b retired the *version* reason
   and it stays retired. But `/v1/join`'s DIRECT response now carries `hostIdentity`
   (`master.rs:618`), and a joiner needs that value to bind the host's key before it will
   send a lobby-password proof. Until the deployed master serves it:
   **a PASSWORD-LOCKED lobby hosted in DIRECT mode cannot be joined from the browser at
   all** -- the client refuses itself with "nothing told us which host we were dialling".
   Open direct lobbies and every AUTO lobby are unaffected, and the client treats the field
   as optional so an old master degrades rather than breaks.
   Check before shipping a release that advertises the lock:
   `curl -s <master>/v1/join -d '{"lobbyId":"<a direct lobby>"}' | grep hostIdentity`.

   **THE BINARIES ARE BUILT AND PROVEN; ONLY THE CUTOVER IS OUTSTANDING (2026-08-31).**
   Both were built ON the box from `HEAD:tools/coop-server-rs` (`cargo test --release`
   **15/15**), staged on ports 10010/10011 beside production, and measured there:
   `/v1/join` on a DIRECT lobby returns `hostIdentity` (this step's own check, **green**),
   an identity-less b<=133 host is refused with the named 400, and
   `sig_gate --remote 127.0.0.1:10010 --plaintext` is **PASS 14/14**. The same gate against
   the *deployed* relay is **FAIL C -- it does not challenge at all**, so A59 is unfixed in
   production and any b145+ client fails closed on P2P there.

   **WHY IT WAS NOT CUT OVER, and this is the real content of this step: the redeploy
   RETIRES the published cohort, and today that cohort is the only one there is.** The
   newest published release is `v0.9.0n-b133-dev` (LEDGER, 2026-07-31), the master refuses
   every host without a `gen:` key, and the relay refuses every legacy `h<16hex>` name -- so
   the moment both restart, a b133 host is de-listed and told *"this build is too old to
   host -- update Multivoid"*, with **no newer build to update to**. Three live lobbies were
   registered while this was measured. So step 0's ordering is not a formality: **redeploy
   both, gate, THEN publish -- as one sitting, not as a chore done early.** Deploying ahead
   of the release buys nothing (no shipped build advertises the lock) and costs every player
   currently hosting.

   Staging recipe, so the next run is a restart and not a rebuild: upload
   `git archive HEAD:tools/coop-server-rs`, build on the box, run each binary with
   `COOP_*_PORT` overridden and `COOP_REQUIRE_TLS=0`, tunnel the relay port, gate it.
   (`pkill -f stage-coop-master` also matches the shell running it -- kill by PID.)
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
