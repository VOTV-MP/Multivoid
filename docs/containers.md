# Containers

## Purpose

What a container prop holds is the same for every player: a put, a take and a move inside one are the
host's to decide and every peer's to show. This is the container part of [props.md](props.md).

## How it works

A container's contents are on its inventory component: `canvasInventory`, an array of inventory entries (a
`struct_betterSave` record of the item, its place on the inventory canvas and its amount; `ue_wrap/actors/inventory_item`),
which the container serialises into its own save record, so a nested container's contents travel inside its entry. Every
mutating verb (`addItemAsObject`, `addItemAdvanced`, `removeInventoryItem`, `trimInventory`, `transferFromInvToInf`) runs
inside the Blueprint, below the two dispatch seams a hook can intercept. The script-body gate does see it -- that is how
this lane learns a container changed -- but it runs on the presser's own machine, and no local body can wait for an answer
from across the network. So the host publishes the contents (`coop/props/container_contents_sync`): one slice of its array
per live container is sent at a joiner's ready edge and on change, and a client's mirror is display only. A client's only
slice is a thrown container's birth, bounded by its arrival rate; the host refuses every other client slice and answers the
author with its own truth (`coop/props/container_truth`). A change a client makes to a shared container is an act the host
judges, and an extraction on a client is a container-extract birth through the intent door above.

The only way a player takes from or puts into a container is the inventory screen, `ui_FreeLayoutInventory_C`, which the
container's own `openContainer` opens onto its inventory component. Dragging an entry out of the container's canvas runs, in
one synchronous flow, `addItemAdvanced` on the player's bag (the copy the drag then holds), then the item widget's
`removeItem`, which is `removeInventoryItem` on the container's component: it blanks the entry in place, and `trimInventory`
drops the blanks when the screen closes. A mass selection runs the same pair per entry, a drop onto the world spawns the
item in `dropItems` and removes it from its owner the same way, and a crafting combination removes both items from theirs.
`removeInventoryItem` on the container's component is therefore the one place every take passes, with the component, the
entry's index and the entry in hand (`coop/props/container_act_request`). On a client, for a shared container, that call
runs at once, so the drag goes on without a wait, and the entry's address (its index and the hash of its wire form) goes to
the host as a take in the same step. The host judges it as before (the sender's world, the holds, the reach, the container,
the address), takes the entry out of the container into escrow and grants; the client's commit ends the escrow. A refusal
gives the take back: the copy the drag filed in the player's bag is taken out again (the item widget's own `removeItem`, and
the screen's `dropDragged` when it was the one being dragged), a line is shown, and the host sends its truth of the
container, which the client's mirror applies and the screen rebuilds from. A take the transport could not carry, or one past
the sixty-four a client keeps pending, is cancelled and its copy taken out at once; one the host does not answer in 10 s is
given back the same way, and kept so that a grant still to come is answered with an abort. The host's own take is
authoritative: the writer watch marks the container and the sweep publishes it. Dragging with the right button or the stack
key takes part of a stack: `takeFromStack` lowers the entry in place and a copy with the part goes in the bag. The seat asks
the host before the body runs (`TakeStack`); the host keeps the part in escrow like a whole entry, a refusal takes the copy
out of the bag, and a restore files the part as an entry of its own. The canvas's combine-stacks action runs `addToStack` on
the entry that stays, then `removeItem` on the other: the seat waits for the body to raise the stack, asks the host to fold
the one entry into the other (`MergeStack`: one write, nothing held, no commit), and lets the removal that follows pass as
the merge's own. The host merges only entries of one class and name.

A put by dragging an entry onto the canvas runs the inventory's `addItemAdvanced` (the screen's `enddrag`, or `clickoff` for
a mass selection), then the bag widget's `removeItem`. The seat cancels `addItemAdvanced`, which the screen reads as a drop
that did not fit (the widget stays in the bag), and asks the host with the entry as the inventory would store it, in the
`PutRecord` family. On a grant the verb runs again under the replay latch if the widget still holds that item in the bag;
the widget leaves the bag once the inventory has stored the entry, and the commit says where. A refusal undoes nothing. An
entry dropped back on the canvas it lies on (a mass selection moved inside one container) is not carried and snaps back. An
`addItemAdvanced` the screen did not make (a prop a container absorbs, the vacuum, a flashlight's battery taken out over the
canvas) is not a drop and runs natively `[?]`.

The other guarded put is hold and run (the container's `playerUsedOn` with the run key down): the client cancels it and asks
the host with the record, the host reserves room, and on the grant the same verb runs again under the replay latch if the
hand still holds that actor; the commit says where the container stored the record. The host's own hold-and-run put waits while a client holds the container.
The host's room test for a put is the inventory's own: the volume of the record's name times its amount against the container's
`totalVolume` and `maxVolume`. The volume is the game's `lib_C::Get Volume` asked with the name as the gamemode's `propRenderer.Names`
spells it (no name is made for a string a peer sent); a name that list lacks is refused. In 0.10 that function returns `valid` for every
name and answers by mesh only with `getVolume` set. On build b284 it answers 0 for every name and mesh until the game's volume tables arrive, about 2.5 minutes after a world loads (just after the game's own "Icons finished and saved" / "Render finished" / "Cleanup" hints; measured on a client: crowbar 2986, kerfurO 105905, mre 3245, soap 751 once they had, against the player's `maxVolume` 50000), and until then a container's `totalVolume` stays 0 with records in it, so the game's own room test never refuses and the host's mirrors it `[V]` (the drill logs `put fit:` lines; the honest act drill's `kerfur pocket` and `full` rows wait for the tables, because the game's refusal, "Not enough space", exists only after them). `[V]` the container take drill (`coop/dev/container_take_drill`, env
`VOTVCOOP_CONTAINER_TAKE_DRILL=1` on both peers) walks a client to a world container, takes a record the way the screen's drag does
(the bag's `addItemAdvanced`, then `removeInventoryItem` on the container), and puts a held prop back with the run input down; it
reads both peers' record counts. `[V]` The stack acts run on the screen's own widgets under `coop/dev/container_stack_drill` (`VOTVCOOP_CONTAINER_STACK_DRILL=1`, both peers, it mutates the container): the host seeds a stack of 6 and two of 3, the client drags one unit off the 6 (`dragRMB`, then the screen's `beginDrag` on that stack's widget, which runs `takeFromStack`), then merges the two 3s (`addToStack`, then the other entry leaves); each leg is granted by the host, stands on the client at a second read, and the host's container reads `[5, 3, 3]` and then `[5, 6]`. `[V]` The drone's cargo take is run by `coop/dev/drone_cargo_take_drill` (`VOTVCOOP_DRONE_CARGO_TAKE_DRILL=1` with `join_at_host=1`): the host seeds the cargo with two records and sets the dormant drone (parked far off, where no route leads) down beside its player, and the client's take is granted, stands, and leaves the host's cargo one record fewer. An ATV's trunk is covered by `coop/dev/atv_trunk_drill` (`VOTVCOOP_ATV_TRUNK_DRILL=1` with `join_at_host=1`, both peers, it mutates the first ATV's trunk): the host fills the trunk with two records, sets the ATV down beside its player and says the trunk's id; the client takes the first record the way the screen's drag does and the host reads the trunk one record fewer; the client then opens the screen on the trunk (`begin(component)`) and drops the bag item on its canvas (the screen's own `enddrag`, which calls `addItemAdvanced` from the screen's script, the put seat's caller test) and the host reads the count again; last the host adds a record itself and the client's mirror shows it `[V]`.
The off hand is driven by `coop/dev/offhand_drill` (`VOTVCOOP_OFFHAND_DRILL=1`, both peers): the off-hand key's handler moves a held crowbar into `holdingActor_offhand`, the host logs the `off-hand mirror SPAWNED` of the hand lane, and `dropItem_offhand` lands a keyed prop the host resolves `[V]`.

The drone's cargo is the same lane with a reserved target id (`coop/props/container_target`): the host reads and writes it
through `ue_wrap/devices/drone`, judges reach of the drone itself, and publishes a granted act through `drone_cargo_sync`.

An ATV's trunk is a third shared target of the same lane. In 0.10 the ATV opens its own `inventoryComponent` (the open option runs
the inventory screen's `begin(atv.inventoryComponent)`); the `spawnedContainer` prop of 0.9 is never created, so no element, no
container lane and no `container_openers` pair exists for it. The trunk is keyed by the ATV's game key (`coop/interactables/atv_sync` names
every ATV by it on every peer): its target id is `container_target::kAtvTrunkBase` (0xFFE00000) plus 20 bits of an FNV hash of the key
(`atv_trunk_sync::EidOfKey`; two live ATVs that map to one id are neither shared, said once). The host reads each ATV's trunk through
`ue_wrap/devices/atv` (`InventoryOf`, `ReadTrunk`, `WriteTrunk`: `container_inventory::ReadContents` / `WriteContents` on the ATV's
component, the entry array a world container holds), twice a second, and sends it whole as a chunked blob in the container slice grammar
(`ReliableKind::AtvTrunk`, host to client, never relayed, element id = the target id) when its hash changed; a joiner gets every trunk at
its connect replay (`connect_replay`). A client holds the newest blob per trunk until the ATV it names is live and writes it over its
component. A client's take, put, stack and merge on the open trunk are the same seats as a world container's (`container_act_request`:
`SharedTargetOfInventory` names the trunk, the host judges and writes through `container_record_edit` with the ATV's component as the
target) and the reach is judged at the ATV itself (`container_reach::Judge`, the same look-at trace, `kReachUU` plus the ATV's bounds).
A client's hand put (the ATV's `playerUsedOn` drops the main hand and files the dropped actor with `addItemAsObject` on the trunk) writes
a copy the host never sees, so it is cancelled at `inventoryComponent_C::addItemAsObject` when the component is a trunk (the dropped
item stays in the world, as the drone's `playerUsedOn` is refused); the host's own put runs and the next read publishes it.
`container_view_close` closes a client's trunk view when the ATV leaves its reach, as for a container. `[V]` (`atv_trunk` drill: take and
put granted, trunk reads on both peers, the host's own add reaches the client). `container_opener_probe` names each ATV's trunk both ways
and logs every client's reach to it; `container_view_drill` opens the screen on the trunk.

The game's overlap box, the trigger a thrown prop entered to be filed, is not a path in 0.10: `putObjectIn_overlap` has no
body and nothing calls `dingus`, so a prop thrown into a container is not filed and the lane carries no arming. A client
whose screen shows a container the host moves out of reach has its view closed (`coop/props/container_view_close`), so a
take is not refused for reach. When contents arrive for a container whose screen is open, the entries are written under the
screen's widgets taken off and shown again (`DetachDisplay`, `AttachDisplay`), since the widgets are indexed like the array.
A joiner needs no replay: a take is one host step, so a joiner seeded before it receives the container's publication after,
and one seeded after receives the container without the entry `[?]`. The open and closed state is a keyed-device channel
like a door's ([devices.md](devices.md)). The full break-and-spill behaviour, and the single-slot verbs, are
reverse-engineered and designed and not built.
