# Props

## Purpose

Everything a player can pick up, carry, throw, drop, stick to a wall, put in a pocket or take out
of a container: how it is named across peers, where the mod catches its birth and death, who
owns it at rest and in a hand, and what happens to a prop whose own Blueprint keeps changing
it. Trash piles and clumps are a family of their own on [piles.md](piles.md); the item in a
player's hand is on [players.md](players.md); the tools that leave a persistent actor behind, the
grappling hook and rope among them, are on [deployables.md](deployables.md).

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
| was born by a spawner (mushrooms, pinecones, forage) | the prop's initialisation on the host; a client's shared-world spawners are refused (`coop/world/spawn_authority`) and the host mirrors what its own produce (`coop/props/host_spawn_watcher`) |
| was spawned by the spawn menu or extracted from a container on the host | the engine's finish-spawning call, because those births run their initialisation inside the Blueprint and that call is where the actor is finished |
| was destroyed | the engine's destroy call on either role: eaten, broken, picked up into a pocket (`coop/props/prop_lifecycle`); a prop that vanishes inside a Blueprint (the truck, culling, a lifespan) is caught by the host's death-watch and destroyed by id on every peer (`coop/props/registry_reaper`) |

A birth message carries the prop's class, key, id, transform and physics flags. For a class that
keeps save state of its own -- a tape reel's progress, a floppy disc's files -- a second message
follows it on the same lane with the prop's whole save record, the bytes the prop's own `getData`
produces, addressed by key (`coop/props/prop_save_data`). Which classes those are is not a list we
keep: it is whether the class declares a `getData` of its own, read from the live class chain, so a
save-backed class the game adds is carried without a change here. A record that arrives for a prop
this peer has not created yet waits under its key until that prop appears, because a key survives
the destroy-and-recreate that made the record need to travel in the first place. A newer record
for the same key replaces the waiting one whether it applies at once or waits too, so an older
record never lands on top of a newer one.

On the host, a prop born during play -- a device's eject, a drop, a place -- gets its record from
the finish-spawning drain a tick after the birth, because the Blueprint that spawned it fills its
state in after the finish call returns: a disc's eject loads it from the slot's JSON there. The
birth's own initialisation is too early for a record, and without the drain a disc the host
ejected reached every client blank. The drain logs its traffic once a minute
(`HOST BIRTH RECORDS`).

Point-sack redemption is a client intent: the host authorizes reach, destroys its copy, and pays
only once that incarnation has stopped being live (`coop/items/point_sack_intent`). A successful dispatch with the sack still live pays
nothing. A joining client's action is also intercepted: a refused send leaves the sack intact
for another press after joining. `[?]` Concurrent redemption needs a two-peer test; a consumed sack whose payment fails
has no automatic recovery.

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

While a prop is held, its holder owns it. The holder streams the held prop's world transform every
frame, unreliable and newest-wins, under a generation it mints at the grab
(`coop/player/local_streams`). A receiver resolves the prop by key, turns its physics off, and
follows the stream with a fixed-delay snapshot interpolation that renders one interval behind the
newest pose on its own clock (`coop/props/remote_prop`, `coop/props/active_drive`). A grab changes
the prop as well as moving it, on the grabbing machine only: the base prop's grab prelude clears its
lifespan, broadcasts that it was touched and wakes and unfreezes it, and a prop carrying the
wall-attach component runs the component's unstick instead. So the first pose of a hold runs the
same prelude on the receiver's copy -- a fire extinguisher on its wall mount, a drive in a slot, a
prop the toolgun froze or a save left asleep, a spawner's prop on a lifespan -- the unstick with the
tool, since the holder had the prop off the wall. The release closes the hold, as do a stick and a
slot that takes the prop from the hand, so a pose still in flight from an ended hold starts nothing:
MTA's sync time context, minted here by the holder rather than assigned. A driven copy the game here
switches back to simulating, as a laptop exit does to a chair another peer carries, is re-latched
kinematic. A copy static here with no unstick is not driven, since the game cannot hold a body that
does not simulate; the refusal is logged once per hold. On release, a reliable message carries the
prop's transform, its frozen and sleep and its linear and angular velocity at that instant -- the
game's throw is not an impulse but the tracking velocity the physics engine accumulated while the
player flicked the camera. The receiver's copy takes the flags and lets go from the holder's
transform, so lost last poses still end where the holder's did, and gets the velocity back unless it
ended frozen in a slot or a mount, where it stays; a stuck wall-attachable keeps the place its stick
gave it. A release for a prop something else moves now -- this peer's own grab, another hold, the
host's driven-prop channel -- changes nothing but its own hold. Above a launch speed the receiver
also calls the prop's own thrown event, so the sound and the trail come from the engine. A stream
that stops for half a second, or a holder that switches to another prop, is an implicit release.
Grab and throw sounds the game plays only for the local player are synthesised on the receiver at
the prop's position (`coop/props/prop_sound`), and the pocket blip is relayed
(`coop/items/inventory_pickup_sync`).

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
streams open after the tick's spawns are named, so each has its id by then. The third feeder is
the host's own births: a prop it spawns simulating -- a spawn-menu barrel in mid-air, a forage
drop, a prop built from a client's intent -- coasts from its birth until it rests, since every
peer would otherwise simulate its own fall and the copies came to rest apart; at most 32 props
are coasting at once, and a burst past that falls locally (`coop/props/host_spawn_watcher`). A
prop that a pushed prop knocks has no verb of its own and is not streamed. A receiver parks the prop on the first pose
and follows the stream the way a carried trash clump is followed -- the fixed-delay interpolation,
frozen at the last pose across a gap -- where a held prop snaps to each pose; a reliable end edge
carries the final pose, the host's physics flags and the velocity once the prop has rested or a hand
has taken it, and closes the stream's generation so a pose still in flight cannot park the prop
again (`coop/props/prop_drive_stream`). A hand always wins: a claimed prop somebody grabs leaves the
set and rides the held-prop stream, and a receiver yields at once to its own player's grab, which
no stream from the host knows of yet, handing back the physics its park took.

A mirror's physics and collision are set to what the game's own initialisation would have
produced on this peer (`coop/props/prop_wire_parity`); a fresh mirror starts kinematic while it
is remote-owned. A spawn or destroy the receiver applied is marked so the symmetric observer
does not broadcast it back (`coop/props/prop_echo_suppress`).

### Sticking to a wall

A prop sticks through its wall-attach component, which the wall-attachable props (the cameras, the
signs) and the plasma TV carry. The component commits its stick inside the Blueprint; the one
visible seam is the commit entry, reached through a latent delay that the engine resumes through the
function dispatch. The sticking peer broadcasts the frozen or static state and the commit pose
before its own hold breaks, on the same ordered lane as the release so the order is structural; the
receiver re-poses the prop and dispatches the component's own force-stick, so the attach, the effect
and the destroy binding are the game's (`coop/props/prop_stick_sync`). The unstick crosses too,
since a crowbar's pry frees a pryable sign with nobody holding it: the script gate watches the
component's unstick on every route, the unsticking peer broadcasts the prop's pose and velocity, and
a receiver runs the same unstick with the tool and lets its copy go.

### Containers

A container's contents are not on the container: every container reads them from one global
per-peer array in the save object, addressed by an index the container holds, and every mutating
verb runs inside the Blueprint, below the two dispatch seams a hook can intercept. The
script-body gate does see it -- that is how this lane learns a container changed -- but it runs on
the presser's own machine, and no local body can wait for an answer from across the network. So
the peer whose verb fired authors the contents and the host arbitrates them (`coop/props/container_contents_sync`): one slice of its
array per live container is sent at a joiner's ready edge and on change, a client's slice is
accepted only if its author could have reached that container, has not sent more in the last
second than an honest client produces, and edited the contents the container holds now, and an
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

### Notebook

A notebook's pages are `text` on the prop, which its own `getData` and `loadData` save, so the
text is the prop's save record and crosses on the record lane above with no message of its own.
`coop/interactables/notebook_sync` adds what that lane cannot know, the editor.

- **One editor at a time.** The open editor claims the notebook, a claim keyed by the prop
  (`coop/interactables/device_occupancy`); a second peer pressing E on it is denied.
- **What the editor wrote.** The editing peer publishes the notebook's record when it closes the
  editor and, while it holds the claim, at the editor's own commit (focus leaving the box, or the
  game's commit) when `text` differs from the last text published or applied, at most once every
  2 s; a change the gate held back goes with the next commit or the close. Live text crosses at a
  commit and at the close, not per keystroke: the widget writes `text` only then, and the lane reads
  that array, never the box. The host's own edit publishes the canonical the same way.
- **Who the host believes.** The host takes a client's record for a notebook from the slot that
  holds its claim, or from any slot while nobody holds it: a free notebook is anyone's to change,
  which a torn page added to it is. A record from another slot while a peer holds it is refused
  and logged. The rule is judged where the record lands, so a record that parked first (the
  host did not index the notebook yet) is judged by it too, and is republished to the other
  peers only when it lands, never at arrival. It is registered with
  `coop/props/prop_record_gates`; the record lane names no class.
- **The close reaches the host while its sender holds.** A client keeps the claim after it closes
  the editor until the host's
  republish of its last publish comes back (the lane's acknowledgement: the canonical record for
  that notebook's Key arriving after its publish), and lets go anyway after 10 s, logged. Opening
  an editor while that wait is pending ends it: the old claim goes, then the new open is handled.
  A host's own close needs no wait: it is the canonical.
- **No echo onto the editor.** A peer that holds the claim and has published the notebook in this
  claim applies no record for it, whether it arrives live or from the park: its own edit is the
  newer state. Before its first publish it has nothing newer to protect, so the previous editor's
  close, which the host sent before the grant and the grant overtook, is applied, and the open
  editor's page is reloaded from the notebook's `text` through the widget's own page load
  (`init`), so the box shows what was applied and the lane measures later edits from it. Text
  typed in the box and not yet committed at that instant is dropped by the reload; the window is
  the round trip after the grant. If the reload does not dispatch, a warning says so once. The
  mark, the wait and the baseline reset when the claim ends, by whatever route: a release, a lost
  race, the world going down, the session ending.
- **A page added outside the editor.** Using a torn sheet on a notebook (`playerUsedOn`) appends the
  sheet's text to the notebook's and destroys the sheet, with no editor open and no claim. The
  verb's end publishes the notebook's record when its page count grew, from whichever peer ran it;
  the host's rule takes it while nobody holds the notebook. While another peer has the notebook open
  the verb is cancelled at its start on the peer that ran it, when the hand holds a torn sheet: the
  sheet stays in the player's hand and a notice names the holder, because the game cannot merge two
  writers and the open editor's close would overwrite the page on every peer. Any other use of a
  held notebook runs as the game has it. One window remains: a page added before this peer has heard
  the other peer's claim (about one round trip after that peer opened the editor) stays on this peer
  and is overwritten at that holder's close. The root fix, the host running the page add on behalf of
  the peer that pressed it, is a recorded follow-up and not built.
- **Forced exits.** Ragdoll, a faint and death leave the interface without `quit`, as in the
  single-player game, so the box's uncommitted text is lost there and nothing is published.
- **Late join.** A joiner is handed every prop's record in the snapshot, the notebook's last
  published text included, and the claim table, so it reads the text as last published and is
  denied the editor while another peer holds it; the close then reaches it as an ordinary record.
- **The ceiling.** A record over 32 KiB is refused with a log line and stays on the editing peer.

A page torn out of a notebook is `prop_notebook_paper_C`, born by a client's prop birth route
(`coop/props/prop_drop_intent`). The drawn pixels of `prop_paper_drawable_C` are in no save
record and do not cross; that paper has a claim only. `[RD]` for the Blueprints, `[?]` until the
notebook drill has run.

### Sleeping bags and mattresses

`prop_sleepingbag_C` exposes `actionOptionIndex` and has an `unwrapOnBed` field. Its generated
Blueprint graph carries a `FinishSpawningActor` result typed as `sleepingbagWrap_C`. The wrapped
form derives from `bed_C`, which derives from `Aprop_C`; it exposes `fold()`, whose graph carries a
`FinishSpawningActor` result typed as `prop_sleepingbag_C`. The blue, brown, Stolas and mattress
forms are subclasses of these two base classes. `[RD]`

The deployed Blueprint form finishes with `Key=None`; after `FinishSpawningActor` the host assigns
it a random prop key (the persisted kind) and retries the ordinary lifecycle express. The client gates the
`actionOptionIndex` press of the packed bag (action 4, the unroll) and of the deployed bag (action
7, whose graph calls `fold()`); it watches the press and not `fold()` itself, which the deployed
bag's own `BeginPlay` also runs when its foundation traces fail and which no player pressed. It
sends the source prop's host element id and the action byte, and cancels its local Blueprint
verb once the host was asked; a press the client cannot send (the source has no host prop id, or the
send fails) runs on its own copy as it would with no lane. The host checks that the sender is in
reach, that no other peer holds the prop, and that the named prop is still in the expected form
with the action byte of that form, then invokes that same Blueprint verb on its copy. The Blueprint
owns the replacement birth and source destruction; the host spawn watcher and prop lifecycle carry
those edges to every peer. The joiner's host save and ordinary prop snapshot seed whichever form is
live. The client does not choose the replacement class or transform. `[RD]`
`coop/interactables/sleepingbag_sync`, `ue_wrap/actors/sleepingbag`, `coop/props/host_spawn_watcher`,
`coop/props/prop_lifecycle`. Whether a deployed bag already in the host's world before the session
(loaded from a save) carries a key is not measured; one without a key gets no host prop id and its
press takes the local path above. `[?]`

Unfolding is relayed as the interaction action byte so the Blueprint retains its own placement and
mattress branches. The host runs it with the sender's remote-player puppet, preserving the game's
player-relative checks. The intent is refused if the source lacks a host prop id, is out of reach,
or has already changed form. `[RD]` the packed Blueprint graph uses two sphere traces, checks the
target bed, spawns the wrapped actor and destroys its source; the wrapped graph's `fold()` spawns a
packed actor and destroys its source. `[V]` sleepingbag CFG and generated SDK parameters.

A mattress lying on a bunk bed (`bed_bunkMattress_C`) is taken off by its action 7, which spawns the
packed mattress and destroys the source the same way; that press rides the same intent with the
mattress's host element id and the action byte, and the host accepts only that class and action. The
packed mattress is a `prop_sleepingbag_C` and keys itself like any prop, so the ordinary prop
lifecycle carries it. `[V]` `bed_bunkMattress` and `prop_sleepingbag_mattress` Blueprint decompiles.

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

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a prop at rest: existence, transform, physics | the host | the birth broadcast; a client's births are intents |
| a held prop | the holder | a per-frame stream under a generation the grab mints; the release carries the transform, the velocity and the frozen and sleep flags, and closes the generation |
| a thrown prop after release | each peer's physics, from the same velocity | no stream in flight |
| a prop a hook drags, and one still sliding after the hook lets go | the host | a per-tick stream while it moves; the end edge hands the velocity back |
| a prop's key | the game, once; the host's copy wins on a mirror | never rewritten on the owner |
| container contents | the host | slices of its object array; an extraction is an intent |
| a stuck prop, and its unstick | the peer that sticks or frees it; every peer runs the component's own verb | one reliable message |
| a self-changing prop's progression | the host | park, corrector or anchor, per prop |
| notebook editor occupancy | the host arbitrates | a claim keyed by the owning prop |
| notebook page text | the editing peer; the host takes it from the claim's holder, or from anyone while it is free | the notebook's own save record on the record lane, at the editor's commit and at the close |
| a client's shared-world spawners | the host | refused on a client at the script-body gate; shared-world content arrives only from the host |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PropPose` (stream) | the holder to all | the held prop's world transform, per frame, and its hold's generation |
| `PropSpawn`, `PropDestroy` | the host to all; a destroy from either role | class, key, id, transform, physics flags, the birth scalar; the key and id |
| `PropRelease` | the holder to all | the prop's transform, its inherited linear and angular velocity and its frozen and sleep at the edge, and the hold it closes |
| `PropDrivePose` (stream) | the host to all; a syncing client to the host | the poses of the props under a hook's drive or a broom's push that moved since their last one, with the claim generation; a client's rows of the prop its own interaction moves, which the host judges before it relays one `[?]` |
| `PropDriveEnd` | the host to all; a syncing client to the host | the final pose and velocity of a driven prop that rested or that a hand took; closes its generation; a client's End is admitted when it is the row's syncer `[?]` |
| `PropSyncClaim`, `PropSyncAnswer` | a client to the host; the host to that client | a client's request to sync a prop its interaction moves, and the host's grant, refusal (with the reason) or revoke (with the generation) `[?]` |
| `PropDropIntent`, `ReelEjectIntent` | a client to the host | a place, or an unavoidable birth, for the host to author |
| `PropStickState` | the peer that sticks or frees it, to all | frozen or static, the commit pose and the hold the stick ended; or an unstick, with the pose and velocity |
| `PropSnapPos` | the host to one joiner | a position correction for a save-authoritative prop moved in the join window, with a keyed prop's frozen and sleep; it carries the host's drive context for the prop (claim generation and open), the client drops one older than a drive END still on its copy, and a prop a hand held after its correction went out gets a follow-up after the release `[?]` |
| `ContainerState`, `ContainerContents` | the presser; the host | open or closed; one slice of the host's object array |
| `InventoryPickup` | each peer, relayed | the pocket blip, so others hear a pickup |

## Late join

The join page owns the mechanism: explicit deletes for the props the joiner's save had that are
gone from the host's world, then the snapshot bracket with one spawn per live keyed prop (adopted by
key, created when missing, transform converged), then position corrections for what the host moved
during the window, then the membership sweep that removes the locals the host never claimed. A keyed
prop's frozen and sleep converge with its snapshot row and its correction, and a flag change without
a move gets a correction too, so an extinguisher taken off its mount during the window is loose on
the joiner; a wall-attach component's owner converges its stuck state there through the component's
own unstick or force-stick, so a sign pried in the window is off its wall. A stuck prop otherwise
reaches a joiner through the save, which carries the frozen and static state. Container slices are
sent per live container at the ready edge. A joiner gets no held-prop or driven-prop pose until its
world is ready, since the release and the end edge that close them are not sent to a world that is
not ready either: a prop held at the join resolves on the first pose after it, and at the ready edge
the host re-sends every driven prop's pose, the resting ones included, which the delta gate would
otherwise never send. An end edge that reaches a joiner before the prop it names is kept until the
prop resolves.

## Known limits

| Limit | Evidence |
|---|---|
| Two peers grabbing the same prop both stream it; nothing assigns the prop to one holder, so a receiver's two drives both write it each tick, the higher slot last. For the same reason a release only knows its own holder's holds: one delayed past another peer's whole hold of the prop puts it back where the first holder let go. And a join's snapshot row read before a live stick, unstick or release can reach the joiner after it and undo it there, until the window's late re-flush corrects the prop | `[V]` `coop/props/remote_prop` has no claim and orders holds per holder, where MTA's server keeps one syncer per unoccupied vehicle and relays only that syncer's packets; `[RD]` the rows carry no version; the join gate holds a live stick or release behind the snapshot rows, so a joiner applies the rows first `[?]` |
| A prop let go of lands by each peer's own physics from the same release transform and velocity, and nothing converges where it comes to rest: a fire extinguisher dropped from the hand ended between 0.2 and 152.6 cm apart on the two peers over fourteen drops, the widest where the client's copy stayed on a surface about a metre up that the host's copy landed on and rolled off | `[V]` the fire extinguisher drill (`coop/dev/fireext_drill`): the host's rest line (its last watch line where its copy had not settled) against the client's last watch line, which prints a move of 3 cm or more |
| A prop whose own grab prelude adds to the base one gets only the base one on a receiver: the sprinkler's also detaches its hose, so the hose stays tied to a sprinkler another peer carries away. The glowstick's replaces the base one with its own release, and a receiver runs the base one | `[V]` the sprinkler and glowstick Blueprints: `prop_sprinkler_C`'s prelude calls `prop_C::playerGrabbed_pre` and `hose->detachSprinkler`, `prop_glowstick_C`'s calls `releaseGlowstick` alone; `coop/props/remote_prop` runs the base prelude |
| A prop the toolgun freezes, unfreezes or makes static changes on the tool user's machine only; the next hold of it converges frozen and sleep, never static, and a copy static on one peer, without a wall-attach component's unstick, is not driven there while another peer holds the prop -- a refusal that lasts until a lane carries static | `[V]` no lane under `src/coop` catches `setPropProps`; `coop/props/remote_prop` refuses a stream for a copy static here |
| Another peer's hold can carry away the chair this peer sits in at the laptop, which single player never allows: the laptop freezes the seat only on the machine whose player sits | `[V]` the laptop Blueprint: `stabilizeSeat` sets the seat frozen, and the exit's `setPropProps` clears it; `coop/props/remote_prop` unfreezes a held copy at the hold's first pose |
| A keyed prop a client creates outside the intent door (a place after a pickup, the whitelisted births, a container extract, a player's own spawn verb) never reaches the host. It is logged now: the drain names its exit, and `[dev] prop_birth_key_probe` tallies every exit by name | `[V]` `coop/props/prop_drop_intent` drops it at the drain; `coop/dev/prop_birth_key_probe` |
| Concrete, food and every other local-accumulator prop drift between peers; only the tape reel has its corrector | `[V]` `coop/interactables/tape_caddy_sync` is the only corrector |
| A prop tied by any hook -- a player's, an anchored one, the level's own -- is parked on every client for as long as the tie holds, since the host streams it, so it cannot be grabbed there until the hook lets go. A joiner parks one at rest on the host's ready-edge re-send, a single unreliable datagram: lost, it leaves that prop unparked there until it moves | `[V]` `coop/items/hook_prop_claim` claims every tied prop on the host every pass; `coop/props/prop_drive_stream` parks it; `coop/props/prop_drive_host` re-sends at the ready edge |
| A wall-attachable whose stick replay finds no surface here is frozen in place without the attach, so when the thing it stuck to is destroyed it stays in the air here while every other copy falls | `[V]` `coop/props/prop_stick_sync`'s fallback writes the flag without the component's attach or its destroy binding; `[RD]` the stick carries no parent |
| A prop the driven-prop channel set coasting can come to rest a few centimetres from the host's copy: the host ends a coasting stream half a second after the prop stops moving, before its body sleeps, and the end edge sets the client's copy to the host's pose and hands it back its physics, so each copy settles on its own. The broom drill measured 0 to 6.4 cm, and 12.1 cm for a prop whose support another prop knocked away on the host | `[V]` `coop/props/prop_drive_host` (`kRestMs`), `coop/props/prop_drive_stream` (the end edge restores simulation per `coop/props/prop_wire_parity`); the broom drill's push and dispense phases |
| A prop that a pushed prop knocks moves on the host only: the driven-prop channel is fed by verbs, and a knock is none. One drill run measured a knocked prop moving 25.1 cm on the host and resting 15.9 cm from the client's copy | `[V]` the broom drill's push phases (`harness/autotest/autotest_broomstroke.cpp`); `coop/items/broom_push` streams only what a stroke itself pushed |
| A prop parked under the host's drive cannot be grabbed on a client while the drag lasts: the park turns the body kinematic and the game's grab needs a simulating one. It is grabbable again after the end edge | `[V]` `coop/props/prop_drive_stream` parks by switching the mesh's simulation off (`ue_wrap/engine/engine_physics`); `coop/props/prop_wire_parity` records why a kinematic mirror is ungrabbable |
| A pose can arrive milliseconds before the spawn that names its prop; that is a race, not a defect, and a ledger tells the two apart instead of warning per packet | `[V]` `coop/props/unresolved_pose_ledger` |

## Code map

| Concept | Files |
|---|---|
| sleeping bag and mattress replacement | `coop/interactables/sleepingbag_sync`, `ue_wrap/actors/sleepingbag`, `coop/props/host_spawn_watcher`, `coop/props/prop_lifecycle` |
| the birth and death seams | `coop/props/prop_lifecycle`, `coop/props/host_spawn_watcher`, `coop/props/prop_element_tracker`, `coop/props/prop_synth_key`, `coop/props/registry_reaper`, `coop/props/prop_echo_suppress` |
| the receivers | `coop/props/remote_prop` (held), `coop/props/remote_prop_spawn` (birth: adopt, converge, create), `coop/props/prop_fresh_spawn` (the materialiser), `coop/props/prop_wire_parity`, `coop/props/active_drive`, `coop/props/prop_sound` |
| a client's intents | `coop/props/prop_drop_intent` |
| the stick | `coop/props/prop_stick_sync` |
| a prop under a hook's drive or a broom's push | `coop/props/prop_drive_host` (the host's set and stream), `coop/props/prop_drive_stream` (the receiver), `coop/items/hook_prop_claim` (the hook lane feeding it), `coop/items/hook_constraint` (the tie itself, host-only), `coop/items/broom_push` (the broom's push feeding it) |
| containers | `coop/props/container_contents_sync`, `coop/items/save_record_wire`, `coop/interactables/interactable_sync` |
| a record that changes in place | `coop/props/prop_record_refresh` (the drive box, the reel case) |
| notebook sync | `coop/interactables/notebook_sync`, `coop/props/prop_save_data`, `ue_wrap/actors/notebook` |
| the pocket blip | `coop/items/inventory_pickup_sync` |
| the join | `coop/props/prop_snapshot`, `coop/props/snapshot_census`, `coop/props/join_membership_sweep`, `coop/props/unresolved_pose_ledger` |
| the spawners a client must not run | `coop/world/spawn_authority` |
| tests | `harness/autotest/autotest_grab.cpp` |
