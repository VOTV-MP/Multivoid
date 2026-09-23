# Props

## Purpose

Everything a player can pick up, carry, throw, drop, stick to a wall, put in a pocket or take out
of a container: how it is named across peers, where the mod catches its birth and death, who
owns it at rest and in a hand, and what happens to a prop whose own Blueprint keeps changing
it. Trash piles and clumps are a family of their own on [piles.md](piles.md); the item in a
player's hand is on [players.md](players.md).

## How it works

### Identity

A prop is `Aprop_C` or a subclass, and every keyed prop carries the game's own save key: a
string the prop's construction script mints as a fresh GUID when none is set, and the save
persists. The mod reads it and never overwrites it, with one exception: on a receiver, the
host's key is written into a mirror before its spawn finishes, so both peers hold the same
name. A save-loaded prop has the same key on both peers because both loaded the same bytes.
An element id rides alongside the key on every message (`coop/props/prop_element_tracker`,
`coop/props/prop_synth_key` for the families whose Blueprints mint no key).

### Where a prop is caught

Nothing about a prop is reflected as replication, so the mod watches four seams:

| A prop that | Is caught by |
|---|---|
| came from the save | the shared discovery pass at world start (`coop/element/object_scan_hub`, over the object index the engine's own notifications keep) |
| was born by a spawner (mushrooms, pinecones, forage) | the prop's initialisation on the host; a client's shared-world spawners are parked (`coop/world/spawn_authority`) and the host mirrors what its own produce (`coop/props/host_spawn_watcher`) |
| was spawned by the spawn menu or extracted from a container on the host | the engine's finish-spawning call, because those births run their initialisation inside the Blueprint and that call is where the actor is finished |
| was destroyed | the engine's destroy call on either role: eaten, broken, picked up into a pocket (`coop/props/prop_lifecycle`); a prop that vanishes inside a Blueprint (the truck, culling, a lifespan) is caught by the host's death-watch and destroyed by id on every peer (`coop/props/registry_reaper`) |

A birth message carries the prop's class, key, id, transform and physics flags. For a class that
keeps save state of its own -- a tape reel's progress, a floppy disc's files -- a second message
follows it on the same lane with the prop's whole save record, the bytes the prop's own `getData`
produces, addressed by key (`coop/props/prop_save_data`). Which classes those are is not a list we
keep: it is whether the class declares a `getData` of its own, read from the live class chain, so a
save-backed class the game adds is carried without a change here. A record that arrives for a prop
this peer has not created yet waits under its key until that prop appears, because a key survives
the destroy-and-recreate that made the record need to travel in the first place.

### How a birth finds its local actor

A birth names a key, so the receiver looks that key up first and, finding it, converges the actor
it already has rather than making a second one. When the key resolves to nothing there is still a
case to answer: the per-peer natural spawners -- mushrooms, underground garbage -- place the same
logical thing on each peer with a different key and at slightly different spots, and a birth taken
at face value would stand a duplicate beside the copy this peer already grew. So a birth with no
key match falls back to a scan for a prop of the same class, carrying the same `list_props` row,
within 30 cm of where the birth says it is; the first such prop is adopted, re-keyed to the wire
key and bound. With no candidate at all, a fresh mirror is made
(`coop/props/remote_prop_spawn`, `coop/props/prop_fresh_spawn`).

What that scan must never take is an actor that only looks like a world prop. A player's hand
item is one: it is a real actor of the right class, it stands at that player's hands, and it
belongs to the hand lane, which destroys it the moment the hand changes ([players.md](players.md)).
Adopting one binds a wire identity to an actor that is about to disappear for reasons of its own,
and the prop it was meant to name never appears on that peer at all. The hand axis -- this peer's
own hotbar actor and every peer's display mirror -- is excluded from the scan, the same set the
prop census leaves out of its own walk.

### Who authors a prop

At rest, the host. A client never authors the existence of a shared-world prop: its own fresh
spawn of a keyed prop is not broadcast. Instead a client's actions become intents the host
performs (`coop/props/prop_drop_intent`):

- **Pick up.** The client's pickup destroys the world actor; that destroy crosses the seam, the
  host destroys its copy, and the key is parked, which records that the host holds no copy now.
- **Place.** The client's fresh spawn is caught at the finish-spawning seam and sent as a drop
  intent naming the parked key, the class and the transform; the host spawns it as the sole
  author and the mirror comes back down the ordinary birth broadcast.
- **Births the client cannot avoid.** A reel ejected from a caddy, a module or a drive taken from
  a rack, an item extracted from a container all materialise on the client first; they go
  through the same door, class-checked at the host, born asleep where the class needs it.
- **Births a player asked for.** The sandbox spawn menu and the toolgun. These cannot be named by
  class -- the menu is the whole catalog, and the world's own spawners, morphs and impacts mint
  those same classes per peer, so a class test would double the world. They are admitted on
  AUTHORSHIP instead: the birth seam asks whether a player's own spawn verb is on the stack, which
  is a fact only that instant holds, since the VM still has the calling Blueprint frame there
  (`coop/props/prop_spawn_authoring`). Such a birth crosses as an ordinary drop intent and is NOT
  slept on the host -- it was dropped at the player's aim and must fall there as it falls here.

Any other keyed prop a client creates is dropped at that door and never reaches the host: that is
the world's own churn, which every peer produces for itself.

### The data row a prop carries

A prop resolves one row of the game's master table when it initialises -- `list_props`, 2,471 rows,
keyed by the prop's own name -- and copies the whole row into `propData` (all but one path: a prop
spawned with `ingoreFix` set takes only the mesh from the row and keeps its own flags). Four of
its seventeen fields are switches the game itself reads on the interaction path: `heavy` picks
lift versus drag,
`canHold` refuses the grab with the same "too heavy" hint a heavy prop gets, `canCollect` keeps the
prop out of the pocket, and `ignoreInteractions` makes the prop no target at all -- the use press
plays the deny sound and returns, and the look-at prompt never appears over it. In the shipped
table 188 rows are heavy, 196 refuse holding, 28 refuse collecting and 8 ignore interactions; a
prop whose name matches no row is holdable and collectable.

This mod reads exactly one of them, `heavy`. That costs nothing wherever the game's own trace
chooses the subject, because the game applies the rest itself; it costs something wherever a lane
picks the subject on its own, which today is the client's pile cone described in
[piles.md](piles.md).

### Holding, throwing, dropping

While a prop is held, its holder owns it. The holder streams the held prop's world transform
every frame, unreliable and newest-wins (`coop/player/local_streams`). A receiver resolves the
prop by key on the first frame, turns its physics off, and follows the stream with a fixed-delay
snapshot interpolation that renders one interval behind the newest pose on its own clock
(`coop/props/remote_prop`, `coop/props/active_drive`). A grab changes the prop as well as moving
it: every grab verb of the game wakes and unfreezes the prop it takes, on the grabbing machine only.
So each hold carries a generation the holder mints at its grab, and at the first pose of a new hold
a receiver runs the same verb on its copy -- a fire extinguisher on its wall mount, a drive in a
slot, anything the toolgun froze or a save left asleep -- while a stuck wall-attachable gets its own
lane's unstick. The release closes the hold, and a stick closes the hold it ended, so a pose still in
flight from a hold that ended with the prop frozen on the holder starts nothing (MTA's sync time
context, and the driven-prop channel's claim generation below). A driven copy stays kinematic even
when the game here switches its simulation back on: this peer's own laptop exit re-initialises a
chair another peer carries, and the drive re-latches it. A copy that is static here is not driven,
since the game cannot hold a body that does not simulate; the two copies disagree on the flag, and
the refusal is logged once per hold. On release, a reliable message carries the linear and angular
velocity the body had at the instant of release: the game's throw is not an impulse but the
tracking velocity the physics engine accumulated while the player flicked the camera, and a
receiver that re-enables physics and hands that velocity back reproduces it. It carries the prop's
frozen and sleep at that edge as well, which the receiver's copy takes on before its physics comes
back: a hold that ended in a slot's insert stays frozen, and one none of whose poses arrived still
ends unfrozen. A release that arrives after the holder took the same prop again, or for a prop
another hold has since taken, closes its hold and changes nothing. The receiver also calls the
prop's own thrown event, so the sound and the trail come from the engine. A stream that stops for
half a second, or a holder that switches to another prop, is an implicit release. Grab and throw sounds that the game plays only for the local player are
synthesised on the receiver at the prop's position (`coop/props/prop_sound`); the pocket blip
is relayed the same way (`coop/items/inventory_pickup_sync`).

### Dragged by a hook

A prop nobody is holding can still be in motion: a hook's constraint pulls it along behind the
player, an anchored hook ties it to something that moves, and after the hook lets go it slides until
it rests. The held-prop stream never sees it, since that stream is sourced from the player's grab
slot alone, so the host keeps a set of the props the hooks on its machine are tied to -- its own,
the anchored ones it adopted, its mirrors of the clients' hooks, and the save's and the level's own
-- fed by the hook lane rather than by a per-frame walk, and streams each one's pose while it moves
(`coop/props/prop_drive_host`). Every hook's constraint exists on the host and nowhere else ([deployables.md](deployables.md)),
so this one set is every prop any hook can move. A broom's push is
the set's second feeder: every stroke is the host's ([piles.md](piles.md)), and each prop a stroke
pushes coasts under the stream until it rests, while a prop a verb already holds stays that verb's
(`coop/items/broom_push`). The trash a stroke knocks out of a dispenser pile coasts too, from its
spawn: the host catches the finish of each spawn the pile's own `broomed` body makes, and the
streams open after the tick's spawns are named, so each has its id by then. A prop that a pushed
prop knocks has no verb of its own and is not streamed. A receiver parks the prop on the first pose
and follows the stream the way a carried trash clump is followed -- the fixed-delay interpolation,
frozen at the last pose across a gap -- where a held prop snaps to each pose; a reliable end edge
carries the final pose, the host's physics flags and the velocity once the prop has rested or a hand
has taken it, and closes the stream's generation so a pose still in flight cannot park the prop
again (`coop/props/prop_drive_stream`). A hand always wins: a claimed prop somebody grabs leaves the
set and rides the held-prop stream.

A mirror's physics and collision are set to what the game's own initialisation would have
produced on this peer (`coop/props/prop_wire_parity`); a fresh mirror starts kinematic while it
is remote-owned. A spawn or destroy the receiver applied is marked so the symmetric observer
does not broadcast it back (`coop/props/prop_echo_suppress`).

### Sticking to a wall

Wall-attachable props (the cameras) commit their stick inside the Blueprint; the one visible seam
is the commit entry, reached through a latent delay that the engine resumes through the
function dispatch. The sticking peer broadcasts the frozen state and the commit pose before its
own hold breaks, on the same ordered lane as the release so the order is structural; the
receiver re-poses the prop and dispatches the component's own force-stick, so the attach, the
effect and the destroy binding are the game's (`coop/props/prop_stick_sync`).

### Containers

A container's contents are not on the container: every container reads them from one global
per-peer array in the save object, addressed by an index the container holds, and every mutating
verb runs inside the Blueprint, below the two dispatch seams a hook can intercept. The
script-body gate does see it -- that is how this lane learns a container changed -- but it runs on
the presser's own machine, and no local body can wait for an answer from across the network. So
the peer whose verb fired authors the contents and the host arbitrates them (`coop/props/container_contents_sync`): one slice of its
array per live container is sent at a joiner's ready edge and on change, a client's slice is
accepted only if its author could have reached that container, has not sent more in the last
second than an honest client produces, and edited the truth the host last published, and an
extraction on a client is a container-extract birth through the intent door above. The open and closed state is a keyed-device channel like a
door's ([devices.md](devices.md)). The full break-and-spill behaviour, and the single-slot verbs, are reverse-engineered
and designed and not built.

### A record that changes in place

A prop's save record crosses on every birth path and on no other, so a prop whose record changes
while it sits there tells no one. Two classes do that under a player's hand: the drive box and
the tape reel case. Each has a lid and its contents in the record, taking the lid off spawns it
in the presser's hands, and putting a drive or a reel in destroys the held one and stores it in
the record. The lane watches each class's own `upd()`, which every mutation ends in, and
republishes the record: the host broadcasts it, a client's goes to the host as an intent the
host re-publishes. The far side applies it through the prop's `loadData`, whose `upd()` redraws
the reel case; the drive box's look follows its name, which a record does not carry, so the
receiver re-derives it from `opened` (`coop/props/prop_record_refresh`).

### Props that change on their own

A joiner loads the host's save, so every keyed world prop is a real save-loaded actor with its
full Blueprint brain ticking on every peer, and no wire carries arbitrary per-prop scalars. A
prop whose brain advances a local accumulator (concrete drying, a wall curing, food rotting, a
plant growing) therefore simulates on each peer independently and drifts; a prop whose change
is derived from the shared clock does not. The rule is that the host owns autonomous
progression, in one of three shapes chosen per prop: park the brain and let the host author the
progression through the existing lanes (the pile shape); a host corrector that re-snaps a
deterministic, clamped accrual when the game keeps re-enabling the tick (the tape reel, at one
hertz); or an anchor, a start stamp every peer computes from, valid only while the rate is
constant ([architecture.md](architecture.md), where the authority is going). The reel corrector
is built; concrete and food are designed and not built.

### Deployables

The tools that leave a persistent actor behind -- the grappling hook and rope, which are built,
and the nail gun, the wall builder, explosives, the fishing rod and the physgun, which are not --
have a page of their own, [deployables.md](deployables.md). A prop a hook drags rides the
driven-prop channel above.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a prop at rest: existence, transform, physics | the host | the birth broadcast; a client's births are intents |
| a held prop | the holder | a per-frame stream under a generation the grab mints; the release carries the velocity and the frozen and sleep flags, and closes the generation |
| a thrown prop after release | each peer's physics, from the same velocity | no stream in flight |
| a prop a hook drags, and one still sliding after the hook lets go | the host | a per-tick stream while it moves; the end edge hands the velocity back |
| a prop's key | the game, once; the host's copy wins on a mirror | never rewritten on the owner |
| container contents | the host | slices of its object array; an extraction is an intent |
| a stuck prop | the sticking peer commits; every peer runs the native stick | one reliable message |
| a self-changing prop's progression | the host | park, corrector or anchor, per prop |
| a client's shared-world spawners | parked | shared-world content arrives only from the host |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PropPose` (stream) | the holder to all | the held prop's world transform, per frame, and its hold's generation |
| `PropSpawn`, `PropDestroy` | the host to all; a destroy from either role | class, key, id, transform, physics flags, the birth scalar; the key and id |
| `PropRelease` | the holder to all | the inherited linear and angular velocity, the prop's frozen and sleep at the edge, and the hold it closes |
| `PropDrivePose` (stream) | the host to all | the poses of the props under a hook's drive or a broom's push that moved since their last one, with the claim generation |
| `PropDriveEnd` | the host to all | the final pose and velocity of a driven prop that rested or that a hand took; closes its generation |
| `PropDropIntent`, `ReelEjectIntent` | a client to the host | a place, or an unavoidable birth, for the host to author |
| `PropStickState` | the sticking peer to all | frozen or static, the commit pose, and the hold the stick ended |
| `PropSnapPos` | the host to one joiner | a position correction for a save-authoritative prop moved in the join window, with a keyed prop's frozen and sleep |
| `ContainerState`, `ContainerContents` | the presser; the host | open or closed; one slice of the host's object array |
| `InventoryPickup` | each peer, relayed | the pocket blip, so others hear a pickup |

## Late join

The join page owns the mechanism: explicit deletes for the props the joiner's save had and the
host's world no longer has, then the snapshot bracket with one spawn per live keyed prop
(adopted by key, created when missing, transform converged), then position corrections for what
the host moved during the window, then the membership sweep that removes the locals the host
never claimed. A keyed prop's frozen and sleep converge with its snapshot row and with its
correction, so an extinguisher the host took off its mount during the window is loose on the joiner
too. Container slices are sent per live container at the ready edge. A stuck prop
reaches a joiner through the save, which carries the frozen state. A prop held by someone at
the moment of the join is resolved on its first streamed frame, and a prop under a hook's drive
parks on its first one the same way. A pose of a hold that ended before the joiner's world came up
can still drive the loading world's copy until the stream-stop release; the snapshot converges the
prop after it. At the joiner's ready edge the host re-sends every driven
prop's pose, the resting ones included, which the delta gate would otherwise never send it; an end
edge that reaches a joiner before the prop it names is kept until the prop resolves.

## Known limits

| Limit | Evidence |
|---|---|
| Two peers grabbing the same prop both stream it; nothing assigns the prop to one holder, so receivers follow whichever stream is newest | `[V]` `coop/props/remote_prop` has no claim |
| A prop let go of lands by each peer's own physics from the same release velocity, and nothing converges where it comes to rest: a fire extinguisher dropped from the hand, which rolls, ended between 0.2 and 101.5 cm apart on the two peers over six runs, the widest with the client's copy on a ledge 95 cm above the floor the host's copy fell to | `[V]` the fire extinguisher drill (`coop/dev/fireext_drill`), each peer's last watch line |
| A prop whose own grab does more than wake and unfreeze it gets only the wake and unfreeze on a receiver: the sprinkler's grab also detaches its hose, so the hose stays tied to a sprinkler another peer carries away | `[V]` the corpus index: `prop_sprinkler_C`'s ubergraph calls `prop_C::playerGrabbed_pre` and `hose->detachSprinkler`; `coop/props/remote_prop` runs `awakeUnfreeze` alone |
| The plasma TV sticks to a wall through the wall-attachable component without the wall-attachable class, so its stick is dropped on a receiver and a stream for a stuck one takes the static refusal instead of the unstick | `[V]` the corpus: `prop_tv_plasma_C` carries `comp_wallAttachable` and descends `prop_tv2_C`, `prop_corded_C`, `prop_C`; `coop/props/prop_stick_sync` tests the class lineage |
| A prop the toolgun freezes, unfreezes or makes static changes on the tool user's machine only; the next hold of it converges frozen and sleep, never static, and a copy static on one peer is not driven there while another peer holds the prop -- a refusal that lasts until a lane carries static | `[V]` no lane under `src/coop` catches `setPropProps`; `coop/props/remote_prop` refuses a stream for a copy static here |
| Another peer's hold can carry away the chair this peer sits in at the laptop, which single player never allows: the laptop freezes the seat only on the machine whose player sits | `[V]` the laptop Blueprint: `stabilizeSeat` sets the seat frozen, and the exit's `setPropProps` clears it; `coop/props/remote_prop` unfreezes a held copy at the hold's first pose |
| A keyed prop a client creates outside the intent door (a place after a pickup, the whitelisted births, a container extract, a player's own spawn verb) never reaches the host. It is logged now: the drain names its exit, and `[dev] prop_birth_key_probe` tallies every exit by name | `[V]` `coop/props/prop_drop_intent` drops it at the drain; `coop/dev/prop_birth_key_probe` |
| Concrete, food and every other local-accumulator prop drift between peers; only the tape reel has its corrector | `[V]` `coop/interactables/tape_caddy_sync` is the only corrector |
| A prop tied by any hook -- a player's, an anchored one, the level's own -- is parked on every client for as long as the tie holds, since the host streams it, so it cannot be grabbed there until the hook lets go | `[V]` `coop/items/hook_prop_claim` claims every tied prop on the host every pass; `coop/props/prop_drive_stream` parks it |
| A prop the driven-prop channel set coasting can come to rest a few centimetres from the host's copy: the host ends a coasting stream half a second after the prop stops moving, before its body sleeps, and the end edge sets the client's copy to the host's pose and hands it back its physics, so each copy settles on its own. The broom drill measured 0 to 6.4 cm, and 12.1 cm for a prop whose support another prop knocked away on the host | `[V]` `coop/props/prop_drive_host` (`kRestMs`), `coop/props/prop_drive_stream` (the end edge restores simulation per `coop/props/prop_wire_parity`); the broom drill's push and dispense phases |
| A prop that a pushed prop knocks moves on the host only: the driven-prop channel is fed by verbs, and a knock is none. One drill run measured a knocked prop moving 25.1 cm on the host and resting 15.9 cm from the client's copy | `[V]` the broom drill's push phases (`harness/autotest/autotest_broomstroke.cpp`); `coop/items/broom_push` streams only what a stroke itself pushed |
| A prop parked under the host's drive cannot be grabbed on a client while the drag lasts: the park turns the body kinematic and the game's grab needs a simulating one. It is grabbable again after the end edge | `[V]` `coop/props/prop_drive_stream` parks with `DriveSimulate(mesh, false)`; `coop/props/prop_wire_parity` records why a kinematic mirror is ungrabbable |
| A pose can arrive milliseconds before the spawn that names its prop; that is a race, not a defect, and a ledger tells the two apart instead of warning per packet | `[V]` `coop/props/unresolved_pose_ledger` |

## Code map

| Concept | Files |
|---|---|
| the birth and death seams | `coop/props/prop_lifecycle`, `coop/props/host_spawn_watcher`, `coop/props/prop_element_tracker`, `coop/props/prop_synth_key`, `coop/props/registry_reaper`, `coop/props/prop_echo_suppress` |
| the receivers | `coop/props/remote_prop` (held), `coop/props/remote_prop_spawn` (birth: adopt, converge, create), `coop/props/prop_fresh_spawn` (the materialiser), `coop/props/prop_wire_parity`, `coop/props/active_drive`, `coop/props/prop_sound` |
| a client's intents | `coop/props/prop_drop_intent` |
| the stick | `coop/props/prop_stick_sync` |
| a prop under a hook's drive or a broom's push | `coop/props/prop_drive_host` (the host's set and stream), `coop/props/prop_drive_stream` (the receiver), `coop/items/hook_prop_claim` (the hook lane feeding it), `coop/items/hook_constraint` (the tie itself, host-only), `coop/items/broom_push` (the broom's push feeding it) |
| containers | `coop/props/container_contents_sync`, `coop/items/save_record_wire`, `coop/interactables/interactable_sync` |
| a record that changes in place | `coop/props/prop_record_refresh` (the drive box, the reel case) |
| the pocket blip | `coop/items/inventory_pickup_sync` |
| the join | `coop/props/prop_snapshot`, `coop/props/snapshot_census`, `coop/props/join_membership_sweep`, `coop/props/unresolved_pose_ledger` |
| the spawners a client must not run | `coop/world/spawn_authority` |
| tests | `harness/autotest/autotest_grab.cpp` |
