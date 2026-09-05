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

## Known limits

| Limit | Evidence |
|---|---|
| A refused coin-gun sale has already destroyed the prop on the client, and no heal re-asserts it; the loss is rare since the key index was fixed, and it is the one place a client authors a shared-world destruction before the arbiter answers | `[V]` three refusals, three lost items in one field log before the index fix |
| A mirrored coin is born with the default denomination, so its material can be wrong until the birth blob names its points | `[V]` measured on the coin's begin-play |
| The coin collect seam has two entries, and the interceptor has never been observed firing on the press entry | `[?]` no line in six real credits |
| A client's earnings from anything but the drone and the coin gun (a point sack, a chest, an achievement) reach only its own machine and are erased by the host's next broadcast | `[V]` the credit sites are invisible to every hook and leave no artifact |
| The light groups' host-authored state has not been observed by hand; the field report of a client's index dropping to zero after a join is still open | `[?]` issue 11 |

## Code map

| Concept | Files |
|---|---|
| the engine and the adapters | `coop/interactables/interactable_channel.h`, `coop/interactables/interactable_sync`, `ue_wrap/devices/door`, `ue_wrap/devices/door_box`, `ue_wrap/devices/lightswitch`, `ue_wrap/devices/garage`, `ue_wrap/devices/appliance` |
| keypads | `coop/interactables/keypad_sync`, `ue_wrap/devices/passwordlock` |
| power, turbine, windows, grime | `coop/interactables/power_sync`, `coop/interactables/turbine_sync`, `coop/interactables/window_sync`, `coop/interactables/grime_sync`, `ue_wrap/devices/power_control`, `ue_wrap/devices/windturbine`, `ue_wrap/devices/base_window`, `ue_wrap/devices/grime` |
| the drone | `coop/interactables/drone_sync`, `ue_wrap/devices/drone` |
| the economy | `coop/world/balance_sync`, `coop/items/order_sync`, `coop/items/coingun_sync`, `ue_wrap/world/economy`, `ue_wrap/world/order_economy`, `ue_wrap/world/store_catalog` |
| identity | `coop/element/portable_identity` |
| tests and probes | `coop/dev/order_selftest`, `coop/dev/container_selftest`, `coop/dev/door_probe`, `coop/dev/lightswitch_probe`, `coop/dev/drone_probe`, `coop/dev/light_group_census` |
