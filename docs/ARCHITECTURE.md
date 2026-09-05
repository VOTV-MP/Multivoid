# Architecture

Multivoid is a co-op layer for *Voices of the Void* (Unreal Engine 4.27) that modifies no game
file. It ships as a UE4SS mod folder (`Mods/Multivoid/dlls/main.dll` plus `enabled.txt`): UE4SS
loads the DLL and calls its exported `start_mod()`, and from that call on everything is the mod's
own code. The mod finds the game's classes and functions through Unreal's reflection, hooks the
engine's function dispatch, and adds the other players by driving the engine's own pawn and
controller classes. Single-player keeps working untouched; a co-op session is single-player with
more people attached.

This page is the one read: the layers, who owns which state, how bytes move, how peers and
entities are named, and how a join works. Each subsystem has its own page for the details, and
[CODE_MAP.md](CODE_MAP.md) says where the code is.

## The layers

```
coop/          gameplay and network: elements, sync lanes, sessions, players, the wire protocol
ui/            what the player sees: the menus, the server browser, the HUD, the F1 overlay
harness/       boot glue and the scripted test scenarios
-------------- the API boundary: headers in include/ ---------------------------------------
ue_wrap/       the engine wrapper: reflection, signatures, hooks, the game-thread pump, and one
               wrapper per engine or game class. No network, gameplay or co-op state
loader/        start_mod() and uninstall_mod(), the two functions UE4SS calls
third_party/   MinHook (the detour), GameNetworkingSockets (transport), Dear ImGui, Opus
               Unreal Engine 4.27 + Voices of the Void, unmodified
```

Two subtrees carry the design (principle 7). `ue_wrap/` describes the engine: one class per
engine or game class, wrapping reflection access, struct offsets and function calls. `coop/`
owns the mod's own state: packets, interpolation, ownership, sessions. A file that both reads
engine memory and owns network state is in the wrong place and is split. The boundary is what
lets either side change without breaking the other: the wrapper is re-measured when the game
updates; the co-op layer is not.

## Own substrate

UE4SS is the loader and the development tool, never the engine layer. The DLL imports zero
symbols from it; `tools/loader/abi_gate.py` checks the built artifact in CI. Mods that link
UE4SS's C++ API import dozens of mangled symbols and break across UE4SS versions; a zero-import
binary loads on every version that honours the `start_mod()` contract.

| Capability | What the mod uses |
|---|---|
| Reflection: `GUObjectArray`, `GNames`, `ProcessEvent` | resolved by the mod's own signature scan (`ue_wrap/core/sig_scan`, `ue_wrap/core/reflection`); the algorithms are ported from UE4SS with attribution |
| Function hooking | a MinHook detour on `ProcessEvent` (`ue_wrap/core/pe_detour`, `ue_wrap/core/hook`) with a relay that composes with UE4SS's own detour; per-function native hooks (`ue_wrap/core/ufunction_hook`); a bytecode-level seam (`ue_wrap/core/vm_dispatch`) |
| Game-thread context | a posted-task pump (`ue_wrap/core/game_thread`); engine calls happen only there |
| UI | the mod's own vendored Dear ImGui for the overlay; native UMG widgets built through reflection for the menus |
| The SDK | a header dump of the game's classes per game version; the offsets the mod relies on live in `ue_wrap/core/sdk_profile` |

A DLL in the game's process can reach everything. The reflected surface covers most of it: every
class, property and function, Blueprint or native, is enumerable, readable, callable and hookable
at the `ProcessEvent` level, and VOTV is mostly Blueprint. What is not reflected (inlined engine
internals, native helpers) is reached the way any native mod reaches it: signature scans,
detours, direct reads. UE4SS's Live View, Lua probes and header dumps are how that surface is
explored during development; none of it ships.

## Who owns what: the authority model

Three concepts, taken from MTA:SA. An **element** is a synced thing with a stable id. Its
**syncer** is the one peer allowed to author changes to it. The **arbiter** assigns syncers and
validates every inbound write; today the arbiter is the host's game process. The invariant that
everything else follows from:

> An inbound write is applied only if its sender is the element's current syncer. Authority is
> assigned, never asserted.

Which peer owns a piece of state is decided by the first matching row:

| The state is | Owner | Shape |
|---|---|---|
| shared-world progression: weather, power, events, world props that change on their own, NPC brains, randomness | the host | the host simulates or rolls; clients mirror. A client never rolls a shared outcome |
| a discrete, persistent, shared-world change a client initiates: buy, sell, destroy, place, take from a container | the host, by **intent** | the client suppresses its own producer and sends an intent naming *what*; the host validates, performs and prices it from its own tables; the result comes back as ordinary state. The reference lane is `coop/items/order_sync` |
| a continuously simulated element one peer is interacting with: a held prop, a driven vehicle, a pressed panel | the interacting peer, by assignment | the holder authors the stream, the host validates and may reassign; on release the authority returns to the host |
| a peer's own body, pose, camera, voice | that peer | a sender-authored stream; the receiver never gates display |
| presentation: UI, sounds, particles | nobody | mirrored on receive, never sent back |

Four rules ride on that table. The side that is suppressed is always the client-side producer,
never the receiver: a receive-side gate turns a cheat fix into a loss defect, because the client's
legitimate action then produces state the arbiter never learns of. An intent names what, never
what it costs. If the arbiter cannot hook the trigger, the intent points at an artifact the
arbiter can resolve (a shop purchase leaves an order row; the host prices the row). And the host
may cheat: it is the definition of truth for its own world, so every bound, clamp and
plausibility check is client-scoped, and a check applied to the host is a bug.

Where the intent rule stops: continuous, locally predicted state (a round trip through the
arbiter feels wrong, so the syncer is the acting peer and the arbiter validates); outcomes the
arbiter cannot reproduce (the intent carries the outcome and the arbiter validates it instead of
re-simulating); replays that would double an effect another lane already carries (every lane says
what the client re-applies and what it must not); and per-peer state that is not shared at all.

Two rules bind the receivers. **Park the brain**: a value mirrored into an actor whose own
Blueprint logic still ticks diverges or fights, so every mirror restorably parks what would
overwrite it, by a field latch, by clearing the timer or tick that drives self-simulation, or by
driving a physics body kinematic while it is remote-owned, and never by replacing or gutting the
engine's actor ([props.md](props.md)). **Replicate
authoritative state, re-derive the rest**: the pose stream is played onto a real `mainPlayer_C`
pawn, so animation, IK, footsteps and the held item come from the engine's own systems.

## Transport and sessions

Peer traffic runs on GameNetworkingSockets (Valve's UDP library, vendored), encrypted end to end.
Two topologies share one session code path, chosen in `coop/net/session_start`: a direct listen
on a port (a LAN, or a forwarded port), and NAT traversal brokered by the master server's
signaling, with TURN as the fallback. A session is a host plus up to three clients (`kMaxPeers`
in `coop/player/players_registry.h`); clients talk only to the host, and the host relays what
the other clients need. A dedicated net thread drives the sockets; the game thread sees pose
slots and a reliable inbox.

One connection carries two kinds of traffic, split by reliability rather than by transport:

- **Unreliable streams**, sent with no delay and freely dropped: the pose snapshot and the other
  per-tick streams (`MsgType`, fourteen kinds). The receiver interpolates; newest wins.
- **The reliable ordered channel**: every discrete event and state change, routed by
  `ReliableKind` (over a hundred kinds, from Join and Chat to the prop, container, workstation
  and vehicle lanes). Routing is `coop/dispatch/`: one drain and five family routers, each owning
  its kinds.

Reliable traffic rides three lanes (high, normal, bulk) so a join's bulk stream never delays a
teleport. A reliable send either enters the stream, enters a per-lane backlog, or the connection
dies; it is never warned about and dropped (`coop/net/send_backlog`). The wire format is
semantic: names, keys and positions, never engine addresses. Every message kind and the build
number live in `coop/net/protocol.h`; a change to any wire format bumps the build number in the
same commit, and two peers play together only when their game-version and build pair is
byte-equal. That gate runs three times: the browser warns before a connect, the join handshake
refuses, and the packet header is the backstop.

## Identity

**Peers.** Every install holds an Ed25519 keypair (`coop/net/peer_identity`); the public key *is*
the peer's network identity, and the id the master server and the per-player inventory store
use is derived from it. A name proves nothing by itself, so before a host spends
a seat the two ends sign each other's nonces with the key their identity names
(`coop/net/peer_admission`); the host proves itself first. The lobby password, when set, is a
proof carried inside that exchange and bound to the host's key, so a host you were steered to
cannot harvest it (`coop/net/lobby_password`). What the challenge does not close is a pure relay
on the path; [../SECURITY.md](../SECURITY.md) says what holds and what does not.

**Entities.** Every synced actor has an element id (`coop/element/registry`): 16 bits on the
wire, an O(1) resolver behind it. The id space is split in two ranges, the host allocating from
the lower half and each client from the upper half for the elements it creates locally, so no two
peers mint the same id. One actor is one row: a wire-received entity is bound through a single
create-or-adopt path (`coop/element/identity_create`) that adopts a known actor, re-binds a
morphed one, and refuses a live conflict rather than duplicating it. Identity is assigned at the
birth seam (a spawn catch, a drop intent, a birth channel), never minted by a later census.
Per-type mirror managers (`coop/element/mirror_manager`) own the mirrors' lifetimes, MTA's
`CClient*Manager` shape.

Keyed devices need a second name. The game persists a `Key` on the objects the save keeps and
mints a random one per process for the rest, so a door both peers loaded can carry two different
keys. The interactable lanes address those by a **portable identity** both peers derive
independently (`coop/element/portable_identity`): a child actor is its parent's identity plus its
component name, a level-baked actor is its object name, a top-level runtime actor is its key. The
game's own key is never written.

## The join

A joiner never generates its own world. The order is fixed, and a new lane picks its slot in it
explicitly:

1. **Admission.** The mutual challenge, the password proof, then the slot assignment, which is
   also the client's signal to begin. The version pair is checked here.
2. **Save transfer.** The host serializes its live world and streams it on the bulk lane
   (`coop/save/save_transfer`); the client writes it as a session-scoped slot the game's own menu
   never lists and loads it through the game's own loader, so every prop is placed by the engine,
   at rest, with the host's keys. Clients never save; the host is the only peer that writes one.
3. **Pre-world state.** What must exist before the world does: the joiner's own inventory, seeded
   from the host's per-player store.
4. **World load and quiescence.** While the client's world loads, the destroy seam is inside an
   episode that does not broadcast the load's own churn, and a probe watches the load tail; the
   client announces ready only when the population is stable
   (`coop/session/world_load_episode`). This is MTA's join barrier: no authoritative state lands
   in a world that is still churning.
5. **Connect replay.** On the ready announce the host runs the connect replay for that slot
   (`coop/session/subsystems`): the snapshot bracket of every synced element, a true-up for
   whatever moved since the save was written; then each state lane's current state; then the
   per-lane seeds for the lines broadcast during the load window (`coop/session/join_seed`).
6. **Ready.** The joiner is placed, the loading screen lifts, and the per-tick streams take over.

Mid-activity join is always handled (principle 8): a peer joining mid-event, mid-download,
mid-drive or mid-anything converges to the host's current state, and a lane is not done until its
late-join answer (snapshot, seed, park, replay or unlatch) is written and built. History is never
replayed to a joiner; current state is, which is what MTA sends at join-complete time.

## Hooks and seams

Everything the mod observes or intercepts arrives through one of four seams, and which seam sees
a call is a property of how the Blueprint VM dispatches it, not of the function:

| Seam | Sees | Can cancel |
|---|---|---|
| the `ProcessEvent` interceptor | engine-originated calls: ticks, BeginPlay, input events, delegates, timers, interface events | yes |
| the native function seam (`ue_wrap/core/ufunction_hook`) | calls into native functions on every route, including from inside a Blueprint's own graph | no |
| the bytecode seam (`ue_wrap/core/vm_dispatch`) | the Blueprint-internal virtual calls neither of the above sees; the call site only, no arguments | no |
| per-site reconcile | anything else: let the verb run, diff the observable state, converge to the authority's answer | after the fact |

[COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md) is the per-function table and the
decision guide; [COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md) says how each
entity family gets its identity, its expression and its destruction, and where two seams overlap;
[COOP_SYNC_DOCTRINE.md](COOP_SYNC_DOCTRINE.md) is the method for adding a sync lane end to end.

Hot-path rules bind every hook: no object-array scan per frame, no heavy work per `ProcessEvent`
or per tick, engine calls on the game thread only. Discovery of the game's objects goes through
one shared, sliced scan (`coop/element/object_scan_hub`) that every index rides.

## Where the authority is going

The arbiter is the host's game process today, so it can read the engine whenever it wants, and
the lanes that derive their canonical state by reading the engine are the ones that cannot leave
that process. The destination ([ROADMAP.md](ROADMAP.md), phase 2) is an arbiter in its own
process: spawned as a child when hosting from in-game, launched by hand for a dedicated box,
physically the same binary, never reading the engine. The rule it is built around: **the arbiter
holds values and anchors; the engine holds only what has a world-dependent rate.** An accumulator
is never streamed, it is anchored: store the start stamp once and let every peer compute the
value, which is how MTA's clock works. That makes divergence impossible and late join free, and it
is valid only while the accumulator's rate is constant. The blob-then-overlay join already exists;
the arbiter becomes the party holding both halves, and how often the save must be re-donated by a
live game is the inverse of how much of the world the arbiter's own record covers.

## The principles, and why

1. **No modification of original game files.** Hooks and runtime patches, yes; editing the
   executable, the paks or the cooked assets, no. Every edit compounds: an edited asset forks the
   asset pipeline, an edited binary forks the game, and the mod becomes a redistribution. Editing
   a Blueprint to add Unreal replication counts as editing an asset.
2. **An engine-extension layer, not a set of hooks.** Modules own their lifecycles and talk
   through APIs. Fifty hooks in one file calling each other through globals is a patch
   collection, and it cannot be reasoned about when the game updates.
3. **A parallel class hierarchy.** `RemotePlayer` owns network state, the interpolation buffer
   and the pointer to its pawn; the engine's `APawn` and `APlayerController` own rendering,
   animation and physics. The engine does the heavy part; the mod does only what the engine does
   not know about. MTA's `CClientPed` to `CPlayerPed` shape.
4. **Targeted crash fixes, never broad suppression.** A single-player assumption that crashes on
   the second pawn is fixed at its call site. A broad filter masks many crashes behind one
   mechanism and hides every root cause; one shipped here once and was retired the same week.
5. **Minimum viable subset.** [SCOPE.md](SCOPE.md) is the law of what is synced, so every new
   feature has a binary answer instead of a debate, and it is amended in the commit that changes
   a decision.
6. **Augment single-player, never replace it.** Where co-op meets a per-player thing in the game
   (the input mapper, the camera, the save slot), the first player's path stays and a second path
   is routed alongside. The game ships as single-player and must keep working.
7. **Two layers, two subtrees.** The engine wrapper describes the engine and holds no co-op
   state; the co-op layer holds the mod's state and reaches the engine only through the wrapper.
8. **Mid-activity join is always handled.** Every sync lane defines its late-join answer; "don't
   join during X" is not an answer.

The precedent behind all of them is MTA:SA, vendored read-only in `reference/mtasa-blue/`: the
parallel hierarchy, the element registry and the per-type managers, assigned sync, the join
barrier, the anchored clock, the child-process server. When a design question has an MTA answer,
that answer is the default, and a deliberate divergence says so in a comment at the site.

## Where to go next

- [CODE_MAP.md](CODE_MAP.md): where every concept lives, and the checklist for adding a sync lane.
- [STATUS.md](STATUS.md): what is synced, system by system, with its owner, its late-join answer
  and how far it is.
- [SCOPE.md](SCOPE.md): what is deliberately not synced, and the rules a new item is classified
  against.
- [COOP_SYNC_DOCTRINE.md](COOP_SYNC_DOCTRINE.md), [COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md),
  [COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md): read before writing any
  entity-sync, hook or spawn-catch code.
- [../CONTRIBUTING.md](../CONTRIBUTING.md): the rules a change must respect.
