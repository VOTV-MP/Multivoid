# Versioning

## Purpose

What the mod's version is, why two peers must match it byte for byte, and what happens when the
game updates: where the version-specific knowledge lives, how a broken build says so, and the
runbook for porting to a new game version.

## The version pair

The mod is identified by two numbers, the way Paper identifies a Minecraft build: the game
version it targets and its own build number, displayed as `Multivoid 0.10.0 b285`. There is no
separate semantic version. The game target is set once in `src/votv-coop/CMakeLists.txt` and
generated into a header; the build number is the wire protocol's version in `coop/net/protocol.h`,
bumped by every change to any wire format and by every release. The pair is stamped into the
DLL's version resource, printed in the boot banner, and carried in the release zip's name; the
deploy and publish scripts refuse on a mismatch between the resource and the tree. The build
numbers themselves are minted in an append-only ledger in `.github/ci/`, and a tag or a
release page is a drift detector, never the authority ([release.md](release.md)).

Build 286 added the `CheatRequest` and `CheatResult` kinds (a player's cheat request and the host's answer,
[players.md](players.md)), so it is the first build whose F1 menu offers a host's players the cheat tools; a peer on 285 cannot
join one on 286.

Build 290 gave existing kinds new meanings, with no kind added and no size changed: `AmbientFixtureState` families 3, 4, 5, 8,
9 and 10 (level fixtures, a basalt pillar's growth, the Easter bunny, a vending machine's stock, a door's jam, the Erie
plush's flags), `PropIgniteIntent` verbs 2 to 6 and 10 to 12 (a dig, footprints, the bunny's talk ends and egg, a vending buy
and coin, a hit on the plush) and `EventCue` ids 160, 161, 162 and 170 ([events-and-weather.md](events-and-weather.md), [easter-hunt.md](easter-hunt.md),
[erie.md](erie.md), [vending.md](vending.md)). A peer on 289 would drop each as malformed, so it cannot join one on 290.

An unofficial build -- any `main.dll` without its release signature -- shows `(unofficial build)`
after the version. A host refuses a player on a build of the other kind -- an unofficial build on an
official host (MV-H30), the official build on a modified host (MV-H31) -- unless it allows other
builds (`net.allow_other_builds`).

Two peers play together only when their pairs are byte-equal. The build number rides every
packet header, so a peer on another build never parses a message; the game target rides the
Join and is compared on the host, which refuses with a reason the client shows; and the browser
marks a mismatched lobby amber before a connect is tried. An old cohort keeps playing among
itself, and the update check on the main menu informs and never gates.

## Which game is running

The pair says which game the mod was made for, not which game it runs on: a player can update
the game and keep the mod. So the mod reads the running build at start from the game's own
files -- the executable's link stamp and the main pak's index hash, both read without hashing a
file (the file size names nothing: one build measured three sizes, packed and unpacked) -- and
matches them against the builds it knows (`ue_wrap/facts/game_builds.inc`). On a game it was not
made for, it warns and never forbids: a popup names both versions, with Check for an update
(the releases page) and Continue anyway, which is remembered for that build; the menu's version
label reads "(untested game version)"; and the first line of `multivoid-compat-report.txt` says
what runs. Where the engine itself does not read as the mod expects, the mod stands down instead,
and its dialog names both versions too.

The build each side runs also rides the Join, after the game target: one Multivoid on two
different games is refused (MV-J07 on the joiner, MV-H22 on the host), while two players who both
run the same untested build may play. The lobby lists it, so the browser marks a host on another
game build and refuses it before a connect. The update check answers per game version: a master
lists every game's newest release, and the mod offers the one for the game it runs.

## What a game update breaks

Almost all of the mod resolves the game by name at runtime and is version-agnostic. The
version-specific knowledge is deliberately in two files:

| Knowledge | Where | Count | Moves when |
|---|---|---|---|
| signatures for the engine primitives (the object array, the name table, function dispatch, allocation, the present call) | `ue_wrap/core/sdk_profile.h` | five | any recompile of the game |
| engine struct offsets | the same file | about forty | an engine version bump, not a recook |
| game Blueprint offsets | the same file | about fifty, each checked by name | a recook that adds or removes a property before them |
| content names (classes, functions, properties, levels, assets) | `ue_wrap/core/sdk_profile_names.h` | about two hundred and fifty | a rename or a removal |
| every other lookup | the whole tree | over a thousand | only if the name changes; nothing to re-derive |

So a recook costs the signatures it recompiled, the Blueprint offsets it shifted and whatever
names moved: one file plus a name file, not a sweep. The move from 0.9.0n to 0.10.0 is the
measured case: the five signatures and the engine offsets held, the Blueprint offsets shifted
(every `mainPlayer` and `prop` member moved), and the names changed where a class was reworked
(the save, the hands, the inventory) `[V]`. An engine version bump moves the forty engine
offsets together and has never happened to this project; both game versions run UE4.27.

## How a broken build says so

The boot health check runs on every launch and prints a block: each resolved address with its
offset from the image base, a pass or fail per signature, and then a functional round trip, a
known object's name read back, known classes and a known function resolved, because a signature
can match the wrong site and still "succeed". It also logs the executable's version and size and
warns when they differ from the build the mod was built against. A compatibility report is
written beside the DLL on every boot with every resolved address, class, function and offset and
a verdict per primitive; a second check runs once gameplay has loaded and resolves every content
name, since many classes load only on the first level transition, and then reads every fixed
Blueprint offset of `sdk_profile.h` back from the live reflection by property name
(`harness/sdk_check`, the offset section): a moved offset prints the profile value, the live one
and the system it breaks, and a removed property says so. A diff step compares two reports, or
two SDK dumps, and names the constant each change belongs to.

| Symptom | Almost certainly | Look at |
|---|---|---|
| the health check fails a signature | one of the five | the signatures section of the profile |
| signatures pass but the object array reads tiny | an engine offset, an engine bump | the struct offsets |
| a name round trip or a known class fails | the name file, or the name layout | the health-check output |
| everything resolves and one system reads garbage | a game offset moved, or a name changed | the offset section of the SDK check, then the diff against the previous report |
| a hooked Blueprint function never fires | it was renamed, or its dispatch changed | the dispatch map and a fresh dump |
| no chat, no scoreboard, no F1 at all | the present-call signature, which fails closed by design | the overlay's seam |

## The runbook

1. Freeze a baseline: keep the old game install and its header dump.
2. Record the boot artifact: the health-check block, the size and version warning, the first
   lines of the log.
3. Take a fresh SDK dump from UE4SS installed into a copy of the new build (the header dump and
   the object dump); UE4SS is the dump tool here, and the shipped install carries it only as
   the loader.
4. Diff the two dumps and transcribe the offsets that moved. The SDK check's offset section
   lists each one by class and property, so the transcription is checked against the live game,
   not against the dump alone.
5. Keep the signatures and the engine offsets unless the health check failed: they carried over
   unchanged between 0.9.0n and 0.10.0 `[V]`. When one does fail, the loader's log gives
   ground-truth addresses, a disassembler confirms each, and a unique byte pattern with
   wildcarded displacements replaces the old one. An open-source signature resolver for Unreal
   executables is a useful independent first answer.
6. Find the hooks whose logic changed while their names held: run the build with
   `VOTVCOOP_FINGERPRINT_CAPTURE` on the old game and on the new one, and diff the two capture
   files function by function ([bytecode-fingerprint.md](bytecode-fingerprint.md)). A function
   whose fingerprint differs is a hook to re-read; a function that is gone or new is a rename.
   The reference table for the new build (`ue_wrap/facts/fingerprints_<target>.inc`) is
   generated from its capture.
7. Update the identity: the game target, the new build's row (its link stamp and pak hash, from
   the boot's `game-build:` line) with the profile's `kTargetBuild` naming it, the last Multivoid
   build for the previous game version, the build number, and the game target named on
   [install.md](install.md), which a lint checks in CI.
8. Run the gates in order: the health check all green, the config self-test, the two-peer smoke,
   the launcher's scenarios for the systems the diff said moved, and a play session by a person.

## Standing risks

The signatures are the real bill when they break; they need someone who can read a
disassembler, and everything else in a migration is mechanical. Two builds is one measured
migration, not a schedule: a recook that reworks a system moves its offsets, names and logic
together, and that system's sync is rewritten rather than retuned. Do not publish a duration
for the next one.

## The loader

UE4SS loads the mod and is never its engine layer: the DLL imports no symbol from it, a gate
checks that on every build, and the installer pins one UE4SS build by hash, the same build the
mod manager's loader delivers. That gate runs in the build workflow, which is started by hand
rather than on a push, so a local build is what enforces the rule day to day. Moving the
substrate onto UE4SS's own C++ API was weighed and refused: the game half of a migration, the
signatures, the offsets and the names, is the mod's under any substrate, and that API's engine
core sits behind an access gate that would end the plain recursive clone this repository is
built from.

## Code map

| Concept | Files |
|---|---|
| the pair | `src/votv-coop/CMakeLists.txt`, `coop/net/protocol.h`, `coop/session/player_handshake_version.cpp` |
| the version surface | `ue_wrap/core/sdk_profile.h`, `ue_wrap/core/sdk_profile_names.h` |
| the checks | `ue_wrap/core/reflection` (the boot health check), `harness/sdk_check` (names), `harness/fingerprint_census` (logic, [bytecode-fingerprint.md](bytecode-fingerprint.md)), `harness/compat_report` (their one report) |
| the loader | `.github/ci/abi_gate.py`, `loader/cppmod_entry.cpp` |
