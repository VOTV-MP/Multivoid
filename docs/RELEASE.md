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

**Identity.** `N` = whatever `kProtocolVersion` reads at tag time — **150** as of writing. Game
target `0.9.0n`, so: tag `v0.9.0n-b<N>`, GitHub asset + Thunderstore zip
`Pelmentor-Multivoid-0.9.<N>.zip`, Thunderstore `version_number` `0.9.<N>`. The last published
row is **b133-dev (2026-07-31)**; 134-149 were never released and the sequence keeps the gap.

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
5. **First support-rail decision.** The live buttons (repo README badge + Support row +
   `.github/FUNDING.yml`) are **pulled and staying pulled** — `7ebc2554`, restored in `1aca131b`
   and pulled again in `c18003aa` on the user's word, *after* `https://boosty.to/pelmentor` was
   confirmed live. The page existing is not the same decision as the buttons going live.
   **Still open and it is one-shot:** `tools/release/README_thunderstore.md` keeps its badge, and
   the store README is immutable after upload — so the badge is **in or out before `package.ps1`
   runs**, and putting it back later costs a whole version number. The *other* half is cheap and
   should not be confused with it: Thunderstore's package-level `donation_link` is a website
   setting, not a manifest field, so it can be set or changed at any time after publish
   (`docs/THUNDERSTORE.md` §3a — allowed, first-class, 51 of 188 VOTV packages use one, one of
   them a Boosty link).

### The VPS work — DONE, ahead of the day (cutover 2026-08-31)

**Both services now run the current source. This section is AS-BUILT; it used to be the largest
open item on the list.**

The user's call, verbatim: *"Значит Когда я буду zip тестировать, то к этому моменту уже надо
мастер сервер полностью обновить, плевать на когорту."* — the master must be current before their
local zip test, and the b≤133 cohort is not a reason to wait. That overrides the ordering argument
this section used to make (kept below, struck, because its reasoning is still the right *shape* for
the next time a cutover competes with a live cohort).

| service | now running | proven in production |
|---|---|---|
| `coop-signaling` | b149 source, `ce2212a1e8fc7eed` (was a **Jul 20** pre-A59 binary) | `sig_gate --remote` **PASS 14/14** — the same gate against the old one was **FAIL C, no challenge at all** |
| `coop-master` | b149 source, `18663ad7054f6dab` (was **Aug 28**, b143-era) | `/v1/join` on a DIRECT lobby returns `hostIdentity`; an identity-less b≤133 host gets the named 400 |

`[V]` **the before/after differential was taken on the live relay, not inferred from the staged
run**: FAIL C at 15:4x, PASS 14/14 after the restart, same instrument, same tunnel, same token.
Installed binaries are byte-identical to the staged ones (`18663ad7…` / `ce2212a1e8fc7eed`), whose
source was confirmed equal to `HEAD:tools/coop-server-rs` file-by-file modulo line endings.
Both TLS legs verified from **outside** the box afterwards: `https://…:10443/healthz` → 200,
signaling `:10442` → TLS 1.2 handshake OK.

`[V]` **step 6b's retirement is live too, measured on the deployed binary with its negative
control**: a `proto 9999` host is **ADMITTED** (the old ceiling would have refused it) while
`game: "<script>x"` is still refused with `bad game version` — so the version POLICY is gone and
the PARSING check survived, which is exactly the split `24418b66` intended. Every check lobby was
torn down; `healthz` reports 0.

**The cohort retirement is real and immediate** — within seconds of the restart the relay log
carried three separate real IPs refused by name (*"identity 'str:h…' is not a key… that cohort is
retired; it must update"*), and the one listed lobby (`gogofast`, proto 133) was dropped. This was
authorised, and it is the state the release now ships into rather than a change the release makes.

Also retired in the same pass: `COOP_MAX_BUILD` is **deleted from `/etc/coop-master.env`**. The new
binary ignores it (`24418b66`), so it was dead input — but left in place it is a stale claim in a
config file, and it would silently re-arm the tester-denying ceiling on any rollback to the old
binary. `/etc/coop-master.env.bak-20260831` holds the previous file.

**Nothing is owed on the box any more.** `COOP_LATEST_*` was the last item and it is SET (below) —
ahead of any release, on the user's call.

**A trap in our own gate, and it is PYTHON-specific — not the box.** `sig_gate --remote` over TLS
dies with `CERTIFICATE_VERIFY_FAILED: certificate has expired`, while `verify_latest.ps1` reaches
the *same* host over TLS fine (measured 2026-08-31, both against `master.multivoid.dev`). The
served chain is valid to Oct 18; the Windows store carries an **expired cross-signed
`ISRG Root X2`**, and OpenSSL — which Python uses — builds a path through it and stops, where
schannel finds the valid one. So the fix is a `--cafile` (or `certifi`) in `sig_gate.py`, **not** a
machine repair. Until then the only way to run the BLOCKING gate here is `--plaintext` against port
10000. Do not discover this at the tag.

**`COOP_LATEST_*` IS SET, BEFORE ANY RELEASE EXISTS — AND THAT IS DELIBERATE (2026-08-31).**
User's call: *"релиза нету, да, но мы же уже отрезали b133, так что пусть и сообщение показывает
в углу что update есть"*. The cohort is already cut off by the cutover, so the choice was between
telling them and saying nothing; the recommendation to set it anyway was accepted.

| var | value | why this value |
|---|---|---|
| `COOP_LATEST_PROTO` | `134` | only has to EXCEED 133 to light the b133 label; kept **≤ our own dev build (149)** so our builds show the informational `(dev; latest released b134)` line instead of nagging themselves |
| `COOP_LATEST_MOD` | `Multivoid` | free text, set ON PURPOSE — an empty `mod` makes the client render `b<proto>`, i.e. it would name a build number that does not exist. This renders `UPDATE Multivoid AVAILABLE: <url>` |
| `COOP_LATEST_URL` | `multivoid.dev` | **it only matters to the b≤149 cohort now** — see the box below. For them the address cannot be removed (their format string appends it and falls back to a compiled default when it is empty), so the only lever is LENGTH: `[V]` `multivoid.dev` serves HTTP 200 with a real page, and it takes the rendered label from **90 to 66 characters** |

`[V]` differential on the live master: `{"mod":"","proto":0,...}` before the restart →
`{"mod":"Multivoid","proto":134,"url":"github.com/VOTV-MP/Multivoid/releases"}` after, on the
plaintext **and** TLS legs from outside the box. A restart is required and it is required twice
over: the handler resolves the three through a `LazyLock` (`master.rs:907`, once per process) and
systemd's `EnvironmentFile` is itself a start-time snapshot.

What a b133 player now gets, traced through their OWN shipped code (`v0.9.0n-b133-dev`, not HEAD):
`FetchLatest` sets `ok` because `proto > 0`; `RefreshLatestVersion` passes the `info.proto > 0`
guard; `134 > 133` takes the outdated branch; the native menu label turns amber and reads
**`Multivoid 0.9.0n b133 -- UPDATE Multivoid AVAILABLE: github.com/VOTV-MP/Multivoid/releases`**.
It re-polls on boot and on every main-menu entrance, so no mod update is needed to see it.

**THE LABEL NO LONGER PRINTS AN ADDRESS, FROM THE NEXT BUILD ONWARD (user 2026-08-31: *"url не
надо показывать, это длинно, достаточно лаконичного текста что обнова доступна"*).** The outdated
branch now composes `"<identity> -- UPDATE AVAILABLE: <mod|bN>"` and stops there: the label is a
`UTextBlock`, not a hyperlink, so ~37 characters of address on a one-line menu row bought a string
nobody can click and few would retype. What stays is the actionable half — WHICH build supersedes
yours. `LatestInfo::url` lost its only reader and was retired with its parse (RULE 2); the
version-mismatch line on a refused join uses the compiled `net::kReleasesUrl` and is untouched.
**No protocol bump**: the master still serves `url` and ignoring an extra response field is
forward-compatible. `verify_latest.ps1` reads `proto` and prints `mod`, never `url`, so the gate
is unaffected.

**So `COOP_LATEST_URL` is now a LEGACY-ONLY knob.** b133..b149 render it; every build after this
change ignores it. Keep it short and real while that cohort exists, then it stops mattering.

**TWO CONSEQUENCES TO CARRY TO THE RELEASE.**
1. **`verify_latest.ps1` now FAILS BY DESIGN** and will keep failing until step 6 replaces these
   with the real numbers — the master advertises b134 while the ledger's newest published row is
   b133. That is not a regression to investigate; it is this decision's price, and the reason step
   6 stops being optional.
2. **Replace all three at the release** (step 6), including `COOP_LATEST_MOD` — leaving
   `Multivoid` there once a real build exists would hide the build number the player needs.

### Before the day — free, and worth doing

- **THE USER RE-TESTS THE r2modman ZIP LOCALLY (their own item, 2026-08-31: "Я ещё должен буду
  локально нашу r2modman zip снова тестить"), AND IT SHOULD BE THE SAME RUN AS THE FIELD-DEFECT
  RE-TEST.** The 2026-08-29 import proved **LAYOUT ONLY** — the tree matched `UE4SS_ARC` §7.2a's
  prediction and the mod booted, but **the session never came up**, so nothing downstream of boot
  has ever been exercised from a managed install. That was not the package's fault: `[V]` the cause
  was the master's build gate (`COOP_MAX_BUILD` = 143 against a b145 host), and that gate is
  **retired** (`24418b66`). So this re-test is the first one that can actually reach a session.
  - **Re-run `tools/release/package.ps1` and take the name it prints.** Do not reach for a named
    local zip — the build number moves several times a day, and one of the 2026-08-29 zips was
    assembled from a build directory a parallel session had just rewritten.
  - **The same run answers the three open field defects**, all of which were parked on this
    redeploy: **#2** "No players" on tilde, **#3** F1 skin not applying, **#4** hosting fails
    silently. Shipping #2 or #3 into the first Thunderstore package would ship them **immutably**.
  - **Nothing special is needed to reach a session any more — the cutover happened first, on the
    user's instruction, exactly so this test would not have to route around it.** Point the client
    at the normal production endpoints. The whole staged-pair + `ufw allow 10010/tcp` dance this
    bullet used to prescribe is **deleted**, not deferred: the staged listeners are down and the
    ports were never opened.
- **~~THE FINGERPRINT IS STALE AND THE RELEASE RUN WILL REFUSE~~ — CLOSED 2026-09-01 (`d2a85eaa`),
  and the claim it replaces was MEASURED ON THE WRONG MACHINE.** `[V]` the committed
  `build_core_sha256` `411e62b8…` is exactly the sha256 of the CURRENT `build-core.yml` **in the
  runner's CRLF checkout** — reproduced here by converting the worktree's LF file and hashing it.
  `[V]` nothing has touched `build-core.yml` since the run that minted it (`33498716305`, commit
  `e7eedd34`, an ancestor of HEAD), so the build path has not moved and the gate should PASS.
  **The trap, and it is the reason this bullet stood for a day saying the opposite: a local
  `Get-FileHash` can never predict this gate's verdict.** Git checks these files out LF here and
  CRLF on the runner, so a local read produces `db0c3b5a…` for the very file the runner hashes to
  `411e62b8…` — three different-looking values across this section's history, all of them the
  same file. `msvc_toolset` / `windows_sdk` are runner facts too. **Do not run
  `fingerprint.ps1 -Mode check` locally and read anything into it**; the only honest local check
  is "did `build-core.yml` change since the commit named in the last fingerprint mint", which is
  a `git log` and nothing else. Minting still requires a cacheless CI run + its smoke — that rule
  is unchanged and is why `d2a85eaa` was earned rather than typed
  (`[[lesson-a-file-hash-gate-can-only-be-minted-where-it-is-checked]]`).
- **Push.** Dozens of commits were unpushed when this was written and the backlog only grows; the
  tag must be reachable on origin. Check with `git log --oneline origin/main..HEAD | wc -l`, and
  run the 5-axis leak audit per commit before asking.
- **Author `tools/release/notes/b<N>.md`** and show the user (ritual 0.5). It is the changelog
  authority and the release body copies it.

### The day, in order

Ritual steps in brackets.

1. Human gate [0] — smoke with `VOTVCOOP_RUN_CONFIG_SELFTEST=1`, the three named log lines,
   `tripwires.ps1`, `ledger_lint.ps1`. **The "for a stable: hands-on verified" clause is
   superseded** by the user's standing position that autonomous evidence is the ceiling
   (`[[feedback-autonomous-evidence-is-the-ceiling]]`); do not park the release on it.
2. Three forks are **DECIDED (user, 2026-08-31)**: this release is a **dev prerelease**, the live
   Boosty buttons stay **pulled**, and `COOP_LATEST_*` was **set ahead of the release** (above).
   **ONE remains:** the store-page badge in `README_thunderstore.md` (one-shot — see first #5).
3. Tag + consume row + one atomic leak-audited push [1-3].
4. Watch the run green; confirm the release page shows the zip + SHA256 [4]. Append `published` [5].
5. **REPLACE `COOP_LATEST_*` in `/etc/coop-master.env` with the REAL numbers +
   `systemctl restart coop-master`** [6] → `verify_latest.ps1 -AllowDev` [7]. Not a fresh set:
   the three are already populated with the pre-release stand-in (`134` / `Multivoid` /
   schemeless URL — see the VPS section), so **all three must move, `MOD` included**, or the
   label hides the build number the player needs. `verify_latest.ps1` is RED until this step
   runs, by design.
6. **Re-confirm the two server gates on the released build** — `sig_gate --remote` **PASS**
   [0's blocking gate] and `/v1/join` on a DIRECT lobby → `hostIdentity` [6c]. Both passed in
   production on 2026-08-31; this is a re-confirmation after the step-5 restart, not a first look.
7. Thunderstore upload — `docs/THUNDERSTORE.md`, pre-flight checklist first. Irreversible.
8. Site deploy — `zola build` → `npx wrangler pages deploy public --project-name multivoid-site`.

**~~Why the restart is step 6 and not step 0~~ — SUPERSEDED 2026-08-31, but the reasoning is kept
because the situation recurs.** The argument was: ritual step 0 says redeploy *then* publish, and
its reason is real (a new build against an old relay loses P2P for everyone at once) — but at
publish time the new cohort is empty and the old one is live, so restarting first kills real
sessions for the ~40 minutes of the CI build with nothing to download. **The user overruled the
premise rather than the conclusion:** the cohort's comfort was not worth sequencing the release
around, and a master that is already current makes their own zip test straightforward. So the
cutover was done days early and the ordering question dissolved. Next time a cutover competes with
a live cohort, this trade-off is the one to weigh — and *ask*, rather than optimising it silently.

### Rollback

The previous binaries are kept on the box — **`coop-master.bak-20260831`,
`coop-signaling.bak-20260831`** (plus the older `.bak-20260829` / `.prev`), and
`/etc/coop-master.env.bak-20260831`. Restore + restart is seconds and un-retires the b133 cohort.
What a rollback does **not** undo: a Thunderstore upload
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
