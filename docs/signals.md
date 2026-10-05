# The signal workstation

## Purpose

The game's core loop: catch a signal from space on a dish, tune it, download it, watch the
detector, save it, play it back, move it onto a drive, refine it, and file it on the laptop. How
every stage keeps each peer's screens and progress identical, who owns which field, and the parts
of that loop that still diverge. The upgrades that parametrise the machine are here too, because
they are its missing input.

## How it works

### The native pipeline

One desk actor holds four panes and the whole download machine. A sky-signal director rolls
signals into space; a dish aimed at a signal's coordinates catches it; at the desk the player
animates a frequency-filter offset and a polarity angle, and how close each is to the signal's
truth sets the download speed, while a wrong polarity direction halts it. The download integrates
a rate each tick that folds the frequency and polarity match, the detector needle, a noise term,
the upgrades and the servers; the detector needle integrating to one makes the signal savable.
Saving mints the signal an id and appends it to the deck list; the deck plays it back; import and
export move a row between the list and a drive; the refiner pane processes a loaded signal level
by level, with world triggers at the top; the laptop's database files what the deck saves; the
tape caddy accrues two reels that the daily task grades; and every catch slews all twenty-four
big dishes to one target. Two terms of the rate formula and the dish slews were per-peer
randomness, which is where the divergence came from.

Every desk verb dispatches inside the Blueprint. The lanes poll the resulting state rather than
intercepting the verb, because the state is what the other peers need and a watch fires only on
the machine that ran the body. The desk's keyboard enters through one widget's
key router, the one seam the engine dispatches, and the desk's sounds are played by
presser-local paths, so the effects are forwarded at the native audio seam.

### The four shapes, chosen per field

The desk is mixed ownership, and a blob sync of it fuses a passthrough field with a host-owned
one and diverges. Each element picks one shape:

1. **Poll and delta.** State that is inert once set (a toggle, a knob value): a per-field poll
   detects the presser's change and sends a delta; the host applies it and relays it to every
   peer but the originator. Shared last-writer state.
2. **Intent, host performs, state comes back.** A once-only action with a shared consequence:
   the catch, a saved-signal append, a slot insert, a module plug.
3. **Host-run simulation.** The host owns the tick and every roll; the client suppresses its own
   accrual and overwrites its local state with the streamed outputs.
4. **Claim and owner stream.** One occupant streams a continuous quantity while inside a screen;
   the next enterer adopts the last state.

### Occupancy

The eight enterable devices admit one peer at a time. Entering dispatches inside the Blueprint,
so each peer polls the player's active-interface field for the edge; the host arbitrates a
first-wins claim table, a client claims optimistically and force-exits if the host answers that
another slot holds the device, and a second peer's press is denied with the game's own denied
sound (`coop/interactables/device_occupancy`). The claim engages only on the screen edge; the
desk's physical buttons never set it, which is why the input lane below is claim-free.

### The desk: inputs, the simulation, the cursor, the sounds

Every input-class scalar (knob speeds, filter toggles, polarity direction, volume, the selected
row, the target level, the unit power toggles) is polled four times a second and sent as a
field-granular delta (`coop/interactables/desk_input_sync`); receivers apply it through the
field's native side-effect path and prime their own poll baseline for that field, so nothing
echoes. The cooldown charge is detected as an upward jump, and the same jump classifies the
quick-scan, whose accepted-branch effects the mirrors replay.

The download simulation runs on the host only (`coop/interactables/desk_sim_sync`): the host
rolls both random terms and streams the output vector (decoded, the needle, the rate, the
frequency and polarity data and offsets, the cooldown) unreliable, about ten times a second, and
the client interpolates it and overwrites its own accrual, whose garbage the overwrite hides. The
triangulation ping is a latent state machine gated on a flag; it runs on one machine, the
presser's, and receivers treat the flag as bookkeeping and never write it, because writing it
woke a phantom parallel machine on every observer. A desk hold covers the pinger's run so nobody
else can claim the desk mid-ping. The run is the pinger's presentation, and its verdict is the
host's (`coop/interactables/desk_ping_sync`): at stage 3 a client's gate refuses the verdict and
sends its view and triangle, the host checks the claim, the running ping and the triangle and
primes its own machine, whose next coordinate process rolls the verdict, and the outcome reaches
every peer through the normal lanes, the catch as the pinger's and the find on its profile. A client's
cheat-menu insta-catch crosses the same way and runs as the host's own, where the host's game lets a player cheat.

The coordinate-panel cursor is a sixty-hertz unreliable stream from whoever is moving it,
interpolated on the mirror and written as a pure memcpy the widget repaints
(`coop/interactables/desk_cursor_sync`). The sounds the presser hears (key clicks, verb beeps,
the fail tone, the cursor and ping loops) are forwarded at the native audio seam, the component's
play and activate calls, as effect events (`coop/interactables/desk_snd_fx`); the two loops are
state and are re-sent to a joiner from the components' ground truth.

### Signals, the catch, the dishes

The sky-signal set is rolled on the host only; a client kills its own roller timer, keeps its widget
lifetimes wire-driven and reconciles its set to the host's snapshot
(`coop/interactables/console_state_sync`, which also carries the desk's live-visible scalars, the
committed dish-aim coordinates and the eight one-shot log lines). A catch is the host's event,
its verdict rolled there, whose identity half (the caught data, the sky-row delete, the catch's own
two writes to the download machine) is replayed on every client
(`coop/interactables/signal_catch_sync`); the host's unprimed change edge is the authority, because
a claim-gated detector lost a live catch to the hold's own release. The dish theater is host-only:
the client's dish simulation is parked, and the host's dishes slew natively and it streams the poses
of all twenty-four; the download machine's arm and reset are the host's too: its desk's
`formDownload` and its gamemode's `deleteActiveSignal` each reach every client, which runs the same
verb once, whole, with the host's decoded and polarity written into it, and refuses its own
(`coop/interactables/download_arm_sync`), so the object renderer's `begin` and the signal camera's
trigger run on each peer as they run on the host; and so is a dish's precision: the host sends its
changed dishes' values and a joiner all of them, and a client's copy holds the host's values,
whatever moved it off one (a mirrored lightning strike's hit) put back at its poll, before the
gamemode averages the dishes into the rate its desk downloads with, and before a player's verb reads
them. A client player's own two verbs are the exception, the toolgun's calibration tool and the
uncalibrator: they run on its copy and send what they changed to the host, which performs what it
can and answers with the live dishes named, to all once it performed any and to the author alone
otherwise (`coop/interactables/dish_calib_sync`).

### The deck list, playback, the refiner

The deck's saved-signal list is shadowed on every peer as content-hashed rows, diffed once a
second under the append-at-tail invariant, and mirrored as appends and content-keyed deletes
(`coop/interactables/signal_sync`, `coop/interactables/signal_wire`). Playback is a
presser-authored edge at the audio seam: the only activate site in the desk is the play verb and
the only deactivate is stop, so an organic activate is "someone started playback" and any peer may
stop (`coop/interactables/deck_play_sync`). A client's press on the desk's save family -- SAVE and
DELETE, the deck's drive button and send, the refiner's upload, start and stop -- is refused on its
machine and sent to the host, which replays it with the presser's puppet when what the button acts
on matches its own desk; the glossary entry, profile stat and sounds it makes go back to the presser
(`coop/interactables/desk_verb_intent`, `coop/interactables/desk_verb_effects`). The refiner has one
simulator, the host's machine: a client's own `comp_start`, its join's restore of a saved decode
included, is refused, and a decode a client's start began is that client's, its completion gloss and
processed signal sent to it. The host streams the refiner's state while decoding, on its edges and on
each completion; a client is a passive mirror that paints what nothing native repaints
(`coop/interactables/comp_sync`).

### Drives, racks, modules, tapes, the laptop, the database

The drive chain is idempotent per-slot state lines any peer announces and the host canonicalises
(`coop/interactables/drive_sync`); a drive's recorded row is the host's: it goes out when the drive's
own `upd` runs on the host, a client puts back any other row its copy of a drive takes once it holds
the host's row for it, and a drive a client brings into the world (a birth, a re-placement from its
inventory, a container or its hand, one it holds that the host never saw) sends its row to the host,
which takes that one row from that client and answers any other client row with its own
(`coop/interactables/drive_payload_sync`);
the eraser's delete, pressed on a client, runs on the host, and every press and wipe of the host's
eraser shows on each client's own eraser (`coop/interactables/eraser_press_intent`); the
rack is presser index-operations the host terminates, a full canonical array back, and a deny
ring for races (`coop/interactables/drive_rack_sync`). A slot freezes the drive it takes, and only a
grab takes one out -- the drive ejects itself, and the grab unfreezes it -- so a peer applying
another's eject leaves the unfreeze to that grab's own hold, which reaches the drive through the prop
lane: its first pose unfreezes the copy, and its release leaves the holder's flags
([props.md](props.md)). An insert ends the hold of whoever carried the drive in, so a late pose of it
cannot pull the drive back out; a drive a conflicting line ejects is unfrozen with the eject, since
no hold follows it. The desk's physical modules sit in slots, and a type
may sit in more than one, so a plug and an unplug each name their slot; the host applies them and
re-broadcasts the whole array, and a refused one goes back to its author with it
(`coop/interactables/physmods_sync`). The tape caddy's reel slots are presser-authored edges, and
its accrual is deterministic and clamped, so instead of a park the host re-snaps it once a second
(`coop/interactables/tape_caddy_sync`). The stationary PC's power is a presser edge
(`coop/interactables/laptop_sync`); its disc slot is the host's like a server box's, a peer's
insert or eject a claim the host's canonical answers (`coop/interactables/floppy_slot_sync`); its
file buffer is edit-script batches the host anchors and answers with a canonical
(`coop/interactables/laptop_buffer_sync`), on the slot's lane and tagged with the slot's generation,
so a batch or a canonical made on one disc never lands on another, and the disc crate is a stack of tail operations with
a deny that reaps the author's just-spawned disc (`coop/interactables/floppybox_sync`). The
laptop's signal database is a content-hash multiset with a host-canonical order, because the
store has a move verb; each of its writers -- the laptop's add, remove and move, and the rename
window's commit -- is watched at the script-body gate, and its exit sends what it changed, a
rename as a delete and an append with the order line that keeps the row in its place
(`coop/interactables/meadow_db_sync`). The signal servers' break-and-fix
simulation is host-owned: a client refuses its boxes' break and fix verbs, its player's repair runs
on the host, and the state comes back as rows (`coop/interactables/serverbox_sync`).

### The SAT console

A machine has one SAT console terminal, the widget every SAT console panel in the world shows; a
panel points it at its dish, or at ROOT. A line a client types there whose command rests on the
shared world -- a dish's calibration, a floppy's export or eject, a gift box, a server's or a
tower's state, the debug and joke commands that spawn or destroy things -- is refused on the
client and sent to the host with the terminal's context. The host keeps a terminal of the game's
own class for each typist, never shown, and runs the line there, so the command happens once, in
the host's world, and its results cross by their own lanes; every line that terminal prints comes
back to its typist alone, and so does its busy state. A line runs only once the host's terminal is
idle and set to the dish and name the line carried; a terminal still running a command answers it
busy, and a dish the host cannot resolve yet, or an init that fails, answers it err rather than
running it against another context. A line is never re-run on its own, since a command such as an
eject has an effect in the world. A line whose effect is the typist's own -- its
compass, its terminal, the desk's radar filter and sounds on its machine -- runs where it was typed
(`coop/interactables/sat_console_sync`, the command table in `coop/interactables/sat_console_table`).

### Upgrades

The signal upgrades are one persistent struct of eighteen levels that parametrise the download,
ping, coordinate, refiner, radar and detector simulations. No lane mirrors them: they ride only
the transferred save, once, so a level bought mid-session diverges silently. The desk routes
around it by streaming the derived frequency and polarity data, but the struct itself, and the
purchase as an intent the host validates against the shared research points, are designed and
not built. The ATV's physical modules are on [vehicles.md](vehicles.md).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a screen's occupancy | the host arbitrates | first claim wins; a client claims optimistically |
| desk input scalars | the last presser | polled deltas, relayed except to the originator |
| the download simulation, the needle, the rate | the host | a streamed output vector; the client overwrites its own |
| the ping | the presser's machine; its verdict the host | the verdict as an intent; observers keep the flag as bookkeeping |
| the cursor | whoever moves it | a sixty-hertz stream |
| the desk's sounds | the presser | effect events at the audio seam |
| the sky-signal set, the catch, the dishes | the host | roller, the host's event, parked client sim |
| the deck list, the database | every peer's shadow | content-hashed appends and deletes; the host's order |
| playback | the presser; anyone may stop | edge events |
| the desk's save family: save, delete, the deck's drive and send, the refiner's upload, start and stop | the host | a client's press as an intent the host replays; its gloss, stat and sounds back to the presser |
| the refiner | the host; a completion's gloss and processed signal, the one whose start began it | the host's state stream |
| drives, racks, modules, the crate, the file buffer | the host, canonical | any peer's operations, the host's array back |
| a drive's recorded row | the host | sent at the drive's own `upd`; a client's new drive's first row, from the client that brought it |
| the eraser's delete | the host | a client's press as an intent; the host's press and wipe shown on every client's eraser |
| the tape accrual | each peer, host-corrected | a one-hertz re-snap |
| the servers | the host | state driven into each box |
| a SAT console command that rests on the world | the host, on a terminal kept for its typist | the typed line up; the printed lines and the busy state back |
| the upgrade levels | the host's save, once | not mirrored |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `DeviceClaim` | a client to the host; the host to all | a claim, a release, the busy table |
| `DeskInput`, `DeskScanEvent`, `DeskState`, `DeskLogLine` | the presser, relayed | a field delta; a quick-scan; the desk scalars; a one-shot log line |
| `DeskSimPose` (stream), `DeskCursorPose` (stream) | the host to all; the mover to all | the simulation outputs; the cursor |
| `DeskSndFx` | the presser, relayed | an audio effect event |
| `SkySignalState`, `SkySignalCatch`, `DishAimState` | the host; the host; the occupant | the signal set; a catch; committed coordinates |
| `DeskPingVerdict` | a client to the host; the host to that client | a ping's or a cheat insta-catch's verdict to roll, the view and triangle it rolls from; its refusal, or the pinger's find |
| `DishArm`, `DishSnapshot`, `DishCalib`, `DishPose` (stream) | the host; the host to a joiner; the host, to all, to an intent's author and to a joiner; the host | the download's arm and reset, with the host's decoded and polarity; every dish's pose for a joiner; the changed dishes' precision, the live dishes an intent named (to all once the host performed any of it, to its author alone when it performed none), and every dish's for a joiner; dish poses |
| `DishCalibIntent` | a client to the host | the dishes whose precision the client's player just set with the toolgun's calibration tool or the uncalibrator |
| `SavedSignalAppend`, `SavedSignalDelete`, `MeadowAppend`, `MeadowDelete`, `MeadowOrder` | any peer, relayed; order from the host | list rows by content hash; the database's order |
| `DeskVerb` | a client to the host; the host to that client | a desk press with what its button acts on as the client saw it; the verdict, the gloss and sounds the press makes, and the gloss and profile stat of a decode its start began |
| `PlayDeckEvent`, `CompState`, `CompData` | the presser; the host | playback edges; the refiner's state, its completions and its loaded signal |
| `DriveSlotState` | any peer to the host; the host to every other client, a line it accepted; the host to the source, answering a line it refused | a slot's occupant, inserted or ejected |
| `RackState`, `PhysModsState`, `FloppyBoxState` | any peer to the host; the host canonical | rack operations and arrays; the module set; the crate stack |
| `DrivePayload` | the host to all and to a joiner; a client to the host; the host to that client | a drive's row; a client's own new drive's row; the host's own row, answering a client row it refused |
| `EraserPressIntent` | a client to the host; the host to all, or to a presser | the eraser's delete, with the drive the presser saw seated; what the host's eraser did, for each client's eraser to show; a press the host refused |
| `ReelSlot`, `ReelPose` (stream), `ReelEjectIntent` | the presser; the host; a client | slot edges; the corrector; a reel birth |
| `LaptopState`, `LaptopQuad` | the presser and the host | the power edge and the portable PC's lid; the file buffer, with the slot's generation |
| `ServerState` | the host to all | the servers' broken set |
| `SatConsole` | a client to the host; the host to that client | a typed line with its terminal's context; the lines the host's run printed, and its busy state |

## Late join

The occupancy table, the sky-signal set, the desk scalars, the simulation vector, the dish snapshot
and an armed download's arm are sent at the joiner's ready edge, after the desk rows so a dependency
is never applied before its base, and a reset the host made since the joiner's world was captured
goes ahead of them all, the game's own order of a reset, a catch and an arm; the deck list, the
database and the emails are seeded as deltas against the blob instant; every drive slot, every drive
row that differs from its class default (a row that arrives before its drive waits for it through
the join), the rack, the module set, the reel slots, the laptop's power, its slot (before its file buffer) and the crate
arrive as canonical rows from the host, and so do the refiner's state and loaded signal, while the
joiner's own restore of a saved decode is refused; the desk's two loops are re-sent from component
truth.
A joiner never sees a running ping's stage visuals, only its outcome. A SAT console typist that leaves
mid-command while another client keeps the session leaves the command running on the host and, back,
is bound to its terminal and its busy state as its world is ready; the last client's leave ends the
session, and every such terminal is discarded with it, its command stopping there. An eraser press's
show lasts its 3 s and is not replayed; its wipe arrives as the drive's row. The upgrade levels
arrive with the save and never again.

## Known limits

| Limit | Evidence |
|---|---|
| The upgrade levels are not mirrored; a level bought mid-session diverges until the next join | `[V]` no lane exists; `coop/interactables/desk_sim_sync` names the gap |
| The coordinate log's animated line families are generated per peer from inputs that never mirror, so the host's and a client's logs differ; only the eight one-shot lines ride the wire, the reset's line written by each peer's own reset | `[V]` `coop/interactables/console_state_sync` |
| A signal whose download's `begin` spawns a world prop (lifecrystal's crystal at the ROZ ship) spawns it on each peer, and a client's own is a world birth it does not author: the host's arriving birth adopts it when the arm's row came first, and it stays beside the host's when the birth overtakes the arm on its own lane | `[RD]` `objectRenderer.begin`'s fourth step; the arriving birth's same-class match within 30 cm (`coop/props/remote_prop_spawn`) |
| The signal camera's trigger runs on each peer, as its arm's `begin` does; the main map's one, the locker looker, watches that peer's own camera and ends that player's game as in single player | `[RD]` `mainGamemode.deleteActiveSignal`, `objectRenderer.begin`, `trigger_lockerLooker`; by design until decided |
| The desk cursor has degraded to a few frames per second mid-session; two mechanisms were removed, and a warning names an occupancy flap if it recurs | `[?]` `coop/interactables/desk_cursor_sync`; not reproduced since |
| A level-3 refiner completion's world trigger (the evil's spawn, the rozship's, the deer's) runs on the host alone; a client sees what that object's own lane carries | `[V]` `coop/interactables/comp_sync`: the host is the one simulator; `[?]` each object's lane |
| The red phone's ring is per-peer randomness with no lane | `[V]` no lane exists |
| A client's own write to a drive whose row is still its class default is neither put back nor sent; no client-side writer of such a drive exists in the game (every writer is a verb the host runs, or a drive the client brings in) | `[RD]` the writer census of `prop_drive_C.data_0`; `coop/interactables/drive_payload_sync` |
| A SAT console command that spawns or destroys a class no lane carries (the rufus, the thiccfus, the llama's soul, the madness, the centipede, the murder kerfur) acts in the host's world only; its typist does not see it | `[V]` no lane exists for those classes |
| Each player's SAT console screen and log are its own; another player's typing is not shown | `[V]` by design until decided |
| A client's cheat menu (`uncalib`, `summonvirus`) and a virus its own save starts change only its copy of the dishes' precision, and the copy is put back at its next poll; the virus itself waits on a lane for the desk's `virusEvent` | `[V]` `coop/interactables/dish_calib_sync`; no lane carries `virusEvent` |
| The fakeGrays event's level trigger (`trigger_fakeLmaos`) breaks its dish's server and zeroes the dish on the machine where an allowed actor enters its volume; on a client the zero is put back and the break stays that client's | `[V]` the chain: the eventer's fakeGrays event arms the volume, whose first object is the trigger; `[?]` whether a client's body on the host's copy fires it |

## Code map

| Concept | Files |
|---|---|
| occupancy | `coop/interactables/device_occupancy` |
| the desk | `coop/interactables/desk_input_sync`, `coop/interactables/desk_sim_sync`, `coop/interactables/desk_ping_sync`, `coop/interactables/desk_cursor_sync`, `coop/interactables/desk_snd_fx`, `coop/interactables/console_state_sync` |
| signals, the catch, the dishes | `coop/interactables/signal_sync`, `coop/interactables/signal_wire`, `coop/interactables/signal_catch_sync`, `coop/interactables/dish_sync`, `coop/interactables/dish_calib_sync`, `coop/interactables/download_arm_sync` |
| the deck and the refiner | `coop/interactables/deck_play_sync`, `coop/interactables/comp_sync` |
| drives, racks, modules, tapes | `coop/interactables/drive_sync`, `coop/interactables/drive_payload_sync`, `coop/interactables/eraser_press_intent`, `coop/interactables/drive_rack_sync`, `coop/interactables/physmods_sync`, `coop/interactables/tape_caddy_sync` |
| the laptop, the crate, the database, the servers | `coop/interactables/laptop_sync`, `coop/interactables/floppy_slot_sync`, `coop/interactables/laptop_buffer_sync`, `coop/interactables/floppybox_sync`, `coop/interactables/meadow_db_sync`, `coop/interactables/serverbox_sync` |
| the SAT console | `coop/interactables/sat_console_sync`, `coop/interactables/sat_console_table` |
| the engine wrappers | `ue_wrap/desk/` (the dish, the console, the coordinate panel, the refiner pane, the drive chain, the tape caddy, the modules, the saved signals, the database, the audio, the SAT console) |
| the join seeds | `coop/session/join_seed` |
| tests and instruments | `coop/dev/drive_selftest`, `coop/dev/drive_drill`, `coop/dev/download_drill`, `coop/dev/desk_diag`, `coop/dev/sat_console_drill`, `coop/dev/calib_drill`, `coop/dev/desk_ping_drill`, `harness/autotest/autotest_seeddrill.cpp`, `harness/autotest/autotest_driveslot.cpp` (a drive taken out on one peer is out and carried on the other) |
