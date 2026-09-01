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

## THE FLAG DAY IS DONE — b150-dev published 2026-09-01 (this section is the residue)

**The one-time list this section used to carry has been DISCHARGED and is deleted per its own
RULE-2 instruction.** What it planned is now either shipped, recorded in the ritual below, or
owned by another doc; keeping the plan beside the outcome is exactly the parallel-stale-and-fresh
this project forbids. The published state:

| | |
|---|---|
| tag | `v0.9.0n-b150-dev` @ `ba6d8c39`, prerelease, published 2026-09-01T17:55:52Z |
| asset | `Pelmentor-Multivoid-0.9.150.zip`, 11,060,341 B, `sha256 dd21ae37…b53ea5b8` — the downloaded file hashes to the value its own body declares |
| ledger | `consume 150` + `published 150`; `ledger_lint` 0 FAIL, 0 WARN (13 rows) |
| gates on the day | `sig_gate --remote` PASS 14/14 (live relay) · smoke PASS both peers with `config-selftest DONE fail=0` · fingerprint current · tripwires QUIET |
| master | `COOP_LATEST_PROTO=150`, `COOP_LATEST_MOD=0.9.0n b150`; `verify_latest.ps1 -AllowDev` **PASS** |

**Two defects the day itself found, both fixed, both recorded where they belong.** The publish job
checked out `main` while the ritual guarantees main is already at N+1 — see the last bullet of
*Invariants* below. And `COOP_LATEST_*` was left at a pre-release stand-in, which the user caught
as a wrong version label in the main menu — see `[[lesson-a-gate-left-red-on-purpose-carries-no-signal]]`.

### What is still owed — THREE OF FOUR ARE DONE (reconciled 2026-09-02 against the live services)

1. ~~**Thunderstore upload**~~ **DONE 2026-09-01T18:06:11Z.** `[V]`
   `Pelmentor/Multivoid` v`0.9.150`, not deprecated, 22 downloads, and the page returns HTTP 200.
2. **`donation_link` — STILL OWED, and it is the only item here with no owner but the user.**
   `[V]` measured empty on the package API 2026-09-02, together with **`categories`, which is
   also empty** and was never on any checklist. Both are website settings on the package: not in
   the manifest, not in this repo, editable forever, and invisible to every gate we have.
3. ~~**Site deploy**~~ **DONE.** `[V]` `https://multivoid.dev` serves the new build — `og:image`
   → `og-multivoid.jpg` (HTTP 200), the retired `og-airlock.jpg` returns **404**, and the nav
   carries the Boosty button. Both of `site/NOTES.md`'s gates are discharged.
4. ~~**The Boosty buttons**~~ **DONE** (`d9b6ab9a`): repo README badge + Support row +
   `.github/FUNDING.yml`, and on the site a Donate button under the install lanes plus a
   brand-coloured Boosty button in the nav. The store README stays badge-less on purpose — see
   `docs/THUNDERSTORE.md` §3a.

**One item this list never had, surfaced by the reconciliation:** `UE4SS_ARC` §7.4b gates the
GitHub repo's own description/topics/homepage on "a UE4SS-lane build is actually released". That
gate is now clear, and `[V]` two of the three are still wrong: `homepage` is **null** (should be
`https://multivoid.dev`) and the topics still carry **`dll-injection`**, which is the retired
proxy lane. The description is already correct.

### Rollback

The previous service binaries are kept on the box — `coop-master.bak-20260831`,
`coop-signaling.bak-20260831` — plus `/etc/coop-master.env.bak-20260831` and
`/etc/coop-master.env.bak-20260902`. Restore + restart is seconds. What a rollback does **not**
undo: a Thunderstore upload (never delete — deprecate), a published GitHub release (retract per
"When something goes wrong", and a retracted N never republishes), or a site deploy (redeploy the
previous build).

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
   `repertoire selftest: PASS` AND `font selftest: DONE fail=0` on every peer.
   (The font emitter has never printed the literal `PASS` this step used to
   demand — corrected 2026-09-01 after grepping for it; what it prints is
   `font selftest: DONE fail=0 (12/12) -- ... N colour texels in one emoji`,
   which is strictly MORE than the checklist asked for. A release step that
   greps for a string nothing emits stalls on its own wording.) The font
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
   the relay refuses any name its holder cannot sign for. Publish the build
   against an old relay and every install loses P2P at once, with the diagnosis
   only in a log the player cannot see. The same script is the A59 drill against a
   locally built relay (no arguments), so a green release gate and a green drill
   are the same instrument, not two that can disagree.

   **THE REDEPLOY THIS STEP USED TO ORDER IS ALREADY DONE (2026-08-31, `f4e3ed2c`),
   so the gate is now a CHECK rather than a coupled step.** Both services run the
   current source and `sig_gate --remote` was **PASS 14/14** against the live relay
   (the binary it replaced was **FAIL C**). The step stays BLOCKING and stays here,
   because it re-breaks silently on any rollback, rebuild, or box migration — and
   because a gate that is only run when someone remembers the coupling is not a
   gate. **Run it anyway, on the day, and paste the verdict.**

   If a redeploy IS needed again: redeploy `coop-signaling` **and** `coop-master`
   together (the master requires a host to publish its own `gen:` key — the
   `h<16hex>`/`c<16hex>` mints are retired with the b<=133 cohort), run
   `sig_gate --remote`, THEN publish. A pre-b145 host gets a named 400 from
   `/v1/host` rather than a silent rendezvous failure, which is the whole point of
   doing it in that order. Full recipe (the CRLF trap, `ETXTBSY`, taking the
   BEFORE arm): step 6c.
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

Normally a STABLE extra — but it is a POLICY, not a constraint (corrected 2026-08-31):
6. On the master box, edit the master service's env file (the path is in the local-only deploy notes):
   `COOP_LATEST_PROTO=<N>`, `COOP_LATEST_MOD=<game> b<N>`, then
   `systemctl restart coop-master`. (Informational toast only — never gates a join.)

   **Dev releases skip this by default, and that default may be overridden.** `[V]` the client has
   **no dev/stable axis**: `session_manager.cpp:334-347` compares `info.proto` to
   `kProtocolVersion` and nothing else — equal prints `(latest)`, greater prints
   `-- UPDATE <mod> AVAILABLE: <url>` in amber, lesser prints `(dev; latest released bN)`. `[V]` the
   b133 build carries the identical branch and polls `/v1/latest` at boot **and** on every
   main-menu entrance, so pointing this at a dev build reaches the old cohort on their title
   screen. The reason to skip it for a dev release is only that dev builds should not nag stable
   users — **when a dev release is retiring a cohort, that reason is inverted and this step
   applies.** Run step 7 with `-AllowDev` when you do.

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

6c. **~~A MASTER REDEPLOY IS OWED FOR A FEATURE REASON~~ — DONE 2026-08-31, cut over ahead of
   the release on the user's instruction. This step is now a CHECK, not an errand.**
   `[V]` in production: `/v1/join` on a DIRECT lobby returns `hostIdentity`, `sig_gate --remote`
   is **PASS 14/14** (the same gate was **FAIL C** on the previous binary minutes earlier), and an
   identity-less b≤133 host is refused with the named 400. Evidence and the rollback set are in
   the flag-day section above. Everything below is kept as the reason the check exists.
   Step 6b retired the *version* reason
   and it stays retired. But `/v1/join`'s DIRECT response now carries `hostIdentity`
   (`master.rs:618`), and a joiner needs that value to bind the host's key before it will
   send a lobby-password proof. Without it,
   **a PASSWORD-LOCKED lobby hosted in DIRECT mode cannot be joined from the browser at
   all** -- the client refuses itself with "nothing told us which host we were dialling".
   Open direct lobbies and every AUTO lobby are unaffected, and the client treats the field
   as optional so an old master degrades rather than breaks. That is why this is a standing
   check and not a one-time errand: it re-breaks silently on any rollback or rebuild.
   `curl -s <master>/v1/join -d '{"lobbyId":"<a direct lobby>"}' | grep hostIdentity`.

   **Deploy recipe (as run 2026-08-31), so the next one is not re-derived.** Build on the box
   from `git archive <tag>:tools/coop-server-rs` (`cargo test --release`, 15/15). Confirm the
   uploaded source equals the tag **file-by-file after `tr -d '\r'`** -- a raw `sha256sum`
   comparison shows 6-of-6 DIFFERENT purely from CRLF and reads exactly like a stale tree.
   Back up binaries **and `/etc/coop-master.env`** with a dated suffix. A running executable
   refuses an in-place write (`ETXTBSY`), so `install` to `<name>.new` and `mv -f` over it --
   `rename()` onto a busy binary is allowed, `cp` onto one is not. Then
   `systemctl restart coop-master coop-signaling`, and confirm the new PIDs' `/proc/<pid>/exe`
   resolves to the installed path before believing the restart took.
   **Take the gate's BEFORE arm against the old binary**, minutes before the swap -- the
   staged run is evidence about a *file*, and only the differential is evidence about the
   *deployment*.
   Two process traps, same root: `pkill -f stage-coop-master` matches the shell running it,
   and `pkill -f` from Git Bash does not match a Windows `ssh.exe` tunnel at all. Kill by PID.
7. `tools/release/verify_latest.ps1` — must PASS (it FAILs before step 6 by
   design; fold-aware: reads the newest bare-tag published row). **`-AllowDev`**
   admits dev prereleases, for the case where step 6's env was deliberately
   pointed at one; without it the script asserts the stable contract and calls a
   dev-advertising master an "unrecorded release". `[V]` the CLIENT has no
   dev/stable axis at all (`session_manager.cpp:334-347` compares `proto` to
   `kProtocolVersion` and nothing else), so "`COOP_LATEST_*` is stable-only" is a
   convention of THIS checklist, never a property of the code.

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
  **The publish job splits the two halves (2026-09-01):** it checks out the
  TAG — so the zip's identity, store README, icon, legal files and paks come
  from the commit being released — and overlays only `tools/release/*.ps1` from
  main, so the refuse-to-publish logic is never readable from a tag. It used to
  check out main for both, which contradicted steps 1-3 of this very ritual: the
  consume bump puts main at N+1 before the run reaches publish, and `publish.ps1`
  leg 3 threw `'<game> b<N+1>' != '<game> b<N>'`. That was invisible until b150
  because leg 3 landed after the previous release.
