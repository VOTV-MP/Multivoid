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

Every desk verb dispatches inside the Blueprint where no hook fires, so the lanes poll the
resulting state instead of intercepting the verb. The desk's keyboard enters through one widget's
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
else can claim the desk mid-ping, and the outcome reaches every peer through the normal lanes.

The coordinate-panel cursor is a sixty-hertz unreliable stream from whoever is moving it,
interpolated on the mirror and written as a pure memcpy the widget repaints
(`coop/interactables/desk_cursor_sync`). The sounds the presser hears (key clicks, verb beeps,
the fail tone, the cursor and ping loops) are forwarded at the native audio seam, the component's
play and activate calls, as effect events (`coop/interactables/desk_snd_fx`); the two loops are
state and are re-sent to a joiner from the components' ground truth.

### Signals, the catch, the dishes

The sky-signal set is rolled on the host only; a client kills its own roller timer, keeps its
widget lifetimes wire-driven and reconciles its set to the host's snapshot
(`coop/interactables/console_state_sync`, which also carries the desk's live-visible scalars, the
committed dish-aim coordinates and the nine one-shot log lines). A catch is one host-validated
event whose identity half (the caught data, the sky-row delete, the machine reset, the sound) is
replayed on every peer (`coop/interactables/signal_catch_sync`); the unprimed change edge is the
authority, because a claim-gated detector lost a live catch to the hold's own release. The dish
theater is host-only: the client's dish simulation is parked, the host replays the slew and
streams the poses of all twenty-four dishes, the armed download's polarity is host-authored, and
calibration is a symmetric per-dish batch (`coop/interactables/dish_sync`).

### The deck list, playback, the refiner

The deck's saved-signal list is shadowed on every peer as content-hashed rows, diffed once a
second under the append-at-tail invariant, and mirrored as appends and content-keyed deletes
(`coop/interactables/signal_sync`, `coop/interactables/signal_wire`). Playback is a
presser-authored edge at the audio seam: the only activate site in the desk is the play verb and
the only deactivate is stop, so an organic activate is "someone started playback" and any peer may
stop (`coop/interactables/deck_play_sync`). The refiner has exactly one simulator, the peer whose
decode latched natively, which streams its state while decoding; every other peer is a passive
mirror that paints what nothing native repaints (`coop/interactables/comp_sync`).

### Drives, racks, modules, tapes, the laptop, the database

The drive chain is idempotent per-slot state lines any peer announces and the host canonicalises,
with the drive's payload as content rows the host stores (`coop/interactables/drive_sync`); the
rack is presser index-operations the host terminates, a full canonical array back, and a deny
ring for races (`coop/interactables/drive_rack_sync`). The desk's twelve physical modules are a
set, so plug and unplug are value operations the host applies and re-broadcasts whole
(`coop/interactables/physmods_sync`). The tape caddy's reel slots are presser-authored edges, and
its accrual is deterministic and clamped, so instead of a park the host re-snaps it once a second
(`coop/interactables/tape_caddy_sync`). The stationary PC's power and floppy axes are presser
edges with the host as the content author (`coop/interactables/laptop_sync`), its file buffer is
edit-script batches the host anchors and answers with a canonical
(`coop/interactables/laptop_buffer_sync`), and the disc crate is a stack of tail operations with
a deny that reaps the author's just-spawned disc (`coop/interactables/floppybox_sync`). The
laptop's signal database is a content-hash multiset with a host-canonical order, because the
store has a move verb (`coop/interactables/meadow_db_sync`). The signal servers' break-and-fix
simulation is host-owned and driven into each client's server boxes as state, so a client never
authors a false "server down" (`coop/interactables/serverbox_sync`).

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
| the ping | the presser's machine | one machine; observers keep the flag as bookkeeping |
| the cursor | whoever moves it | a sixty-hertz stream |
| the desk's sounds | the presser | effect events at the audio seam |
| the sky-signal set, the catch, the dishes | the host | roller, host-validated event, parked client sim |
| the deck list, the database | every peer's shadow | content-hashed appends and deletes; the host's order |
| playback | the presser; anyone may stop | edge events |
| the refiner | the one machine whose decode latched | a state stream |
| drives, racks, modules, the crate, the file buffer | the host, canonical | any peer's operations, the host's array back |
| the tape accrual | each peer, host-corrected | a one-hertz re-snap |
| the servers | the host | state driven into each box |
| the upgrade levels | the host's save, once | not mirrored |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `DeviceClaim` | a client to the host; the host to all | a claim, a release, the busy table |
| `DeskInput`, `DeskScanEvent`, `DeskState`, `DeskLogLine` | the presser, relayed | a field delta; a quick-scan; the desk scalars; a one-shot log line |
| `DeskSimPose` (stream), `DeskCursorPose` (stream) | the host to all; the mover to all | the simulation outputs; the cursor |
| `DeskSndFx` | the presser, relayed | an audio effect event |
| `SkySignalState`, `SkySignalCatch`, `DishAimState` | the host; the catcher and the host; the occupant | the signal set; a catch; committed coordinates |
| `DishArm`, `DishSnapshot`, `DishCalib`, `DishPose` (stream) | the host; the host to a joiner; any peer; the host | the armed download; all dishes for a joiner; a calibration batch; dish poses |
| `SavedSignalAppend`, `SavedSignalDelete`, `MeadowAppend`, `MeadowDelete`, `MeadowOrder` | any peer, relayed; order from the host | list rows by content hash; the database's order |
| `PlayDeckEvent`, `CompState`, `CompData` | the presser; the simulator | playback edges; the refiner's state and its loaded signal |
| `DriveSlotState`, `DrivePayload`, `RackState`, `PhysModsState`, `FloppyBoxState` | any peer to the host; the host canonical | slot lines; payload rows; rack operations and arrays; the module set; the crate stack |
| `ReelSlot`, `ReelPose` (stream), `ReelEjectIntent` | the presser; the host; a client | slot edges; the corrector; a reel birth |
| `LaptopState`, `LaptopBlob`, `LaptopQuad` | the presser and the host | power and floppy edges; content streams; the file buffer |
| `ServerState` | the host to all | the servers' broken set |

## Late join

The occupancy table, the sky-signal set, the desk scalars, the simulation vector and the dish
snapshot with any armed download are sent at the joiner's ready edge, after the desk rows so a
dependency is never applied before its base; the deck list, the database and the emails are
seeded as deltas against the blob instant; every drive slot and payload, the rack, the module set,
the reel slots, the laptop's power and content and the crate arrive as canonical rows from the
host; the desk's two loops are re-sent from component truth. A joiner never sees a running ping's
stage visuals, only its outcome. The upgrade levels arrive with the save and never again.

## Known limits

| Limit | Evidence |
|---|---|
| The upgrade levels are not mirrored; a level bought mid-session diverges until the next join | `[V]` no lane exists |
| The coordinate log's animated line families are generated per peer from inputs that never mirror, so the host's and a client's logs differ | `[V]` measured line counts on a stress run |
| The desk cursor has degraded to a few frames per second mid-session; two mechanisms were removed and the residual is attributed by a warning if it recurs | `[?]` not reproduced since |
| A refiner completion fires world triggers on the one simulating machine only; other peers mirror the state | `[V]` by design of the single simulator |
| The desk lanes from the input fix onward were verified by the rig and its self-tests, and by one hands-on that found real breaks whose fixes shipped without a replay | `[V]` the status page counts them |
| The red phone's ring is per-peer randomness with no lane | `[V]` open, low |
| The gauge sounds derive from speeds that ride no lane on a mirror; the effect forward covers the presser's one-shots and loops | `[?]` |

## Code map

| Concept | Files |
|---|---|
| occupancy | `coop/interactables/device_occupancy` |
| the desk | `coop/interactables/desk_input_sync`, `coop/interactables/desk_sim_sync`, `coop/interactables/desk_cursor_sync`, `coop/interactables/desk_snd_fx`, `coop/interactables/console_state_sync` |
| signals, the catch, the dishes | `coop/interactables/signal_sync`, `coop/interactables/signal_wire`, `coop/interactables/signal_catch_sync`, `coop/interactables/dish_sync` |
| the deck and the refiner | `coop/interactables/deck_play_sync`, `coop/interactables/comp_sync` |
| drives, racks, modules, tapes | `coop/interactables/drive_sync`, `coop/interactables/drive_rack_sync`, `coop/interactables/physmods_sync`, `coop/interactables/tape_caddy_sync` |
| the laptop, the crate, the database, the servers | `coop/interactables/laptop_sync`, `coop/interactables/laptop_buffer_sync`, `coop/interactables/floppybox_sync`, `coop/interactables/meadow_db_sync`, `coop/interactables/serverbox_sync` |
| the engine wrappers | `ue_wrap/desk/` (the dish, the console, the coordinate panel, the refiner pane, the drive chain, the tape caddy, the modules, the saved signals, the database, the audio) |
| the join seeds | `coop/session/join_seed` |
| tests and instruments | `coop/dev/drive_selftest`, `coop/dev/desk_diag`, `harness/autotest/autotest_seeddrill.cpp` |
