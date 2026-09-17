# Trash piles

## Purpose

The ambient trash piles, the clump a pile becomes in a hand, and the trash-bits dispenser
piles: the whole collect loop of grab, carry, throw and re-pile across peers, and the identity
problem that makes this the hardest prop family. What is built, and how a mirror of the game's
own actor is kept from authoring its own transitions. Ordinary props are on [props.md](props.md).

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
moves across pile, clump and pile again, rebound onto each successor at its birth: the id is the
logical entity, position is never identity, so a dense cluster cannot mis-bind. Every transition (grab, throw, land) bumps a
per-id sync-time context the host stamps on every convert and carry packet, and a receiver drops
a packet older than the id's known generation, so a carry packet still in flight when the entity
re-piles is never applied to the re-skinned entity. This is MTA's element sync-time context.

The pile-to-clump and clump-to-pile links are caught at the one seam that fires on every
dispatch route, the native function seam on the engine's deferred-spawn call. The grab direction
records a clump birth certificate that the held-object edge consumes; the land direction converts
the re-piled clump in place onto the exact spawned pile, the same tick, with no proximity search.

### The mirror on a client

The pile form on a client is the game's own `actorChipPile_C`. At a join it is the client's own
save-loaded pile, bound to the host's id where the host says the pile was at save time
(`coop/props/pile_spawn_bind`); a pile with no counterpart -- one derived during the join window
or born in play -- is a rooted runtime pile instead (`coop/props/trash_mirror`), spawned
with its tick and physics off and its root movable, skinned with the host's chip type, scale and
rotation. Either way it is bound and marked save-native, so it rides the same machinery: the pose
drive, the position correction, the grab route, the morph hand-off, the sweep exemption and
retire. A real pile is what the game's look-at trace accepts, so the hover prompt, collision,
occlusion and rotation are the game's. A rooted native stays live and inert; the earlier belief
that a runtime pile "dies on its own" was garbage collection of an unrooted actor.

A pile mirrors nothing of its own brain. A pile turns itself into a clump when any prop overlaps
its collision component and when something calls its grab event, and a clump re-piles itself on
its first level contact; each spawns the successor and destroys the actor it ran on. On a client
every trash transition is the host's, so those three are refused at the dispatch
(`coop/props/trash_morph_gate`) and the host's own convert performs the change.

The clump form, in a hand or in flight, is the game's own `prop_garbageClump_C`, made by the
same module on the same recipe (`coop/props/trash_mirror`), with one addition: its collision is
off while it is carried, because it renders in a puppet's hand with no holder to be attached to
and would otherwise block the player carrying it. It stays off for the flight too, which is a
host-driven pose stream rather than local physics; the pile it lands as is a fresh actor with the
game's own collision.

A form change is a class change, so there is no re-skin in place: the successor is made parked,
the one identity is rebound onto it at its birth, and only then is the predecessor destroyed. One
mirror implementation serves both forms.

### Grab, carry, throw, land

The host grabs natively. Its held-edge detector streams the clump's pose like any held prop, and
the release edge keeps streaming the clump's flight until it re-piles, because the clump's
release runs through neither of the verbs a hook could see.

A client's grab is an intent. The E press is intercepted before the native grab
(`coop/props/trash_use_intercept`) and the client sends the id of the pile it is looking at;
the host checks that the sender may name that pile (`coop/element/intent_authority`) and
performs the grab on the requester's puppet natively, which engages and holds on an unpossessed
pawn. The puppet's own tick is dead, so the host drives the held clump to the
puppet's hand each tick (`coop/player/puppet_carry_drive`) and streams the clump's pose as a
host-originated per-id batch to every client, the requester included
(`coop/props/trash_clump_pose_stream`), rendered with the same fixed-delay interpolation as a
held prop. A second E press is the release toggle and the left button a hard throw with a
direction; both are throw intents the host performs. Landing is an atomic convert from the host:
the pile materialises at the landing pose under the same id and the clump retires after it.
Pickup and landing sounds are synthesised on peers: the game plays them only for the actor.

### The carry latch and the land settle

The game churns a held clump: about once a second it re-piles on contact with a cluster and
immediately re-grabs the result. Broadcasting every one of those would re-skin and teleport the
client's rendering each cycle, so the host holds a per-id carry latch from the real grab to the
real land, and suppresses the churn inside it -- no convert, no context bump. A churn re-grab
rebinds the id onto the new clump so the pose stream keeps tracking it.

Telling churn from a real landing needs one wait: a re-pile opens a settle window instead of
broadcasting. A re-grab inside the window cancels it as churn; the window expiring commits the
to-pile convert and closes the latch. The window is self-correcting either way -- too short
commits a churn re-pile that the re-grab then re-opens, a brief flicker; too long lags the land
by a few frames. Neither strands the id.

Every open carry must eventually close, so the host's tick also terminates lanes the normal path
would leave open: a clump destroyed mid-carry (consumed) closes the lane and broadcasts a destroy,
so no client is stuck holding a dead mirror; a clump whose holder has stopped being able to hold (it left, it
fell, its puppet is gone) is LET GO, never destroyed -- it falls as a still release does, its flight
streams, and a holder still connected is told its carry is over; a clump left lying un-held closes
the lane silently and leaves the clump world-tracked and re-grabbable, which is what single-player
does. A clump re-piles only on a hit its own gate passes: its re-pile has armed, a random 0.5 to 1 s
after its birth (1 to 2 s for a clump a pile kicks into being); its last holder's hand is empty; the
surface is within about 41 degrees of level (about 104 degrees, short of a ceiling, for one chip
type); and what it hits is not
a simulating body. So a throw whose thrower's hand is busy at the land leaves one, and so does a
roll that ends before the clump arms, or with no such hit after. When such a clump re-piles later,
its convert waits for the same settle, so every peer sets the pile down where it was placed and not
where the clump was, a radius above; a settle whose pile is gone before it commits is dropped, since
whatever took the pile reports itself. `[V]` a clump whose carry had closed at rest, knocked up by
the broom drill, landed as a pile 0.0 cm from the host's on the client.

### Trash-bits piles

The dispenser piles ("uses 6 of 7") are keyed save actors; a press, the vacuum or the broom
dispenses items and decrements a counter pair inside the Blueprint. Each peer polls its
indexed piles and broadcasts the pair on a decrease; receivers apply a per-component minimum,
so concurrent collects converge, and the host's connect snapshot is applied as sent
(`coop/props/trash_pile_sync`). Depletion destroys the pile inside the Blueprint, caught by a
death-watch in the poll and broadcast as the ordinary keyed destroy: outside a transition window,
an indexed pile that vanishes is a depletion if this peer has just run a destroying verb on it, or
if it vanished near the local camera, and any other disappearance is a sublevel stream-out. The
first rule exists because the host runs a client's broom stroke on its own pile, which can die
anywhere in the world; widening the camera test to every player's body instead would read a
stream-out beside a remote player as a depletion. The dispensed item is born keyless and grabbed the same frame; the held-edge broadcast mints it a
key and spawns the mirror on every peer (`coop/props/trash_collect_sync`).

The vacuum and the broom put their items on the floor: each pop spawns a real actor at a rolled
transform, one per vacuum call and up to three per broom stroke, and the pile is destroyed once
both counters reach zero. A client's broom authors none of that, since the host runs every stroke
(below), and the trash, the counter drop and the depletion leave the host on the three channels
above. The host names a pile to the depletion watch as its `broomed` runs, which is how a pile a
client's stroke empties far from the host's camera still reads as a depletion.

### The broom

One stroke acts on whatever a 50 uu sphere at its trace's hit point overlaps, in a fixed order: it
runs `broomed` on each dispenser pile, turns each chip pile into a clump with a copy of the pile's
own morph, and pushes each physics body along the holder's heading with the holder's velocity added.
A held right mouse button strokes once a second. Every stroke is the host's
(`coop/items/broom_stroke`). A client refuses its own at the swing montage's notify and sends the
three things the stroke reads of its holder: the segment its `arm` returned, camera to reach end,
and the holder's heading and velocity. The host checks the heading for a unit vector and the
velocity against terminal velocity as the stroke arrives and queues it: a client's strokes run one a
tick, no faster than three a second after three at once, so a stall's backlog of up to ten waits its
turn instead of being refused or run in one frame. At its turn the host checks both ends against the
client's body and runs the game's stroke on its mirror of that client's broom with the client's
puppet as the holder and each read answered with the client's value: the segment written into
`arm`'s results at the script-body gate, the heading and the velocity into the results of the two
native reads, at seams armed for that call alone. The puppet would have answered with its display
heading, which holds while the camera turns, and a velocity rebuilt from its speed. This is MTA's
context switch, which runs the game's code for a remote ped with that ped's inputs swapped in. `[V]`
for strokes swung through the broom's own right mouse button on a broom each peer holds, the montage
and its notify included (`harness/autotest/autotest_broomstroke.cpp`): a held button struck three
times in three seconds on each peer and the host ran all three of the client's, and a client
teleported facing 40 degrees off its pile and turned onto it -- its body follows its view, while the
host's puppet keeps the teleport's facing -- swept a clump that left along the client's heading, 0.2
degrees off in one run and 1.6 in the next, and not the puppet's, 40 degrees away.

The chip piles a stroke turns into clumps keep their ids. The spawn is issued by the broom's
bytecode rather than the pile's, so the host's deferred-spawn seam (`coop/props/trash_collect_sync`)
reads the pile the stroke loop is on out of the broom's own frame (`ue_wrap/actors/broom`) and moves
that pile's id onto the clump at its birth. A tick later, once the finish has placed the clump and
the push has launched it, the clump opens a carry nobody holds (`coop/props/trash_sweep`): one
to-clump convert, then its pose on the host-originated clump stream, and the clump's own re-pile
lands it through the settle a throw uses. One host stroke on a heap of ten piles showed the client
all ten clumps 31 to 47 ms after the host over two runs; every one rolled there along the host's
path to its end, no row more than 9.6 cm off it, and ended in the same form 0.0 cm apart. A client's
stroke: within 78 ms, no row more than 8.6 cm off, and 0.0 cm `[V]`. A clump that comes to rest
without re-piling closes its carry, and a later push opens it again with no convert: two clumps the
drill kept from re-piling closed their carries at rest and lay 0.0 to 0.1 cm apart on the client, and
one of them, pushed again, rolled 198 to 212 cm on the host and 185 to 190 cm on the client, no row
more than 3.0 cm off the host's path, and ended 0.0 to 0.4 cm apart `[V]`.

The bodies the push moves stream too (`coop/items/broom_push`): a prop coasts on the driven-prop
channel until it rests, and a prop a verb already holds stays that verb's ([props.md](props.md)).
The trash the stroke knocks out of a dispenser pile streams its fall the same way, caught at the
finish of each spawn the pile's own `broomed` body makes; the streams open after the tick's spawns
are named, so each has its id by then. Every prop a host's and a client's stroke pushed or knocked
out ended its stream on both peers at one pose and came to rest within 0.1 cm of the host's copy,
over two runs; a prop that something knocked on the host after its stream had ended lay up to 15.9
cm off in an earlier run, a knock being no verb ([props.md](props.md)) `[V]`.

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
| the pile-to-clump identity | the host | one id, rebound onto each successor at its birth, guarded by the sync context |
| a dispenser's counters | each peer, minimum wins | the host as sent at join |
| a broom stroke | the host | a client's stroke is what it read of its holder; the host runs the stroke with those reads, reach- and rate-checked |
| bagging a pile | the host, by intent | a client's press is refused at the script-body gate and sent as the target's id; the host spawns the bag and destroys the target, so the prop seams carry both |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `GrabIntent`, `ThrowIntent` | a client to the host | the pile id; the release or a hard throw with its direction |
| `PackTrashIntent` | a client to the host | the id of the pile or clump a client bagged, and whether the tool was a folded bag or a roll |
| `BroomStroke` | a client to the host | the segment, heading and velocity a broom stroke read of its holder |
| `PropConvert` | the host to all | the atomic pile-clump convert: id, form, pose, scale, chip type, context |
| `TrashCarryPose` (stream) | the host to all | per-id poses for client-grabbed clumps and the clumps a broom sets rolling |
| `PropPose` (stream) | the host to all | the host's own held clump and its flight |
| `PropSnapPos` | the host to one joiner | a position correction for a pile moved in the window |
| `TrashPileState` | each peer, relayed | a dispenser's counter pair |
| `PropDestroy` | either role | a depleted dispenser |

## Late join

The snapshot carries one spawn per pile with its id and save-time position, the client binds its
own actors by that position, the membership sweep removes what the host never claimed, and a
clump held by someone at the moment of the join binds without a duplicate. A pile the host moved
during the window arrives as a position correction after the snapshot. A clump a broom has swept
and that is still rolling is in no snapshot, having no key. A land before the joiner's world is up
is not sent to it, and the pile reaches it in the world-ready snapshot; a land after is sent to it
as to everyone, carrying the pile's pre-sweep position so the joiner's save-loaded copy at the old
spot is retired `[RD]`.

The client's own piles do not all exist when the snapshot arrives -- its save load is still
draining -- so an expression that finds no actor at its save-time position is HELD rather than
answered: the id is recorded and the quiescence sweep binds the actor when it appears. The hold is
bounded. An id whose actor never appears is given up after the sweep's retry budget and that pile
is absent on that client until a later join expresses it again; nothing is invented at the stale
save position, because the host may have moved or removed the pile since.

## Known limits

| Limit | Evidence |
|---|---|
| A clump has its collision off for its whole life -- carried and in flight -- so a player walks through the ball someone else is holding or has thrown. The pile it lands as has the game's own collision | `[V]` `coop/props/trash_mirror` |
| Trash dropped into a garbage container updates the container on the host only: every client's container has its brain cancelled, so none of them -- not even the one whose player dropped the trash -- ever learns what is inside it, and the two pickup flags the game writes from those contents stay frozen | `[V]` `coop/interactables/garbage_sync`, and the cancelled Blueprint body read from the cook |
| Dispenser piles born by an event carry per-process keys and never resolve across peers | `[V]` `coop/props/trash_pile_sync` |
| A client's vacuum on a dispenser pile spawns its item on that client only: the host runs the broom's stroke, not the vacuum's suction | `[V]` the vacuum verb's bytecode; `coop/items/broom_stroke` refuses the broom's stroke alone |
| A client's bag is spent whatever the host answers: the tool and the roll's count are per-peer state with no lane, so the client spends its own on the press and a host refusal -- out of reach, a target of another type -- costs it that bag | `[V]` code: `coop/props/pack_trash_intent` sends and spends in the same press, and the host's deny paths answer no one |
| A bagged pile makes its bag on the host, so a client sees it a round trip late, and the game's own pack sound and hint play on the packing client alone | `[V]` code: `coop/props/pack_trash_intent` refuses the client's body and the bag rides `PropSpawn` |
| A client's broom stroke acts a round trip late on that client: its swing animates at once, and the clumps, trash and pushes arrive with the host's messages | `[V]` code: `coop/items/broom_stroke` refuses the stroke at its notify, inside the montage the press started |
| A clump the game makes of no pile -- an angry erie flesh, a kerfus possessor and an erie plush each spawn one from their own graphs -- has no id at its birth: its first roll is the host's alone. The adoption scan enrols it where it lies, as it does any clump at rest, and every peer then gets it as a clump under that id | `[V]` code: the three graphs' spawns; `coop/props/trash_collect_sync` names a clump at birth only by the pile it is born of; `coop/props/prop_census` enrols a keyless clump |
| A client cannot pick up a clump that is at rest -- one the save loaded, or one thrown and left on a box. Its use press names piles only, and the game's own pickup refuses a body that is not simulating, which a bound clump is not: the press does nothing. The host's hand can | `[V]` code: `coop/props/trash_use_intercept` recognises a chip pile under the crosshair and nothing else; `[RD]` the pickup's simulating test, `mainPlayer` use handler |
| The join-window bind is by save-time position; the sidecar that replaces it is off by default | `[V]` see [join.md](join.md) |

## Code map

| Concept | Files |
|---|---|
| identity and the transitions | `coop/props/trash_channel`, `coop/props/trash_grab_intent.cpp`, `coop/props/trash_sweep` (a broom stroke's clumps) |
| the mirrors, in both forms | `coop/props/trash_mirror`, and `coop/props/trash_morph_gate` for the verbs a client refuses |
| the client's grab and throw | `coop/props/trash_use_intercept`, `coop/player/puppet_carry_drive`, `coop/props/trash_clump_pose_stream`, `coop/props/active_drive` |
| the dispenser piles | `coop/props/trash_pile_sync`, `coop/props/trash_collect_sync` |
| the broom | `coop/items/broom_stroke` (every stroke the host's), `coop/items/broom_push` (what it pushes), `ue_wrap/actors/broom` |
| bagging a pile | `coop/props/pack_trash_intent`, and the bag family's tests in `ue_wrap/actors/prop` |
| garbage containers | `coop/interactables/garbage_sync` |
| the join | `coop/props/pile_spawn_bind`, `coop/element/quiescence_drain`, `coop/props/save_time_retire_util.h`, `coop/props/save_identity_map`, `coop/props/save_identity_bind` |
| tests | `harness/autotest/autotest_chippile.cpp`, `harness/autotest/autotest_clump.cpp`, `harness/autotest/autotest_broomstroke.cpp` |
