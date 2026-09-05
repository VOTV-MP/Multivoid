# Versioning

## Purpose

What the mod's version is, why two peers must match it byte for byte, and what happens when the
game updates: where the version-specific knowledge lives, how a broken build says so, and the
runbook for porting to a new game version.

## The version pair

The mod is identified by two numbers, the way Paper identifies a Minecraft build: the game
version it targets and its own build number, displayed as `Multivoid 0.9.0n b152`. There is no
separate semantic version. The game target is set once in `src/votv-coop/CMakeLists.txt` and
generated into a header; the build number is the wire protocol's version in `coop/net/protocol.h`,
bumped by every change to any wire format and by every release. The pair is stamped into the
DLL's version resource, printed in the boot banner, and carried in the release zip's name; the
deploy and publish scripts refuse on a mismatch between the resource and the tree. The build
numbers themselves are minted in an append-only ledger in `tools/release/`, and a tag or a
release page is a drift detector, never the authority ([RELEASE.md](RELEASE.md)).

Two peers play together only when their pairs are byte-equal. The build number rides every
packet header, so a peer on another build never parses a message; the game target rides the
Join and is compared on the host, which refuses with a reason the client shows; and the browser
marks a mismatched lobby amber before a connect is tried. An old cohort keeps playing among
itself, and the update check on the main menu informs and never gates.

## What a game update breaks

Almost all of the mod resolves the game by name at runtime and is version-agnostic. The
version-specific knowledge is deliberately in two files:

| Knowledge | Where | Count | Moves when |
|---|---|---|---|
| signatures for the engine primitives (the object array, the name table, function dispatch, allocation, the save call, the present call) | `ue_wrap/core/sdk_profile.h` | six | any recompile of the game |
| engine struct offsets | the same file | about forty | an engine version bump, not a recook |
| game Blueprint offsets | the same file | about thirty | a recook that adds or removes a property before them |
| content names (classes, functions, properties, levels, assets) | `ue_wrap/core/sdk_profile_names.h` | about two hundred | a rename or a removal |
| every other lookup | the whole tree | over a thousand | only if the name changes; nothing to re-derive |

So a recook costs six signatures, thirty offsets and whatever names moved: one file plus a name
file, not a sweep. An engine version bump moves the forty together and has never happened to
this project; the game has run one engine version its whole life.

## How a broken build says so

The boot health check runs on every launch and prints a block: each resolved address with its
offset from the image base, a pass or fail per signature, and then a functional round trip, a
known object's name read back, known classes and a known function resolved, because a signature
can match the wrong site and still "succeed". It also logs the executable's version and size and
warns when they differ from the build the mod was built against. A compatibility report is
written beside the DLL on every boot with every resolved address, class, function and offset and
a verdict per primitive; a second check runs once gameplay has loaded and resolves every content
name, since many classes load only on the first level transition. `tools/sdk_diff.py` diffs two
reports, or two SDK dumps, and names the constant each change belongs to.

| Symptom | Almost certainly | Look at |
|---|---|---|
| the health check fails a signature | one of the six | the signatures section of the profile |
| signatures pass but the object array reads tiny | an engine offset, an engine bump | the struct offsets |
| a name round trip or a known class fails | the name file, or the name layout | the health-check output |
| everything resolves and one system reads garbage | a game offset moved, or a name changed | the diff against the previous report |
| a hooked Blueprint function never fires | it was renamed, or its dispatch changed | the dispatch map and a fresh dump |
| no chat, no scoreboard, no F1 at all | the present-call signature, which fails closed by design | the overlay's seam |

## The runbook

1. Freeze a baseline: keep the old game install and its header dump.
2. Record the boot artifact: the health-check block, the size and version warning, the first
   lines of the log.
3. Take a fresh SDK dump from UE4SS installed into a copy of the new build (the header dump and
   the object dump); UE4SS is a development tool here and does not ship.
4. Diff the dumps with `tools/sdk_diff.py` and transcribe the offsets it names.
5. Re-derive the signatures only if the health check failed: the loader's log gives ground-truth
   addresses, a disassembler confirms each, and a unique byte pattern with wildcarded
   displacements replaces the old one. An open-source signature resolver for Unreal executables
   is a useful independent first answer.
6. Update the identity: the game target, the expected executable size, the build number, and
   the game target named on [INSTALL.md](INSTALL.md), which a lint checks in CI.
7. Run the gates in order: the health check all green, the config self-test, the two-peer smoke,
   the launcher's scenarios for the systems the diff said moved, and a hands-on run by a person.
8. Write down what it cost. Nothing here has been done for real yet: the mod has only ever run
   against one game version, so every duration is unknown, not small.

## Standing risks

The six signatures are the real bill; they need someone who can read a disassembler, and
everything else in a migration is mechanical. The runbook is untested and will change on first
contact. Do not publish a duration for a migration nobody has done.

## The loader

UE4SS loads the mod and is never its engine layer: the DLL imports no symbol from it, a gate in
CI proves that on every build, and the installer pins one UE4SS build by hash, the same build the
mod manager's loader delivers. Moving the substrate onto UE4SS's own C++ API was weighed and
refused: the game half of a migration, the signatures, the offsets and the names, is the mod's
under any substrate, and that API's engine core sits behind an access gate that would end the
plain recursive clone CI proves on every push.

## Code map

| Concept | Files |
|---|---|
| the pair | `src/votv-coop/CMakeLists.txt`, `coop/net/protocol.h`, `coop/session/player_handshake_version.cpp` |
| the version surface | `ue_wrap/core/sdk_profile.h`, `ue_wrap/core/sdk_profile_names.h` |
| the checks | `ue_wrap/core/reflection` (the boot health check), `harness/sdk_check`, `tools/sdk_diff.py` |
| the loader | `tools/install-ue4ss.ps1`, `tools/loader/abi_gate.py`, `loader/cppmod_entry.cpp` |
