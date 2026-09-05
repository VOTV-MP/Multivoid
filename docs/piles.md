# Trash piles

## Purpose

The ambient trash piles, the clump a pile becomes in a hand, and the trash-bits dispenser
piles: the whole collect loop of grab, carry, throw and re-pile across peers, and the identity
problem that makes this the hardest prop family. What is built, what stands in for the engine's
own actor, and what is queued for a rebuild. Ordinary props are on [props.md](props.md).

## How it works

### Why piles are hard

A pile (`actorChipPile_C`) has no identity of any kind: no key, a key setter that writes nothing
anyone reads, and a save record of class, transform and chip type that the game loads by
destroying every pile and respawning them from their positions. Grabbing a pile runs its
Blueprint's to-clump, which spawns a separate clump actor (the carried ball) and destroys the
pile; landing re-piles, which spawns a fresh pile and destroys the clump. One logical thing
changes actor twice per carry, and nothing in the game names it across peers. The only handle
it can have is an element id the host mints and streams. A base save holds several hundred.

### Identity: the host's id and the sync context

The trash channel (`coop/props/trash_channel`) treats a trash entity as a host-minted id that
re-skins in place across pile, clump and pile again: the id is the logical entity, position is
never identity, so a dense cluster cannot mis-bind. Every transition (grab, throw, land) bumps a
per-id sync-time context the host stamps on every convert and carry packet, and a receiver drops
a packet older than the id's known generation, so a carry packet still in flight when the entity
re-piles is never applied to the re-skinned entity. This is MTA's element sync-time context.

The pile-to-clump and clump-to-pile links are caught at the one seam that fires on every
dispatch route, the native function seam on the engine's deferred-spawn call. The grab direction
records a clump birth certificate that the held-object edge consumes; the land direction converts
the re-piled clump in place onto the exact spawned pile, the same tick, with no proximity search.

### The mirror on a client

The pile form on a client is a real, rooted `actorChipPile_C` (`coop/props/native_pile_mirror`):
spawned at runtime with its tick and physics off, its root movable, skinned with the host's chip
type, scale and rotation, then bound and marked save-native so it rides the same machinery as a
save-loaded pile: the pose drive, the position correction, the grab route, the morph hand-off,
the sweep exemption and retire. A real pile is what the game's look-at trace accepts, so the
hover prompt, collision, occlusion and rotation are the game's. A rooted native stays live and
inert; the earlier belief that a runtime pile "dies on its own" was garbage collection of an
unrooted actor, measured by a probe.

The clump form, in a hand or in flight, is a bare static-mesh actor the mod owns
(`coop/props/trash_proxy`): no Blueprint, so it cannot re-pile on contact or expire on its own
lifespan; rooted; re-skinned in place; kinematic; no collision. It is a stand-in for the
engine's actor rather than the actor with its brain parked, and it is the crutch queued for a
rebuild (the limits below).

### Grab, carry, throw, land

The host grabs natively. Its held-edge detector streams the clump's pose like any held prop, and
the release edge keeps streaming the clump's flight until it re-piles, because the clump's
release runs through neither of the verbs a hook could see.

A client's grab is an intent. The E press is intercepted before the native grab
(`coop/props/trash_use_intercept`) and the client sends the id of the pile it is looking at;
the host checks that the sender may name that pile (`coop/element/intent_authority`) and
performs the grab on the requester's puppet natively, which a probe proved engages and holds on
an unpossessed pawn. The puppet's own tick is dead, so the host drives the held clump to the
puppet's hand each tick (`coop/player/puppet_carry_drive`) and streams the clump's pose as a
host-originated per-id batch to every client, the requester included
(`coop/props/trash_clump_pose_stream`), rendered with the same fixed-delay interpolation as a
held prop. A second E press is the release toggle and the left button a hard throw with a
direction; both are throw intents the host performs. Landing is an atomic convert from the host:
the client's proxy retires and the pile native materialises at the landing pose with the same
id. Pickup and landing sounds are synthesised on peers: the game plays them only for the actor.

### Trash-bits piles

The dispenser piles ("uses 6 of 7") are keyed save actors; a press, the vacuum or the broom
dispenses one item and decrements a counter pair inside the Blueprint. Each peer polls its
indexed piles and broadcasts the pair on a decrease; receivers apply a per-component minimum,
so concurrent collects converge, and the host's connect snapshot is applied as sent
(`coop/props/trash_pile_sync`). Depletion destroys the pile inside the Blueprint, caught by a
proximity-gated death-watch in the poll (an indexed pile that vanishes near the local camera
outside a transition window) and broadcast as the ordinary keyed destroy. The dispensed item is
born keyless and grabbed the same frame; the held-edge broadcast mints it a key and spawns the
mirror on every peer (`coop/props/trash_collect_sync`).

### Save-loaded piles at a join

A joiner's world comes from the host's save, so the client has its own copy of every pile. The
host expresses each with its id and the pile's save-time position; the client binds its own
save-loaded actor to the host's id by that position, at spawn time (`coop/props/pile_spawn_bind`)
or at quiescence (`coop/element/quiescence_drain`), and a pile the host moved during the window
is corrected by the host's position message, with the identity following the host's word rather
than the frozen save position. The index-to-id sidecar on [join.md](join.md) is the intended
replacement for the position key.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a pile at rest | the host | an id and a rooted native mirror; the client's save-loaded actor is bound to it |
| a carried clump | the host | a client's grab is performed on the host; the pose is host-originated |
| grab, throw, land | the host, by intent | a client's press is an intent, reach-checked |
| the pile-to-clump identity | the host | one id, re-skinned in place, guarded by the sync context |
| a dispenser's counters | each peer, minimum wins | the host as sent at join |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `GrabIntent`, `ThrowIntent` | a client to the host | the pile id; the release or a hard throw with its direction |
| `PropConvert` | the host to all | the atomic pile-clump convert: id, form, pose, scale, chip type, context |
| `TrashCarryPose` (stream) | the host to all | per-id pose batches for client-grabbed clumps |
| `PropPose` (stream) | the host to all | the host's own held clump and its flight |
| `PropSnapPos` | the host to one joiner | a position correction for a pile moved in the window |
| `TrashPileState` | each peer, relayed | a dispenser's counter pair |
| `PropDestroy` | either role | a depleted dispenser |

## Late join

The snapshot carries one spawn per pile with its id and save-time position, the client binds its
own actors by that position, the membership sweep removes what the host never claimed, and a
clump held by someone at the moment of the join binds without a duplicate. A pile the host moved
during the window arrives as a position correction after the snapshot.

## Known limits

| Limit | Evidence |
|---|---|
| The clump mirror is a static-mesh stand-in, not the engine's actor with its brain parked; two mirror implementations of one concept compile together, and the aim cone that exists only because a proxy cannot be looked at survives with it | `[V]` the modules' own headers; the rebuild is next after the ATV on [ROADMAP.md](ROADMAP.md) |
| The proxy has no collision: a player walks through a carried or flying clump, and the aim cone ignores walls | `[V]` the proxy's header |
| The client grab lane spends engine dispatches per press and a large pass at join on the parallel aim | `[V]` measured in the performance census: about seventeen hundred per press, hundreds of thousands at join |
| Trash dropped into a garbage container updates the container only for the peer who dropped it; the container's contents are not synced, and the client skips the Blueprint that would walk a stale list | `[V]` the garbage lane's header |
| Dispenser piles born by an event carry per-process keys and never resolve across peers | `[V]` the key-hash log |
| The join-window bind is by save-time position; the sidecar that replaces it is behind a developer flag | `[V]` see [join.md](join.md) |

## Code map

| Concept | Files |
|---|---|
| identity and the transitions | `coop/props/trash_channel`, `coop/props/trash_grab_intent.cpp` |
| the mirrors | `coop/props/native_pile_mirror`, `coop/props/trash_proxy` |
| the client's grab and throw | `coop/props/trash_use_intercept`, `coop/player/puppet_carry_drive`, `coop/props/trash_clump_pose_stream`, `coop/props/active_drive` |
| the dispenser piles | `coop/props/trash_pile_sync`, `coop/props/trash_collect_sync` |
| garbage containers | `coop/interactables/garbage_sync` |
| the join | `coop/props/pile_spawn_bind`, `coop/element/quiescence_drain`, `coop/props/save_time_retire_util.h`, `coop/props/save_identity_map`, `coop/props/save_identity_bind` |
| tests | `harness/autotest/autotest_chippile.cpp`, `harness/autotest/autotest_clump.cpp`, `tools/pile-test-assert.ps1` |
