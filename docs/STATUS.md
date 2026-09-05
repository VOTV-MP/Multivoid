# Status, system by system

What is synced today, how it is owned, what a peer who joins mid-way receives, and how far each
row is. This is a count view, never a percentage: the number of facets a system has
is not enumerable, so a single "N% done" would hide which systems are solid and which are untouched.

**State** says how far the row is: `works` (established in play), `tested` (exercised by a scripted
run), `built` (the lane exists and has not been exercised), with the known breaks counted where
there are any; the breaks themselves are on the system's page. Most mechanic-level facets sit at
`built`; that majority is the honest state. **Owner**: `host` (the host writes, clients mirror), `peer` (each peer authors its own slice
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
| Player damage and hazards | enemy hits delivered to the hit peer; vitals as fractions | host delivers, peer computes | none needed | works |
| Death | the native death runs to its end; the level travel is refused and a revive written in its place | peer | none needed | tested |
| Teleport (dev) | host pushes a placement | host | the join placement | works |

## The world

| System | Synced | Owner | Late join | State |
|---|---|---|---|---|
| Physics props | spawn, pose, grab, carry, throw, drop, destroy; identity across saves and rejoins; client-born props | presser while held, host at rest | snapshot | works |
| Chip piles and clumps | the grab, carry, throw and re-pile cycle | host with client intents | snapshot plus a spawn-time bind | works |
| Trash-bits piles | the counter pair | presser and host | snapshot | built |
| Containers | open and close, contents (a slice of the host's object stack) | presser; host for contents | snapshot | works, two known breaks |
| NPCs | spawn, despawn, pose, state for the generic creatures | host | snapshot | works |
| Kerfur | the prop-to-NPC conversion cycle, per-kerfur skins | host | snapshot plus adoption | works |
| Owner-entity creatures | a creature whose AI reads the local player is owned per peer and mirrored to the rest | peer | keepalive | built |
| Roaches | the paged infestation state | host | snapshot | built |
| Wisp | the killer wisp's hunt, grab, tear | host | none (transient) | works |
| Pyramid | the walking-pyramid choreography | host | world-actor snapshot and replay | works |
| World actors | the event-spawned non-character actors | host | replay or seed | works |
| Drone | the delivery drone's flight and state | host | snapshot | built |
| Sky and time | sky rotation, moon phase, the clock (clients never free-run) | host | seed at connect | built |
| Weather | rain, snow, fog, wind, lightning, red sky, the event-born weathers | host | snapshot | tested, one known break |
| Fireflies, ambient spawners | cosmetic spawns and the flora and forage spawners (host only) | peer for cosmetics, host for spawners | none or parked at join | built |
| Story and scheduled events | host-observed fires replayed on clients by a per-event policy; the active-events registry mirrored for late joiners | host | replay and snapshot | built |
| Alarm | the base klaxon | presser | snapshot | built |
| Balance | the shared points total | host | replay at connect | built |
| Email, daily task | host-appended emails, peer-symmetric delete; the host's task state | host | save transfer plus a prime | built |
| Lamp posts | not synced: lockstep from the shared day and night cycle | local | none, by design | built |

## Devices and the workstation

| System | Synced | Owner | Late join | State |
|---|---|---|---|---|
| Doors, keypads, locks | door state and lock state; keypad digits mirrored, accept and deny replayed | host and presser | snapshot | works |
| Lights and light groups | switch state; the group's live state | presser; host for the group | snapshot | built |
| Power panels, turbine, grime, windows, appliances | the mask, the float, the decrease-only cleanliness, the one-bit states | presser or host | snapshot | built; windows tested |
| Device occupancy | who is using a device | arbiter | snapshot of the table | works |
| Desk input and console | field-granular input deltas, cooldown charges, the console text | presser; host relays | seed | works, five known breaks |
| Dish | the dish pose, the client's own simulation parked | host | snapshot | works, one known break |
| Signal catch | the catch as an intent the host replays | presser and host | seed | tested |
| Download and decode simulation | the host-run simulation's outputs | host | adopt | works, one known break |
| Playback deck | the play and stop edges | presser | none | tested |
| Drives and racks, physical modules, tapes, floppy box | slot and payload lanes with compare-and-swap at the host | arbiter | seed from the host's canon | built |
| Laptop | power, floppies and discs, the shared file buffer | presser; the buffer is arbitrated | seed | built; the buffer tested |
| Meadow database, saved signals | the signal database as a merge of both peers' saves | presser and host | seed | tested |
| Server boxes | the signal-server simulation state and its notices | host | snapshot | built |
| Shop orders | the client names a row, the host performs and prices it | arbiter | primed by a watermark | built |
| ATV | the driver authors the pose; a non-driving peer runs the rig natively and is corrected; condition (tyres, fuel, health) travels | driver, host for the rest | snapshot | tested; eject and configuration intents not built |
| Sleep | the sleep tally | arbiter | joins awake | built |
| Player inventory | per-peer, persisted by the host per player identity; the contents never cross the wire | local; host stores | seeded before the world | tested |

## Enforced without a packet

Moderation (kick and ban are a connection close plus a host-local list), save suppression (clients
never write a save), spawn authority (client-side shared-world spawners are parked), the no-pause
rule (a paused world is un-paused every tick while connected). They hold host authority by
construction; a census of wire lanes does not see them.

## What the table says at a glance

The solid core is the visible loop and the prop economy. The signal workstation is the
opposite: most of its lanes are only built, and the ones that have been played carry the most
known breaks. True arbiters are rare and listed by name
(occupancy, the racks, modules, floppy box, the laptop buffer, orders, sleep, containers); everywhere
else authority is host-authored one way or presser-authored with a host relay.
