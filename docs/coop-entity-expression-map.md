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

**No passive mint.** A client's census walk tracks a keyed prop by its key but mints no element: keyed
identity is host-authored by construction, a save-loaded prop adopts the host's id by key, and a
client-born prop is minted at its express seam, which broadcasts in the same breath. A passive
census mint once produced about two thousand zombie double rows per join. The one bind owner
(`coop/element/identity_create`) carries the guards: the host rejects a wire row over a live
host-local actor, a client dissolves its provisional row on the host's word, and one actor is one
row. On the host, a peer's spawn resolving onto a host-authoritative actor is handed back: the
actor is enrolled if untracked and re-expressed under the host's id. `[V]`

**A key on the wire resolves through `coop/props/prop_key_index`**, which holds every live keyed prop
of the current world: membership from the engine's own birth and death notifications, the key from
the four bodies that write it (`init`, `loadData`, the `setKey` event, and a `getKey` after another
object wrote the key). A wire read that misses first catches the object index up, so its miss means
no such prop here; a read that cannot answer yet (no session seed, no current world, a birth still
loading) says so instead, and the host's uniqueness check at enrolment reads the index as it stands. The tracked set the census keeps by key is a different thing, what this peer has
enrolled or tracked, and only the join sweep and the save-transfer key diff count it. `[V]`

**Tracking is gated on hosting, never on a peer being connected.** Every catch and enrol below
runs whenever a session exists with the host role: an event actor spawned while the host is alone
must still enrol, or the connect snapshot has nothing to send when someone joins. Only wire sends
are peer-gated. `[V]`

## The families at a glance

| Family | Birth dispatch | Caught by | Identity |
|---|---|---|---|
| keyed props | the save; a spawner; the spawn menu; a container extract | the object scan at world start; the initialisation post observer; the finish-spawning seam | the save key, plus a host id |
| chip piles | the save; a re-pile | the object scan's keyless lane; the native seam on the re-pile spawn | a host id only, keyless |
| the unrolled sleeping bag (`sleepingbagWrap_C`) | the packed bag's unroll, inside its Blueprint (host-run for a client's press, `sleepingbag_sync`) | the finish-spawning seam on the host | a portable random key minted at its host birth (the game never keys it: its construction runs `gen(false)`, never `init()`), plus a host id; a wrap already in the host's world before a client connects: unmeasured |
| a notebook's page text | the editor's commit and close (`ui_notebook`), a torn page added (`playerUsedOn`) | the prop record lane (`prop_save_data`): the editing peer publishes the notebook's own record | the notebook's save key; one editor at a time through a `device_occupancy` claim keyed by that key (`note_<hash>`); the host takes a record from the claim's holder or, while free, from anyone |
| the trash clump | the grab, inside the Blueprint; a broom stroke; a few creatures' graphs | the native seam on the clump's deferred spawn -- a broom's reads the swept pile out of the broom's frame, and a creature's has no pile to name (piles.md, Known limits) | the same host id, rebound at birth |
| a held physics prop | the grab | the new-held edge of the local stream | the key it already had; a keyless one is minted a key at the edge |
| the hotbar hand item | the hotbar switch | not a world entity: polled as player state | none; display only |
| a pocket pickup and a place | the destroy seam; the finish-spawning seam | the drop-intent door | the parked key |
| characters | a deferred spawn | the spawn interceptor; the object scan; the native seam for graph spawns | a host id |
| the wisp swarm | `EX_CallMath` spawns from the swarm trigger's graph | the native seam, gated by the calling class | a host id |
| the Geom Octa (`geomOcta_C`) | the host's night roll (an Octa, else a night egg); the dev menu's spawn button; the spawn menu's row | the EX spawn catch's gamemode source row on the host; the spawn interceptor for the dev button | a host id (an NPC element) |
| event actors | a deferred spawn, or a graph spawn | a second interceptor with a disjoint allowlist; the native seam | a host id |
| the kerfur | conversion verbs inside the Blueprint | the script-body gate on the verbs: refused on a client, converged at the return on the host; whom it serves, its player-0 reads answered on the host with the puppet of the client whose command it runs (`kerfur_command`, `served_player`) | one host kerfur id across both forms |
| the plain kerfur (the Kerfus) | a keyed prop with a brain; on and off are a field, not a second form | the prop lanes for identity and the record at birth; while on, the host's prop drive for its pose; KerfusState for on, charging and energy; whom it follows, its player-0 reads answered on the host with the puppet of the client that turned it on (`kerfus_follow`, `served_player`) | its prop key and host eid |
| a deployed hook or rope | the fire input, inside the Blueprint | the thrower polls its own `activeHook` field | `(owner slot, sequence)` while the thrower holds it, a host-minted save key once anchored |
| an explosion (`explosion_C`) | a Blueprint's deferred spawn (a projectile, a fuse, a burn, a broken prop, a device) | on the host the capture watch on its begin-play; on a client the deferred-spawn post, which dissolves it after the request half has read its source | none: the host's actor is the only one and each client spawns a copy of its own from `ExplosionSpawn` |
| a remote player's puppet (`mainPlayer_C`, unpossessed) | `RemotePlayer::Spawn` -> `engine::SpawnActor` | registered between its deferred spawn and its `FinishSpawning`, by the before-finish callback; unregistered after its destroy (`DestroySlot`) | the peer's slot in `players::Registry` |
| an ATV (`ATV_C` and its descendants, the witch car included) | the save; a runtime spawn | the object scan's ATV lane (`atv_index`); the host announces each by key (`AtvSpawn`); the seat and the syncer are the host's table (`atv_authority`) | the game's own key, which the save carries; a runtime one gets a synthetic key at its host birth |
| status effects (effect_C) | each peer's own, from its gamemode's addEffect; never sent | -- | none: a per-process actor; at a join the host's effect records are reset out of the joiner's save at the join |
| a fuse tower (the three `coordRadarDish_C` and the one `radiotower_C`) | placed in the level; no runtime birth (the gamemode's `radiotower` is its one `GetActorOfClass`) | not caught: the host reads each tower every tick; a client keeps the host's last row of each tower and applies it when the tower resolves (the dish by the object index, the radio tower by the gamemode's field), looking up again once a second while a row waits | the pair (kind, id): a dish's own `id` 0..2, the radio tower's 0; its state, not its life, is what crosses (`coop/world/fuse_tower_rows`) |

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
- **Key uniqueness.** The level's own content carries duplicate keys (present before any save record
  loads, at the same keys and places every load), a clone family of dozens of
  trash-bit piles on one key among them, and a New Game saves them as they are; every identity
  layer assumes uniqueness. The host is the key authority: at enrolment, a keyed actor whose key
  another live actor already carries is re-keyed with a fresh random key, once. The fix bakes into
  the host's save and every transferred world for an actor that lives to a save; an actor
  that a save's load destroys and re-creates from its records takes its throwaway key with it.
  A dead incumbent's recreate inherits its identity. Clients never re-key. `[V]`
- **A client never authors a save-loaded prop.** Three intent doors route around it: the drop
  intent for a parked key placed after a pickup, the whitelisted births (a reel, a module, a
  drive, a container extract) the client cannot avoid, and a birth a PLAYER's own spawn verb
  asked for; the host performs all three. Any other client-born keyed prop is dropped at the
  door. `[V]`
- **Who asked, for a client's fresh birth.** The third door cannot be a class list, because the
  spawn menu is the whole catalog and every class on it is also what the world's own spawners,
  morphs and impacts mint for themselves on each peer independently -- a class test would double
  the world. So the birth is admitted on AUTHORSHIP, read at the seam where the VM still holds the
  calling Blueprint frame and gone a tick later at the drain
  (`coop/props/prop_spawn_authoring`). Two verbs: the spawn menu, identified by its CALLER, since
  `ui_spawnmenu_C::spawn` and `lib_C::replaceProp` and the physics-impact component all reach the
  same `mainGamemode_C::spawnPropThroughGamemode`; and the toolgun, whose ubergraph finishes its
  own spawns and so names itself in the native seam's caller frame. The mark rides the pending
  entry to the drain and crosses as an ordinary drop intent -- not a fresh birth, which would
  sleep the host's copy, while a menu prop must fall on the host as it falls here. `[V]`
- **The prop's own save record, beside its birth.** A class that keeps save state of its own --
  a reel's progress, a disc's files -- has that state serialized by the game's own `getData` and
  carried on its own message, addressed by Key and sent behind the spawn row on the same lane, so
  a mirror never starts from a class default. Membership is the class declaring a `getData` below
  `Aprop_C`, read off the live class chain rather than from a list. A record whose prop has not
  arrived parks by Key with no expiry: an element id names an actor, and the whole reason the
  record has to travel is that the actor is destroyed and remade. `[V]` For a birth the host
  spawns from a client's intent, the client's record is the record: the host awaits it and
  publishes nothing for the key until it LANDS, applied at once or from the park, and the land
  publishes it; a joiner seeded inside the wait gets it from that publish. `[V]`
- **A container's contents are not its record.** They are a slice of the shared GObjStack, the
  contents lane's ([devices.md](devices.md)). A thrown container is rebound to its slot of the
  thrower's own array by `loadData`, with no verb fired, so its contents travel as the thrower's
  birth slice after the host's echo binds it, and the host's copy is a transfer in progress until
  that slice lands. A container the host throws publishes its slice at its express. `[RD]`
- **Destroy.** The engine's destroy call is caught before it runs on either role, on every
  route, at the native seam; a Blueprint-internal vanish (the truck, culling, a lifespan) is
  caught by the host's reaper death-watch and destroyed by id. `[V]` A floppy disc inserted into
  a laptop or a signal server dies into that device's slot through that seam, and an ejected one is
  born through the birth channels with its content on the save-record lane above. The slot itself
  is state on the wire, so the destroy is not the only thing that crosses: the host owns
  every device's slot, a peer reports the outcome of one its own game changed, and the host's
  re-publish is the answer ([base-computer.md](base-computer.md)). `[V]` **An ejected disc is born where the box that
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

Two different actors are called a pile here, and only one of them is keyless. A CHIP pile -- the
ambient trash a player carries as a clump -- is keyless; its only identity is a host id, and the
pile-to-clump-to-pile transitions
re-skin that id in place under a sync-time context. The grab and a broom's sweep are caught at the
native seam on the clump's spawn, a sweep by the pile the broom's frame names; the re-pile is caught
deterministically at the native seam on the re-pile spawn, which reads the source clump and the
spawned pile in one call. The client's mirror of a resting pile is
the game's own pile actor -- at a join the one the client loaded itself -- and the clump's is the
game's own clump actor, both with the verbs that would author a transition refused.

A clump is also a form the entity RESTS in: one that came to rest on a simulating box, on a slope
or under a busy hand never turns back, and the game saves and loads it as a clump. So a clump at
rest is enrolled, captured, expressed and bound exactly as a pile is -- the census and the capture
take both forms, the snapshot's keyless row names either class, the joiner binds its own loaded
copy of the row's form at the save-time position and parks it (tick and physics off: the host
authors its pose), and the row is followed by a to-clump convert at the entity's current
generation, without which every later pose of it is held. The key on a row says which form the
save held: an entity that changed form in the join window is materialised as it is now, and the
joiner's copy of the old form is retired at the key. `[V]` The key and its form are the blob
capture's: the capture is the only writer, an id sits in exactly one of the capture's pile and clump
maps, and the row is the key's one carrier (no convert repeats it). The snapshot drain is keyed by
id and resolves each to its element's current actor at its turn. `[?]`

A DISPENSER pile (`trashBitsPile_C`, the "uses 6 of 7" one) is the opposite: it is a save actor and
its save key IS its cross-peer identity, which is what its counter mirror, its depletion destroy and
the host's watch on the broom verb all name it by. Every fact and every open item is on
[piles.md](piles.md). `[V]`

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
walk and retired. `[V]` A client's mirror of a character is stamped as the native spawner (`spawner_stamp`): the
constants a class's own spawner writes between the two spawn halves are written there too. `[?]`

The wisp swarm's spawns are `EX_CallMath` from the swarm trigger's graph and are caught at the
native seam gated by the calling class; the ambient sky wisps are host-authored, their spawner
cancelled on clients and their colour variants allowlisted. A wisp mirror keeps its actor tick,
since the class's tick is per-viewer cosmetic, and its landing edge is replayed from the stream;
its self-destroy is retired by the pose walk. `[V]`

**A creature's event count.** A creature that calls `lib_C::setEvent` (the krampus, the orb, the ariral, the
kerfur Omega among them) moves the gamemode's event count, which drives a peer's threat music, its sleep refusal and its
save gate. Its mirror's own calls are refused once the relay binds it; the host's count for that creature crosses whole
by its element id and is applied to the mirror's registration, after the mirror's own registrations from before its
install are taken back, and taken back again at its end (`coop/world/creature_event_relay`). The killer wisp, the
fossilhound, the antibreather and the egg compute their event from the local camera, render state or player 0 and keep
their own per peer. `[RD]`

**A parked creature's brain.** A mirror of the zombie family or the fossilhound runs none of its Blueprint's event
graph. It is marked when it is born (between the two halves of its spawn, or at the bind of a client's own
save-loaded twin), and the graph watches (`coop/creatures/npc_mirror_brain`) refuse every entry of a marked mirror
but the few a table names: the footstep notify, and the fossilhound's per-viewer loop. What its BeginPlay set up
for the eye is done by our code after the install: the zombie's animation instance and walk blend, the fossilhound's
mesh parent, animation instance and owner. Lost on a mirror: the attack animation and sounds, the fossilhound's hit
sound and its hidden-then-dig-out. A client's own creature of these classes is never marked and runs whole. The mark
is the actor's slot and the slot's serial, so a recycled address is never a mirror, and it ends with the mirror.
`[RD]`; the refusals `[?]` until a run reads them.

**A client's hit on a parked creature.** A client's swing ends in the struck mirror's `addDamage` with the player as the
damager. The call is watched (`coop/creatures/creature_hit_intent`): the client plays the zombie family's hit feedback
at once (the fossilhound has none), sends the creature's element id with the claimed damage, the point struck and the
impact, and cancels the call. The host judges the swing against the sender's held item and its reach data, its reach
from the eye the host holds for it, that the sender's newest streamed aim faces the point (within 60 degrees, in 3-D; eye
and aim are captured when the intent arrives), the sight to the point and the point's distance from the creature,
cuts the damage to the item's most and runs the creature's own `addDamage` with a null damager; health, death and
the destroy are the host's, and nothing is answered. The swinger's liveness is judged by the health its pose stream
carries (a dead player's pose says 0, and the puppet's `dead` flag is never set by a peer's death): at most 0 refuses
the swing `dead`. A zombie the host kills this way leaves its ragdoll body on the host alone: the body's class has no
lane (owed). A ranged tool's hit is not a swing and is refused. `[RD]`

**The Geom Octa.** It is a character of the NPC lane. The host's night roll (an Octa, or else a night egg) is a
source row of the EX spawn catch, the dev menu's button spawns through the interceptor, and the allowlist gives
either an element, so its identity, its spawn on every peer, its pose and its destroy (its own rise-away
despawn included) are the generic ones. A client's roll is refused (`coop/world/spawn_authority`), so a client
mints neither the Octa nor the egg; an Octa a client's own menu or toolgun spawns stays local, with its blast
dissolved (GO-3, owed). `[V]` (`octa_drill` births, measure)

*The mirror.* It is marked between the two halves of its spawn (`coop/creatures/npc_mirror_brain`) and runs
the game's Blueprint for the eye, with the twelve entries that decide refused (the wander, the target, the
leave, the fire verdict); its actor tick is kept for the turn toward its target. The mirror tick is a table by
class in `npc_mirror`: the wisp and the Octa keep the actor tick, every other character loses it, and the
movement tick is always off. `[V]` (the refusals: `octa_drill` measure; the kept tick `[RD]`)

*The attack.* The host's two decisions cross as `GeomOctaEvent` (`coop/creatures/geom_octa_sync`, the default lane,
behind the Octa's `EntitySpawn`): an Aggro (the target as a player slot, an element or none, and the Octa's count
of fire verdicts) and a FireVerdict (that count, and whether it fires again). The client replays the Aggro as its
own `minSphere` begin-overlap with the target's counterpart (the host's player is the host's puppet there) and
answers its own refused fire-finish notify with the newest verdict kept, replaying the notify once with the
answer written into its overlap read (`coop/creatures/geom_octa_relay`). The look is the client's own Blueprint
running a few moments behind the host's; every outcome is the host's. `[V]` (`octa_drill` attack)

*Two lasers, one beam.* Each peer's Octa runs its own `fire()`, so the host's laser actor and each client's are
two actors drawing one beam. The client's blast is dissolved by the explosion lane with no request (the laser is
none of its request kinds), and the host's arrives as `ExplosionSpawn`: one explosion per shot. `[V]` (`octa_drill`
measure: the client's applied blasts equal the host's captured ones)

*The join.* The joiner's snapshot carries the Octa and, for one mid-attack on a target the wire can name, one
Aggro to that slot (the target now, the verdict count now). Its graph starts the attack from the beginning, shows
the host's freshest decision and runs about one chain (some twelve seconds) behind. An Octa between attacks, or
attacking nobody, sends nothing; the joiner's next Aggro is the host's next. `[V]` (`octa_drill` join; with
`octa_red=no_join` a verdict arrives with no Aggro and the arm fails). Named limits: an attack on an ATV is
relayed as none (an ATV is no element), so the look idles and its blast still arrives; a joiner's look runs behind.

## Event actors

A second interceptor on the same deferred-spawn call carries a disjoint, name-matched allowlist
(the event classes load lazily), a full-rotation pose batch, a spawn that carries scale and a
class-interpreted birth blob, and a dead-retire in the pose walk for the actors that destroy
themselves at an event's end. A graph spawner (the pyramid's, the jellyfish path's, and the story
events' spawners for the vent crawler, the grays, the eggs and the tentacle balls) is caught at the
native seam; on a client those creatures' own bodies are refused, so a mirror arms no gameplay. A heading that lives outside the actor's rotation streams as an auxiliary yaw. A
mirror's tick is parked and whatever component its class simulates with is stopped, found from the
class's members, so the pose drive alone moves it. `[V]`

**The Bad Sun** is the event actor no mirror expresses: every peer runs its own `badSun_C` copy and
its harm is local, so nothing crosses as a pose. Identity: none, a copy is found by class. Birth:
the save (a joiner's copy comes with the join blob) or `Spawn Bad Sun`. Existence is the host's,
through `coop/world/weather_badsun`: the host's spawn and its copy's `remove()` send an absolute
present or absent, a client's own spawn is refused at the script-body gate, and a joiner is seeded
at its world-ready. A client keeps exactly one copy. End: each copy's own fade; the host's absent
destroys an inactive copy at once and leaves an active one to end by its own clock. The wire half is
`[?]` until the lane's drill runs; the Blueprint routes are `[RD]`.

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
are Blueprint self-calls the script-body gate sees on every route: a client's gate refuses the
verb and asks the host, so a client never converts a kerfur and never holds a form of its own
making; the host's verb, its own or a request's, converges at its return on the successor captured
at its spawn through the gate's window. A captured conversion prop is tracked but its generic spawn
broadcast is suppressed, so exactly one lane expresses it; the conversion's disc is an ordinary
keyed prop, expressed where every fresh prop is. The eye camera both forms carry is a child actor,
which the game itself excludes from its world-object universe; the identity layer excludes child
actors at every surface for the same reason. `[V]`

The id is RELEASED when the kerfur dies for good rather than converts, at the seams that can
decide that: the conversion converge, on each branch where the verb destroyed the old form and no
successor appeared; the prop destroy chokepoint, for a kerfur prop that dies outside a conversion
verb; the character destroy PRE, for a destroy that is visible to it; and the pose walk's retire,
for one that is not. A conversion reaches none of them. The release went unwired for a long time,
and while it was, a dead kerfur kept its record and its element id kept answering "kerfur" once the
registry recycled that id. `[V]`

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
- **A prop a hook is tied to.** The constraint exists on the HOST and nowhere else, whichever
  peer fired the hook (`coop/items/hook_constraint`): a client's game breaks every tie a hook of
  the lane's classes builds on it, in the frame that built it (a post watch on `makeAttachments`, `[V]` on the rig: `hookdrag` on `72742a09`, every role), and the host ties
  a client's hook on its own mirror of it, against that client's puppet, from the bite the owner's
  state names -- the actor's key and element id, the component, the head in the component's frame,
  and whether the hook flew before it bit, which is what wakes a frozen prop. Every prop under a
  hook constraint on the host is claimed into the driven-prop channel (`coop/items/hook_prop_claim`
  reads the host's own hooks, the adopted ones, the tied mirrors, and the save's and the level's
  own hooks through the shared scan), streams while it moves, and every other copy is parked and
  follows; the end edge closes the stream once it rests (`coop/props/prop_drive_host`). `[V]` (the
  driver: a client's hook dragged a prop 308 cm on both peers, 0.1 cm apart at the end; two peers'
  hooks on one prop, 2.8 cm median apart, 0.0 cm at the end). Two hooks on one prop are two host
  constraints on one host body. The velocity hand-back and the hand-take end are `[?]`: every
  measured end edge fired at |v| = 0. ANCHORED phase: `processKeys` -> `makeAttachments` rebuilds
  the constraint on every peer against that peer's own copy, and the same seam breaks it on every
  client, so the host's is the one constraint and the other copies are parented display -- `[V]`
  for the seam on an owner-phase build, `[?]` for an anchored build, which no run has staged.

## Explosions

- **Birth.** The host's own game spawns every explosion that exists: its capture watch on `explosion_C`'s
  begin-play reads the class, the transform and the blast parameters. A client's game spawns none: the
  deferred-spawn post nulls the result of every explosion birth and destroys the actor by a posted task
  (`coop/world/explosion_sync`). `[V]`
- **Identity: none.** No element, key or id is made for an explosion. The one name the wire carries is the
  actor the blast must not hurt, as an element id or a server box's index, so a copy spares the same actor
  the host's did. `[V]`
- **Expression: the host's spawn, fanned out.** The capture is sent as `ExplosionSpawn` to every
  world-ready client, and each spawns its own copy natively at the host's place inside the lane's own spawn
  scope, which the birth seam leaves alone. What a copy may still move is
  listed in `coop/world/explosion_parking`: its own player's damage and knock-back, the scorch. `[V]`
- **A client's request.** An explosion a client's own player is responsible for is requested, not made:
  its dissolved birth sends `ExplosionRequest`, and the host judges it and spawns the blast with its own
  parameters for the source (`coop/world/explosion_request`, `explosion_recipe`); the capture above then
  carries it to every peer, the requester included. Three kinds, told apart at the dissolved birth by
  its source: PROJECTILE (a rocket or sticky bomb the client fired, named by the `ProjectileFired` its
  launcher's fire body sent, the point read from the projectile), HAND (an armed grenade or pipe bomb that
  is the client's hand view, the point read from the hand item) and SELF (the client's gasoline-pilled
  body: no point on the wire, the host blasts at the sender's puppet with the pill level). A request has
  no identity of its own: a shot is the host's record until its blast consumes it, expires or the cap
  evicts it. `[V]`
- **An ATV a client syncs.** The ATV's own `explode` runs on its syncer's copy, which ejects the driver and
  spawns a blast that the client's birth seam dissolves; the client sends `AtvExplodeRequest` (the ATV's key and
  whether the story event's trap fell in that frame), and the host judges that the sender is the ATV's syncer
  and that the ATV has not just blown, then spawns the blast at the ATV's own `tp`
  (`coop/world/explosion_request_atv`), which the capture above carries. The ATV is blown until its syncer's
  health is back at 0 or above, the game's own re-arm. A host-synced ATV's death runs natively and needs no
  request. `[?]`
- **Not requested here.** Every other source (a prop broken, a device run, a prop lit) is its trigger's lane,
  whose host copy explodes natively and so
  reaches the capture. A projectile of the client's with no shot sent is not requested and is counted.
  `[V]`
- **A cord a player plugs.** A cord's link (its prop's `socket`, the cord's `socket`, the socket's `cord`) is
  the host's: a client's own press and release run the game's verbs, and their outcome goes to the host as a
  `CordLinkState` claim (`coop/interactables/cord_sync`); the host checks the sender, reach and its own drag, applies
  the game's occupied-socket rule and answers with the link it now has; every change is broadcast, and a joiner is
  seeded with every corded prop's link, plugged or not, which also repairs a cord its save plugged. A level socket is
  named by its key, a generator's outlet by its owner prop's key and its component.
- **A coffee device a player uses.** A press of the coffee vending machine or the coffee maker is a request the host
  performs: a client's own press is cancelled at the gate and goes as a `CoffeeActionIntent` naming the device by its
  key (`coop/interactables/coffee_intent`); the host checks the sender's reach and that the key names exactly one of
  the two classes, then runs the game's own action with the sender's puppet as the player, so the debit crosses by the
  balance and the cup by PropSpawn on the Default lane. The maker's state is its save record, published by the host after a brew
  and after a fill (`coop/interactables/coffee_state`) and applied on each client through the record's own `loadData`;
  the fill (a cup or a bag touching the maker) is the host's, a client's own run of the hit handler is cancelled, and
  the cup's destroy crosses by the native destroy seam. A client's record for a maker or a bag is refused, except the
  one behind its own drop. The maker's power follows the cord lane on each peer and is not in the record.
  A joiner's maker and bags arrive with the prop snapshot's records. `[V]` (`coffee_drill`)
- **A salt rocket a client lights.** The rocket's own fuse and thrust run only on the host: a client's gate refuses its `attemptIgnite` and its `ignite`, and a light its own player's lighter or torch made goes to the host as `PropIgniteIntent` (`coop/world/salt_rocket_ignite`); the host checks the sender's reach and the tool its hand held, performs the verb on its copy, and its `fire()` puts the rocket under the drive claim (`coop/world/salt_rocket_flight`) until its blast, which the capture above carries. `[V]`
- **Destroy.** Nothing crosses. Each copy ends by the actor's own lifespan, 10 s by the class default.
  `[RD]`
- **Late join.** None: a one-shot cue, not re-sent; a request's state clears with its session. `[V]`

## The ATV

- **Birth.** The save places an ATV, or the host's own game spawns one at runtime. The host's index
  (`coop/interactables/atv_index`) files every live ATV under its game key and announces a key it gains with
  `AtvSpawn`; a client's mirror (`atv_runtime_mirror`) is spawned carrying that key, parked until its world is
  ready. `[V]`
- **Identity.** The ATV's own game key, on every peer. An ATV a client spawned, if one can be, is unknown to the
  host's index; a sit in it is answered `unknown_atv` and the client dismounts. `[?]`
- **Authority: the seat and the syncer are the host's table.** `AtvEntry::occupantSlot` (the seat) and
  `syncerSlot` (who writes the rig) are decided by the host alone (`coop/interactables/atv_authority`): a client
  asks with `AtvSeat` (sit or leave) and `AtvTakeover` (a grab), each judged and answered with `AtvAnswer`; the
  host's own player is judged by the same table. The three are never aliased: the host's index row (the birth
  owner), the occupant, the syncer. `[?]`
- **Expression: the syncer's stamped stream.** The syncer streams `AtvState`; a client's reaches the host alone,
  which applies only its syncer's blocks and re-sends each, marked and stamped with the seat and the syncer, to
  every other client. The host's own copy streams while it syncs. Every machine follows the stamp, yielding to
  an answer or a stamp that names another occupant or syncer through the game's own dismount or a dropped grab.
  `[?]`
- **Expression: the syncer authors the damage and the death.** A copy that does not own the ATV's tick cancels
  its `addDamage`, `exploded` and `explode` (`coop/interactables/atv_damage_gate`, beside the collision guard) and
  takes the health from the syncer's condition block. The death is the owner's own `explode`, whose blast is
  dissolved at the birth seam, and the host's blast that `AtvExplodeRequest` asks for (the explosion section
  above). The story event's trap is the host's state: the stamp's bit 4 (`kAtvTrap`) carries it to every copy,
  and a client whose trap fired tells the host so in the request. `[?]`
- **Destroy.** `AtvDestroy` from the host, for a key its index pruned; the client destroys the mirror or the
  native twin under that key. `[V]`
- **Late join.** The connect snapshot stamps every live ATV with the host's table and its trap and sets the adopt
  flag, so a joiner places its copy once whoever syncs it; a joiner sends no request before its world is ready.
  `[?]`

## The dupe matrix

Every place two seams can express one actor, and what deduplicates it:

| Overlap | Dedup |
|---|---|
| the initialisation post observer and the finish-spawning post (a menu spawn) | one processed-initialisation latch `[V]` |
| the connect snapshot and a live spawn during a join | the receiver's exact key and id dedup; registering a mirror is idempotent `[V]` |
| a re-seed and a second peer's connect snapshot | additive with no bracket; the receiver dedups; no sweep re-arm `[V]` |
| the kerfur converge and a re-seed re-expressing the kerfur | the known-keyed mark, the kerfur skip in the incremental express and in the reaper `[V]` |
| a client's save-loaded pile and the host's pile id | the position bind retires the client-local identity `[V]` |
| a client grabbing or sweeping shared trash and the host's authoring | the client suppresses the native grab and sends an intent, and refuses its broom stroke whole and sends what the stroke read of its holder; the host performs both, so a client authors shared trash by neither `[V]` |
| the character interceptor and the event-actor interceptor on one call | disjoint allowlists; the substrate supports several interceptors `[V]` |
| a nested deferred spawn stealing a pending id in the post observer | correlation on the parameter pointer `[V]` |
| a client's own conversion of a kerfur, and the host's | none of a client's exists: its gate refuses the verb and asks, so the host's is the one conversion `[V]` (`kerfur_convert_drill`) |
| the kerfur's eye camera and the whole prop identity universe | child actors are excluded at six surfaces `[V]` |
| a joiner's save-loaded anchored hook and the host's replay of the same hook | the host mints the save key, and the joiner skips a key the game already resolves `[V]` |
| a level-placed hook variant, which attaches itself on every peer | it is not in the lane's class table, and the owner's own field can never name it `[V]` |
| a conversion's fresh prop and the generic spawn seams | the captured form is tracked and its broadcast suppressed `[V]` |
| a joiner's save-loaded Bad Sun copy and the seed's present, or the host's live present | the apply counts the live copies first: one copy is left alone, none is spawned, two are logged and not repaired `[V]` (the drill's rejoin) |
| a client's menu-spawned prop and the host's copy of it, spawned from the intent | the client's own actor IS the mirror: the host sets the client's key, and the echo resolves it back to that actor and converges its pose instead of minting a second `[V]` |
| a client's own rocket, sticky bomb, held grenade or pipe bomb, or pilled burn: the client's birth of its explosion and the host's blast its request makes | one blast: the client's birth is dissolved and never spawns; the host's blast reaches every peer as `ExplosionSpawn`, the requester included `[V]` (the request drill's arms) |
| a joiner's save-loaded ATV and the host's announce of the same ATV | the joiner resolves the announce by key (the lane's row, else the game's key map) and adopts the copy it holds; it spawns a mirror only for a key it holds none under, and the host's set edge (`AtvSetComplete`) removes the save's ATVs the host has dropped `[V]` (`atv_spawn_drill`) |
| a client's light on a salt rocket and the host's | one fuse: a client's copy never runs `fire()` (its gate refuses the light and asks); the host lights its copy and drives its flight `[V]` (`fire_ignite_drill`) |
| a client's local sit or grab of an ATV and the host's table | one seat and one syncer: the client acts at once, asks, and yields to the answer or stamp that names another; the host drops a block from any slot that is not the ATV's syncer `[?]` |
| the host's idle stream and a client syncer's stream of one ATV | the host streams an ATV only while its table names the host; until the hand-back a client syncer's blocks are the ones applied and re-sent `[?]` |
| a client's plug of a cord and the host's | one link: the client's own verb runs and its outcome is a claim; the host compares, applies the game's rule and answers every claim, and a client copy's stretch break is refused `[V]` (`cord_drill`) |
| a client's press or fill of a coffee device and the host's | one run: the client's body is cancelled at the gate (the press is sent, the hit handler is counted) and the host performs it once; the maker's flags change on the host's copy and reach a client as the record, never from the client's own run `[V]` (`coffee_drill`) |
| a client's Octa laser and the host's | two actors, one beam: each peer's Octa runs its own `fire()`; the client's blast is dissolved with no request and the host's `ExplosionSpawn` is the one explosion `[V]` (`octa_drill`) |
| a client's roll of the night Octa and the host's | one Octa: the client's roll body is refused at the script gate and the host's roll is the only source `[V]` (`octa_drill` births) |

## Needs a probe

- A save-loaded kerfur prop whose class match is ambiguous in a cluster resolves by nearest pose;
  the failure mode with two identical bodies within the match radius has not been staged. `[?]`
