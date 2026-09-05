# tools

Build, deploy, test, release and repository helpers. Nothing here is load-bearing game state;
everything is regenerable. Scripts that served one investigation or one operator's habits are
not tracked, so what is listed here is what a contributor needs.

## Build and deploy

- `install-ue4ss.ps1 [-Win64Dir <path>] [-Quiet] [-Force] [-ZipPath <zip>]`: the one-time
  install of the pinned UE4SS build into a game copy. UE4SS is the loader on every copy. It never
  overwrites existing `Mods/mods.txt` or `UE4SS-settings.ini` state; `-Quiet` is the play profile
  (GUI console off). It verifies `UE4SS.dll` and `dwmapi.dll` by SHA256 and fails closed on a
  mismatch; a mismatch on an already-installed copy re-extracts, so moving the pin reaches
  everyone. It refuses while the game is running. Source order: `-ZipPath`, then a mod manager's
  package cache, then the download. The package version is one of four copies (here,
  `release/ledger_lib.ps1`, `release/package_drill.ps1`, this file); they move together, and
  nothing enforces it yet.
- `deploy-mod.ps1 -GameWin64 <path> [-Remove]`: idempotent deploy of the built DLL into the mod
  folder (`Mods\Multivoid\dlls\main.dll` plus `enabled.txt`), skipping when byte-identical, and a
  one-time removal of the retired proxy files beside the exe. `-Remove` deletes the mod folder;
  the UE4SS substrate stays.
- `deploy-all.ps1`: the same into all four game copies at once. Run after `cmake --build`.
- `maprva.py <rva>...`: maps a faulting address the game-thread firewall logs to its enclosing
  function in the build's `.map` file.

## The rig

- `mp.py`: the launcher and the scenario runner; `docs/testing.md` is its manual.
  `host`, `client`, `client2` and `client3` deploy and launch one peer each; `smoke` and `smoke4`
  run the two- and four-peer LAN smokes; the scenario subcommands drive the in-game harness and
  grade its logs.
- `game_lock.py`: one game, one rig session at a time. `mp.py` takes the lock, because every
  scenario begins by killing every game process on the box.
- `capture_window.ps1 -ProcId <pid> -Out <png>`: grabs a game window from outside the process
  (PrintWindow, with a screen BitBlt fallback); the in-process screenshot path captures black from
  the 3D swap chain.
- `pile-test-assert.ps1`: the log-truth harness for the pile carry and throw loop, one verdict
  line per invariant.
- `fake_master.py`: a local master serving a synthetic lobby list, the row source for the browser
  scenarios.
- `p2p_smoke.py`: the two-peer rendezvous smoke through the signaling relay, local by default,
  against the live relay with `--signaling`.
- `net/roster_shot.ps1`: photographs a client's roster screen in a four-peer run.

## Gates in CI

Each refuses a class of defect the project has shipped once. `build-core.yml` and
`repo-gates.yml` run them on every push; several carry a `_drill` sibling that proves the gate
still fires on a known-bad fixture.

- `loader/abi_gate.py`: the shipped DLL imports nothing from UE4SS.
- `config/registry_gate.ps1`: the config registry's invariants, each job with a must-fire
  control run.
- `gc/gc_pin_gate.ps1`: a GC pin is owned by one object, never a hand-written flag pair.
- `hooks/minhook_free_gate.ps1`: nothing frees a MinHook trampoline.
- `net/reliablekind_gate.py`: every reliable message kind reaches a receiver, or names why not.
- `net/peerconn_gate.ps1`: the per-slot occupancy-generation invariant.
- `net/master_contact_gate.py`: a census, by operation, of everything that can tell an outside
  server where a player is.
- `text/atlas_regime_gate.ps1`: the glyph-atlas regime, checked positively.
- `docs/public_prose_gate.py` and `docs/public_leak_gate.py`: the public tree measured against
  `CONTRIBUTING.md` as a ratchet, and the leak classes that bit before.
- `git/commit_msg_check.py`: the shape of a commit message, installed as the `commit-msg` hook
  (`git config core.hooksPath tools/git/hooks`).

## Gates run by hand

- `text/nick_gate.ps1`: no code path cuts a nickname mid-character; it polices the operation
  kind, not a list of sites.
- `ui/browser_parity_gate.py`: the native server browser still does everything the fallback
  browser does.
- `sig_gate.py`: the signaling relay refuses a name its caller cannot sign for; locally against a
  built relay, or `--remote` against the live one.
- `cert_check.py`: the off-box freshness check of the control plane's TLS certificate.

## Release

`release/` is the release lane and `docs/RELEASE.md` the procedure. `package.ps1` assembles the
one distributable zip; `publish.ps1` is the draft-first publish step; `judge.ps1` the
refuse-to-publish predicate; `ledger_lint.ps1` the drift detector over `LEDGER.tsv`;
`fingerprint.ps1` the proven-runnable toolchain gate; `tripwires.ps1` the UE4SS-pin trip-wires;
`verify_latest.ps1` the closing check; `tag_regex_selftest.ps1` the tag-grammar fixtures;
`notes/` the per-build release notes, with `notes_regen.ps1` to rebuild a live release body from
them; `README_thunderstore.md` the package page.

## The master server

`coop-server-rs/` is the master and the signaling relay, in Rust. Its README says how to build
and run them; `docs/master-server.md` says what they are for.

## The skin converter

`client_model/` turns a GoldSrc model into a cooked skeletal mesh and a pak the mod loads as a
player skin, without the Unreal editor. Its README is the pipeline and `SPEC.md` the byte-level
cook spec.

## Text tables

`text/build_repertoire.py` mints the emoji donor font and the repertoire constant from one
measurement; `text/case_fold.py` and `text/unicode_case_14_0_0.py` are the frozen
simple-lowercase tables, restricted to what the build draws.

## Reverse-engineering instruments

None of these ship; `docs/RE_WORKFLOW.md` is the method.

- `bp_cpp.py <BP>`: a whole Blueprint as readable pseudo-C++ (`--offsets` for the listing where
  every line carries its bytecode offset).
- `bp_cfg.py <BP> [--fn <Fn>]`: per-function control-flow graphs of a Blueprint, as text and as
  rendered graphs.
- `bp_reflect.py`: the raw bytecode of a Blueprint as JSON.
- `sdk_diff.py <old_dump_dir> <new_dump_dir>`: diffs two UE4SS header-dump trees after a game
  update; `docs/versioning.md` is the procedure.
- `debug/ida_aob_derive.py`: derives a unique byte signature for a function in IDA, for the seams
  reflection cannot name.
