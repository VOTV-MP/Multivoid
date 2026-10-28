# Devices

## Purpose

The keyed things around the base that are not the workstation: doors and their keypads, the
light switches and the light groups, the garage, the appliances, the lockers, the security cameras,
the power panel, the wind turbine, the windows and the grime, the delivery drone, and the signal
servers. Who owns each and how a peer's press reaches the others. The laptop and the economy the
drone serves (the balance, the shop orders, the coin gun) are in
[base-computer.md](base-computer.md).

## How it works

### One engine, one adapter per device

Every open-or-closed, on-or-off device rides one replication engine
(`coop/interactables/interactable_channel`, driven by `coop/interactables/interactable_sync`):
a key-to-actor index that heals itself, per-key dedup, a deferred apply with a throttled retry for
an instance that has not streamed in yet, echo suppression, and the connect snapshot. A device
family is an adapter over its engine wrapper (`ue_wrap/devices/`), a few lines each: doors, light
switches, light groups, container lids, the garage, the appliances, the lockers, the oven's repair,
the security cameras.

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
A lid a player drags with the mouse (`mouseAssign`, bound to the player's `input_mouseXY` while the grab
lasts) is not a verb of this lane: the game leaves the lid in the dragger's `grabbing_actor`, so the
held-item stream carries its pose and the host relays it to the other clients. The game's own `Key` of a
lid differs between peers for most lids (a barrel's or crate's cap), so the pose and the release go under
this lane's key for the lid, the portable `mv_` token; a lid the lane files under its game Key (no portable
identity) streams under that key and is logged once, and one the lane has not indexed streams nothing and
is logged. The receiving side resolves a portable `mv_` key through this lane's index alone, and any other key through the prop key index first and this lane's index
(`interactable_sync::ContainerActor`) second; it parks the lid's mesh and follows the pose as it follows any
held prop. A lid with nothing to hang on (a barrel's or crate's cap) falls when it is let go, so the holder
keeps streaming its pose from its own copy, under the identity the hold streamed, until that copy is at rest,
and only then sends the release (`PropRelease`): the others follow the same fall, parked, and the release puts
the lid where it stopped and unparks it with no throw velocity, thrown event or whoosh. At rest is the body
asleep, or within a quarter of a centimetre and a tenth of a degree of one pose for 30 equal reads (about half a second), so a slow creep is
not rest. The coast is bounded at ten seconds from the release: a lid that never rests (on a moving carrier, or jittering against
its limit) is released at its current pose and each peer's own physics takes it from there. A grab of another
item while a lid coasts sends the lid's release at once. A lid the lane has not indexed streams nothing, held or
let go. One limit stays: a second player who drags the same lid during the first one's coast makes two slots
stream one lid, and each receiver follows whichever pose came last, as for any prop two peers grab; the
coasting peer, whose copy the other drag parks, ends its coast and sends its release, and a receiver that
follows the other drag applies nothing for it. A peer whose own player is dragging the same lid does not apply
the remote poses for it, whether its drag began before the stream or in the middle of it.
The held-item broadcast never expresses a lid as a spawned prop. Mid-join: the game saves nothing for a lid
(a child actor is skipped by the save), so a joiner's lid spawns closed at its hinge default, and the connect
snapshot carries a lid's `opened` and nothing else. A closed lid is locked at that default on every peer and
needs no more. An open one rests wherever it was left, which only the host's copy knows, so right after the
snapshot the host sends each open lid's place (`SwingerRestPose`, under the lane's portable key), and the
joiner puts its copy there, with no velocity and still simulating, once the key resolves and its copy reads
open. A lid without a portable key is skipped and counted in the host's log line. A place that cannot be applied
yet is parked and retried once per index pass, and dropped by the lane's own rule for its parked states with one
warning naming the key. A lid a remote stream drives, or the joiner's own player drags, is left to that owner, and a stream or release that reaches a lid whose place is still parked drops the place, as newer. A
joiner arriving mid-drag or mid-coast gets the place, then the live stream, then the release.
`[?]` until the swinger drill has a two-peer proof.
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

### Security cameras

A camera has two saved bits, each its own lane keyed by the lane's key for the camera (its portable
identity where it has one, else its prop key): `active` (the LED, the click, the laptop's camera
list and capture gate) and `isLight` (the spotlight, flipped from the laptop's camera panel). Both
are the host's. Besides a player's use, a wisp, the gray events, the cursed camera's night roll and
a save's load write `active`, and most of those are host-owned world systems, so the host's lanes
read both bits after a camera's `actionOptionIndex`, `upd` and `updState` and after the cursed
camera's tick, and send a change to every peer. A client renders what it is sent: the apply writes
the bit, then runs `updState` when the write turns a camera other than the cursed one on, which
plays the click, else `upd`, then repaints a laptop camera panel that has the camera selected. A
joiner hears one click for a camera the host has on that its save had off, and none for one that is
already on.

A client's own use of a camera and its press of the laptop's camera-light button are refused at
their bodies and sent to the host (`coop/interactables/camera_toggle_intent`). The host checks the
sender's reach, of the camera for the use and of any device showing the laptop interface (the base
laptop or a portable PC) for the light, and sets the bit to the value the sender wanted, so two
presses that race collapse to one change. A bit already at that value changes nothing and is sent
again to that sender alone, so a client whose copy diverged converges and its button is never dead.
The host's own open laptop panel is repainted after it performs a request. The cursed camera's tick
is refused on a client: the host rolls it. A camera whose key is empty, or the key its class carries
by default, is addressed by no lane: a client's press on it is refused and sends nothing, and a
cursed one's tick stays that peer's own. A client's copy of a wisp still switches that client's own
cameras off until the creature mirror stops running the wisp's world writes `[?]`.

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

A container that is thrown changes hands without either verb: the game rebinds the thrown actor to
its old slot of the thrower's own array, and the drop intent carries no contents, so the host's copy
of a client's throw was born empty and every later client slice for it was refused for its base.
The host's copy of a client's throw is now a transfer in progress: it is not published, to a joiner
either, until the thrower's slice lands. The thrower ships its slice once the host's echo binds the
container, and retries while its own slot is not readable yet; every slice of that author for the
container is the newest version of the transfer, an edit made while the first one waited included,
and is taken without a base. Whatever the host put in meanwhile is kept beside it, and the merged
contents go to every peer, the thrower included; if the two do not fit one slice, the thrower's is
applied alone and the host's records are dropped, with a warning. A birth slice is taken unjudged
but not unbounded: it spends its author's arrival budget like an edit, and past that budget it is
held in the pen and replayed rather than refused, since it is the thrower's only copy and is sent
once. The pen holds eight per author, so a ninth simultaneous held slice evicts that author's oldest, said. Each author
has at most 64 transfers in flight, its own oldest given up past that. The wait ends after thirty
seconds or when the thrower leaves. A peer that joins during a transfer gets the container from the
fan-out that completes it, not from its seed. A container the host throws publishes its contents at the express, since the
mirrors are born empty, retrying while its slot is not readable.

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

### The fuse towers

Each of the three coordinate towers can break, and a broken one blocks the ping. A tower's state is
its broken flag, eight fuses (empty, good or blown) and a lights-out puzzle, and its panel, on a
platform up the tower's ladder, opens on a montage and carries a lever that runs a timeline. The
tower rolls its dice in one function, `Scramble Radar Dish`, which blows a good fuse or, with none
left, scrambles the puzzle and breaks the tower; three callers reach it -- the decay timer the
generators' saboteur runs, an explosion nearby, and the tower's own load when it was saved broken
-- and the lever's end judges the puzzle. A client refuses both at the script gate on every route,
its world's load included, so the host's world is the towers' only author
(`coop/world/fuse_tower_ops`). The host reads its towers each tick and sends every tower's row
whenever one changes (`coop/world/fuse_tower_rows`); a client applies each change as the tower's
own graph makes it: the painters for the state, `solvePuzzle` for a repair with its success sound,
`moveLever` for the lever, and its own retract for the panel once its own montage is idle; a press,
a pull, an insert and a failed lever play their own sounds. The lever's end, refused on a client, is
what would clear its moving flag, so a client takes that flag from the row. A client's press of a
puzzle button, the lever or the panel's retract is refused and sent to the host, which runs the
tower's own use with the look-at answered and then gives its own player's look-at back. A pull or an
insert moves a fuse into or out of the player's own hand, so the client's own game makes it and
claims the slot. The host takes a pull when its copy has the panel open, the lever at rest and that
fuse blown, and an insert when the slot is empty and, above the two easiest difficulties, the host saw
that player spend a good fuse at the tower; it refuses one otherwise, answering in the rows to the
sender alone, and a refused pull's fuse is taken back out of the puller's hand, a refused insert's
fuse given back where the player stands. A press or a lever the tower's own gates would ignore on the
host's copy is refused the same way, and a dead player's acts are. Every act is judged within reach of
the part it acts on, in the player's order, at a few a second.

The radio tower is the second kind (`coop/world/fuse_tower_kind` holds what differs between the two). It
is one actor, the gamemode's `radiotower`, and the drone's shop run reads its broken flag on the host, so
a client's own break or repair that stayed on its own copy left the order flow judged by the host alone.
It has the same fuses, a lights-out puzzle on a torus of up to 5x5 cells, a confirm lever whose forward
end judges the puzzle and one `animation` flag that gates every use, and no panel; its second lever, the
reset lever, re-rolls the tower. Its one roll is `scramble` (the decay timer, an explosion nearby and the
reset lever all reach it), refused on a client with the lever's judgement, so the host is its only
author. Only its fuses are saved: it loads whole and rolls nothing at load. Its row rides the same
message: the confirm lever's two bits are its timeline's (running, and running forward), the reset
lever's one is new, and a client starts each lever's timeline once when the host's row first has it
running, takes `animation` from the row, and repairs through `completeMinigame`. A client that is shown
a fuse blowing or the tower breaking raises the console's "Radio tower has been damaged!" and "is
down!" alert by hand (the handler the host's broadcast would call), never for a tower's first applied
row, since the handler also wakes the player. A client's press of a puzzle button or of either lever is
sent to the host when its own copy is broken and idle; a whole tower answers with its own hint on the
client. The host takes a pull of a blown fuse when no lever runs, and an insert on an empty slot. At
the two easiest difficulties the game keeps the held fuse and the insert solves the puzzle (the radio
tower also writes the slot, the coordinate tower does not): a client's insert there is refused at the
gate and sent to the host like a press, which judges reach and the slot on its copy and runs the solve,
its rows answering. No spend is asked, since the game spends none, so a client holding no fuse can
solve an easy tower. A client keeps the host's last row of each tower and applies it when that tower
resolves, looking it up again once a second meanwhile; a tower's first applied row is silent in every
sound, and a tower that is another object than the row last went to starts over as a first row. The
radio tower regenerates its puzzle and fuses at construction and at BeginPlay, so a client watches both
generators and puts the kept row back onto the tower after each.

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
drone's leave timer and clicks -- and the console holds its drone as a level reference, so every peer's
console points at that peer's own drone. A client's press therefore reached a mirror whose flight
tick is suppressed: nothing moved and nobody heard about it, which is why only the host could work
the button. The client now refuses its own body at the script-body gate and sends the press. The
console carries no identity to name, so the host takes the console of its own world that the sender
stands at, re-tests its own copy of the lid, then runs the same verb, so the flight starts on the
machine that owns it and arrives on the stream that already carries it
(`coop/interactables/drone_call_intent`). A press that reaches the host before it holds the sender's
body, from a joiner who presses at once, waits for the body rather than being refused, as the
door-verb, keypad and container lanes wait. The leave-timer face rides the same lane: the host flips
its own drone's setting, which every peer's copy takes from the drone's state stream, and the console's
click plays on the presser's machine and on the host.

### The computer and the economy

The base laptop (its tab and inbox), the shared balance, the upgrade levels, the shop orders, the
coin gun and the floppy slot are in [base-computer.md](base-computer.md). They ride the engine
above and the drone's host-only sale.

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
| a security camera's `active` and `isLight` | the host | each is sent after the camera's use, `upd` and `updState` and the cursed camera's tick; a client's use and light button are intents the host runs |
| a light switch, a lid, the garage, an appliance, an oven's repair, a locker | any peer | symmetric state edges, relayed |
| the power panel, the generators | the host | a client's press, repair, service, upgrade install and hit are ops the host takes from a player within reach; the host sends the breakers after its panel's apply and every generator's row after its verbs |
| a keypad | the host | its verbs replayed on every client and its settled state after each chain; a client's own entries are an intent the host runs |
| the fuse towers (the three coordinate towers and the radio tower) | the host | a row per tower whenever one changes; a client's presses are intents the host runs, its fuse pulls and inserts claims the host takes or refuses |
| the turbine | the host | six floats a second |
| a window, the grime | any peer, minimum wins | monotone decreases |
| the drone | the host | a transform stream; the client's tick suppressed |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `DoorState`, `LightGroupState` | the host to all | a key and a state |
| `LightState`, `ContainerState`, `GarageDoorState`, `ApplianceState`, `LockerDoorState` | each peer, relayed | a key and a state |
| `DoorVerbIntent` | a client to the host | a door's key, the verb, a hit's damage |
| `CameraActiveState`, `CameraLightState` | the host to all | a camera's key and one bit |
| `CameraToggleIntent` | a client to the host | a camera's key, which bit, the value wanted |
| `KeypadState` | the host to all | a verb the host's keypad ran, or the state a chain settled on: the buffer, the verdict, the set-new-code mode, the password, and whether the chain handed the verdict on |
| `KeypadIntent` | a client to the host | a keypad's key and the entry: a digit, a submit, a cancel, a keycard's verdict, a reset |
| `PowerControlState` | a client to the host; the host to all | the breakers a press flipped; the breakers, the lockout, a lever's click and each slot's last press taken |
| `PowerGridState` | the host to all; a client to the host | every generator's row and each slot's last op taken; a repair, a service, an upgrade install or a hit |
| `FuseTowerState` | the host to all; a client to the host | every tower's row (kind, broken, fuses, puzzle, panel, levers), each slot's last op taken and last claim refused; a press of a puzzle button, a lever or the retract, a fuse pulled or inserted |
| `TurbineState`, `WindowCleanState`, `GrimeState` | each peer or the host | the driver floats; a decrease |
| `DroneState` | the host to all | the drone's transform and flags, among them its leave timer |
| `DroneFlyIntent` | a client to the host | the face pressed, the keyboard or the leave timer; the host finds the console by the sender's reach |

## Late join

Every channel snapshots the full state of every indexed instance to a joiner at its ready edge,
open and closed alike, because the save the joiner loaded, the host's captured at its join, holds
neither a state the game does not save (a server box's `active`, a sink's tap) nor one changed while
the joiner loaded; an oven's repair is in that save, and the snapshot says it again; the keypads, the power masks, the turbine, the windows, the grime and the
drone's pose are sent the same way.

The coordinate towers go to a joiner at the same edge, every tower's row. Its own world's load
would have scrambled again any tower that was saved broken; that roll is refused, so the row is the
state it shows, and the first rows a world takes play no sound.

## Known limits

| Limit | Evidence |
|---|---|
| A cord's link is the host's: a client reports the plug its player made and the host's answer settles both copies, so two clients on one cord settle in arrival order and a claim on a cord the host's player is dragging is answered with the host's link. A socket's power, a generator's outlets and a Kerfus's charging are not this lane's. Not yet exercised in a two-peer game run | `[RD]` the cord and socket Blueprint call surface; `[?]` `coop/interactables/cord_sync`, `ue_wrap/devices/cord` |
| A client's light-group index has been reported dropping to zero after a join; not reproduced | `[?]` [issue 11](https://github.com/VOTV-MP/Multivoid/issues/11) |

## Code map

| Concept | Files |
|---|---|
| the engine and the adapters | `coop/interactables/interactable_channel.h`, `coop/interactables/interactable_sync`, `coop/interactables/door_verb_intent`, `coop/interactables/door_state_verbs`, `coop/interactables/toggle_verbs`, `coop/interactables/verb_lanes`, `ue_wrap/devices/door`, `ue_wrap/devices/door_box`, `ue_wrap/devices/lightswitch`, `ue_wrap/devices/garage`, `ue_wrap/devices/appliance` |
| cords and sockets | `coop/interactables/cord_sync`, `coop/interactables/cord_sync_watch`, `coop/interactables/cord_sync_sockets`, `ue_wrap/devices/cord` |
| security cameras | `coop/interactables/camera_toggle_intent`, `coop/interactables/toggle_verbs`, `coop/interactables/interactable_sync`, `ue_wrap/devices/security_camera` |
| keypads | `coop/interactables/keypad_sync`, `ue_wrap/devices/passwordlock` |
| power, turbine, windows, grime | `coop/world/power_panel`, `coop/world/power_grid`, `ue_wrap/devices/generator`, `coop/interactables/turbine_sync`, `coop/interactables/window_sync`, `coop/interactables/grime_sync`, `ue_wrap/devices/power_control`, `ue_wrap/devices/windturbine`, `ue_wrap/devices/base_window`, `ue_wrap/devices/grime` |
| the fuse towers | `coop/world/fuse_tower_rows`, `coop/world/fuse_tower_ops`, `coop/world/fuse_tower_kind`, `ue_wrap/devices/coord_tower`, `ue_wrap/devices/radio_tower` |
| the drone | `coop/interactables/drone_sync`, `coop/interactables/drone_call_intent`, `ue_wrap/devices/drone`, `ue_wrap/devices/drone_console` |
| the signal servers | `coop/interactables/serverbox_sync`, `coop/interactables/server_upgrade_sync`, `ue_wrap/devices/serverbox` |
| identity | `coop/element/portable_identity` |
| tests and probes | `coop/dev/container_selftest`, `coop/dev/door_drill`, `coop/dev/lightswitch_probe`, `coop/dev/drone_probe`, `coop/dev/drone_call_drill`, `coop/dev/light_group_census`, `coop/dev/tower_drill` |
