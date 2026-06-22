# AUTONOMOUS_TESTING — test harness usage

**Living document.** Reflects the current standalone-C++ harness (the
UE4SS Lua `coopTestHarness` it replaced has been retired per RULE №2).

## Intent

A scenario-driven test runner that boots VOTV with the coop mod, drives
the engine through scripted actions via reflected UFunction calls, runs
for a fixed number of frames or to completion, and writes logs +
optional screenshots. Lets us regression-test every replication change
without manual play.

Per methodology WP8: an autonomous pass validates ONE process's code
paths — NOT coop-correctness, performance under load, or hands-on
behaviour. Autonomous pass = ship to live test, not to production.

## Architecture

The harness is **part of the shipping DLL** (`src/votv-coop/src/harness/`).
On load, the DLL reads a `scenario.txt` file next to itself; the
scenario string selects a code path inside `harness::Run` that posts
engine actions onto the game thread via `GT::Post`. Reads + dispatches
run on a worker thread; all engine touching is funnelled through the
game-thread pump.

Scenarios:

| Scenario | What it does | Used by |
|---|---|---|
| `play` | Skip-to-gameplay (LoadStorySave), then idle for hands-on play. | `mp_host_game.bat`, `mp_client_connect.bat` |
| `netloopback` | Single process, single session in loopback (host self-connected, peer=self, initiate=true). Verifies the transport + serialization + session + interp end-to-end without a 2nd machine. | `tools/run-test.ps1 -Scenario netloopback` |
| `load:<slot>` | Load a specific save slot. | one-off probes |
| `none` | Launch only; no harness automation (manual driving). | manual launch |
| `probe_terminals:<save>` | Phase 5T probe scenario — load save + drive the UE4SS Lua probe harness to dump terminal state. | `tools/probe-terminals.ps1` |

Per-scenario screenshots happen via `harness::screenshot` (which calls
VOTV's `HighResShot` console command — **never** during hands-on `play`
since the "saving screenshot" toast is distracting; autonomous only).

## Common harness env vars

The harness reads env vars BEFORE `votv-coop.ini`, so the launchers
override per-run config without editing the ini:

- `VOTVCOOP_SCENARIO` — overrides `scenario.txt`.
- `VOTVCOOP_NET_ROLE=host|client`, `VOTVCOOP_NET_PORT`, `VOTVCOOP_NET_PEER`,
  `VOTVCOOP_NET_NICK` — net config.
- `VOTVCOOP_AUTOTEST_{X,Y,Z,YAW,PITCH}` — autotest spawn pose
  (continuous-correction teleport for ~30 s after session start).
- `VOTVCOOP_RUN_GRAB_TEST=1` — autonomous grab test on both peers.
- `VOTVCOOP_NPC_SYNC=1` — enable the wire-layer NPC detection (Phase
  5N1 Inc2 gate).
- `VOTVCOOP_RUN_CHIPPILE_TEST=1` — the chipPile scripted actor (host drives
  a real grab → 8 s moving carry → directional throw → re-pile; client
  scan-only). See the pile harness below.
- `VOTVCOOP_PILE_SHOWCASE=1` — (with the above) the CLIENT teleports to a
  standoff facing the nearest mirrored pile proxy and holds, so an external
  window capture frames the client rendering a pile.
- `VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1` — HOST-only. Settles the docs/piles/08
  Increment-2 gate: drives `playerGrabbed` on the slot-1 PUPPET (an unpossessed
  `mainPlayer_C`, `GetController()==null`) and asserts whether the puppet holds
  + tracks the clump. Verdict (2026-06-22): ENGAGED+HELD but NOT tracked — the
  puppet's tick doesn't drive the PHC, so Increment 2 must drive the hold pose
  host-side. Finding: `research/findings/votv-puppet-grab-feasibility-RE-2026-06-22.md`.
- `VOTVCOOP_RUN_GRAB_INTENT_TEST=1` — the Increment-2 host-side synthetic test. The
  CLIENT picks a mirrored pile proxy, resolves its eid, and sends a `GrabIntent` over
  the wire; the HOST validates + executes `playerGrabbed` on the puppet + broadcasts
  the convert + drives the held clump. Exercises the full client→host wire + router +
  handler + hand-drive WITHOUT the phase-2 client suppress-native / collision. Assert
  via `pile-test-assert.ps1` (`grab-intent-roundtrip`, `puppet-drive-active`,
  `grab-intent-client-echo`). VERDICT PASS 2026-06-22.

## Pile carry/throw log-truth harness (2026-06-22) — the autonomous self-test loop

Built when the user stepped out of the manual-tester role ("истина в логах,
не в пикселях"). The whole pile carry/throw/dup feature is regression-tested
with NO human, by asserting invariants over the host+client logs. Canonical:
the auto-memory `[[reference-pile-test-harness]]`.

- **`tools/pile-test-assert.ps1`** — reads the host + client `votv-coop.log`
  and checks **13 log-truth invariants** (ToPile-loc-nonzero, land-commit-reread,
  client-render-matches-host = the proxy lands at the host pose drift<5cm,
  sound x2, dup-destroy, arc, carry-stuck, fps-reseed-rate, no-crash, …),
  prints a PASS/FAIL table + VERDICT + exit code (DATA-DRIVEN: add an invariant
  by appending to `$Invariants`). This REPLACES the per-round manual greps.
  ```powershell
  pwsh tools/pile-test-assert.ps1                       # default host/client logs
  ```
- **The scripted actor** = `src/votv-coop/src/harness/autotest_chippile.cpp`
  (env `VOTVCOOP_RUN_CHIPPILE_TEST=1`): a REAL host scenario — find a tracked
  chipPile, teleport to a standoff, GRAB via the production `InpActEvt_use` +
  `playerGrabbed`, 8 s moving carry, then a DIRECTIONAL throw
  (`SetActorRootPhysicsVelocity`) so the flight ARC path runs (not a drop).
- **Run a full scenario headless** (host + client + grab/carry/throw, then assert):
  ```
  VOTVCOOP_RUN_CHIPPILE_TEST=1 python tools/mp.py smoke --duration 200 --join-grace 90
  pwsh tools/pile-test-assert.ps1
  ```
  Read the LOGS for the pile verdict, not the smoke's own exit (which only
  checks RSS/connectivity). The DLL writes `[PILE] CLIENT ToPile SNAP ...
  drift=Ncm` so the harness can assert the client renders the host pose.
- **Window capture (for an eyeball screenshot):** `tools/capture-pile-shots.ps1`
  (joined/carry/landed) + `tools/capture-showcase.ps1` (the client-facing-pile
  hold) shoot both windows by title→PID (PrintWindow → foreground-BitBlt
  fallback for real frames). Used to send pile screenshots to the user's phone.

## Available scripts

- **`tools/run-test.ps1`** — single-process autonomous scenario runner.
  Writes `scenario.txt` next to the DLL, then launches the shipping exe.
  ```powershell
  ./tools/run-test.ps1 -Scenario netloopback
  ./tools/run-test.ps1 -Scenario "load:s_may2026" -Save s_may2026
  ```

- **`tools/lan-test.ps1`** — two-process LAN test (host + client) in
  `Game_0.9.0n_dev/`. Per-PID log capture in `tools/test-runs/`. The
  canonical autonomous test harness — found and fixed multiple real
  handshake bugs that single-process loopback hid.
  ```powershell
  ./tools/lan-test.ps1                  # default: play scenario, port 47621
  ./tools/lan-test.ps1 -GrabTest        # adds VOTVCOOP_RUN_GRAB_TEST=1
  ```

- **`mp_host_game.bat`** / **`mp_client_connect.bat`** — hands-on
  launchers (NOT autonomous; for the user playing). Deploy + launch
  via env vars. Same scenario plumbing, just with `VOTVCOOP_SCENARIO=play`.

- **`tools/probe-terminals.ps1`** — Phase 5T terminal probe driver
  (disables the standalone proxy, deploys the UE4SS Lua probe to the
  dev copy, sets `scenario.txt` to `probe_terminals:<save>`, launches
  the game; `-Restore` re-enables the proxy after).

## What the harness logs

Every scenario writes `votv-coop.log` (in the Win64 directory).
Levelled (`[INFO]`, `[WARN]`, `[ERROR]`), timestamped. Boot health
check + scenario startup + every UFunction call site that hits an edge
case logs at least one line.

In netloopback / LAN runs the per-process log is named
`votv-coop-host.log` / `votv-coop-client.log` (the harness sets the log
filename from `VOTVCOOP_NET_ROLE`).

## What's NOT here

- **UI/HUD inspection** — `tools/probes/coopTestHarness` (UE4SS Lua)
  retains a richer Live View inspection surface, used only in the dev
  copy for hypothesis-checking. See `docs/RE_WORKFLOW.md`.
- **JSON report on exit** — never built. The log is the report. If you
  want a structured pass/fail, grep the log for specific tokens (e.g.
  `audit-pass`, `audit-fail`, `health: PASS`).
- **Synthetic OS input injection** — not built. The harness drives the
  engine via reflected UFunctions (SetActorLocation, K2_TeleportTo,
  PrimComp.AddImpulse, etc.) directly; OS-level input synthesis is not
  needed for our scope.
