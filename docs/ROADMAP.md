# Roadmap

Multivoid follows the arc that MTA:SA and Garry's Mod took: functional co-op first, then the
authority moves out of the host's game into its own process, then modes, scripting, resources,
and finally dedicated public servers. Each phase gates the next unless stated otherwise.
Phase 1 is where the work is today.

| # | Phase | Gate | Status |
|--|--|--|--|
| 1 | **Functional co-op** | the game's systems are synced one by one on the mod's own substrate; hands-on verified breadth | **current** |
| 2 | **The arbiter** | per-element authority lives in a separate, engine-free process; the host's game is an ordinary client of it | planned |
| 3 | **Sandbox mode** | VOTV's sandbox rules run as an explicit, portable "mode" layer | planned; independent of 2 |
| 4 | **Scripting substrate** | a sandboxed runtime over the engine and co-op APIs | planned; independent of 2 |
| 5 | **Mode rules in script** | the co-op and sandbox rule sets are the first two reference resources; the C++ core stays native | planned |
| 6 | **Resource system** | custom modes and plugins are one mechanism: manifest, server and client scripts, events, start and stop | planned |
| 7 | **Dedicated server** | the phase-2 process launched by hand, 24/7, with zero players | planned; gated by 2 |
| 8 | **Resource infrastructure** | client-side resource download, sandboxing of untrusted server code, a public server browser | planned |

## Phase 1 — functional co-op (current)

**Built.** The multiplayer foundation: the transport (GameNetworkingSockets, LAN and Internet with
NAT traversal), sessions of up to four peers with a mutual key-based admission and an optional
password, the master server and the server browser, the join pipeline (the host's world streamed
to the joiner, then a connect snapshot), remote players with skins, ragdolls, nameplates, chat and
voice, and the synced world listed on the front page: props, piles, NPCs and the kerfur cycle,
events, weather, the keyed devices, the signal-processing pipeline, the ATV, sleep, damage and
death.

**Remaining.** Hands-on verification breadth (much of the synced world is verified by the
autonomous rig and not yet by people playing), the tail of game systems still unsynced
([COOP_SCOPE.md](COOP_SCOPE.md) says which), and the subsystems shipped in a shape the project
does not accept as final: the ATV mirror and the trash-pile mirror both neutralise the engine's own
actor instead of driving it, and both are queued for a proper rebuild. The per-system status with
its evidence is [COOP_SYNC_PROFILES.md](COOP_SYNC_PROFILES.md).

**Right now.** The repository itself: the public tree is being rewritten for people, one document
per subsystem, with the rules in [../CONTRIBUTING.md](../CONTRIBUTING.md) enforced by a commit hook
and CI gates.

## Phase 2 — the arbiter

Today the host's game process holds the authority: it validates every inbound write and performs
every client intent. Phase 2 moves that arbiter into its own process, spawned as a child when
hosting from in-game and launched by hand for a dedicated box, so that the two deployments are
physically the same binary. The arbiter never reads the engine: it holds values and anchors, and
the engine holds only what has a world-dependent rate. The host's game becomes an ordinary client
of it and loses its privileged in-process path.

The measured work is inverting the lanes that today derive their canonical state by reading the
engine into lanes that remember it. Intent production (reading the local player) stays; handle
validation disappears, because the arbiter holds ids rather than pointers; outcome capture stays,
because the engine's own machines still decide and the arbiter records.

This phase also closes the authority-shaped security findings, which are the absence of this
architecture rather than bugs to patch. Ordering is deliberate: the architecture first, the
findings after.

## Phases 3 to 6 — modes, scripting, resources

Sandbox mode produces the first "rules of a mode" as an explicit layer. The scripting substrate is
a sandboxed runtime over the mod's engine and co-op APIs; which runtime is an implementation
detail, and the modder-facing lean is an engine-level bridge, because VOTV's own modding ecosystem
is Blueprint- and table-shaped. The C++ core (transport, sync, identity, interpolation) stays
native, as MTA keeps its core native. Custom modes and plugins share one resource mechanism; there
is no separate plugin API.

## Phase 7 — dedicated server

Phase 2 delivers the binary, so this phase is much smaller than its name: the same arbiter launched
by hand instead of spawned. What remains is the ghost host (the architecture assumes the host is a
live player with a pawn; a zero-player host needs its local pawn parked and excluded), a Linux
build of the engine-free arbiter behind a launch-of-child abstraction, and the rule that an empty
world freezes: time-linear accumulators are recomputed on unfreeze and gated random rolls simply do
not fire. The server package is the mod plus a launcher and a config dropped onto the operator's
own copy of the game; no game files are ever shipped.

## Phase 8 — resource infrastructure

Clients download resources from the server instead of installing mod packs; server-provided code
runs sandboxed on clients; the signaling service grows into the public master list. Once servers
are public, client-side cheating returns as a threat. Today only the host is trusted, and the host
is the admin; a public-server future needs its own anti-cheat decision here, which MTA answers with
a client-side layer plus server-side validation hooks rather than with authority architecture.

## What this roadmap is not

It is not a rewrite of the game in C++: server-side authoritative physics without the engine is a
decade-class trap. The authority that moves is rules and state machines, which phases 5 and 6
produce as resources. The reverse-engineering record of the game's systems, kept by the maintainer,
is the specification those rules are written from.
