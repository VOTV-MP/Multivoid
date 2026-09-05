# Testing

## Purpose

How a change is tested without a human in the loop, what that proves and what it cannot, and
the gates a push and a release run. The rig is two to four copies of the game on one machine,
driven by one Python launcher, judged by their logs.

## How it works

### The rig

Four copies of the game live beside the repository, each with its own saved games and logs:
a host, two clients, and a fourth used for development and autonomous runs. One script deploys
the built DLL into every copy's mod folder, skipping a copy whose bytes already match
(`tools/deploy-all.ps1`, over `tools/deploy-mod.ps1`); another installs the pinned UE4SS
loader into a copy once, verifying its hash and refusing on a mismatch
(`tools/install-ue4ss.ps1`). Nothing in a game copy is tracked.

### The launcher

`tools/mp.py` deploys and launches peers and runs the scenarios; the root `.bat` files are thin
shims over it for a person hosting or joining by hand. Every step prints a line as it happens,
and the game is launched detached so the launcher never holds its pipes.

The smoke is the standing regression check:

```
python tools/mp.py smoke --duration 60
```

It deploys, launches the host windowed, waits for it to bind its port, launches a client, samples
every process's memory on an interval for the duration, kills everything if any peer crosses the
memory threshold, tails both logs, kills both peers, and passes only if both were alive at the
last sample and nothing breached the threshold. Its own verdict is liveness and memory; the
subsystem verdicts are read from the logs. `smoke4` runs a host and three clients with a
staggered connect and proves the host relay end to end by finding, in each client's log, a
puppet spawned for another client. The other subcommands are one scenario each: join churn,
world-reload churn, NPC and kerfur drills, the death run, the container take race, the browser
lab run, the chat history and seed drills with a must-fail injection, the admission drill, a
dead master, a graceful exit, and screenshot runs for the menu, the scoreboard and the puppet.

### The in-game harness

The harness is part of the shipping DLL (`harness/`). At load it reads a scenario from a file
beside itself or from the environment, and the scenario selects a code path that posts engine
actions onto the game thread: `play` loads a save and idles for hands-on play, `load:<slot>` loads
a named slot, `none` launches with no automation. Environment variables override the ini for one
launch, so a launcher sets the role, port, peer, nickname and spawn pose without editing files.
Each feature's autonomous scenario is one file under `harness/autotest/`, armed by its own
environment gate: grab, ragdoll, weather, events, death, damage, the pause guard, the save UI,
scan parity and the rest. A scenario drives the engine through reflected function calls, never
through synthesised input.

The log is the report. Every copy writes a levelled, timestamped `multivoid.log` beside the mod,
and a scenario's verdict is a line in it. Where a feature has invariants worth asserting across
both peers' logs, a script does it: `tools/pile-test-assert.ps1` checks the pile carry and throw
loop against thirteen log-truth invariants and prints a verdict table. Screenshots for a human's
eye come from an external window capture (`tools/capture-window.ps1`); the game's own screenshot
command is used only in autonomous runs, because its toast is distracting in play.

### What autonomy proves

An autonomous pass validates one process's code paths. It does not prove co-op correctness under
load, the feel of a mechanic, or anything visual; the evidence ladder on [STATUS.md](STATUS.md)
puts a hands-on observation above a log line, a log line above a self-test, and a self-test above
the lane merely existing. A change is not called working on the strength of the smoke alone.

### The gates a push runs

Every push builds and gates in CI (`.github/workflows/`). The build workflow checks out the
requested source, initialises only the third-party submodules, and runs the code gates before
the compile: the config registry, the peer-slot generation, the master contact (nothing tells
an outside server where a player is unasked), the reliable-kind routing (every wire kind reaches
a receiver or says why not), the MinHook free-call rule, the GC pin ownership, the font atlas
regime, and the package drill. Then it restores the pinned dependency cache, builds Release,
runs the zero-import ABI gate on the built DLL, packages and verifies the mod zip, and uploads
the artifacts. A second workflow gates the repository itself: the public-leak gate, the
commit-message check over every commit since the checker was added, and the public-prose gate,
each preceded by a drill that proves the gate can fail.

A release is a tag: the release workflow judges the tag against the ledger, refuses early on a
stale fingerprint, rebuilds the tagged source without caches, and publishes the zip with its
checksum ([RELEASE.md](RELEASE.md)).

## Known limits

| Limit | Evidence |
|---|---|
| No autonomous player walks the world; every scenario teleports to a standoff and injects one action, so the concurrent races two humans cannot stage are not staged by the rig either | `[V]` a navmesh-walking director is designed and not built |
| The smoke's own verdict is liveness and memory; a subsystem regression that keeps both peers alive passes it | `[V]` by design; the log assertions are per feature |
| No structured report; a pass or fail is a token in a log | `[V]` |
| Visuals need a person; the screenshot runs frame a capture but do not judge it | `[V]` |

## Code map

| Concept | Files |
|---|---|
| the launcher and the rig | `tools/mp.py`, `tools/deploy-all.ps1`, `tools/deploy-mod.ps1`, `tools/install-ue4ss.ps1`, the root `.bat` shims |
| the in-game harness | `harness/harness`, `harness/session_runtime`, `harness/autotest`, `harness/autotest_dispatch`, `harness/autotest/`, `harness/sdk_check`, `harness/screenshot` |
| log assertions and captures | `tools/pile-test-assert.ps1`, `tools/capture-window.ps1` |
| the gates | `tools/loader/abi_gate.py`, `tools/docs/public_prose_gate.py`, `tools/docs/public_leak_gate.py`, `tools/git/commit_msg_check.py`, `.github/workflows/build-core.yml`, `.github/workflows/repo-gates.yml`, `.github/workflows/release-core.yml` |
