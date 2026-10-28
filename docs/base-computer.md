# The base computer and the station's economy

## Purpose

The laptop and the money the station runs on: the shared balance, the upgrade levels, the laptop's
tab, its inbox, the shop orders and the delivery queue, the coin gun, and the floppy slot of a
laptop or a signal server. Who owns each, how a client's purchase or order reaches the host, and
where the group's money can still be lost. These ride the replication engine and the host-owned
drone described in [devices.md](devices.md).

## How it works

### The balance

The host owns the balance. It polls the points field every tick, which catches every writer, and
broadcasts the absolute value on change and to a joiner; a client writes the host's value directly.
If no peer accepts a broadcast into its stream or backlog, the next tick retries the current value.
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

### The laptop's tab

The base laptop and every portable PC show one widget whose tabs are a switcher. The peer holding the
laptop claim reports its index after each tab change (`coop/interactables/laptop_tab`); the host checks
that the sender holds the claim and the index is one of its own tabs, writes it to its widget and sends
it on. A peer that does not hold the claim writes the switcher's index and nothing else: the game's own
tab call also binds that peer's keys to a remote drone. A joiner gets the host's index at its world-ready.

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

For host-owned coins, the client's overlap and E-press entries both forward a collect intent and
cancel local credit and destruction. A refused request or failed send leaves the coin available
for retry without a phantom balance. A client's unpublished sale coins stay under the sale barrier.
`[V]` `.github/ci/coingun_collect_drill.py` executes the production E-press callback against isolated
boundaries; real-client collection is a separate acceptance check.

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

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the balance | the host | one-way, absolute, on change |
| the upgrade levels | the host | the struct whole, on change; a purchase is an intent |
| an order | the client names the row; the host performs and prices | an intent |
| a coin gun sale | the client names the prop; the host prices, mints and destroys | an intent ahead of the destroy |
| a device's floppy slot | the host | a 1 Hz digest-gated poll; a peer claims the outcome of its own insert or eject, and the host's canonical is the answer |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `LaptopTabReport`, `LaptopTabState` | the claim holder to the host; the host to the other clients | the switcher's index |
| `BalanceSync` | the host to all | the absolute balance |
| `UpgradeLevels` | the host to all | the eighteen levels, whole |
| `UpgradeIntent` | a client to the host | the panel row's index and whether it is a buy or a sell |
| `OrderRequest`, `OrderRefused` | a client to the host; the host to one client | the items by row; a refusal and its reason |
| `OrderQueue` | the host to all | a change to the delivery queue: a reset, an order queued (its items by row or by class), the first one taken off |
| `CoinGunSell`, `CoinGunResult`, `CoinCollect` | a client to the host; the host to one client; a client to the host | the sold prop's key; the outcome; a coin the client tripped |
| `FloppySlotState` | a peer to the host with a claim; the host to all with the canonical | one device's slot, or a set of them -- the server boxes' and the laptop's: the type, the writes, the rows and the save JSON, and the laptop's generation |

## Late join

The balance is sent to a joiner at connect. The delivery queue goes to a
joiner as a reset and then every queued order, and the joiner ignores the queue's changes until that
reset arrives.

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
| Map-placed coins are not enrolled as host-owned world actors. Their local collection can temporarily credit only the client until the host's balance changes | `[RD]` `coop/items/coingun_collect` leaves non-mirrored map coins native |
| A client's earnings from a chest or an achievement reach only its own machine and are erased by the host's next broadcast | `[V]` `coop/world/balance_sync` is one-way |
| A slot change reaches the other peer on the next poll, so up to a second plus the round trip. A player who reaches a box inside that window acts on the slot as it was: an eject of a disc the other peer has just inserted answers "No floppy disc in the slot" and is not retried, and a client's insert into a slot the host has just filled replaces the host's disc when the claim lands, which loses that disc | `[V]` the lane polls at 1 Hz, and the host applies a claim over whatever its slot holds (`coop/interactables/floppy_slot_sync`); a faster poll would narrow the window rather than close it |

## Code map

| Concept | Files |
|---|---|
| the laptop's tab | `coop/interactables/laptop_tab` |
| the floppy slot | `coop/interactables/floppy_slot_sync`, `coop/interactables/laptop_buffer_sync`, `ue_wrap/devices/floppy_slot`, `ue_wrap/devices/serverbox`, `ue_wrap/devices/laptop` |
| the inbox | `coop/world/email_sync`, `ue_wrap/world/email`, `coop/session/join_seed` |
| the economy | `coop/world/balance_sync`, `coop/items/order_sync`, `coop/items/order_queue_sync`, `coop/items/coingun_sync`, `coop/interactables/upgrade_sync`, `ue_wrap/world/economy`, `ue_wrap/world/order_economy`, `ue_wrap/world/store_catalog`, `ue_wrap/world/upgrades` |
| tests and probes | `coop/dev/order_selftest`, `coop/dev/floppy_selftest` |
