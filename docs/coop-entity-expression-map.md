# How each entity gets its identity, its expression and its destroy

The map of every synced entity family: where its birth is caught, what names it across peers, who
owns that name, and how its destruction propagates. This is the question "which seam expresses
this thing?" answered once. Companion: [coop-dispatch-visibility.md](coop-dispatch-visibility.md).
Evidence tags: `[V]` verified from code or a run, `[RD]` from a comment or a reverse-engineering
record, `[?]` needs a probe.

## Identity ranges

The element registry (`coop/element/registry`) splits one id space in two: the host allocates
from the lower half and each client from the upper half for the elements it creates locally, and
two trust gates check which range a sender may have allocated from. Every wire receiver is
host-authoritative except the kinds either range may send, a destroy and a convert. `[V]`

**No passive mint.** A client's census walk key-indexes a keyed prop but mints no element: keyed
identity is host-authored by construction, a save-loaded prop adopts the host's id by key, and a
client-born prop is minted at its express seam, which broadcasts in the same breath. A passive
census mint once produced about two thousand zombie double rows per join. The one bind owner
(`coop/element/identity_create`) carries the guards: the host rejects a wire row over a live
host-local actor, a client dissolves its provisional row on the host's word, and one actor is one
row. On the host, a peer's spawn resolving onto a host-authoritative actor is handed back: the
actor is enrolled if untracked and re-expressed under the host's id. `[V]`

**Tracking is gated on hosting, never on a peer being connected.** Every catch and enrol below
runs whenever a session exists with the host role: an event actor spawned while the host is alone
must still enrol, or the connect snapshot has nothing to send when someone joins. Only wire sends
are peer-gated. `[V]`

## The families at a glance

| Family | Birth dispatch | Caught by | Identity |
|---|---|---|---|
| keyed props | the save; a spawner; the spawn menu; a container extract | the object scan at world start; the initialisation post observer; the finish-spawning seam | the save key, plus a host id |
| chip piles | the save; a re-pile | the object scan's keyless lane; the native seam on the re-pile spawn | a host id only, keyless |
| the trash clump | the grab, inside the Blueprint | the use input's pre observer; the native seam | the same host id, re-skinned |
| a held physics prop | the grab | the new-held edge of the local stream | the key it already had; a keyless one is minted a key at the edge |
| the hotbar hand item | the hotbar switch | not a world entity: polled as player state | none; display only |
| a pocket pickup and a place | the destroy seam; the finish-spawning seam | the drop-intent door | the parked key |
| characters | a deferred spawn | the spawn interceptor; the object scan; the native seam for graph spawns | a host id |
| the wisp swarm | `EX_CallMath` spawns from the swarm trigger's graph | the native seam, gated by the calling class | a host id |
| event actors | a deferred spawn, or a graph spawn | a second interceptor with a disjoint allowlist; the native seam | a host id |
| the kerfur | conversion verbs inside the Blueprint | a death-watch poll; the bytecode seam's capture | one host kerfur id across both forms |
| a deployed hook or rope | the fire input, inside the Blueprint | the thrower polls its own `activeHook` field | `(owner slot, sequence)` while the thrower holds it, a host-minted save key once anchored |

## Keyed props

- **Birth.** A save-loaded prop is caught by the object scan at world start, never by an
  observer. A spawner-born prop fires the initialisation post observer on the host. A spawn-menu
  or toolgun birth runs its initialisation inside the Blueprint, so it is caught at the
  finish-spawning post. A container extract is caught by the host spawn watcher on the
  finish-spawning native seam; the take verb itself dispatches locally and its observer has
  never fired. `[V]`
- **Identity.** The Blueprint save key is the cross-peer name, minted by the game and read by
  the mod; a receiver writes the host's key into a mirror before its spawn finishes. A host id
  rides alongside. `[V]` Keyed devices the save does not persist are named by the portable
  identity on [architecture.md](architecture.md).
- **Adoption by position, and what it may not take.** A birth whose key resolves to nothing falls
  back to a same-class, same-row scan within 30 cm and re-keys what it finds, which is what keeps
  the per-peer natural spawners from doubling ([props.md](props.md)). `[V]` **A player's hand item
  is not a candidate**: the hotbar actor and every peer's display mirror are real props of the
  right class standing at a peer's hands, owned by the hand lane and destroyed when the hand
  changes, so an adoption binds a wire identity to an actor that is about to vanish on somebody
  else's schedule and the prop the birth named never appears there. The hand axis has one owner
  and both walks that must respect it -- the census outward, this scan inward -- ask it. `[V]`
- **Key uniqueness.** The game's own saves ship duplicate keys, a clone family of dozens of
  trash-bit piles on one key among them, and every identity layer assumes uniqueness. The host
  is the key authority: at enrolment, a keyed actor whose key another live actor already carries
  is re-keyed with a fresh random key, once, and the game re-saves the live keys so the fix bakes
  into the host's save and every transferred world. A dead incumbent's recreate inherits its
  identity. Clients never re-key. `[V]`
- **A client never authors a save-loaded prop.** Two intent doors route around it: the drop
  intent for a parked key placed after a pickup, and the whitelisted births (a reel, a module, a
  drive, a container extract) the client cannot avoid; the host performs both. Any other
  client-born keyed prop is dropped at the door. `[V]`
- **The prop's own save record, beside its birth.** A class that keeps save state of its own --
  a reel's progress, a disc's files -- has that state serialized by the game's own `getData` and
  carried on its own message, addressed by Key and sent behind the spawn row on the same lane, so
  a mirror never starts from a class default. Membership is the class declaring a `getData` below
  `Aprop_C`, read off the live class chain rather than from a list. A record whose prop has not
  arrived parks by Key with no expiry: an element id names an actor, and the whole reason the
  record has to travel is that the actor is destroyed and remade. `[V]`
- **Destroy.** The engine's destroy call is caught before it runs on either role, on every
  route, at the native seam; a Blueprint-internal vanish (the truck, culling, a lifespan) is
  caught by the host's reaper death-watch and destroyed by id. `[V]` A floppy disc inserted into
  a laptop or a signal server dies into that device's slot through that seam, and an ejected one is
  born through the birth channels with its content on the save-record lane above. The slot itself
  is state on the wire, so the destroy is not the only thing that crosses: the host owns
  every device's slot, a peer reports the outcome of one its own game changed, and the host's
  re-publish is the answer ([devices.md](devices.md)). `[V]` **An ejected disc is born where the box that
  ejected it turned its own collision off, and the box on the other machine never ran that eject**,
  so it would take the disc through the same overlap the insert uses. A disc is therefore marked IN
  TRANSIT at its own spawn and no device swallows one on any peer while the mark holds. The disc
  itself travels as an ordinary prop birth, on the whitelist a client's fresh spawn needs to reach
  the host at all. `[V]` A desk module plugged in is destroyed in the
  hand by the native path and rides the same seam; an unplugged one is born into the hand. `[V]`
- **The connect reconcile.** Explicit deletes, the claim-tracked snapshot bracket, position
  corrections, and the quiescence-gated divergence sweep bounded by a per-class completeness floor
  and a half-of-the-world valve, then the pile bind ([join.md](join.md)). The join barrier means
  no wire expression lands while the loader's churn runs. `[V]`

## Trash piles and clumps

A pile is keyless; its only identity is a host id, and the pile-to-clump-to-pile transitions
re-skin that id in place under a sync-time context. The grab is caught at the use input's pre
observer; the re-pile is caught deterministically at the native seam on the re-pile spawn, which
reads the source clump and the spawned pile in one call. The client's mirror of a resting pile is
a rooted real pile actor; the clump's is a static-mesh stand-in. Every fact and every open item is
on [piles.md](piles.md). `[V]`

## Held items

A physics grab puts the prop in the physics-handle slot; the local stream's new-held edge
broadcasts it, and a freshly dispensed item born keyless is minted a key there. `[V]` The hotbar
hand item is not a world entity: the game destroys and respawns it on every switch, so it is
player expression on [players.md](players.md), excluded from the object scan and the prop
pipeline. `[V]` A pocket pickup is the destroy seam; a place is the drop intent. `[V]`

## Characters

The host's interceptor on the deferred-spawn call allocates the element for an allowlisted
character class and binds the actor once the spawn finishes; a client's own spawn of one is
cancelled. Characters that loaded with the level are enrolled by the object scan; characters a
graph spawns are caught at the native seam and enrolled at the next pose tick. A save-persisted
character on a joiner is adopted by class after a deferred poll, never by key, because the
kerfur's key is random per peer. Pose is a batch, rotated fair-share past a batch's worth. A
destroy is caught before it runs; a self-destroy inside the Blueprint is found dead by the pose
walk and retired. `[V]`

The wisp swarm's spawns are `EX_CallMath` from the swarm trigger's graph and are caught at the
native seam gated by the calling class; the ambient sky wisps are host-authored, their spawner
cancelled on clients and their colour variants allowlisted. A wisp mirror keeps its actor tick,
since the class's tick is per-viewer cosmetic, and its landing edge is replayed from the stream;
its self-destroy is retired by the pose walk. `[V]`

## Event actors

A second interceptor on the same deferred-spawn call carries a disjoint, name-matched allowlist
(the event classes load lazily), a full-rotation pose batch, a spawn that carries scale and a
class-interpreted birth blob, and a dead-retire in the pose walk for the actors that destroy
themselves at an event's end. A graph spawner (the pyramid's) is caught at the native seam. A
heading that lives outside the actor's rotation streams as an auxiliary yaw. `[V]`

**The materialisation window.** A mirror's component delegates bind during its begin-play, inside
the finish-spawning call, while its row is installed only after that call returns; so for a
moment a row lookup answers "not a mirror" for an actor that is. The window is published as a
scope with the actor being installed. The predicate is "the actor is a mirrored actor, or it is
the actor being materialised", never "some mirror is being born right now": registering the new
actor's collision fires delegates on other actors it lands on, and the loose predicate resolved
those under the wrong id. `[V]`

## The kerfur

The game gives the kerfur no stable identity and it has two forms of two classes. One host-only
kerfur id spans both; the rendered form is an ordinary character or prop mirror at its own
element id, rebound in place on a conversion with one transition broadcast. The conversion verbs
dispatch locally, so a conversion is detected by a death-watch poll, a mirror whose actor died
while its wire element is present, and the successor actor is captured deterministically at its
spawn through the bytecode seam's bracket. A client relays its own conversion as a request, the
host performs it, and the client claims and adopts its conversion ghost, parked, never destroyed
and respawned. A captured conversion prop is tracked but its generic spawn broadcast is
suppressed, so exactly one lane expresses it. The eye camera both forms carry is a child actor,
which the game itself excludes from its world-object universe; the identity layer excludes child
actors at every surface for the same reason. `[V]`

The id is RELEASED when the kerfur dies for good rather than converts, at the three seams that can
decide that: the conversion converge, on each branch where the verb destroyed the old form and no
successor appeared; the kerfur first refusal at the prop destroy chokepoint, but only when no
conversion bracket is open, since a capture that missed inside one is still a conversion and the
converge behind it rebinds the record; and the character destroy PRE, for a destroy that is visible
to it. A conversion reaches none of them. The release went unwired for a long time, and while it
was, a dead kerfur kept its record and its element id kept answering "kerfur" once the registry
recycled that id. `[V]`

## Deployables: the hook and the rope

- **Birth.** Nothing catches it. The firing player already holds a pointer to its own deployed
  hook, so the owner polls that one field and the other peers learn of it from the owner. The
  pointer is precise in a way a class scan is not: a variant of the hook class is placed by the
  level and attaches itself in its own begin-play on every peer, so a scan that adopted by class
  would express a hook the world already has once. The field is also left dangling when the game
  destroys a hook without clearing it, so every read is liveness-checked first. `[V]`
- **Identity, and it changes hands once.** While the field names the hook it is
  `(owner slot, sequence)`, outside the element registry, the shape the owner-entity lane already
  uses for a peer-owned thing every peer must see. The game clears that field the instant the
  second end anchors, while the actor lives on -- that edge is the handover, and the pair survives
  it: the anchor moves who may write the hook, it does not rename it. `[V]`
- **The handover carries the actor's own save record**, which already names both attach keys, both
  component names and the cable length. The host builds the record it applies rather than taking
  the sender's -- class, save key and base transform are its own, the cable length is clamped, and
  every attach key that resolves is reach-checked -- then hands it to the actor's own load verbs.
  The record does not carry the two attached flags, which the game writes only inside its attach
  verbs; the host restores them, because without them the game destroys the hook on release
  instead of returning the player's item. A mirror deliberately keeps them clear, which is what
  stops every peer minting that item. `[V]`
- **An anchored hook needs no pose on the wire.** The load path re-resolves both attach keys in
  the receiver's own world and re-attaches, so every copy is parented to that peer's own copy of
  whatever the hook is tied to and derives its pose from it. A hook tied to the ATV rides that
  peer's ATV. `[V]`
- **Destroy.** While the thrower holds it, the owner's death-watch announces it. After the
  handover the host death-watches its own copy and announces on the same pair. Either anchor
  actor dying is not announced at all: the game binds both anchors' destroy on every peer, so
  each copy retires itself on the same event. `[V]`
- **Late join.** The owner's keepalive re-announce covers a hook still in its thrower's hands. An
  anchored hook is in the host's save and a joiner boots it as a real actor; the host also replays
  every hook it has adopted at the joiner's ready edge, for the window between that joiner's save
  capture and its world-ready, and the joiner skips the ones whose key the game already resolves.
  The load path is what registers a key, so a resolvable key is exactly the set the save gave it.
  `[V]`
- **Not synced: what a client's hook does to a host-owned prop.** Attaching unfreezes the prop on
  that client alone and the constraint drags that client's copy. The answer is sync ownership of
  the prop moving to the client for the duration; it is not built. `[?]`

## The dupe matrix

Every place two seams can express one actor, and what deduplicates it:

| Overlap | Dedup |
|---|---|
| the initialisation post observer and the finish-spawning post (a menu spawn) | one processed-initialisation latch `[V]` |
| the connect snapshot and a live spawn during a join | the receiver's exact key and id dedup; registering a mirror is idempotent `[V]` |
| a re-seed and a second peer's connect snapshot | additive with no bracket; the receiver dedups; no sweep re-arm `[V]` |
| the kerfur converge and a re-seed re-expressing the kerfur | the known-keyed mark, the kerfur skip in the incremental express and in the reaper `[V]` |
| a client's save-loaded pile and the host's pile id | the position bind retires the client-local identity `[V]` |
| a client grabbing shared trash and the host's authoring | the client suppresses the native grab and sends an intent; it never authors shared trash `[V]` |
| the character interceptor and the event-actor interceptor on one call | disjoint allowlists; the substrate supports several interceptors `[V]` |
| a nested deferred spawn stealing a pending id in the post observer | correlation on the parameter pointer `[V]` |
| a client's conversion ghost grabbed | the ghost is claimed and parked at once `[V]` |
| the kerfur's eye camera and the whole prop identity universe | child actors are excluded at six surfaces `[V]` |
| a joiner's save-loaded anchored hook and the host's replay of the same hook | the host mints the save key, and the joiner skips a key the game already resolves `[V]` |
| a level-placed hook variant, which attaches itself on every peer | it is not in the lane's class table, and the owner's own field can never name it `[V]` |
| a conversion's fresh prop and the generic spawn seams | the captured form is tracked and its broadcast suppressed `[V]` |

## Needs a probe

- A save-loaded kerfur prop whose class match is ambiguous in a cluster resolves by nearest pose;
  the failure mode with two identical bodies within the match radius has not been staged. `[?]`
