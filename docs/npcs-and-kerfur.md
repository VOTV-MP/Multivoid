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
such as the wisp's fade and bob (`ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp`). The zombie family and the
fossilhound go further: their Blueprint's event graph is refused entry by entry on a mirror, but for the footstep and
the fossilhound's per-viewer loop (`coop/creatures/npc_mirror_brain`). The Geom Octa is parked the other way
round: its BeginPlay, tick, timelines and sounds run on a mirror, with its actor tick kept for the turn toward its
target, and only the twelve entries that decide (the wander, the target, the leave, the fire verdict) are refused
`[V]` (`octa_drill`). What the host decides crosses as `GeomOctaEvent` (`coop/creatures/geom_octa_sync` on the
host, `geom_octa_relay` on a client) and runs in the mirror's own Blueprint; a joiner mid-attack is replayed it
(`docs/coop-entity-expression-map.md`). The host
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

### Which spawners a client refuses

A connected client ticks no shared-world spawner and rolls no shared-world spawn RNG: shared
content arrives over the wire, or it is not shared. `coop/world/spawn_authority` is the single
owner of that suppression: each row of its table is a body a client must not run, a spawner's
tick or its entry function, refused at the script gate by a watch on the row's class and function
names. A row therefore holds for every instance, one that streams in late included, on every
dispatch route of the game thread, where the gate fires and every spawner body runs, and for a
class the game re-creates with each world. The refusal is gated on a running client session, so
leaving one leaves nothing to restore.

| Spawner | On a client | Why |
|---|---|---|
| insomniac, fossilhound | tick refused | the host mirrors the product |
| cockroach master and summoner | tick, summon and timers refused | its tick also drains and destroys shared food props; `coop/creatures/roach_sync` drives the client's population |
| mushroom master and spawner | spawn refused | they spawn at their own world transform; the host's mushroom crosses as a prop under its key, and a client's own spawner makes nothing on either peer `[V]` (`harness/autotest/autotest_mushroom`) |
| yellow wisp spawner | tick refused | anchors on a random navmesh point; the product is host-mirrored |
| sky wisp spawner | tick refused | absolute map coordinates, not player-anchored |
| night egg spawner | tick refused | a weighted night roll mints one egg at random absolute map coordinates; the host's egg mirrors through the source-gated catch |
| jellyfish path | spawn refused | its seven fish are made inside its graph; the host's run is mirrored through the source-gated catch |
| vent crawler, grays, eggs, tentacle balls | gameplay entries refused (BeginPlay, tick, overlaps, AI and despawn verbs); the AnimBP and the pose stay | on a client they exist only as host mirrors through the npc lane's allowlist. A mirror loses what its BeginPlay also set up: a ball's and a gray's mesh attach, a ball's ambient loop. The crawler's crawl is the host's (any player's body in its box starts it there), and a client plays what its graph plays beside the pose: the vent steps, the steps along the crawl, the metal break and its burst, the vent's swing and the signal room door's power, from one EventCue at the crawl's start (`coop/world/vent_crawler_sync`) A peer that joins mid-event gets every living creature from the npc lane's join census; the event's own scene is not replayed |
| gray controller, balls follower, super egger | the mint and activation bodies refused | their creature BeginDeferred calls are bytecode-internal: no spawn interceptor can see them, so the bodies must not run |
| firefly, pinecone | left running | anchored on the local player's camera: each peer owns its roll and the others render a mirror |
| eyer | left running | it stalks its own peer's player: each peer owns its eyer and the others render a mirror (`coop/creatures/owner_entity_sync`) |
| underground garbage | left running | per-peer loot mounds; refusing one would delete a client's loot with no host replacement |
| deer, walking tree | tick refused | they roll absolute map points, so the host rolls and the npc lane mirrors (`coop/creatures/ambient_creature_sync`); a client's own roll would be a second, unsynced creature |
| hexahive | tick refused | a hexahive is a field of dirt holes, so the host's holes cross by place (`coop/world/ambient_fixture_sync`) |
| beehive | the timer's handler (`spawn`) refused | it runs on a repeating timer with no tick to take off, so the row is on the handler. The host's branch crosses by place and by the key its nest was born with, its nest as a prop; a joiner binds the loaded branch of that key, spawns a copy only when none is, and a census-end ends the loaded branches the host lacks `[?]` |
| sus hole | left running | rolled by each peer's own spawner around its own camera and made by the host: a client's hole is destroyed at its birth and its place sent (`coop/world/sus_hole_intent`), and the host's holes cross by place (`coop/world/ambient_fixture_sync`) |
| walking mannequin | tick refused | its tick is the only root of the spawner's graph, so the walker's spawn, its delays and the reap of its walkers are refused with it `[?]`. The host's walker crosses as a prop, its brain gated on a client (every entry of its graph but the begin play and the destroy is refused, and its nav pawn takes no controller, `coop/creatures/walker_brain` `[?]`), and the host's drive stream carries its walking. A walker freezes on its target's gaze only: while a client is its target, the host answers the walker's own gaze reads (`bpCodeLib.isBoxInFrustum` once and the mesh's `WasRecentlyRendered` twice, post hooks that act only at the walker graph's pinned calls and are armed only while a client hunts a walker) with that client's level, and the client judges the game's own test on its copy each frame, only while a copy hunts it, and sends the level as `WalkerSeen` on each change (`coop/creatures/walker_gaze_host`, `walker_gaze` `[?]`); the walker's own timer then releases it, as in single player. When the target is the host the reads are the game's own. The host sends `WalkerState` {key, `looking`, target slot} on each edge of either field (the `looking` edge is the Blueprint's own damping write, a post hook on `SetAngularDamping` armed only while a walker is held, `coop/creatures/walker_state_host` `[?]`) and again to a joiner at its ready edge; a client writes `looking` onto its copy's `anim.stand` and holds a state whose copy is not born yet until the prop lane binds it (`coop/creatures/walker_state_client` `[?]`). The walker's anger does not cross: its `looking` is false while it is angry and crosses as such. The ticker's statue route runs on the host alone: the statue's end, the broken mannequin left in its place and the walker at a spawn point nobody sees each reach a client on their own lanes |
| bp7, bush, tick | left running | the product has no mirror of its own for these, and refusing a spawner whose output nothing replaces would just remove the content |
| winter bush ticker | spawn refused | its timer plants one bush on a random trace, and a bush is a `growingPlant_C`: the host's crosses as `PlantState` (`coop/world/plant_sync`, `docs/events-and-weather.md`), so a client's own would stand beside it under another key `[?]` |
| mushroom spore | hit refused | a spore's hit plants a `shroom_7` plant where it lands. Spores are not mirrored: each peer's broken puffball throws its own, and only the host's landing plants. The host's plant crosses as `PlantState`; a client's spore hit plants nothing on either peer `[V]` (`harness/autotest/autotest_mushroom`, driving the spore's hit on grass; the game's own thrown spores came to rest without planting on the rig) |
| outside church | herb roll refused | `trySpawnHerb` rolls the stolas herb while it rains; the host's herb crosses as `PlantState` `[?]` |
| plants, sprinklers, growth area | tick, the herb's `grown` and the sprinkler's watering refused | not spawners, but what changes a plant on its own: the host runs them and sends the result (`coop/world/plant_gate`); the growth area's glow, which its refused tick no longer spawns on a client, crosses as an EventCue born at the plant (`coop/world/event_cue_sync`) `[?]` |

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

There are two robots. The regular one, the kerfus, is a corded prop with three radial actions: its
brain runs on the host alone, a client shows the host's state and drive, its verbs are asked of the
host, and one a client turns on follows that client (`coop/creatures/kerfus_lanes`). The upgraded
one, the kerfur Omega, is a character with ten actions and is what follows.

It has two forms of two classes: the active robot, `kerfurOmega_C`, a character; and the
powered-off object, `prop_kerfurOmega_C`, a prop. Turning it off spawns the prop at the robot's
transform and destroys the character; turning it on does the reverse. Both run inside the
Blueprint, and the game gives the kerfur no stable identity: its key is
re-minted at random on every peer at every load. So the mod keeps one host-only kerfur id per
logical robot across both forms (`coop/creatures/kerfur_entity`). The rendered form is an
ordinary character or prop mirror at its own element id, and on a conversion the kerfur id is
rebound in place and the host broadcasts the one transition signal. Both verbs are Blueprint
self-calls the script-body gate sees on every route (`coop/creatures/kerfur_convert`): a client's
gate refuses its own turn-on or turn-off and asks the host, which runs the real verb, so a client
never converts a kerfur itself; the host's verb converges at its return on the successor actor
captured deterministically at its spawn through the gate's window
(`coop/creatures/kerfur_form_assembler`), and every client takes the new form from the broadcast.

The radial menu (follow, idle, patrol, fix the servers, get the reports, fix the transformers) is
relayed host-authoritatively (`coop/creatures/kerfur_command`): the host cancels the local
dispatch on both roles and re-runs the game's own verb with the kerfur serving the peer who asked
(`coop/creatures/served_player`), so its reads of the player answer that peer: follow follows it,
patrol circles it. A request waits while the host has no body for its sender, is refused when that body is out of an arm's reach of the Omega (the intent authority's reach, as a container act), and a verb the game
refuses changes nobody's service. Get the reports reads the asker's hand and destroys the disc it takes, so the press
carries the floppy disc the client holds, as its record, with the command (`coop/creatures/kerfur_command_wire`,
`coop/creatures/kerfur_disc_carry`): the host builds a copy of that disc in the puppet's hand for the verb's one call,
the game's own verb reads and destroys it, and the host answers that the disc was taken, on which the client's own
machine destroys its disc if it is still the one it held at the press. The copy is never published. A disc the Omega
already holds is not carried, and a Wh disc is not carried either (the game's own hint answers); a press with no
disc gets the game's refusal hint, which the host routes to that client (`coop/requester_feedback`, the verb runs
inside the requester's scope). A command the host refuses before running it, or holds, is answered to its requester as
`KerfurCommandAnswer` (`coop/creatures/kerfur_command_answer`), worded there in the player's language. A player's pick, and a
drill's reflected call, are caught at one seat, the script gate's watch on the Omega's `actionName`
(`coop/creatures/kerfur_command_seat`): a client cancels a relayed
verb there and asks the host, the host hands the kerfur back to its own player there. At a join a save-loaded kerfur prop is adopted by class and
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
| `KerfurCommand` | a client to the host | a radial verb with the sender's request number, which the host runs serving the sender |
| `KerfurCommandAnswer` | the host to the requester | the host's refusal of a command (busy, gone, murderous, out of reach, queue full, not an Omega, unavailable) or its wait for a body of the sender, as an enum the client words itself |

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
| The Omega's take-object, pat, equipment, kill and sit-on-the-ATV verbs are not synced | `[V]` `coop/creatures/kerfur_command`, the verb table |
| The Omega's floppy state is not synced, so a relayed get-reports diverges on its result; its accessories and carried object likewise | `[V]` no lane under `coop/creatures` carries them |
| Only one stalker class is owned per peer so far | `[V]` `coop/creatures/owner_entity_sync`, the member table |
| The conversion is detected by a poll, five times a second, not at the verb | `[V]` `coop/creatures/kerfur_convert`; the lane reads the result rather than the call, because a peer's own conversion has already happened by the time any watch of ours runs |
| Three ambient spawner families (bp7, the bush and the tick) are neither mirrored nor refused, so each peer grows its own | `[V]` they appear in no lane under `coop/creatures` and in no row of `coop/world/spawn_authority` |
| A walking mannequin freezes on the gaze of the player it hunts only, and a client's look reaches the host a moment late, so it can slide on for that long. It goes for the nearest player within its range and keeps that player until it is lost (the player left, died, or is beyond the range), and the host's walker lands the hit, which runs on the victim's machine with its sound: the host refuses its own 2D copy of a hit on a peer. Its anger does not cross: a copy holds the host's `looking`, which is false while the walker is angry | `[V]` `prop_wMannequin_C`'s tick and `timer_damage`; `coop/player/player_damage` (the hit relay and the sound hush), `coop/creatures/walker_target` (its target, and the walker served as that player: `coop/creatures/served_player`), `coop/creatures/walker_state_host`, `coop/creatures/walker_gaze_host` |
| A dirt hole's own end is each viewer's: past 10 minutes, one its viewer has not had on screen in the last second and is not within 50 m of goes for that viewer, and a client's copy can go while the host's stands. No hole's end crosses: each peer's copy ends by its own viewer, the host's included | `[V]` `susDirtHole_C`'s begin play; `coop/world/ambient_fixture_sync` |
| A client's sus hole roll can be lost: the host judges the point (100 to 500 m from the player's body, on grass by its own trace, at most 4 of that player's holes standing, a burst of 2 then one in 300 s) and a refusal costs that roll, the next being 300 to 600 s away. A drone's camera can be far from the body, so its roll is refused | `[?]` `coop/world/sus_hole_intent` |
| A deer's dissolve is each viewer's: one a client walked up to is gone for that client and stands on the host until the host comes near. A walking tree halts and wanders by the host's view alone, so a client can watch one move | `[V]` `deer_C` and `walkingTree_C` read local player and render state; the deer mirror keeps its actor tick (`coop/creatures/npc_mirror`) and its pose drive refreshes the floor that tick reads `[?]` |
| The host's own camera coming near a deer ends it on the host, and that end crosses as any NPC end: the deer is gone for every peer, however far from the host | `[I]` read from the deer's tick and the NPC end lane; no run measured it (the drill ends a deer on the host with a direct dissolve, not by the host's camera) |

## Code map

| Concept | Files |
|---|---|
| host-owned characters | `coop/creatures/npc_sync`, `coop/creatures/npc_mirror`, `coop/creatures/npc_world_enum`, `coop/creatures/npc_adoption`, `coop/creatures/npc_pose_host.cpp`, `coop/creatures/npc_pose_drive.cpp` |
| the engine wrappers | `ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp` |
| per-peer creatures | `coop/creatures/owner_entity_sync` |
| the killer wisp | `coop/creatures/wisp_attack_sync`, `coop/creatures/wisp_grab_hold`, `coop/creatures/wisp_tear_mirror`, `coop/player/ragdoll_gate` |
| roaches | `coop/creatures/roach_sync` |
| the kerfur | `coop/creatures/kerfur_entity`, `coop/creatures/kerfur_convert` with its host and client halves, `coop/creatures/kerfur_form_assembler`, `coop/creatures/kerfur_command`, `coop/creatures/kerfur_command_seat`, `coop/creatures/kerfur_command_answer`, `coop/creatures/kerfur_command_wire`, `coop/creatures/kerfur_disc_carry`, `coop/creatures/kerfur_prop_adoption`, `coop/creatures/kerfur_reconcile`, `coop/creatures/served_player` |
| the kerfus | `coop/creatures/kerfus_lanes` with `kerfus_brain`, `kerfus_state`, `kerfus_intent` and `kerfus_follow`; `ue_wrap/actors/kerfus` |
| tests | `harness/autotest/autotest_kwisp_probe.cpp`, `harness/autotest/autotest_wisplane.cpp`, `coop/dev/kerfur_convert_drill` (a client turns a kerfur off and on beside a disc of its own), `coop/dev/kerfur_serve_drill` (a client's commands to the Omega run for that client), `coop/dev/kerfus_drill` (a kerfus a client turns on follows it) |
