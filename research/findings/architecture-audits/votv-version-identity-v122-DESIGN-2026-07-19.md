# v122 version identity: the multivoid Paper pair + equality gate + popups (AS-BUILT, 2026-07-19)

> The s29 feature session ("заняться вопросом версий... смотреть на Minecraft и Paper"; then
> "тупо game ver + build number без отдельных версий мода"; then "dll назывались multivoid" +
> the duplicate-DLL popup ask). 13-round /qf pass ("that holds" R13) + a mid-implementation
> USER REFRAME (the mod-semver axis deleted in favor of the pure Paper pair) + the multivoid
> rename. Status: AS-BUILT, drill-verified with real logs (NOT hands-on; rides take 4).

## The identity model (user-ratified, Paper-Minecraft shape)

The mod's version identity is the PAIR `(game target, build number)` — exactly Paper's
"Paper 26.1.2 Build #74" / `paper-26.1.2-74.jar` (the user supplied the screenshot):

- **game target** = `coop::version::kGameTarget` ("0.9.0n"), CMake `VOTVCOOP_GAME_TARGET` →
  generated `coop/version.h`. Bumps on a game-recook adaptation.
- **build number** = `kProtocolVersion` (122) — the wire revision. It moves exactly when
  compatibility moves (the standing wire-bump rule) and EVERY release bumps it (release
  checklist, docs/RELEASE.md), so a released build number uniquely names a released DLL.
- **NO mod semver.** The old `project(VERSION 0.0.1)` axis was a hand-maintained string
  nobody bumped (and session_manager.cpp:28 carried a SECOND rotting duplicate "0.9.0-n"
  announced as the "mod version" — RULE-2 rot). Both deleted whole. The
  "forgot-the-semver-bump" residual class from the /qf pass DISAPPEARED with the axis.
- Display everywhere (top-left menu label, browser header, boot banner):
  `votv-coop 0.9.0n b122`. The browser Version column: `0.9.0n b122` per row
  (build number shows everywhere — user directive).
- Artifact name: `multivoid-<game>-<build>.dll` (`multivoid-0.9.0n-122.dll`) — the filename
  IS the release identity; CMake parses the build number out of protocol.h at configure
  (`CMAKE_CONFIGURE_DEPENDS` re-runs it on a proto bump).

## The equality gate (per-lobby, Minecraft shape — old cohorts keep playing)

Per the user's directive: a 0.9.0n host stays joinable by 0.9.0n peers FOREVER — the gate is
per-lobby EQUALITY, never "latest-only"; the update check is informational-only.

Three enforcement layers, deterministic tier order `game -> build`:

1. **Browser pre-flight** (`session_manager::JoinLobby` + `VersionMismatchVerdict`): the row
   carries `game` + `proto`; a mismatch REFUSES with the connect-failed POPUP via the new
   `join_progress::RefuseJoin` (no `Active()` gate — pre-flight has no cover/abort; lifecycle
   = cleared by OK or the next BeginConnect, measured bounded). Amber "(!)" in the Version
   column ALWAYS corresponds to a refusal (no amber-but-joinable state). Empty/0 fields (old
   host) skip their tier — the wire layers cover.
2. **Wire gate at the Join seam** (`player_handshake::HandleJoinMessage`): Join gains the
   trailing field `[u8 gamelen][game ASCII]` (proto 121→122); BOTH sides validate at the TOP
   of the handler — a pure pre-pass (`ExtractJoinVersionFields`, every length bounds-checked,
   malformed = fail-closed refuse) BEFORE any identity side effect, so a refused joiner ==
   the already-handled "connected, never Joined, disconnected" lifecycle. Host: WARN + local
   feed line "<nick> was turned away: ..." (deduped per nick+reason, 30 s window) +
   `Session::Kick(reason)`. Client: WARN + `join_progress::Fail(reason)`. Covers EVERY entry
   surface incl. direct connect + env boot. The build half needs no field — it IS the packet
   header's protocol version (the frozen prologue).
3. **Header backstop** (unchanged, `session.cpp` PeekProtocolVersion close): cross-build peers
   never reach the Join parse; the close reason travels GNS and lands in the popup (measured
   pre-existing since ~2026-06-06); loss floor = the generic "could not connect" popup.

## The multivoid loader (versioned artifact + duplicate detection)

`xinput1_3.dll` proxy: scans its own dir for `multivoid-*.dll`, parses the trailing build
number, loads the HIGHEST (unparseable names never load). When >1 version file is present —
or a stale legacy `votv-coop.dll` — it sets `MULTIVOID_DUP_FILES`/`MULTIVOID_LOADED` env
vars; `dllmain` reads them at boot and arms the NEW `ui::boot_warning_dialog` ("MOD INSTALL
PROBLEM": loaded X, also found Y, delete the others), rendered by imgui_overlay over any
surface until OK'd. No fixed-name fallback (RULE 2 — the votv-coop.dll name retired whole;
deploy-loader.ps1 deletes legacy + stale versions, deploys the current file).

## Master plane (the LIVE Rust master; the Python one is superseded, pending RULE-2 delete)

- `tools/coop-server-rs/src/bin/master.rs` (deployed on the coop box): `Lobby.game` stored
  (clamp 23) + served in rows; `version` stays as the legacy pass-through display tag (no
  fabricated "0.0.0" default). `/v1/latest`: compiled default now proto 0 = "no released
  record" — the client (`lobby_client` `info.ok = proto>0` + a session_manager belt) treats
  it as NO VERDICT (silent identity label; a stale/absent record can never fake "(latest)"
  or nag). Release sets `COOP_LATEST_PROTO`/`COOP_LATEST_MOD` in `/etc/coop-master.env`.
- Deployed 2026-07-19: musl build, unit tests 5/5, installed + restarted; verified FROM
  OUTSIDE (`/healthz`, `/v1/latest` = proto 0, `/v1/lobbies`) AND end-to-end: a real host
  announce logged `host ... game=0.9.0n b122` in the box journal; the env-host lobby went
  hidden as designed; the client's latest-check stayed silent (no-verdict path proven).
- `tools/coop_master_server.py` received the same +game/latest.json changes for parity but
  is SUPERSEDED by the Rust master (cutover 2026-07-16) — flagged for RULE-2 deletion once
  the user confirms the cutover is final.

## Verification (all real logs, scratchpad s29, session 67b608d1)

| Drill | Setup | Proven |
|---|---|---|
| A1 | old b121 client → b122 host | old side's own backstop closes "peer=v122, ours=v121"; reason crosses the wire to the host log |
| A2 | b122 client → old b121 host | symmetric, new client displays the mismatch reason |
| B / B2 | game-target 0.9.0-x build vs canonical (both b122) | CLIENT-side wire gate + `join FAILED` popup path (the client always validates first — the host's Join lands before the client's own Join goes out) |
| B3 | GAMEX host × NOVAL client (client validation compiled out, drill-only) | HOST-side gate: `Join REFUSED (nick='Client' game='0.9.0-n')` + feed line "Client was turned away: …" + Kick reason delivered to the client's popup path ("HOST CLOSED OUR CONNECTION (reason: Game version mismatch…)") |
| smoke | full mp.py smoke on the renamed artifact | PASS: proxy scan+load boots, client connects, puppet spawns, RSS stable |
| dup | planted multivoid-0.9.0n-121.dll beside -122 | proxy loaded 122, boot WARN, in-game "MOD INSTALL PROBLEM" popup captured on screenshot (dup_popup.png) |
| e2e master | host announce → live box | journal `game=0.9.0n b122`; /v1/latest proto 0 → client silent |

Audits (both on the full diff, findings >=80 only):
- **perf**: 0 CRITICAL (function-by-function table; the one always-on addition —
  `BootWarningOpen` in the overlay gates — is a relaxed atomic load; all string building
  sits on COLD/WARM click/connect/menu paths). Finding 1 = player_handshake.cpp grew
  828→965 past the soft cap → FIXED same-session: the freshly-written gate cluster
  extracted to `player_handshake_version.cpp` (121 LOC; player_handshake back to 797,
  UNDER cap; FromUtf8/SanitizeNickname flipped shared via player_handshake_detail.h).
  Finding 2 (LOW) = DisplayVersion per-frame rebuild → function-static, applied.
- **correctness**: CLEAN on parse bounds / wire layout / ternary lifetime / RefuseJoin
  lifecycle / all signature-change callers / zero semver leftovers / CMake regex / master
  field wiring / deploy scripts. ONE finding (85): boot_warning_dialog missing from the
  imgui_overlay SEH re-fault guard → FIXED (`Clear()` added + called in `__except`).
Final bytes `multivoid-0.9.0n-122.dll 4C994D0E1380DE20` x4 hash-verified; re-smoke PASS
on the final bytes (both peers stable, client joined, puppet spawned).

## Accepted residuals (named, not claimed covered)

- **Forgot-BOTH-bumps** (a wire change shipped without a proto bump): identity fields all
  equal by definition — an uncatchable process-failure class; mitigated by the standing
  wire-bump rule + audits.
- **Install skew under equal mod build** (mod for cook X running against exe Y): OUT OF
  SCOPE of the peer-compat lane (it breaks SP boot identically). Mitigation: the boot banner
  logs the real exe identity (path+size) beside kGameTarget — one-look diagnosis. Fail-loud
  AOB behavior on a wholly-foreign cook is INFERRED (no foreign exe exists to measure);
  deferred measure row: when the next cook ships, boot the old DLL against it first.
- Same-build different-bytes dev builds: discriminated by the banner compile timestamp
  (`__DATE__/__TIME__`, banner-only) + the deploy hash-verify.

## /qf trail (13 rounds, "that holds")

R1 direct-connect silence → refuted by measurement (net_pump close-reason → popup). R2 game
target became a HARD axis (proto insufficient across cooks; offsets are per-exe). R3 the
wire hole at direct connect closed at the Join seam (+proto bump). R4 kModVersion provenance
measured; CLAUDE.md versioning conflict surfaced (later dissolved by the user's pair
decision). R5 frozen-prologue/reason-loss/tier-parity/fail-closed equality. R6 authority
dedup + host-side visibility (feed line) + both-direction master tolerance. R7 zero
pre-validation minting + feed dedup + close-reason cap + drillability. R8 forgot-bump
residual named; old-client floor measured; take-4 stacking deliberate. R9 3-tier drill
matrix + latest.json bounded consequence. R10 old-client drill criteria + clean-fail assert
+ teardown row + compile-time drill knob. R11 reverse-direction symmetry + absent-field
render + which-axis-popup ordering. R12 install-skew scoped out + fail-loud tagged inferred.
R13 that holds. Mid-impl USER reframes: (1) drop the semver axis → pure pair (simplified
tiers 3→2, killed the semver comparator + the forgot-semver residual); (2) multivoid rename
+ dup popup (added the loader scan lane).
