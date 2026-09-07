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

The verbs are Blueprint-internal, so no observer can see a press; every peer polls each indexed
instance's state field once a tick, which catches every writer at once (a press, an NPC's
proximity, a keypad unlock, a script). On a change it broadcasts the new state with the
instance's key, the host relays a client's edge to the other clients, and a receiver resolves the
instance by key and applies idempotently, moving its own poll baseline so nothing echoes.

The key is the game's own for the save-persisted instances and a portable identity computed
by both peers for the rest (`coop/element/portable_identity`): the game mints a random key per
process for anything the save does not keep, so a child actor is named by its parent plus its
component name and a level-baked actor by its object name. Before that, half the doors, lights
and containers in a world were addressable only by the peer that loaded them.

Two modes. A device that reverts on its own, a door that auto-closes or a light group the game
re-derives, is host-authoritative: a client sends an open request, the host applies it under the
real lock and jam guards and its poll broadcasts the authoritative state back, and a hold register
keeps a door open while any peer holds it and closes it when the last holder leaves. A device
with no auto-revert (the garage, an appliance, a locker, a lid) is symmetric: any peer's edge is
the state.

### What is inside a container

A container's contents are not on the container. Every one of them reads from a single global
per-peer array, `saveSlot.GObjStack`, addressed by an index the actor holds alongside a cached
volume. That array reaches a client once, inside the join save-transfer blob, and never again on
its own, because every verb that mutates it dispatches internally to the Blueprint and so is
invisible to both hook seams (`docs/COOP_DISPATCH_VISIBILITY.md`). Without a lane of its own, a
drone delivery landed full on the host and empty on the client.

The seam is the bytecode dispatcher on the add and take verbs, and it only marks the container
dirty -- it reads no arguments and takes no action, which is what makes it correct for every
caller. The peer whose verb fired then authors the slice on the next sweep. There is no request
the host could refuse: the item has already moved on the presser before any seam of ours exists.
So the host arbitrates instead, accepting a client slice only if its author edited the truth the
host last published and no host-side change is in flight, and relaying to every peer except the
author -- echoing a peer's own state back would revert its newer local value. A refused write is
answered by re-publishing the host's truth to that author, and counted.

Applying a slice raw-writes the receiver's own array slot and then re-derives everything a setter
owns through the game's own verbs -- the volume and mass recalculation and the display-name
rebuild -- rather than writing those fields directly. The add verb cannot be the apply verb: it
takes a live actor and serialises it itself, so it cannot ingest a record off the wire. The
overflow check is not called on apply either: it ejects contents.

Two boundaries, both fail-closed. Personal inventory is not this lane's business even though it
is backed by the same global array, so a container whose component is flagged as a player's -- or
whose flag cannot be resolved at all -- is skipped; writing over another peer's slice would wipe
that player's inventory. And a nested container ships without its index, because that index names
a slot in the sender's array and would resolve to an unrelated container on the receiver: a
record whose class descends from the container class ships with its integers cleared and arrives
empty rather than broken.

### Keypads and locks

A keypad is a typed digit buffer plus three state bits, and its accept verb is unreachable from
outside, so the lane mirrors the input: each peer polls its keypads' buffer and active flag and
broadcasts a change, classified as a plain state mirror or, for a short code's native submit
edge, an accept or a deny (`coop/interactables/keypad_sync`). A receiver replays the digit
delta through the keypad's own input function (display, beep, auto-submit at five digits) and,
on an accept or deny, runs the keypad's own open chain: the sound, the LED, the buffer clear and
the lock propagation to its pair and its gated door. An accept unlocks a door; opening it is an
ordinary press on the door channel.

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

### The drone

The delivery drone is one host-simulated actor: its flight is a fragile per-tick integrator not
worth reproducing, so the host streams its transform while it is active, the client suppresses
the drone's own tick and drives the streamed transform through an interpolation window, and the
cargo it drops rides the ordinary prop lanes (`coop/interactables/drone_sync`). The drone's sale
runs on the host only, which is what makes selling into it the one economy path that credits the
group correctly.

### The balance

The host owns the balance. It polls the points field every tick, which catches every writer, and
broadcasts the absolute value on change and to a joiner; a client writes the host's value directly.
The wire is one-way. A client-to-host balance delta once existed with no bound, so any peer could
set the group's money to anything; it was retired whole rather than clamped
(`coop/world/balance_sync`). A client's own earnings are therefore not shared unless a lane
carries them as an intent.

### The laptop's inbox

The laptop's messages -- the hash-collection task mails, the scientist and alien replies, the
mails an event or a caught signal produces -- are the same on every machine, and deleting one
deletes it for everyone. Every producer in the game funnels through one Blueprint-internal
function into the save's email array, and calling that same function on a receiver reproduces the
whole arrival at once: the stored row, the list entry, the ding at the physical laptop and the
tab highlight, with the date re-stamped from the synced clock.

Each peer keeps a shadow of the array and diffs it once a second. The array only ever grows at
the tail, so the diff is positional. A new row is broadcast; a removed row is broadcast as the
row's content HASH, never its index, because a producer writes its own row before that row
reaches anyone and two peers therefore hold the same messages in different orders.

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

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a door, a light group | the host | a client sends a request; the host's poll answers; a hold register |
| a light switch, a lid, the garage, an appliance, a locker, the power panel | any peer | symmetric state edges, relayed |
| a keypad's buffer and its accept | the presser | the input mirrored; the native chain replayed |
| the turbine | the host | six floats a second |
| a window, the grime | any peer, minimum wins | monotone decreases |
| the drone | the host | a transform stream; the client's tick suppressed |
| the balance | the host | one-way, absolute, on change |
| an order | the client names the row; the host performs and prices | an intent |
| a coin gun sale | the client names the prop; the host prices, mints and destroys | an intent ahead of the destroy |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `DoorState`, `DoorOpenRequest`, `LightState`, `LightGroupState`, `ContainerState`, `GarageDoorState`, `ApplianceState`, `LockerDoorState` | each peer, relayed; the request to the host | a key and a state |
| `KeypadState` | each peer, relayed | the buffer, the active flag, an accept or deny event |
| `PowerControlState`, `TurbineState`, `WindowCleanState`, `GrimeState` | each peer or the host | the mask; the driver floats; a decrease |
| `DroneState` | the host to all | the drone's transform and flags |
| `BalanceSync` | the host to all | the absolute balance |
| `OrderRequest`, `OrderRefused` | a client to the host; the host to one client | the items by row; a refusal and its reason |
| `CoinGunSell`, `CoinGunResult`, `CoinCollect` | a client to the host; the host to one client; a client to the host | the sold prop's key; the outcome; a coin the client tripped |

## Late join

Every channel snapshots the full state of every indexed instance to a joiner at its ready edge,
open and closed alike, because the joiner loaded its own save and a switch the host turned off
must be pushed too; the keypads, the power masks, the turbine, the windows, the grime and the
drone's pose are sent the same way, and the balance is sent at connect. A pending order is
primed by a watermark so the joiner's next order is the first it forwards.

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

## Code map

| Concept | Files |
|---|---|
| the engine and the adapters | `coop/interactables/interactable_channel.h`, `coop/interactables/interactable_sync`, `ue_wrap/devices/door`, `ue_wrap/devices/door_box`, `ue_wrap/devices/lightswitch`, `ue_wrap/devices/garage`, `ue_wrap/devices/appliance` |
| keypads | `coop/interactables/keypad_sync`, `ue_wrap/devices/passwordlock` |
| power, turbine, windows, grime | `coop/interactables/power_sync`, `coop/interactables/turbine_sync`, `coop/interactables/window_sync`, `coop/interactables/grime_sync`, `ue_wrap/devices/power_control`, `ue_wrap/devices/windturbine`, `ue_wrap/devices/base_window`, `ue_wrap/devices/grime` |
| the drone | `coop/interactables/drone_sync`, `ue_wrap/devices/drone` |
| the inbox | `coop/world/email_sync`, `ue_wrap/world/email`, `coop/session/join_seed` |
| the economy | `coop/world/balance_sync`, `coop/items/order_sync`, `coop/items/coingun_sync`, `ue_wrap/world/economy`, `ue_wrap/world/order_economy`, `ue_wrap/world/store_catalog` |
| identity | `coop/element/portable_identity` |
| tests and probes | `coop/dev/order_selftest`, `coop/dev/container_selftest`, `coop/dev/door_probe`, `coop/dev/lightswitch_probe`, `coop/dev/drone_probe`, `coop/dev/light_group_census` |
