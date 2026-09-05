# Testing

## Purpose

How to run the test rig, what it can stage, and the gates a push and a release run. The rig is two
to four copies of the game on one machine, driven by one launcher and judged by their logs.

## How it works

### The rig

Four copies of the game live beside the repository, each with its own saved games and logs: a
host, two clients and a fourth kept for development. `tools/deploy-all.ps1` deploys the built DLL
into every copy's mod folder (over `tools/deploy-mod.ps1`, which skips a copy whose bytes already
match); `tools/install-ue4ss.ps1` installs the pinned UE4SS loader into a copy once, verifying its
hash. Nothing in a game copy is tracked.

### The launcher

`tools/mp.py` deploys and launches peers and runs scenarios. `host`, `client`, `client2` and
`client3` launch one peer each for hands-on play; `kill` stops every game process. Every step
prints a line as it happens, and the game is launched detached so the launcher never holds its
pipes.

The smoke is the standing regression check:

```
python tools/mp.py smoke --duration 60
```

It deploys, launches the host windowed, waits for it to bind its port, launches a client, samples
every process's memory on an interval, kills everything if a peer crosses the memory threshold,
tails both logs, kills both peers, and passes only if both were alive at the last sample. Its own
verdict is liveness and memory; feature verdicts are read from the logs. `smoke4` runs a host and
three clients with a staggered connect and proves the host relay end to end by finding, in each
client's log, a puppet spawned for another client.

The other subcommands stage one scenario each:

| Area | Subcommands |
|---|---|
| joins and the world | `joinchurn`, `reloadchurn`, `menutravel`, `wirewindow`, `fogprobe`, `deadmaster`, `gracefulexit`, `authdrill` |
| players, entities and props | `npctest`, `kerfurtoggle`, `death`, `ragdollshot`, `ragdollspawn`, `puppetshot`, `walkgrab`, `clumpvis`, `spawnmenutest`, `navprobe` |
| devices and races | `lightgroup`, `ctakeprobe`, `ctakerace` |
| screens and captures | `browser`, `nativeui`, `menushot`, `scoreshot`, `hudtint`, `chathistory`, `chatseed` |
| variants of the smoke | `smoke_phystele`, `smoke_i18n` |

`ctakerace` is the shape a concurrency test takes here: the host and a client both walk to the
same container and take the same item at one GO barrier (a future-timestamp sentinel file, which
gives sub-millisecond simultaneity on one machine); each peer counts the item afterwards and the
launcher sums across peers, so one is correct, two is a duplicate and zero is a loss. Its
`control` mode, where only the host takes, must sum to one before a race result is trusted.

### The in-game harness

The harness ships inside the DLL (`harness/`). At load it reads a scenario from a file beside the
mod or from the environment, and the scenario selects a code path that posts engine actions onto
the game thread: `play` loads a save and idles, `load:<slot>` loads a named slot, `none` launches
with no automation. Environment variables override the ini for one launch (role, port, peer,
nickname, spawn pose), which is how the launcher configures a peer without editing files. Each
scenario is one file under `harness/autotest/`, armed by its own environment gate. A scenario
drives the engine through reflected function calls and walks a bot by asking the game for a path
to a target, never through synthesised input.

### Logs and assertions

Every copy writes a levelled, timestamped `multivoid.log` beside the mod, and a scenario's verdict
is a line in it. `tools/pile-test-assert.ps1` checks the pile carry-and-throw loop against
thirteen invariants across both peers' logs and prints a verdict table. `tools/capture_window.ps1`
grabs a game window from outside the process for the screenshot scenarios.

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
| The rig stages what a scenario scripts: a bot walks to a chosen target and performs the scripted action. Nobody plays the game freely, so a mechanic's feel and anything visual are judged by a person | `[V]` `tools/mp.py`, `harness/autotest/` |
| A race is staged only where a scenario provides a barrier; the container take is the one that has it, other concurrent interactions have no scenario yet | `[V]` `tools/mp.py` (`ctakerace`) |
| The smoke's own verdict is liveness and memory; a regression that keeps both peers alive passes it unless a feature assertion reads the logs | `[V]` by design |
| No structured report; a verdict is a line in a log | `[V]` |

## Code map

| Concept | Files |
|---|---|
| the launcher and the rig | `tools/mp.py`, `tools/deploy-all.ps1`, `tools/deploy-mod.ps1`, `tools/install-ue4ss.ps1` |
| the in-game harness | `harness/harness`, `harness/session_runtime`, `harness/autotest`, `harness/autotest_dispatch`, `harness/autotest/`, `harness/sdk_check`, `harness/screenshot` |
| log assertions and captures | `tools/pile-test-assert.ps1`, `tools/capture_window.ps1` |
| the gates | `tools/loader/abi_gate.py`, `tools/docs/public_prose_gate.py`, `tools/docs/public_leak_gate.py`, `tools/git/commit_msg_check.py`, `.github/workflows/build-core.yml`, `.github/workflows/repo-gates.yml`, `.github/workflows/release-core.yml` |
