# Reverse-engineering workflow

How the game is reverse-engineered during development: reflection first, then a disassembler,
then UE4SS as a live probe. None of it ships. The mod is a UE4SS mod, so every game copy runs the
UE4SS loader (installed once by `tools/install-ue4ss.ps1`) and the mod loads from its mod folder;
the own-substrate rule is about imports, not presence: the DLL imports nothing from UE4SS, and
beyond being the loader, UE4SS is the everyday reverse-engineering tool.

## The four game copies

| Path | Role | Use |
|---|---|---|
| `Game_0.9.0n_HOST/` | host | hands-on hosting (`tools/mp.py host`) |
| `Game_0.9.0n_CLIENT_1/` | client | hands-on joining (`tools/mp.py client`) |
| `Game_0.9.0n_CLIENT_2/` | second client | three-peer runs |
| `Game_0.9.0n_CLIENT_3/` | development | autonomous runs (`tools/mp.py smoke`), Live View, Lua probes, Blueprint dumps |

Each copy keeps its own saved games, logs and screenshots, so an autonomous run never collides
with hands-on play. The development copy also carries UE4SS's bundled Lua mods and the UE4SS
log. `tools/deploy-all.ps1` deploys the built DLL to all four after a build.

## What UE4SS gives during development

**Live View**, the object inspector. Browse every object in the object array, expand any object's
property tree, and edit values live. Use it to pick an instance by name and watch its fields tick
by tick (the animation blend inputs of a puppet, a prop holder's state), to test a hypothesis by
editing a field, and to check an offset suspected of having moved between game versions.

**The function caller.** From Live View, call any reflected function with typed parameters. Use
it to test-fire a function before wiring it into the DLL, and to catch a parameter-name mismatch
between the game's declaration and the mod's marshalling.

**The object dump** (F8): every object in the array to a text file. Use it to see what exists in a
given game state and to find the exact name of a class you suspect exists.

**The header dump** (F7): every class layout as C++ headers. This is the SDK the offsets in
`ue_wrap/core/sdk_profile.h` are derived from; re-dump it after a game update
([versioning.md](versioning.md)).

**Lua probes**, run inside UE4SS's Lua VM in the development copy (a folder under its `Mods`,
enabled in its mods list): list every function matching a pattern, dump properties per tick,
sanity-check an offset the header dump claims, and validate a new game build before the DLL is
touched. A probe is an experiment; nothing in this tree ships one.

**Blueprint bytecode.** The game's logic is compiled Blueprint, and a function's signature tells
you nothing about what it does. The project's own wrappers read the cooked assets out of the pak:
`tools/bp_cpp.py` renders a whole Blueprint as readable pseudo-C++ (with `--offsets` for the
listing where every line carries its bytecode offset, the currency the docs cite),
`tools/bp_cfg.py` draws per-function control-flow graphs, and `tools/bp_reflect.py` gives the raw
bytecode as JSON. Control flow is read only from those, never inferred from a header.

## The escalation ladder

Reflection and the dumps first. When an offset, a layout or a crash cannot be explained from
reflection, drop to a disassembler on the exact call site. When the disassembler cannot answer
either (live behaviour, Blueprint semantics), use UE4SS as the probe. The order is reflection,
then the disassembler, then UE4SS.

## Porting from UE4SS's source, without depending on it

Reflection patterns and signatures are ported from UE4SS's open-source tree
(`reference/RE-UE4SS/`) into the mod's own wrapper with attribution: the signatures for the
object array, the name table and function dispatch, the property traversal, the name decoder. The
workflow is to read the relevant source, adapt the algorithm into the wrapper's own style with no
UE4SS types or headers so the ported code compiles from this tree alone, confirm parity with a Lua
probe, and leave a comment naming the file it was adapted from.

Not borrowed: UE4SS's C++ mod API (the ABI fragility the zero-import contract exists to avoid),
its pak-mounting mod loader, its Lua VM (scripting is its own roadmap phase over the mod's own
APIs), and its UI framework (the overlay links the mod's own vendored ImGui).

## Two habits

Deploy to all four copies when iterating on the shipping DLL, and never assume what works in the
development copy works in the play copies: the development copy runs UE4SS's GUI console and its
bundled Lua mods, which hook the same dispatch and force some classes to load earlier.

For a reverse-engineering session, launch the development copy, open Live View and find the
relevant object before writing the first line of code. Examples: expand the player class and its
animation instance and watch its pawn, controller, movement and velocity fields as you walk; call
a container's take function from the caller and inspect the returned actor's key; filter the
kerfur classes and see the spawner relationships and the per-instance divergences.
