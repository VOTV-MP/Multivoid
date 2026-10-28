# Status, system by system

What is synced today, how it is owned, what a peer who joins mid-way receives, and how far each
row is. This is a count view, never a percentage: the number of facets a system has
is not enumerable, so a single "N% done" would hide which systems are solid and which are untouched.

**State** says how far the row is: `works` (established in play), `tested` (exercised by a scripted
run), `built` (the lane exists and has not been exercised); known breaks are counted where there are any,
and the breaks themselves are on the system's page. Most mechanic-level facets sit at `built`; that
majority is the honest state. **Owner**: `host` (the host writes, clients mirror), `peer` (each peer authors its own slice
and streams it), `presser` (whoever performs the action authors it, the host relays), `arbiter` (the
host validates and commits contested writes), `local` (never shared).

## The visible loop

| System | Synced | Owner | Late join | State |
|---|---|---|---|---|
| Session, join, roster | admission with a mutual key challenge and an optional password, slot assignment, the version gate, the roster and nicknames | host | the join itself, behind a world-ready barrier | works |
| Save transfer | the host's live world streamed to the joiner, then the connect snapshot of every synced element | host | this is the join | works |
| Remote player body | pose stream, ragdoll and faint display, the held item, footsteps | peer | the spawn is the seed | works |
| Nameplates, colours, skins | nickname, ping, health bar, visibility preference, colour, the body skin | peer, arbitrated by the host for uniqueness | at join | works |
| Chat and the event feed | text lines, join and leave lines, a retained history shown to late joiners | presser; host relays | seeded from the host's retained log | works |
| Voice | Opus frames, 3D positional | peer | state replay | works |
| Player damage and hazards | vitals as fractions; a zombie or fossil hound hit on a peer delivered to that peer `[RD]` | host delivers, peer computes | none needed | vitals work; the hit delivery is read from the code and not yet run |
| Death | the native death runs to its end; the level travel is refused and a revive written in its place | peer | none needed | tested |
| Teleport (dev) | host pushes a placement | host | the join placement | works |

## The world

| System | Synced | Owner | Late join | State |
|---|---|---|---|---|
| Physics props | spawn, pose, grab, carry, throw, drop, destroy, a hook's drag and a broom's push (built); identity across saves and rejoins; each prop's own save record, for the classes that keep one; client-born props, including what a player spawns from the sandbox menu | presser while held, host at rest and under a hook | snapshot; a player-authored birth is claimed at its seam and re-claimed by the host's echo, so a pending sweep cannot doom it | works |
| Sleeping bags and mattresses | a client interaction action sent to the host; the host runs the Blueprint that replaces the packed bag or mattress with its deployed form, or folds it back | host | host save and prop snapshot contain the currently live form | built (see [props.md](props.md)) |
| Chip piles and clumps | the grab, carry, throw and re-pile cycle, and a broom's sweep | host with client intents | snapshot, a spawn-time bind, and a bounded hold for a pile the save load has not reached; a clump rolling at the join arrives as the pile it lands as | works |
| Trash-bits piles | the counter pair | presser and host | snapshot | built |
| Containers | open and close, a dragged lid's pose (the held-item stream), contents (a slice of the host's object stack) | presser; host for contents | snapshot | works, two known breaks; a dragged lid is built, not yet proven on two peers |
| NPCs | spawn, despawn, pose, state for the generic creatures | host | snapshot | works |
| Kerfur | the prop-to-NPC conversion cycle, per-kerfur skins | host | snapshot plus adoption | works |
| Owner-entity creatures | a creature whose AI reads the local player is owned per peer and mirrored to the rest | peer | keepalive | built |
| Roaches | the paged infestation state | host | snapshot | built |
| Wisp | the killer wisp's hunt, grab, tear | host | none (transient) | works |
| Geom Octa | the host's night roll or dev spawn (the Octa and, from the same roll, the night egg), a mirror with its decisions parked, and the host's attack relayed into each client's own Blueprint (whom it hunts, each fire's outcome); its blast is the host's explosion, and a client's own roll is refused | host | the snapshot carries the Octa; one mid-attack sends the joiner an Aggro, replayed from the start | tested; a client's own menu or toolgun Octa stays local, an attack on an ATV is not relayed (its blast still arrives), a joiner's look runs about one chain behind |
| Pyramid | the walking-pyramid choreography | host | world-actor snapshot and replay | works |
| World actors | the event-spawned non-character actors | host | replay or seed | works |
| Drone | the delivery drone's flight and state | host | snapshot | built |
| Sky and time | sky rotation, moon phase, the sky eye, the clock and the day number (a client's clock never runs on its own: neither its own advance nor its cheat menu's day buttons roll a midnight; at the host's midnight it performs its own share: its profile's days, midnights since load, music flags and achievements) | host | the clock stream from the connect; the sky seeded at connect | built |
| Weather | rain, snow, fog, wind, lightning, red sky, the event-born weathers | host | snapshot | tested, two known breaks |
| Explosions | every explosion runs on the host and reaches each peer once; a client's own rocket, sticky bomb, held grenade or pipe bomb and pilled burn are requested by the client and performed on the host; a client's light on a salt rocket is performed on the host, and a lit rocket's flight is streamed to every peer | host | none (a one-shot act) | built; the other client-triggered sources are known limits with their owners |
| Fireflies, ambient spawners | cosmetic spawns and the flora and forage spawners (host only) | peer for cosmetics, host for spawners | none; a client's spawners are refused from its session's start | built |
| Story and scheduled events | host-observed fires replayed on clients by a per-event policy; the active-events registry mirrored for late joiners | host | replay and snapshot | built |
| Alarm | the base klaxon | presser | snapshot | built |
| Balance | the shared points total | host | replay at connect | built |
| Email, daily task | host-appended emails, peer-symmetric delete; the host's task state | host | save transfer plus a prime | built |
| Deployed hook and rope | existence, flight, the head pose and the reel while the thrower holds it; the handover to the host when both ends anchor, and the anchor itself; the tie, which exists on the host alone, so a prop any peer's hook drags streams to everyone and two hooks on one prop are two host constraints | peer while held, arbiter at the anchor, host after; the host for every constraint | keepalive while held; the save plus a ready-edge replay once anchored; a mirror re-bites from the state it is rebuilt from | built |
| Lamp posts | not synced: lockstep from the shared day and night cycle | local | none, by design | built |

## Devices and the workstation

| System | Synced | Owner | Late join | State |
|---|---|---|---|---|
| Doors, keypads, locks | a door's state, sent at its own verbs; a keypad's verbs, replayed from the host, and the state each chain settles on; a client's own door press, hit and pry and its keypad entries run on the host | host | snapshot | tested; a pry, a keycard and a pass changer built |
| Lights and light groups | switch state; the group's live state | presser; host for the group | snapshot | built |
| The power grid | the panel's breakers and lockout; each generator's break, wear, upgrades and repair puzzle | host; a client's presses, Activate presses, installs, hits and puzzle inputs are ops | snapshot | built |
| Power cords and sockets | a corded prop's socket link, including plug and unplug changes | arbiter: a client reports its player's plug, the host decides | every corded prop's link at the joiner's world-ready | built |
| Turbine, grime, windows, appliances | the float, the decrease-only cleanliness, the one-bit states | presser or host | snapshot | built; windows tested |
| The bay window's dirt | each sponge dab on its render target: the pixel, the edge, the brush's opacity and colour | presser, relayed by the host | the host's transferred save | built |
| Device occupancy | who is using a device | arbiter | snapshot of the table | works |
| Desk input and console | field-granular input deltas, cooldown charges, the console text | presser; host relays | seed | works, five known breaks |
| Dish | the dish pose, the client's own simulation parked; the precision, a client's own verbs sent to the host; the download's arm and reset, run on each client with the host's values | host | snapshot; the precision a seed | works, one known break; the precision tested |
| Signal catch | the ping's verdict as an intent the host rolls; the catch the host's, relayed as the pinger's | host | seed | tested |
| Download and decode simulation | the host-run simulation's outputs | host | adopt | works, one known break |
| Playback deck | the play and stop edges | presser | none | tested |
| Drives and racks, physical modules, tapes, floppy box | slot and rack lanes with compare-and-swap at the host; a drive's row the host's, a client's copy put back, a client's new drive's row sent to the host; the eraser's delete run by the host | arbiter | seed from the host's canon | built |
| Laptop | power, floppies and discs, the shared file buffer | presser; the buffer is arbitrated | seed | built; the buffer tested |
| Meadow database, saved signals | the signal database as a merge of both peers' saves | presser and host | seed | tested |
| Server boxes | the signal-server simulation state and its notices; a client's repair run on the host | host | snapshot | built |
| Disc slots | the slot a laptop or a signal server holds a disc in, and the disc that comes back out of one | host owns the slot; the peer whose game changed it reports the outcome | every device's slot at the barrier | tested |
| Shop orders | the client names a row, the host performs and prices it; the delivery queue is the host's, mirrored | arbiter | a reset and every queued order | built |
| ATV | the driver authors the pose; a non-driving peer runs the rig natively and is steered; condition (tyres, fuel, health) travels | driver, host for the rest | snapshot | tested; eject and configuration intents not built |
| Sleep | each peer's rest state and the host's sleeping flag | each peer for its own state; the host relays | the host's flag and every peer not awake | built |
| Player inventory | per-peer, persisted by the host per player identity; the contents never cross the wire | local; host stores | seeded before the world | tested |

## Enforced without a packet

Moderation (kick and ban are a connection close plus a host-local list), save suppression (clients
never write a save), spawn authority (client-side shared-world spawners are refused at the script-body gate), the no-pause
rule (a paused world is un-paused every tick while connected). They hold host authority by
construction; a census of wire lanes does not see them.

## What the table says at a glance

The solid core is the visible loop and the prop economy. The signal workstation is the
opposite: most of its lanes are only built, and the ones that have been played carry the most
known breaks. True arbiters are rare and listed by name
(occupancy, the racks, modules, floppy box, the laptop buffer, orders, sleep, containers); everywhere
else authority is host-authored one way or presser-authored with a host relay.
