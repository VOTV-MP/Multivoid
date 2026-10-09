# Devices and the economy

## Purpose

The keyed things around the base that are not the workstation: doors and their keypads, the
light switches and the light groups, the garage, the appliances, the lockers, the power panel,
the wind turbine, the windows and the grime, the delivery drone, and the economy the drone
serves: the shared balance, the shop orders, and the coin gun. Who owns each, how a peer's press
reaches the others, and where the group's money can still be lost.

## How it works

### One engine, one adapter per device

Every open-or-closed, on-or-off device rides one replication engine
(`coop/interactables/interactable_channel`, driven by `coop/interactables/interactable_sync`):
a key-to-actor index that heals itself, per-key dedup, a deferred apply with a throttled retry for
an instance that has not streamed in yet, echo suppression, and the connect snapshot. A device
family is an adapter over its engine wrapper (`ue_wrap/devices/`), a few lines each: doors, light
switches, light groups, container lids, the garage, the appliances, the lockers, the oven's repair.

Each channel is sent at the verbs that write its state, which the script-body gate watches: every
live writer of a door's open state goes through `doorOpen` or `doorClose`, and of a light group's
`isActive` through its `runTrigger`, both sent by the host (`coop/interactables/door_state_verbs`,
`coop/interactables/lightgroup_verbs`); a symmetric device is sent by the peer whose verb ran
(`coop/interactables/toggle_verbs`, below), and the host relays a client's edge to the other
clients. After the body the lane sends the state the call left; a send no peer can take (none is
world-ready) counts as made, since each joiner's snapshot at its ready edge carries the state. A
receiver resolves the instance by key and applies it, moving its lane's baseline so nothing
echoes. A write no verb makes, a save's load, reaches a joiner in the connect snapshot; a send refused
while a peer's connection goes is sent again each tick. No channel polls: a dev probe
(`channel_shadow_probe`) polls every indexed instance and logs a change the lane neither sent nor
applied as a `SHADOW MISS`, and sends nothing. A door is sent again where its swing ends
(`move__FinishedFunc`), for a swing that settles otherwise than it began.

The key is the game's own for the save-persisted instances and a portable identity computed
by both peers for the rest (`coop/element/portable_identity`): the game mints a random key per
process for anything the save does not keep, so a child actor is named by its parent plus its
component name and a level-baked actor by its object name. Before that, half the doors, lights
and containers in a world were addressable only by the peer that loaded them.

Two modes. A device that reverts on its own, a door that auto-closes or a light group the game
re-derives, is host-authoritative: the host's copy is the one that moves, and a client renders it.
A client's copy of a door moves only by the host's state: its own `doorOpen` and `doorClose` are
refused unless the door lane's own apply to that door is running them, so its autoclose, a creature
or a trigger on that machine cannot move it alone -- nor a creature mirror that overlaps a door as
our own code moves it. A client's copy of a light group likewise: its `runTrigger` is refused unless
the group lane's own apply is running it, so a switch pressed there, an eventer's flicker or that
machine's own breaker cannot move the group; the switch still flips, and its bit reaches the host
on the switch lane, whose replay of the press is what moves the host's group. The switch lane sends
a switch's `a` at its `use()`, the one writer of `a`, on the peer that ran it
(`coop/interactables/toggle_verbs`), the garage lane a garage's Open at its `runTrigger`, the one
writer of Open past the load, and the appliance lane an appliance's bool at its `actionOptionIndex`
(a faucet's, sink's, shower's, oven's and tape unit's toggle) or a server box's `visual` (the kerfur
Omega's call), the box lane a locker's `opened` at its `open` (its toggle's, a murder kerfur's) and the drone
console's at its `actionOptionIndex`, and the container lane a lid's `opened` at its `open` or `close`
(which its setup, grab, damage, padlock and resting swing, and a cremator's door, call), and the oven
lane an oven's `fixed` at its `fix()`, the repair widget's last step, which goes one way: a receiver
writes it and repaints on 1, `fix()`'s work without its screen, and refuses 0.
A client's own press, hit or pry of a door never runs on its copy either: the
script-body gate refuses the door's entry verb there and sends it to the host
(`coop/interactables/door_verb_intent`), which
runs the same verb on its own copy, so the door's own body decides it once -- its power gate with
the blackout clause, a swing already moving, the pry. A verb that reaches the host before the
sender's body has taken its first pose waits for it, since the host measures the sender's reach
from that body. A pry is run only for a sender whose hand held a crowbar last (the prying crowbar
takes the held one as it goes in), and a hit only with an item whose swing the weapon table allows,
its damage cut to the most that swing deals. The cut is at those entry verbs rather than
at `doorOpen`/`doorClose`: every in-door caller reaches those two through the door's own event
graph, where a press cannot be told from a hit or a trigger, and a hit moves both leaves before it
ever reaches `doorOpen`. The host's door then closes by its own
autoclose, at the first of its five-second checks that finds its sensor list empty, and a client's
puppet counts in that list exactly when the client's own player counts in the client's own copy of
it. A device with no auto-revert (the garage, an appliance, an oven's repair, a locker, a lid) is
symmetric: any peer's edge is the state.

### What is inside a container

A container's contents are not on the container. Every one of them reads from a single global
per-peer array, `saveSlot.GObjStack`, addressed by an index the actor holds alongside a cached
volume. That array reaches a client once, inside the join save-transfer blob, and never again on
its own, because every verb that mutates it dispatches internally to the Blueprint, where neither
hook seam can intercept it and only the script-body gate can watch it
(`docs/coop-dispatch-visibility.md`). Without a lane of its own, a
drone delivery landed full on the host and empty on the client.

The seam is a gate on the Blueprint body of the add and take verbs, and it only marks the
container dirty -- it reads no arguments and takes no action, which is what makes it correct for
every caller. The peer whose verb fired then authors the slice on the next sweep. There is no
request the host could refuse: the gate fires at the body's entry, on the presser's own machine,
and the item has moved there before an answer from anywhere else could arrive. So the host arbitrates instead, and a client's slice has to pass four things: the author
is close enough to have used the container (the game's own reach, widened by the container's size
and by how far a player can move while their position is in flight), it has not sent more slices
in the last second than any honest client can produce, it edited the truth the host last
published, and no host-side change is in flight. The host then relays to every peer except the
author -- echoing a peer's own state back would revert its newer local value. A refused write is
answered by re-publishing the host's truth to that author, and counted; the one exception is the
rate refusal, which answers with nothing, because a limit on how fast someone may make the host
work must not make it work harder. Whatever the host publishes becomes the truth the next write is
judged against, including a client's slice it has just accepted and passed on.

Two things can arrive before they can be judged, and neither is refused for it. A slice whose
container has not spawned on this machine yet waits in a holding pen -- one entry per container,
replayed until it lands, and dropped after thirty seconds; while a join is streaming in, that
clock does not run at all, because contents normally arrive ahead of the props they belong to. And
a slice from a player the host has not yet placed in the world -- the first seconds of a join --
waits in the same pen rather than being turned down, since refusing it would throw away a real
edit at exactly the moment the game is least able to judge it. A client's pen is bounded per
author; the host's own slices are not bounded, the way nothing of the host's is.

Applying a slice raw-writes the receiver's own array slot and then re-derives everything a setter
owns through the game's own verbs -- the volume and mass recalculation and the display-name
rebuild -- rather than writing those fields directly. The add verb cannot be the apply verb: it
takes a live actor and serialises it itself, so it cannot ingest a record off the wire. The
overflow check is not called on apply either: it ejects contents.

Two boundaries, both fail-closed. Personal inventory is not this lane's business even though it
is backed by the same global array, so a container whose component is flagged as a player's -- or
whose flag cannot be resolved at all -- is skipped; writing over another peer's slice would wipe
that player's inventory. And a nested container's own index into that array never travels: it
names a slot in the sender's array and would resolve to an unrelated container on the receiver. A
record whose class descends from the container class carries a sentinel there instead -- written
when a slice is sent and again when one is applied, since enforcing it only on the way out would
trust every sender to be this build -- and the nested container arrives empty rather than broken.

### Keypads and locks

A keypad is a typed buffer and a verdict, and the verdict is also the power it hands on to its pair
and its gated door. The host's copy decides. Every change goes through one of the keypad's own
verbs -- a digit, an open with its verdict, the scripted guesser, the set-new-code mode, a false
entry -- and the script-body gate watches them by name (`coop/interactables/keypad_verbs`): before
each body on the host the verb goes to every client, which runs the same verb on its own copy, so
the keypad's own chain plays its sounds and lands the same state; after it the state the chain
settled on goes too -- at the keypad's setActive, its pair's with it and the password in it; at a
digit or a reset that started no open -- and a joiner gets each keypad's state in its snapshot
(`coop/interactables/keypad_sync`), handed on to its pair and door where the host's already hold
it, so a door ends as the host's even when a chain ran after the host's save was taken. A client's
copy is written to that state whatever its own
replay met, but one that arrives while the copy's own replayed open, on the keypad or its pair, is
still in its 0.2 s wait is written at that chain's end, so it never lands under the tail that
reads the set-new-code mode and writes the pair. On a client every call of those
verbs is refused but the lane's own; a player's own entries -- a digit, the accept or cancel key,
the numpad's (told apart by the key, since the numpad's accept passes the copy's own verdict), a
keycard's swipe, a pass changer -- go to the host as an intent, and the host runs the verb on its
copy, judging a submit against its own password. A digit clicked on the keys needs the light's power
on the presser's own copy, as single player's click does, and a client's copy holds the host's
light power, since a client applies the host's canonical through the panel's own buttonsVisibility,
whose tail runs setPower once the panel is enabled and its generators read whole: a client's clicks
stop in a blackout and come back with the power. The entries run in order, and the next one waits
while the host has no body for the sender and while the keypad's open is in the 0.2 s tail that
clears its buffer, so none is lost to either wait (a full queue still refuses). A keycard's
verdict is taken only while the sender holds a keycard. An accept unlocks a door; opening it is
an ordinary press of the door.

### Power, turbine, windows, grime

The power panel's five latched breakers have one author, the host (`coop/world/power_panel`). A
press on any peer, a lever or the laptop's breaker page, runs as its prediction, and a client's
reaches the host as the breakers it flipped, caught where the panel's own apply runs. The host takes
presses in arrival order, a few a second, from a presser within reach of the lever or of the
terminal it worked the page through (the laptop, or a portable PC); it runs a lever press as the
game's own press by the presser's puppet, tutorial gate and click included, and applies a page
press's flip, whose wait the presser already served. It sends every peer the breakers and the
panel's lockout with the last press it took from each, and a refused press to its author alone. A
client puts its own untaken presses on top and runs the panel's apply only when that differs from
what its panel shows, so each machine's own setPower drives its base: the desk's units, the laptop,
the lights, the servers and the sockets follow the grid everywhere. The desk virus's 60 s lockout
runs on the host alone: a client refuses its own, switches its servers off as the host's lockout
starts, and as it ends switches them on with the calc breaker and plays the turn-on cue. The
generators behind the panel wear on a 30 s decay tick that rolls its own dice; the host runs it,
and a client refuses its own tick, break, wear and fullFix at the body. The host sends every
generator's row after the outermost of its generator verbs and its Activate presses, so a break's
rows follow the blackout canonical it produced; a client runs the same edges from the rows and then
puts its breakers back to the canonical, since a break's own blackout rewrote them
(`coop/world/power_grid`). A client's player acts on a generator as ops: its Activate press and its
upgrade install run on the client first and are reconciled like the panel's presses, and its hit is
the host's to run, with a held item that swings. The host takes an op from a player within reach, in
the player's order, and judges an Activate press on its own copy of the repair puzzle, branching as
the button does: a broken generator is mended, a whole one serviced. An install rests on the upgrade
its insert spent, which the host saw that player destroy beside the generator
(`coop/world/power_upgrade`): an install without one is refused, and one refused after it, the second
of two players at a generator's last free place, gets its upgrade back where the player stands. A repair, the host's of a client's press and a client's of the host's rows alike, runs as
the game's repair with the puzzle solved followed by the Activate route's turn-on at the generator
and its completion trigger. The puzzle crosses in the rows (`coop/world/power_puzzle`): its targets
are the host's rolls, which a client never makes, and its values belong to the one player inside the
panel (the device lock). That player's knob, switch and rotator inputs run on its client first and
reach the host as values, taken in its order, so a press follows the inputs it rests on; every peer
draws the host's puzzle with the panel's own setters and moves, and a joiner gets it with the rows.
The wind turbine's heading integrator is not saved and chases the synced wind at a degree per
second, so the host mirrors six driver floats about once a second and the turbine's own tick
interpolates (`coop/interactables/turbine_sync`). The base window's dirt and the wall grime are
monotone: a wipe only lowers them, so each peer broadcasts a decrease and the receiver keeps the
minimum, and two peers wiping at once converge with no oscillation; the grime decals are keyed by
their quantised world position, since both peers place them from the same save
(`coop/interactables/window_sync`, `coop/interactables/grime_sync`).

The base's bay window is not one of those: its dirt is a 1645x512 render target that a sponge
wipes one dab at a time, so there is no scalar to compare. A stroke reaches the window's own
`cleanPhys` through a script call no hook here carries arguments for, so the dab is observed at
its native draw instead -- a `UCanvas::K2_DrawMaterial` whose calling frame is the window with its
canvas session open, which the window's periodic dirt splotch cannot be, since that refuses to run
while the session is open. The stroking peer sends the dab's pixel, its edge and the brush's
opacity and colour; every other peer draws the same dab through the window's own `Canvas` event
with a brush material the mod owns, and the host relays a client's dab, so the host's save carries
every peer's wipes. Never `setDraw`, `endDraw` or `dirty` from outside: the session belongs to the
window, and an outside `endDraw` deletes the world canvas the game's own next dab draws into
(`coop/interactables/window_stroke_sync`, `ue_wrap/devices/window_canvas`).

### The coordinate towers

Each of the three coordinate towers can break, and a broken one blocks the ping. A tower's state is
its broken flag, eight fuses (empty, good or blown) and a lights-out puzzle, and its panel, on a
platform up the tower's ladder, opens on a montage and carries a lever that runs a timeline. The
tower rolls its dice in one function, `Scramble Radar Dish`, which blows a good fuse or, with none
left, scrambles the puzzle and breaks the tower; three callers reach it -- the decay timer the
generators' saboteur runs, an explosion nearby, and the tower's own load when it was saved broken
-- and the lever's end judges the puzzle. A client refuses both at the script gate on every route,
its world's load included, so the host's world is the towers' only author
(`coop/world/coord_tower_ops`). The host reads its towers each tick and sends every tower's row
whenever one changes (`coop/world/coord_tower_rows`); a client applies each change as the tower's
own graph makes it: the painters for the state, `solvePuzzle` for a repair with its success sound,
`moveLever` for the lever, and its own retract for the panel once its own montage is idle; a press,
a pull, an insert and a failed lever play their own sounds. The lever's end, refused on a client, is
what would clear its moving flag, so a client takes that flag from the row. A client's press of a
puzzle button, the lever or the panel's retract is refused and sent to the host, which runs the
tower's own use with the look-at answered and then gives its own player's look-at back. A pull or an
insert moves a fuse into or out of the player's own hand, so the client's own game makes it and
claims the slot. The host takes a pull when its copy has the panel open, the lever at rest and that
fuse blown, and an insert when the slot is empty and the host saw that player spend a good fuse at
the tower; it refuses one otherwise, answering in the rows, and a refused pull's fuse is taken back
out of the puller's hand, a refused insert's fuse given back where the player stands. Every act is
judged within reach of the part it acts on, in the player's order, at a few a second.

### The drone

The delivery drone is one host-simulated actor: its flight is a fragile per-tick integrator not
worth reproducing, so the host streams its transform while it moves (it glides on after its active
flag drops) and its state the moment it changes, the client suppresses
the drone's own tick and drives the streamed transform through an interpolation window, and the
cargo it drops rides the ordinary prop lanes (`coop/interactables/drone_sync`). The drone's sale
runs on the host only, which is what makes selling into it the one economy path that credits the
group correctly.

The garage console calls and sends it. Its one action option dispatches on what the presser is
looking at -- the keyboard runs the drone's own call-or-send verb, the other face toggles the
drone's leave timer -- and the console holds its drone as a level reference, so every peer's
console points at that peer's own drone. A client's press therefore reached a mirror whose flight
tick is suppressed: nothing moved and nobody heard about it, which is why only the host could work
the button. The client now refuses its own body at the script-body gate and sends the press. The
console carries no identity to name, so the host takes the console of its own world that the sender
stands at, re-tests its own copy of the lid, then runs the same verb, so the flight starts on the
machine that owns it and arrives on the stream that already carries it
(`coop/interactables/drone_call_intent`). A press that reaches the host before it holds the sender's
body, from a joiner who presses at once, waits for the body rather than being refused, as the
door-verb, keypad and container lanes wait. The leave-timer face has no lane and stays local.

### The balance

The host owns the balance. It polls the points field every tick, which catches every writer, and
broadcasts the absolute value on change and to a joiner; a client writes the host's value directly.
The wire is one-way. A client-to-host balance delta once existed with no bound, so any peer could
set the group's money to anything; it was retired whole rather than clamped
(`coop/world/balance_sync`). A client's own earnings are therefore not shared unless a lane
carries them as an intent.

### The upgrades

The eighteen upgrade levels are one struct on the save, so they used to ride only the transferred
save and diverged in silence from the moment a level was bought. The host now polls the struct and
broadcasts it whole on any change, which catches every writer: the panel, the physical racks, the
transformer prop. A client applies what it is sent and repaints the rows it has open.

A purchase is an intent. A client's button is refused at the script-body gate, so it never debits
itself, and the host re-derives the price and the bounds from its own table and its own level,
charges the shared balance and republishes. The rows have no identity of their own to name, so
holding the laptop claim is the sender's reach, and the index must be one of the fifteen rows that
buy a level. The host asks exactly what the button asks, including the game's own asymmetry: the
balance is tested against the row's unaccumulated price and charged the accumulated one.

### The laptop's inbox

The laptop's messages -- the hash-collection task mails, the scientist and alien replies, the
mails an event or a caught signal produces -- are the same on every machine, and deleting one
deletes it for everyone. Every producer in the game funnels through one Blueprint-internal
function into the save's email array, and calling that same function on a receiver reproduces the
whole arrival at once: the stored row, the list entry, the ding at the physical laptop and the
tab highlight, with the date re-stamped from the synced clock.

Each peer keeps a shadow of the array and diffs it once a second. The array only ever grows at
the tail, so the diff is positional. A new row is broadcast by the host -- see below -- and a
removed row is broadcast by whoever deleted it, as the row's content HASH and never its index,
because a producer writes its own row before that row reaches anyone and two peers therefore hold
the same messages in different orders.

Writing a mail is the host's alone: nothing in the game authors a mail from a player action, so a
client that started producing them could only be a diverged simulation writing into everyone's
permanent inbox, and a client's append is dropped at the host rather than relayed. Deleting is
symmetric, because the only thing in the game that removes a mail is a player pressing the row's
delete button (`coop/world/email_sync`).

### Shop orders

A client's laptop order is entirely local to its machine: the game's order function queues it in
the client's own save and hands it to the client's own mirror drone. So on a client the order function's
queueing (`addOrderCart`) and its send to the drone (`sendShop`) are refused at the script-body gate, and
the order goes to the host as an intent naming each item's shop row and nothing else, read from the
function's own parameter at its entry; the host prices the row from its own
store table, checks its own balance, rolls its own delivery time, commits through the game's own
order function and charges (`coop/items/order_sync`). An order a world event makes on a client (the
daily delivery, a gift) is not sent: the host's copy of the event makes it. A refused order comes back with a reason, and the refused items
are put back in the client's cart, because the game's own affordability gate runs before the cart
is cleared while the refusal arrives after. This is the reference intent lane: the intent used to
carry a client-chosen price, and every client shopped free.

The intent names a shop ROW, not an object class, because a class cannot name an item: the game's
473 shop rows map onto 368 distinct classes, and `prop_C` alone is shared by 50 of them, so 112
rows have no unique class. The row name is the shop's real identity, and the game stamps it into
each generated store entry, so a forwarded order already carries it.

The host's price comes from the game's own `list_store` table, read two independent ways -- a walk
over the row map, and a fully reflected column read that needs no layout knowledge. They are
compared, and a single disagreement invalidates the whole catalog: the host then refuses client
orders outright rather than charge a number it cannot vouch for. Setting
`VOTVCOOP_STORE_CATALOG_BREAK=1` makes the walk read the wrong field on purpose, so that refusal
can be seen firing before it is trusted (`ue_wrap/world/store_catalog`).

The delivery queue is the host's. Each change to it -- an order queued, the delivered one taken off --
goes to every client as the laptop's own queue functions run, and a client's queue follows through
the same functions (`coop/items/order_queue_sync`). A shop item travels by its row; an item a world
event built outside the shop has no row and travels by its class, and by the list_props name a generic
prop carries in its asProp (the daily delivery's reel case), rebuilt in the one shape every such builder
makes. A change waits, in its place, while the client's laptop, its shop catalog or the engine's name
conversion is not ready yet; an item the client can never build is left out, down to an order of no
items that still keeps its place, so the host's next delivery takes the same order off both queues; and
a change counts as applied only when the queue moved by exactly one.

### The coin gun

Shooting a prop with the coin gun sells it: the game destroys the prop and mints coins whose
material is their denomination. A client's shot is uncancellable, so the lane captures the
client's own coins at their birth and releases or destroys them at the next barrier, sends a sale
intent naming the prop by key ahead of the client's ordinary destroy on the same lane, and the
host resolves the prop in its own world, prices it from its own copy, mints through the gun's own
sell function and destroys the sold prop itself; the coins are host-owned world actors that the
event-actor mirror carries, and whoever's body trips one on the host credits the host, a client's
puppet included (`coop/items/coingun_sync`). The client is told the result.

### The floppy slot

A laptop and a signal server hold a disc the same way: inserting one moves its type, its remaining
writes, its data rows and the JSON of its whole save struct into four fields of the device and
destroys the actor, and ejecting one spawns the disc back from those fields. The four are one
concept with one owner, and they are state, not an event: the host holds every device's slot, each
peer polls its own devices once a second behind a digest that reads the raw field bytes, and a peer
whose own game changed a slot sends the host the outcome as a claim. The host applies it and answers
with the canonical, which is also the acknowledgement (`coop/interactables/floppy_slot_sync`).

The laptop also edits the files of the disc it holds, which its file quad carries as edit scripts
(`coop/interactables/laptop_buffer_sync`). So the laptop's slot is watched for which disc it holds
alone: the rows and the writes travel with a change of that and are the quad's between two. A
canonical naming the disc a laptop already holds leaves its files alone, and the slot lane primes
the quad at each change it takes -- a claim sent, a publish, a canonical written -- so an edit made
after it is the quad's to send, even one made before the host answered the claim. The host counts
each change of the laptop's disc; the quad's batches and canonicals carry that count on the slot's
lane, a batch made on another disc is answered with the canonical instead of applied, a canonical of
another disc is dropped, and neither end applies or sends one while its own change of the disc waits
for the other.

Emptying a slot writes only what the device's own eject writes -- the type and the rows. The eject
runs in two phases: it clears those two, and about a second later, when the carrier's timeline
finishes, the deferred spawn reads the remaining writes and the save JSON to rebuild the disc.
Anything that zeroes those in between hands the player back a blank disc under a new identity.

A disc a peer ejects reaches the others through the ordinary birth channel: a client's own fresh
prop spawn is not broadcast, so its device's eject is reported to the host, which authors the disc
and broadcasts it like any other world prop. Three other lineages travel that way, and all three
are born into a hand and spawn inert on the host until the holder's pose stream drives them; a
disc is not held by anyone, so it falls on the host instead. Either way the disc's files and
remaining writes travel as its save record behind the birth; a disc the host ejects gets its record
from the host's finish-spawning drain, since the eject fills the disc in after the finish call
([props.md](props.md)).

The laptop's slot crosses as one blob, and the save JSON carries the rows a second time, so a disc
with a few dozen rows is kilobytes. The blob is held to the ceiling the server slot uses and refused
whole past it, never cut: a cut JSON is one the receiver's own eject cannot parse, and the disc it
hands back comes out blank under a new key. The laptop keeps at most 31 rows in the slot itself
(`ui_laptop.updFloppy` trims `floppyData`); the JSON keeps all of them.

A device takes a disc by two entries, and only one of them is a player. Pressing E with a disc in
hand is deliberate, happens on one machine, and the slot carries its outcome. The other is the
hitbox reporting whatever touches the slot -- and an eject spawns the disc INSIDE the box it came
out of, which the Blueprint handles by turning that device's own hitbox off for a moment. In single player
the only birth that can land in a slot is that eject, so guarding the one box is enough; in coop
the disc is also born on the other machine, in a box whose hitbox nobody turned off and where
nobody ejected anything. So the rule sits on the disc rather than the box: a disc is in transit for
a moment after it materialises, and no device swallows one in transit, on any peer. The window is
anchored at the disc's own appearance on each machine, so both peers run one rule against one local
event and neither waits on a message; when it lapses, the native rule simply resumes.

The window is one number for every device, because the mark is on the disc and a disc does not know
which slot it came out of. It has to clear the frame or two the hitbox entry needs to fire, and it
has to end before the device that ejected re-enables its own hitbox -- re-enabling a collider
re-reports every body already inside it, which is how the game re-takes a disc still sitting in the
slot, and that report comes once. The two devices disagree on the pause: a signal server waits a
second, a laptop half of one. The window is set under the shorter of the two, so neither device's
own re-take is touched and an ejecting peer behaves exactly as it does in single player.

The hitbox also takes a disc another peer is holding. That peer's pose drives this machine's copy
of the disc, and bringing it to the slot brings the copy into this machine's copy of the slot, so
the watching peer would insert it too and claim a second insert of one disc. The entry refuses a
disc under another peer's hold on every peer; the holder's own insert is the one the slot carries.

### The signal servers

A base runs dozens of signal boxes, and the game breaks them on its own timer and on world events:
lightning, damage, triggers, the desk's virus. Three verbs write a box's state -- its break, the
virus's typed break and its fix -- and the script-body gate watches all three by name. A client
refuses every one of them through its whole connected session, since each break is a world event
the host's own game makes, and its player's own repair -- the fix the gamemode's repair widget
calls when the minigame succeeds -- goes to the host as an intent, which the host runs on its box
when it is broken and within the player's reach, answering a refusal with its state to that player
alone. The widget's points are the shared balance's, so a client's own credit from the widget is
refused and the intent carries what the reward reads (the lol mode and the solve time): the host
pays the repair (15, or 50 in lol mode, nothing for a box broken by damage) and a record under its
own best time (30, or 100) in the same step as the fix, and a repair it refuses pays nothing. The
lol mode and the solve time are the client's report, which the host cannot check: a time under a
second settles no record, and the host's best rides the server row to every peer, so each widget
judges a record against the best the host pays from. The repair count stays the player's; the
points total is the host's, its payment counting it. The host polls each box's broken and
damaged flags, the repair type its break rolled, and the three totals the gamemode keeps for the
farm, broadcasts a change at once after a repair and within a second otherwise, and a client writes
the flags and the type and calls the box's own re-skin, which is notify-free and so repaints
without firing the notice a real break fires. The box list, the verbs, the label, the break state
and the repair widget resolve in one engine wrapper (`ue_wrap/devices/serverbox`); the lane beside
it owns the wire half -- the row, its width, the poll, the repair intent and who may author any of
it (`coop/interactables/serverbox_sync`).

A box also takes up to three physical upgrades, a count of its own that its break dice weigh. A player
installs one by using a held upgrade on the box, which destroys the upgrade, and takes one out with E at
the box's upgrade bay, which hands a new upgrade over; both run on the machine of the player who acted.
The two verbs are watched on the box's class at the script-body gate, and each sends what its body
changed: a client sends the host an install or a take-out, naming the box by its place in the
gamemode's list; the host applies it within the game's own limits, re-meshes the box and sends every
box's count, which every peer adopts. An op that lost a race goes back to its author with the counts --
a refused install is refunded by an upgrade the host spawns at the box, a refused take-out's upgrade is
removed from its author's hand -- and a joiner gets every box's count at its world-ready
(`coop/interactables/server_upgrade_sync`).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a door, a light group | the host | each is sent at its own verbs; a client's own door verb is an intent the host runs, and its own group writes are refused |
| a light switch, a lid, the garage, an appliance, an oven's repair, a locker | any peer | symmetric state edges, relayed |
| the power panel, the generators | the host | a client's press, repair, service, upgrade install and hit are ops the host takes from a player within reach; the host sends the breakers after its panel's apply and every generator's row after its verbs |
| a keypad | the host | its verbs replayed on every client and its settled state after each chain; a client's own entries are an intent the host runs |
| the coordinate towers | the host | a row per tower whenever one changes; a client's presses are intents the host runs, its fuse pulls and inserts claims the host takes or refuses |
| the turbine | the host | six floats a second |
| a window, the grime | any peer, minimum wins | monotone decreases |
| the drone | the host | a transform stream; the client's tick suppressed |
| the balance | the host | one-way, absolute, on change |
| the upgrade levels | the host | the struct whole, on change; a purchase is an intent |
| an order | the client names the row; the host performs and prices | an intent |
| a coin gun sale | the client names the prop; the host prices, mints and destroys | an intent ahead of the destroy |
| a device's floppy slot | the host | a 1 Hz digest-gated poll; a peer claims the outcome of its own insert or eject, and the host's canonical is the answer |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `DoorState`, `LightGroupState` | the host to all | a key and a state |
| `LightState`, `ContainerState`, `GarageDoorState`, `ApplianceState`, `LockerDoorState` | each peer, relayed | a key and a state |
| `DoorVerbIntent` | a client to the host | a door's key, the verb, a hit's damage |
| `KeypadState` | the host to all | a verb the host's keypad ran, or the state a chain settled on: the buffer, the verdict, the set-new-code mode, the password, and whether the chain handed the verdict on |
| `KeypadIntent` | a client to the host | a keypad's key and the entry: a digit, a submit, a cancel, a keycard's verdict, a reset |
| `PowerControlState` | a client to the host; the host to all | the breakers a press flipped; the breakers, the lockout, a lever's click and each slot's last press taken |
| `PowerGridState` | the host to all; a client to the host | every generator's row and each slot's last op taken; a repair, a service, an upgrade install or a hit |
| `CoordTowerState` | the host to all; a client to the host | every tower's row (broken, fuses, puzzle, panel, lever), each slot's last op taken and last claim refused; a press of a puzzle button, the lever or the retract, a fuse pulled or inserted |
| `TurbineState`, `WindowCleanState`, `GrimeState` | each peer or the host | the driver floats; a decrease |
| `DroneState` | the host to all | the drone's transform and flags |
| `DroneFlyIntent` | a client to the host | the face pressed, the keyboard; the host finds the console by the sender's reach |
| `BalanceSync` | the host to all | the absolute balance |
| `UpgradeLevels` | the host to all | the eighteen levels, whole |
| `UpgradeIntent` | a client to the host | the panel row's index and whether it is a buy or a sell |
| `OrderRequest`, `OrderRefused` | a client to the host; the host to one client | the items by row; a refusal and its reason |
| `OrderQueue` | the host to all | a change to the delivery queue: a reset, an order queued (its items by row or by class), the first one taken off |
| `CoinGunSell`, `CoinGunResult`, `CoinCollect` | a client to the host; the host to one client; a client to the host | the sold prop's key; the outcome; a coin the client tripped |
| `FloppySlotState` | a peer to the host with a claim; the host to all with the canonical | one device's slot, or a set of them -- the server boxes' and the laptop's: the type, the writes, the rows and the save JSON, and the laptop's generation |

## Late join

Every channel snapshots the full state of every indexed instance to a joiner at its ready edge,
open and closed alike, because the save the joiner loaded, the host's captured at its join, holds
neither a state the game does not save (a server box's `active`, a sink's tap) nor one changed while
the joiner loaded; an oven's repair is in that save, and the snapshot says it again; the keypads, the power masks, the turbine, the windows, the grime and the
drone's pose are sent the same way, and the balance is sent at connect. The delivery queue goes to a
joiner as a reset and then every queued order, and the joiner ignores the queue's changes until that
reset arrives.

The coordinate towers go to a joiner at the same edge, every tower's row. Its own world's load
would have scrambled again any tower that was saved broken; that roll is refused, so the row is the
state it shows, and the first rows a world takes play no sound.

Every device's floppy slot goes to a joiner at the same edge, an empty one as much as a full one:
the joiner's world came from the host's save file, which coop stops the game refreshing, so it knows
nothing the host has done since. The laptop's file quad follows the laptop's slot on the same lane
and under the same count, so the joiner takes the disc before its files, and a set the transport
refused is sent again with the quad behind it.

The inbox rides the joiner's save transfer, so it arrives whole. A mail written during the 30 to
60 seconds the joiner spends loading is in neither that save nor any later diff, so the host
captures the array at the instant it hands over the save and sends the difference on the joiner's
ready edge -- once per slot. A mail that arrives before the joiner's world can take it is parked
in arrival order and applied once the array settles, rather than dropped on the spot; a row the
settled world still refuses after thirty attempts is malformed for that world and is dropped with
an error line.

## Known limits

| Limit | Evidence |
|---|---|
| A refused coin-gun sale has already destroyed the prop on the client, and no heal re-asserts it: a client authors that destruction before the arbiter answers, as its generator install does, whose refusal refunds the upgrade. The host rebuilds its key index periodically, so a refusal is rare | `[V]` `coop/items/coingun_sync` |
| The coin collect has two entries; the interceptor sits on the overlap entry, and the E-press entry dispatches inside the Blueprint where it cannot fire, so a coin a client collects by pressing is credited on the client only and the host's next balance broadcast erases it | `[V]` `coop/items/coingun_sync` |
| A client's earnings from anything but the drone and the coin gun (a point sack, a chest, an achievement) reach only its own machine and are erased by the host's next broadcast | `[V]` `coop/world/balance_sync` is one-way |
| A client's light-group index has been reported dropping to zero after a join; not reproduced | `[?]` [issue 11](https://github.com/VOTV-MP/Multivoid/issues/11) |
| A slot change reaches the other peer on the next poll, so up to a second plus the round trip. A player who reaches a box inside that window acts on the slot as it was: an eject of a disc the other peer has just inserted answers "No floppy disc in the slot" and is not retried, and a client's insert into a slot the host has just filled replaces the host's disc when the claim lands, which loses that disc | `[V]` the lane polls at 1 Hz, and the host applies a claim over whatever its slot holds (`coop/interactables/floppy_slot_sync`); a faster poll would narrow the window rather than close it |

## Code map

| Concept | Files |
|---|---|
| the engine and the adapters | `coop/interactables/interactable_channel.h`, `coop/interactables/interactable_sync`, `coop/interactables/door_verb_intent`, `coop/interactables/door_state_verbs`, `coop/interactables/toggle_verbs`, `coop/interactables/verb_lanes`, `ue_wrap/devices/door`, `ue_wrap/devices/door_box`, `ue_wrap/devices/lightswitch`, `ue_wrap/devices/garage`, `ue_wrap/devices/appliance` |
| keypads | `coop/interactables/keypad_sync`, `ue_wrap/devices/passwordlock` |
| power, turbine, windows, grime | `coop/world/power_panel`, `coop/world/power_grid`, `ue_wrap/devices/generator`, `coop/interactables/turbine_sync`, `coop/interactables/window_sync`, `coop/interactables/grime_sync`, `ue_wrap/devices/power_control`, `ue_wrap/devices/windturbine`, `ue_wrap/devices/base_window`, `ue_wrap/devices/grime` |
| the coordinate towers | `coop/world/coord_tower_rows`, `coop/world/coord_tower_ops`, `ue_wrap/desk/coord_tower` |
| the drone | `coop/interactables/drone_sync`, `coop/interactables/drone_call_intent`, `ue_wrap/devices/drone`, `ue_wrap/devices/drone_console` |
| the floppy slot | `coop/interactables/floppy_slot_sync`, `ue_wrap/devices/floppy_slot`, `ue_wrap/devices/serverbox`, `ue_wrap/devices/laptop` |
| the inbox | `coop/world/email_sync`, `ue_wrap/world/email`, `coop/session/join_seed` |
| the economy | `coop/world/balance_sync`, `coop/items/order_sync`, `coop/items/coingun_sync`, `coop/interactables/upgrade_sync`, `ue_wrap/world/economy`, `ue_wrap/world/order_economy`, `ue_wrap/world/store_catalog`, `ue_wrap/world/upgrades` |
| identity | `coop/element/portable_identity` |
| tests and probes | `coop/dev/order_selftest`, `coop/dev/container_selftest`, `coop/dev/door_drill`, `coop/dev/lightswitch_probe`, `coop/dev/drone_probe`, `coop/dev/drone_call_drill`, `coop/dev/light_group_census`, `coop/dev/floppy_selftest`, `coop/dev/tower_drill` |
