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
switches, light groups, container lids, the garage, the appliances, the lockers.

The owner polls each indexed instance's state field once a tick -- every peer on a symmetric
channel -- which catches every writer at once (a press, an NPC's proximity, a keypad unlock, a
script) without watching each. On a change it broadcasts the new state with the instance's key,
the host relays a client's symmetric edge to the other clients, and a receiver resolves the
instance by key and applies it, moving its own poll baseline so nothing echoes. Doors and light
groups are sent at their own verbs instead: every live writer of a door's open state goes through
`doorOpen` or `doorClose`, and of a group's `isActive` through its `runTrigger`, and the
script-body gate watches them (`coop/interactables/door_state_verbs`,
`coop/interactables/lightgroup_verbs`). After the body on the host the lane sends the state it left;
a send no peer can take (none is world-ready) counts as made, since each joiner's snapshot at its
ready edge carries the state. While that is measured, the host's poll runs beside them as a probe
that logs and sends any change no verb reported (a `SHADOW MISS`). It retires, and the door and
group polls with it, once a long session with every writer class live -- creatures, the keypad, a
blackout, the eventers -- logs no `SHADOW MISS` on the host.

The key is the game's own for the save-persisted instances and a portable identity computed
by both peers for the rest (`coop/element/portable_identity`): the game mints a random key per
process for anything the save does not keep, so a child actor is named by its parent plus its
component name and a level-baked actor by its object name. Before that, half the doors, lights
and containers in a world were addressable only by the peer that loaded them.

Two modes. A device that reverts on its own, a door that auto-closes or a light group the game
re-derives, is host-authoritative: the host's copy is the one that moves, and a client renders it.
A client's copy of a door moves only by the host's state: its own `doorOpen` and `doorClose` are
refused unless our apply is running them, so its autoclose, a creature or a trigger on that machine
cannot move it alone. A client's copy of a light group likewise: its `runTrigger` is refused unless
the group lane's own apply is running it, so a switch pressed there, an eventer's flicker or that
machine's own breaker cannot move the group; the switch still flips, and its bit reaches the host
on the switch lane, whose replay of the press is what moves the host's group. A client's own press, hit or pry of a door never runs on its copy either: the
script-body gate refuses the door's entry verb there and sends it to the host
(`coop/interactables/door_verb_intent`), which
runs the same verb on its own copy, so the door's own body decides it once -- its power gate with
the blackout clause, a swing already moving, the pry. The cut is at those entry verbs rather than
at `doorOpen`/`doorClose`: every in-door caller reaches those two through the door's own event
graph, where a press cannot be told from a hit or a trigger, and a hit moves both leaves before it
ever reaches `doorOpen`. The host's door then closes by its own
autoclose, at the first of its five-second checks that finds its sensor list empty, and a client's
puppet counts in that list exactly when the client's own player counts in the client's own copy of
it. A device with no auto-revert (the garage, an appliance, a locker, a lid) is symmetric: any
peer's edge is the state.

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

A keypad is a typed digit buffer plus three state bits, and its accept verb is unreachable from
outside, so the lane mirrors the input: each peer polls its keypads' buffer and active flag and
broadcasts a change, classified as a plain state mirror or, for a short code's native submit
edge, an accept or a deny (`coop/interactables/keypad_sync`). A receiver replays the digit
delta through the keypad's own input function (display, beep, auto-submit at five digits) and,
on an accept or deny, runs the keypad's own open chain: the sound, the LED, the buffer clear and
the lock propagation to its pair and its gated door. An accept unlocks a door; opening it is an
ordinary press of the door.

### Power, turbine, windows, grime

The power panel carries five latched breakers, so it has its own lane with a five-bit mask, any
peer flips a breaker, and the receiver mirrors the panel's own levers and LEDs; the effects on
servers, doors and lights are synced by their own channels (`coop/interactables/power_sync`).
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

### The drone

The delivery drone is one host-simulated actor: its flight is a fragile per-tick integrator not
worth reproducing, so the host streams its transform while it is active, the client suppresses
the drone's own tick and drives the streamed transform through an interpolation window, and the
cargo it drops rides the ordinary prop lanes (`coop/interactables/drone_sync`). The drone's sale
runs on the host only, which is what makes selling into it the one economy path that credits the
group correctly.

The garage console calls and sends it. Its one action option dispatches on what the presser is
looking at -- the keyboard runs the drone's own call-or-send verb, the other face toggles the
drone's leave timer -- and the console holds its drone as a level reference, so every peer's
console points at that peer's own drone. A client's press therefore reached a mirror whose flight
tick is suppressed: nothing moved and nobody heard about it, which is why only the host could work
the button. The client now refuses its own body at the script-body gate and sends the console's
element id; the host re-tests reach and its own copy of the lid, then runs the same verb, so the
flight starts on the machine that owns it and arrives on the stream that already carries it
(`coop/interactables/drone_call_intent`). The leave-timer face has no lane and stays local.

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

A client's laptop order is entirely local to its machine: the order lands in its own save and its
own mirror drone would fly. So the client polls its order list, and on an increment forwards the
order as an intent naming each item's shop row and nothing else; the host prices the row from its
own store table, checks its own balance, rolls its own delivery time, commits through the game's
own order function and charges, and the client's mirror drone is reset so it cannot fake a
takeoff (`coop/items/order_sync`). A refused order comes back with a reason, and the refused items
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

### The signal servers

A base runs dozens of signal boxes, and the game breaks them on its own timer. Nothing about that
is replicated, so each peer's breaker would fire on its own dice and author a false "server down"
on one screen only. The break and fix verbs run inside the Blueprint, where the script-body gate can watch them but
only on the machine running them -- a watch cannot make the other peers roll the same dice -- so
the state is mirrored instead: the host polls the per-box broken flag and the three totals the
gamemode keeps for the farm, broadcasts on a change, and a client writes the flag and calls the
box's own re-skin, which is notify-free and so repaints without firing the notice a real break
fires. A client also disables its own breaker's tick, and gets it back when the session ends.
The box list, the two disc verbs, the label and the break state all resolve in one engine wrapper
(`ue_wrap/devices/serverbox`); the lane beside it owns only the wire half -- the mask, its width,
the poll and who may author it (`coop/interactables/serverbox_sync`).

A laptop and a signal server hold a disc the same way: inserting one moves its type, its remaining
writes, its data rows and the JSON of its whole save struct into four fields of the device and
destroys the actor, and ejecting one spawns the disc back from those fields. The four are one
concept with one owner, and they are state, not an event: the host holds every device's slot, each
peer polls its own devices once a second behind a digest that reads the raw field bytes, and a peer
whose own game changed a slot sends the host the outcome. The host applies it and answers with the
canonical, which is also the acknowledgement.

Emptying a slot writes only what the device's own eject writes -- the type and the rows. The eject
runs in two phases: it clears those two, and about a second later, when the carrier's timeline
finishes, the deferred spawn reads the remaining writes and the save JSON to rebuild the disc.
Anything that zeroes those in between hands the player back a blank disc under a new identity.

A disc a peer ejects reaches the others through the ordinary birth channel: a client's own fresh
prop spawn is not broadcast, so its device's eject is reported to the host, which authors the disc
and broadcasts it like any other world prop. Three other lineages travel that way, and all three
are born into a hand and spawn inert on the host until the holder's pose stream drives them; a
disc is not held by anyone, so it falls on the host instead.

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

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a door, a light group | the host | each is sent at its own verbs; a client's own door verb is an intent the host runs, and its own group writes are refused |
| a light switch, a lid, the garage, an appliance, a locker, the power panel | any peer | symmetric state edges, relayed |
| a keypad's buffer and its accept | the presser | the input mirrored; the native chain replayed |
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
| `KeypadState` | each peer, relayed | the buffer, the active flag, an accept or deny event |
| `PowerControlState`, `TurbineState`, `WindowCleanState`, `GrimeState` | each peer or the host | the mask; the driver floats; a decrease |
| `DroneState` | the host to all | the drone's transform and flags |
| `DroneFlyIntent` | a client to the host | the garage console whose call button it pressed |
| `BalanceSync` | the host to all | the absolute balance |
| `UpgradeLevels` | the host to all | the eighteen levels, whole |
| `UpgradeIntent` | a client to the host | the panel row's index and whether it is a buy or a sell |
| `OrderRequest`, `OrderRefused` | a client to the host; the host to one client | the items by row; a refusal and its reason |
| `CoinGunSell`, `CoinGunResult`, `CoinCollect` | a client to the host; the host to one client; a client to the host | the sold prop's key; the outcome; a coin the client tripped |
| `FloppySlotState` | a peer to the host with a claim; the host to all with the canonical | one device's slot, or a set of them: the type, the writes, the rows and the save JSON |

## Late join

Every channel snapshots the full state of every indexed instance to a joiner at its ready edge,
open and closed alike, because the joiner loaded its own save and a switch the host turned off
must be pushed too; the keypads, the power masks, the turbine, the windows, the grime and the
drone's pose are sent the same way, and the balance is sent at connect. A pending order is
primed by a watermark so the joiner's next order is the first it forwards.

Every device's floppy slot goes to a joiner at the same edge, an empty one as much as a full one:
the joiner's world came from the host's save file, which coop stops the game refreshing, so it knows
nothing the host has done since.

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
| A refused coin-gun sale has already destroyed the prop on the client, and no heal re-asserts it: the one place a client authors a shared-world destruction before the arbiter answers. The host rebuilds its key index periodically, so a refusal is rare | `[V]` `coop/items/coingun_sync` |
| The coin collect has two entries; the interceptor sits on the overlap entry, and the E-press entry dispatches inside the Blueprint where it cannot fire, so a coin a client collects by pressing is credited on the client only and the host's next balance broadcast erases it | `[V]` `coop/items/coingun_sync` |
| A client's earnings from anything but the drone and the coin gun (a point sack, a chest, an achievement) reach only its own machine and are erased by the host's next broadcast | `[V]` `coop/world/balance_sync` is one-way |
| A client's light-group index has been reported dropping to zero after a join; not reproduced | `[?]` [issue 11](https://github.com/VOTV-MP/Multivoid/issues/11) |
| A slot change reaches the other peer on the next poll, so up to a second plus the round trip. A player who reaches a box inside that window acts on the slot as it was: an eject of a disc the other peer has just inserted answers "No floppy disc in the slot" and is not retried | `[V]` the lane polls at 1 Hz; a faster poll would narrow the window rather than close it |

## Code map

| Concept | Files |
|---|---|
| the engine and the adapters | `coop/interactables/interactable_channel.h`, `coop/interactables/interactable_sync`, `coop/interactables/door_verb_intent`, `coop/interactables/door_state_verbs`, `ue_wrap/devices/door`, `ue_wrap/devices/door_box`, `ue_wrap/devices/lightswitch`, `ue_wrap/devices/garage`, `ue_wrap/devices/appliance` |
| keypads | `coop/interactables/keypad_sync`, `ue_wrap/devices/passwordlock` |
| power, turbine, windows, grime | `coop/interactables/power_sync`, `coop/interactables/turbine_sync`, `coop/interactables/window_sync`, `coop/interactables/grime_sync`, `ue_wrap/devices/power_control`, `ue_wrap/devices/windturbine`, `ue_wrap/devices/base_window`, `ue_wrap/devices/grime` |
| the drone | `coop/interactables/drone_sync`, `coop/interactables/drone_call_intent`, `ue_wrap/devices/drone` |
| the floppy slot | `coop/interactables/floppy_slot_sync`, `ue_wrap/devices/floppy_slot`, `ue_wrap/devices/serverbox`, `ue_wrap/devices/laptop` |
| the inbox | `coop/world/email_sync`, `ue_wrap/world/email`, `coop/session/join_seed` |
| the economy | `coop/world/balance_sync`, `coop/items/order_sync`, `coop/items/coingun_sync`, `coop/interactables/upgrade_sync`, `ue_wrap/world/economy`, `ue_wrap/world/order_economy`, `ue_wrap/world/store_catalog`, `ue_wrap/world/upgrades` |
| identity | `coop/element/portable_identity` |
| tests and probes | `coop/dev/order_selftest`, `coop/dev/container_selftest`, `coop/dev/door_drill`, `coop/dev/lightswitch_probe`, `coop/dev/drone_probe`, `coop/dev/light_group_census`, `coop/dev/floppy_selftest` |
