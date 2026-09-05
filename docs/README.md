# Multivoid documentation

Multivoid adds co-op multiplayer to *Voices of the Void*, a single-player Unreal Engine 4.27 game,
without modifying a single original game file. These pages describe what the mod is, what it syncs,
how it works, and how to work on it. Pick a lane; nobody reads this tree top to bottom.

## I just want to play it

| | |
|---|---|
| [INSTALL.md](INSTALL.md) | Install, update, uninstall. The single owner of that text; the README and every release only link here |
| [../SECURITY.md](../SECURITY.md) | What the mod does and does not protect, and how to report a vulnerability. Read the "what does not hold" part before hosting for strangers |
| [../README.md](../README.md) | The front page: what works today, where to get builds, who contributed |
| [CREDITS.md](CREDITS.md) | Every outside code contribution, report and review that changed the mod, and what shipped from it |

## I want to understand it, or help

Start here, in this order:

| | |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | The one read: the layers, who owns which state, how bytes move, how peers and entities are named, and how a join works |
| [SCOPE.md](SCOPE.md) | What is and is not replicated. This one is law: anything not listed is deliberately not synced |
| [DEVS_GAUNTLET.md](DEVS_GAUNTLET.md) | The VOTV developers' public statement on why multiplayer mods fail. It is the bar this project builds to |
| [FEASIBILITY.md](FEASIBILITY.md) | Whether this is possible at all, answered with measurements |
| [ROADMAP.md](ROADMAP.md) | Where it is going: the phases and where we are in them |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | The rules a change must respect, and the shape a commit and a doc must have |

Then, if you are going to touch code:

| | |
|---|---|
| [RE_WORKFLOW.md](RE_WORKFLOW.md) | How this project reverse-engineers the game: reflection first, then IDA, then UE4SS as a probe. None of those ship |
| [AUTONOMOUS_TESTING.md](AUTONOMOUS_TESTING.md) | The two-instance LAN harness: how a change is smoke-tested without a human in the loop |
| [RELEASE.md](RELEASE.md) | How a build becomes a release, and the gates it must pass |
| [VERSION_MIGRATION.md](VERSION_MIGRATION.md) | What happens when VOTV updates: the measured version surface and the port runbook |
| [MULTIPLAYER_UI.md](MULTIPLAYER_UI.md) | The menus, the server browser, the master and signaling servers behind them |
| [VOTV_UI_STYLE.md](VOTV_UI_STYLE.md) | The game's own widget style, measured; binding for anything drawn inside VOTV's UI |

Before writing any entity-sync, hook or spawn-catch code, read these three:

- [COOP_SYNC_DOCTRINE.md](COOP_SYNC_DOCTRINE.md), how a system gets synced here: foundation first,
  the authority table, the dispatch-seam ladder, brain parking, identity, the mandatory late-join
  answer, and the forbidden-crutch list.
- [COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md), will my hook even fire? Visible versus
  invisible Blueprint dispatch, and the trap that `init()` is Blueprint-internal.
- [COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md), how each entity gets identity,
  expression and destruction, plus the duplication matrix.

## I maintain this

**Models, how authority and state are meant to work**

The authority model, the server model behind the roadmap, the transport, identity and the join are
all in [ARCHITECTURE.md](ARCHITECTURE.md). Beyond it: [COOP_RNG_AUTHORITY.md](COOP_RNG_AUTHORITY.md)
(who rolls shared-world randomness)

**Subsystems**, one page each on the same skeleton

[join.md](join.md) (joining a game: admission, the save transfer, the connect replay, the join
window, and the late-join rule every lane answers) · [players.md](players.md) (the remote player:
the puppet, the pose stream, names, skins, damage, death, sleep, inventory, moderation) ·
[props.md](props.md) (props: identity, the birth and death seams, holding and throwing, containers,
the props that change on their own, the deployables) · [piles.md](piles.md) (trash piles: the
host's id across pile and clump, the client's grab as an intent, the dispenser piles) ·
[npcs-and-kerfur.md](npcs-and-kerfur.md) (host-owned characters, the creatures each peer owns, the
killer wisp, the roaches, and the kerfur robot across its two forms)

**Maps, where a thing lives and what state it is in**

[CODE_MAP.md](CODE_MAP.md) (where every concept lives: one folder each, and the files in it) ·
[STATUS.md](STATUS.md) (what is synced, system by system, who owns it, and how well each claim is established)

**Per-domain trees**, one folder per game system, each with its own README

[events/](events/) · [signals/](signals/) (the signal-processing pipeline) ·
[upgrades/](upgrades/) · [notifications/](notifications/) ·
[vehicles/](vehicles/) (the ATV)

## How to read a claim in these docs

Status and evidence are tagged, and the tags are load-bearing:

| Tag | Means |
|---|---|
| `[V]` | measured, with a citation: a log line, a disassembly, a file and line |
| `[RD]` | derived from reverse engineering, not directly observed running |
| `[A]` | reported by a read-only review pass, not personally re-verified |
| `[?]` | unverified: a hypothesis wearing a claim's clothes |
| DESIGN vs AS-BUILT | what was planned vs what shipped. These drift, and the docs say when they did |

Nothing is marked working on the strength of an automated smoke test alone; that gets called a smoke
pass, and it says so. A doc that says something works without naming its evidence is a bug in the
doc.

## What is not here

The maintainer's working notes, reverse-engineering logs, design drafts, session records and the
review tooling that produced them are kept outside the repository. They serve one development
workflow; a contributor brings their own. What the repository carries is the current state of the
mod and the rules for changing it ([../CONTRIBUTING.md](../CONTRIBUTING.md)).
