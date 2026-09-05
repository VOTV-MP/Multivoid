# NPCs and the kerfur

## Purpose

The creatures: how a host-owned character reaches the other peers, the creatures each peer owns
for itself, the killer wisp, the roaches, and the kerfur robot, the one entity in the game that
is both a character and a prop. What the robot's owner can make it do across peers, and what is
not synced. The event actors that are not characters (the walking pyramid's body, the meteors)
are on the events page.

## How it works

### Host-owned characters

The host runs every shared-world creature's AI, and a client never spawns one of its own. A
client's spawn of an allowlisted character class is cancelled at the engine's deferred-spawn
call; on the host the same interceptor allocates the element, binds the actor once the spawn
finishes, and broadcasts it (`coop/creatures/npc_sync`). Two births that interceptor cannot see
are enrolled by other means (`coop/creatures/npc_world_enum`): characters that loaded with the
level, by a walk at world start, and characters a Blueprint spawns from inside its own graph (the
wisp swarm, the ambient sky wisps, the pyramid's spawner), caught at the native function seam and
gated by the spawning class. A save-persisted character, the kerfur above all, is materialised on
a joiner by the game's own save load at an unpredictable moment, so the joiner adopts its own
local twin by class after a deferred poll instead of spawning a second one
(`coop/creatures/npc_adoption`).

On a client the mirror is the game's actor with its brain parked: the AI timers neutralised and
the movement tick off, with the actor tick kept where the class's tick is per-viewer cosmetic,
such as the wisp's fade and bob (`ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp`). The host
streams a pose batch every tick, rotated fair-share when more characters are tracked than a batch
holds, and the mirror interpolates it in the same window a remote player uses
(`coop/creatures/npc_pose_drive`); a landing edge that depends on movement-component state is
replayed from the stream. A destroy on the host is broadcast; a character that destroys itself
inside its Blueprint is found dead by the host's pose walk and retired the same way.

### Creatures each peer owns

Some creatures are designed to stalk the player: the spawner rolls on the world, but the whole
behaviour loop reads the local player. Suppressing them on clients would delete the encounter,
and rolling them on the host would anchor them to the wrong player. So each peer keeps its native
roll, owns the entity it rolled, and broadcasts it so every other peer renders a display mirror
(`coop/creatures/owner_entity_sync`). The first member is the night stalker, the eyes that watch
from the dark.

### The killer wisp

The host's killer wisp always grabs and kills the host's own body: its verbs address the main
player, never the target it acquired. The host lane reads each wisp's attack state and classifies
the target through the player registry; for a client victim it neutralises the false grab on the
host (a damage cancel while any wisp holds a client, the fatality chain stopped) and sends the
victim a grab message, and the victim dies for real on its own machine after the host's fixed
delay (`coop/creatures/wisp_attack_sync`, `coop/creatures/wisp_tear_mirror`). The bodies go
where the native choreography puts them on every screen: the victim replays the capture against
its own player and its wisp mirror, and the host and the third peers hold the victim's puppet at
the wisp (`coop/creatures/wisp_grab_hold`). The tear plays on every peer's wisp mirror.

### Roaches

Roaches are not actors: they are mesh components of one world-anchored master, and their
simulation mutates shared props, eating food and destroying what it empties. A client running its
own would diverge the shared food, so the host runs it and clients mirror a paged snapshot; a
roach a client stomps or eats is a consumed intent to the host (`coop/creatures/roach_sync`).

### The kerfur

There are two robots. The regular one, the kerfus, is a corded prop with three radial actions and
has no sync of any kind. The upgraded one, the kerfur Omega, is a character with ten actions and
is what follows.

It has two forms of two classes: the active robot, `kerfurOmega_C`, a character; and the
powered-off object, `prop_kerfurOmega_C`, a prop. Turning it off spawns the prop at the robot's
transform and destroys the character; turning it on does the reverse. Both run inside the
Blueprint where no hook fires, and the game gives the kerfur no stable identity: its key is
re-minted at random on every peer at every load. So the mod keeps one host-only kerfur id per
logical robot across both forms (`coop/creatures/kerfur_entity`). The rendered form is an
ordinary character or prop mirror at its own element id, and on a conversion the kerfur id is
rebound in place and the host broadcasts the one transition signal. A conversion is detected by a
death-watch poll, a mirror whose actor died while its wire element is still present
(`coop/creatures/kerfur_convert`), and the successor actor is captured deterministically at its
spawn through the bytecode seam, so the converge no longer guesses by proximity
(`coop/creatures/kerfur_form_assembler`). A client's own turn-on or turn-off cannot be
prevented, so the client relays the request, the host runs the real verb, and the client claims
and adopts its own conversion ghost, parked and frozen, instead of destroying and respawning it.

The radial menu (follow, idle, patrol, fix the servers, get the reports, fix the transformers) is
relayed host-authoritatively (`coop/creatures/kerfur_command`): the host cancels the local
dispatch on both roles and re-runs the verb, and follow follows the peer who asked, where the
native follow pins the host's pawn. On a client the chosen verb is read at the E-input seam, the
one dispatched through the engine, before the local dispatch runs
(`coop/creatures/kerfur_menu_input`). At a join a save-loaded kerfur prop is adopted by class and
nearest pose once the load tail settles (`coop/creatures/kerfur_prop_adoption`), and a kerfur
that was off in the save but on by the time the joiner is ready has its stale off-prop retired by
an id the character's spawn carries (`coop/creatures/kerfur_reconcile`). The robot's thirty skins
are data-only subclasses, so the spawned class is the skin; the same bodies serve as player skins
on [players.md](players.md).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a shared-world character: existence, pose, death | the host | an interceptor on the spawn, a pose batch, a destroy broadcast; the client's spawns are cancelled |
| a stalker creature | the peer it rolled for | native on the owner, a display mirror elsewhere |
| the killer wisp's kill | the host detects, the victim dies | a grab message to the victim, the tear to all |
| the roach infestation | the host | a paged snapshot; a client's stomp is an intent |
| the kerfur's identity across forms | the host | one id, rebound in place, one transition signal |
| the kerfur's conversions and commands | the host | a client's verb is a request the host performs |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `EntitySpawn`, `EntityDestroy` | the host to all | class, id, transform, scale, whether save-persisted; the id |
| `EntityPose` (stream) | the host to all | a pose batch, fair-share rotated |
| `OwnerEntitySpawn`, `OwnerEntityPose`, `OwnerEntityDestroy` | the owner to all | a stalker's birth, pose and death |
| `WispGrab`, `WispTear` | the host to the victim; the host to all | the grab and its delay; the tear to play on the mirror |
| `RoachState`, `RoachConsumed` | the host to all; a client to the host | a page of the infestation; a stomped or eaten roach |
| `KerfurConvertRequest`, `KerfurConvert` | a client to the host; the host to all | a turn-on or turn-off to perform; the form transition with both ids, the form, the class and the pose |
| `KerfurCommand` | a client to the host; the host to all | a radial verb, and whom to follow |

## Late join

The host re-sends a spawn for every tracked character at the joiner's ready edge; the joiner
adopts the save-loaded twins by class and creates the rest. A kerfur prop is adopted by class
and pose after the load tail settles; a kerfur turned on during the window arrives as a character
whose spawn names the off-prop to retire. The roach infestation is re-sent as pages. A stalker
that another peer owns keeps announcing itself, so a joiner picks it up on its next keepalive. A
wisp in flight is transient and owes nothing.

## Known limits

| Limit | Evidence |
|---|---|
| The regular robot, the kerfus, is not synced at all; each peer's copy runs its own work queue | `[V]` its class is absent from the source |
| The Omega's take-object, pat, equipment, kill and sit-on-the-ATV verbs are not synced | `[V]` the capability census |
| The Omega's floppy state is not synced, so a relayed get-reports diverges on its result; its accessories and carried object likewise | `[V]` the capability census |
| Only one stalker class is owned per peer so far | `[V]` the lane's member table |
| The conversion is detected by a poll, five times a second, not at the verb | `[V]` the verb is invisible to every seam that can cancel |

## Code map

| Concept | Files |
|---|---|
| host-owned characters | `coop/creatures/npc_sync`, `coop/creatures/npc_mirror`, `coop/creatures/npc_world_enum`, `coop/creatures/npc_adoption`, `coop/creatures/npc_pose_host.cpp`, `coop/creatures/npc_pose_drive.cpp` |
| the engine wrappers | `ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp` |
| per-peer creatures | `coop/creatures/owner_entity_sync` |
| the killer wisp | `coop/creatures/wisp_attack_sync`, `coop/creatures/wisp_grab_hold`, `coop/creatures/wisp_tear_mirror`, `coop/player/ragdoll_gate` |
| roaches | `coop/creatures/roach_sync` |
| the kerfur | `coop/creatures/kerfur_entity`, `coop/creatures/kerfur_convert` with its host and client halves, `coop/creatures/kerfur_form_assembler`, `coop/creatures/kerfur_command`, `coop/creatures/kerfur_menu_input`, `coop/creatures/kerfur_prop_adoption`, `coop/creatures/kerfur_reconcile` |
| tests | `harness/autotest/autotest_kwisp_probe.cpp`, `harness/autotest/autotest_wisplane.cpp`, `coop/dev/kerfur_toggle` (the turn-on and turn-off drill), `python tools/mp.py npctest` |
