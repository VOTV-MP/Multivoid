# Feasibility

The questions a co-op mod asks of its game before any code, and the measured answers for
*Voices of the Void*. The game is Unreal Engine 4.27 (the engine version string is in the
shipping executable; the physics library and the packaging layout corroborate it) and the mod
targets one game version at a time ([versioning.md](versioning.md)).

## Why this engine changes the method

The co-op method the project follows was written around a closed native engine that needed blind
binary reverse engineering. Unreal is a documented engine with reflection: every class, property
and function is enumerable at runtime through the object array and the name table, so most of the
"engine archaeology" is introspection rather than disassembly, and a disassembler is needed only
for what reflection cannot see. The engine already runs several pawns and player controllers at
once (split screen, listen servers), so "spawn a second protagonist" is a factory call it supports
by design. A mature modding stack, UE4SS, provides a loader, a live object browser, header and
object dumps and Lua probes; the mod uses it as the loader and the development tool and imports
nothing from it ([ARCHITECTURE.md](ARCHITECTURE.md)).

## The questions

**Is the binary unpacked?** Yes. A standard shipping build with no anti-tamper; the executable is
not encrypted at runtime. Not a blocker.

**What is the rendering API?** DirectX 11 or 12, or Vulkan, selectable in the launcher. The mod's
overlay hooks the swap chain's present call with its own detour and draws on DirectX 11 and 12
through a per-API backend ([ui.md](ui.md)); Vulkan has no DXGI swap chain to hook and is not
covered.

**What is the input API?** The engine's own input pipeline over the window's message pump. The
mod reads its own hotkeys through a window-procedure subclass and the asynchronous key state, and
replicates a remote player at the pawn and movement-component level, never at the OS layer.

**What is the entity model?** Actors, pawns and controllers with one universal factory. The
player is a character class driven by a player controller, and a second instance of that class
spawns and ticks as an unpossessed orphan with auto-possession and the AI controller disabled;
whether an actor has a controller is how the mod tells the local player from a puppet
([players.md](players.md)).

**What is the script VM?** The Blueprint VM; the game's logic is almost entirely Blueprint. The
mod hooks the engine's function dispatch with its own detour, patches individual native functions,
and observes the VM's internal virtual calls where neither reaches; which calls each seam sees is
mapped on [COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md).

**What is the save format?** The engine's own save-game serialisation, one flat unencrypted file
per slot under the user's local application data, about twenty megabytes with photos inline,
written in place with no rename. The host owns the save and streams it to a joiner, who loads it
through the game's own loader ([join.md](join.md)).

**Is the game single-instance?** No. There is no mutex or instance lock, so two or more copies run
on one machine, which is how the whole test rig works ([testing.md](testing.md)).

## Blockers

None. A documented engine, no anti-tamper, a hookable dispatch and native multi-pawn support make
the game a good fit for the method. The risk is scope, not feasibility: the game is a large
simulation of signals, a base and a day cycle, which is why [SCOPE.md](SCOPE.md) is law.
