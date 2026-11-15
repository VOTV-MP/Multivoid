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
wisp swarm, the ambient sky wisps, the pyramid's spawner, the ambient killer wisp's and fossilhound's tickers,
the boar war event's hound, the gray tank box's fire tank, the treehouse's pig beater, shooters and slapper, the dropped
gun's stealer, the cave's antibreather and Wolfgang), caught at the native function seam and
gated by the spawning class. A save-persisted character, the kerfur above all, is materialised on
a joiner by the game's own save load at an unpredictable moment, so the joiner adopts its own
local twin by class after a deferred poll instead of spawning a second one
(`coop/creatures/npc_adoption`).

On a client the mirror is the game's actor with its brain parked: the AI timers neutralised and the movement tick off, with the actor tick kept
where the class's tick is per-viewer cosmetic, such as the wisp's fade and bob (`ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp`). The zombie family and
the fossilhound go further: their Blueprint's event graph is refused entry by entry on a mirror, but for the footstep and the fossilhound's per-viewer
loop (`coop/creatures/npc_mirror_brain`). The Geom Octa, the cave's antibreather and Wolfgang are parked the other way round: their BeginPlay, tick,
timelines and sounds run on a mirror, and only the entries that decide (the wander, the target, the leave, the fire verdict: twelve for the Octa, four
let through for the cave pair, see The cave creatures) are refused `[V]` (`octa_drill`). What the host decides crosses as `GeomOctaEvent`
(`coop/creatures/geom_octa_sync` on the host, `geom_octa_relay` on a client) and runs in the mirror's own Blueprint; a joiner mid-attack is replayed it
([coop-entity-expression-map.md](coop-entity-expression-map.md)). The host streams a pose batch every tick, rotated fair-share when more characters
are tracked than a batch holds, and the mirror interpolates it in the same window a remote player uses (`coop/creatures/npc_pose_drive`); a landing
edge that depends on movement-component state is replayed from the stream. A destroy on the host is broadcast; a character that destroys itself
inside its Blueprint is found dead by the host's pose walk and retired the same way.

### Creatures each peer owns

Some creatures are designed to stalk the player: the spawner rolls on the world, but the whole
behaviour loop reads the local player. Suppressing them on clients would delete the encounter,
and rolling them on the host would anchor them to the wrong player. So each peer keeps its native
roll, owns the entity it rolled, and broadcasts it so every other peer renders a display mirror
(`coop/creatures/owner_entity_sync`). The first member is the night stalker, the eyes that watch
from the dark, which the black fog mints too. Every spawn of one is made inside a Blueprint graph, so the owner
catches it at the deferred spawn's native seam and announces it at its next tick, placed. A copy keeps its blink
and loses the class's own ends (the `despawn`, `del` and `addAnger` timers): it stands until its owner's eyer
ends, and a stare at a copy angers nothing `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=spawnBlackFog:50`, which makes the
host's fog full and calls its `spawnGhost`: four eyes announced, each copied once on the client and kept).

Two more of the game's scares read one player and are left per player, unmirrored `[RD]`. The zombie deer's ghost (`deer_ded_C`, a `deer_C` child
minted by the carcass prop `prop_deaddeer_c` five to ten minutes after the carcass's begin play, at night) paths to player 0's camera, hurts player 0
and dissolves when a clear line runs to that camera; the carcass is a prop on every peer, so each peer rolls its own ghost for its own player, and the
join's world walk skips the class by name (`coop/creatures/npc_world_enum`). The Kerfur Omega's sleep scare is a marker (`ticker_kerf_C`, minted at
the origin by the `ker` signal) that the Omega's `timer_kerf` consumes only while the local game mode is asleep, by walking to player 0's camera: the
Omega is the host's on the npc lane, so a client sees it walk, and the scare itself is the host's sleep. Neither is a member of the per-player lane.

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
| insomniac | tick refused | the host mirrors the product |
| fossilhound | tick refused | the ticker rolls at an absolute map point by night (day 7 on, moon not new), so the host rolls: `ticker_fossilhoundSpawner_C` is a spawn source of the character lane and the hound is mirrored with its graph parked (`coop/creatures/npc_mirror_brain`) `[V]` (the `fossilhoundTick` drill, `coop/dev/event_fire_drill`: the host's tick made a hound and enrolled it, the client's tick was refused once, the client made the copy and refused its begin play). The boar war event (`event_fossilBoarWar_C`, minted by the host's midnight roll, which a client never reaches) is a second source row for the same hound `[RD]`: it cannot be forced, so no run has shown it. The boars it throws out are props and need no row: each boar's begin play adds it to the gamemode's `allGrayboars`, which `coop/creatures/boar_sync` reads |
| cockroach master and summoner | tick, summon and timers refused | its tick also drains and destroys shared food props; `coop/creatures/roach_sync` drives the client's population |
| mushroom master and spawner | spawn refused | they spawn at their own world transform |
| yellow wisp spawner | tick refused | anchors on a random navmesh point; `ticker_yellowWispSpawner_C` is a spawn source of the character lane, narrowed to its `killerwisp_C` (the same graph also drops a deer carcass's gibs, props the host keeps to itself `[?]`) `[V]` (the `yellowWispTick` drill, `coop/dev/event_fire_drill`: the host's tick made a killer wisp and enrolled it, the client's tick was refused once and the client made the copy) |
| gray tank box | `spawnParts` refused | `firetankDeploy_C` (the box a Day-44 tank saucer drops) unfolds in `spawnParts` into the fire tank and its scrap props, so it is the tank's spawn source (`coop/creatures/npc_world_enum`), the tank's copy is parked as the boar war's is (`coop/world/copy_graph_gate`), and a client's own unfolding is refused. No lane carries the box itself to a client, and a client never has one to unfold, so the refusal has not been exercised `[RD]`. The host's unfolding was shown `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=graystankUnfold`, which calls the saucer's own `forceDeploy` and the box's own `spawnParts`): the host enrolled the `firetank_C` as a world actor, its five scrap props crossed as props, and the client made the tank's copy and refused its graph entries |
| sky wisp spawner | tick refused | absolute map coordinates, not player-anchored |
| night egg spawner | tick refused | a weighted night roll mints one egg at random absolute map coordinates; the host's egg mirrors through the source-gated catch |
| jellyfish path | spawn refused | its seven fish are made inside its graph; the host's run is mirrored through the source-gated catch |
| vent crawler, grays, eggs, tentacle balls | gameplay entries refused (BeginPlay, tick, overlaps, AI and despawn verbs); the AnimBP and the pose stay | on a client they exist only as host mirrors through the npc lane's allowlist. A mirror loses what its BeginPlay also set up: a ball's and a gray's mesh attach, a ball's ambient loop. The crawler's crawl is the host's (any player's body in its box starts it there), and a client plays what its graph plays beside the pose: the vent steps, the steps along the crawl, the metal break and its burst, the vent's swing and the signal room door's power, from one EventCue at the crawl's start (`coop/world/vent_crawler_sync`) A peer that joins mid-event gets every living creature from the npc lane's join census; the event's own scene is not replayed |
| ariral follower | nothing to refuse: a client runs no eventer verb | `trigger_spawnFollowingArir_C` mints the follower inside its graph, so it is a spawn source of the character lane; the host's follower is mirrored, and a copy runs none of its graph (`coop/creatures/npc_mirror_brain`): the walk to player 0, the slap, the take of a held item and the doors it opens are the host's follower's, and the host answers its reads of player 0 with the player nearest to it (see Hunters go for the nearest player), where it followed and slapped the host's player only `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=arirFollower:60`: the host enrolled it, the client made its copy and refused the copy's graph entries; with the hunt drill below the follower was recorded as hunting slot 1, 8373 reads of player 0 were answered with the client's puppet, and it closed from 600 cm to 265 cm of it) |
| Ariral treehouse | the eater sphere's timer (`checkSurroundings`) and the two shooter rolls (`trySpawnPigShooter`, `trySpawnTankShooter`) refused | `treehouse_C` is a level actor anchored at its own point, so its Arirals are the host's: it is a spawn source of the character lane, narrowed to the pig beater, the shooter and the slapper, and each copy is parked (`coop/creatures/npc_mirror_brain`, see The Arirals). `trySpawnPigShooter` mints a shooter when two or more gray boars stand within 100 m of the treehouse and the pig beater when one does; `trySpawnTankShooter` mints a shooter locked on a fire tank within 50 m. `checkSurroundings` destroys the poo, gun, shrimp and food lying in the eater sphere and pays the Ariral reputation for each, mints the slapper for a prop_poo and throws a camera back, and only while player 0 is more than 50 m off, so a client's own run would pay and destroy again beside the host's. The Ariral crate's use also calls `trySpawnTankShooter`: a client's call returns no shooter and its own slapper follows (below) |
| cave spawner | the 10 s roll of `antibreatherSPawner_C` (entry 30 of its graph) and its `cencipede` (entry 3593) refused | see The cave creatures: the antibreather and Wolfgang are the host's; the spawner's begin play, rock timer and music trigger stay `[V]` (the `caveWolfgang` drill) |
| gamemode rolls of the world's events | `ticker_lockerHead`, `ticker_radioTowerPoof` and `ticker_lakeMonsert` (timer handlers of `mainGamemode_C`), the downloading panel's `redPhoneEventTimer` and the Day-13 tower's `a__FinishedFunc` (its mail loop) refused; a client's own meat-rain spawn is refused at the deferred-spawn seam by its class | see [events-and-weather.md](events-and-weather.md): the host's result reaches each peer as a cue or on the sky lane, and a client's own roll would stand beside it |
| gamemode UFOs, the body, the mushroom duende | `ufo_ball`, `ufo_boo`, `ufo_joel`, `ufo_pill`, `ufo_midas`, `tickerBody` and `tickerFunguy` (timer handlers of `mainGamemode_C`) refused, and a body copy's sphere overlap | none of them is bound to a player once it stands: the UFOs fly from a spawn point (`ufo_ball` places it over player 0's camera, `ufo_joel` by the radio tower's top), the body is a still mesh at a random ground point, the duende is rolled at a random map point and wanders, so they are the host's. `mainGamemode_C` is a spawn source of `npc_funguy_C` (a character-lane mirror whose whole graph is parked: its sensing, wander, dig and despawn are the host's, `coop/creatures/npc_mirror_brain`) and of `theBody_C`, `ufo_ballfo_C`, `ufo_boofo_spawn1_C`, `ufo_joel_C` and `ufo_pillfo_C` (world-actor mirrors, [events-and-weather.md](events-and-weather.md)). The Midas UFO is a level actor, so its timer (which only runs its trigger) is refused and the host's start is a cue. `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=gmUfoBall:100` and the like, `coop/dev/gamemode_drill`: `gmUfoBoo`, `gmUfoPill`, the body and `gmFunguy` too): the host's own handler made the actor and enrolled it, the client's own 1500 calls were refused and made nothing, and the client made the copy. `ufo_joel` rolls only with a player at the radio tower's top, so the drill stood the class and enrolled it by hand, and the seam itself is `[RD]` |
| base computer signal spawns | `analogDScreenTest_C.spawn_trifo` refused (the desk's `autoSave` and the save press are refused by the desk lanes), and, on a copy, `powercontrol_C.solar` when a TriFO's own graph calls it, and `lib.setEvent` from a TriFO or a Tardis | the Tardis, the TriFO and the shitting duende are world-anchored, so the host's are world-actor mirrors, minted by the desk (`analogDScreenTest_C` is a spawn source narrowed to the three: `coop/creatures/npc_world_enum`); the copies keep their own tick and presentation, and the Tardis is born with the host's remaining delay (`coop/world/signal_spawn_copy`). See [events-and-weather.md](events-and-weather.md) `[V]` (the `sigTardis`, `sigTrifo` and `sigDuende` drills, `coop/dev/signal_drill`) |
| Shadow Figura | left running | `figura_C` (mainGamemode `CustomEvent_3`) is bound to one player: it spawns on a 200 m ring about player 0's pawn, its `target` is that player's camera, it walks to it and ends within 1.75 m of it. Each peer owns its own and the others render a parked copy (`coop/creatures/owner_entity_sync`, class 1; the copy's `CustomEvent_0` timer is cleared) `[V]` (the `gmFigura` drill, `coop/dev/gamemode_drill`: each peer's own figura appeared on the other as a copy that followed its owner's path) |
| Red Skeleton, the Walker (`newBlueprint4_C`, `newBlueprint19_C`) | left running, unmirrored | the two rare scares of `mainGamemode_C`'s timers `CustomEvent_4` (every 300 s, a 1 in 65536 roll, a random navmesh point) and `CustomEvent_7` (every 21600 s, a point 50 to 65 km out). Both read the local player alone `[RD]`: `newBlueprint4_C` walks to `getPlayerPawn(0)` (its begin play's move-to), and its `checkProx` takes `getPlayerCameraManager(0)`'s distance, at 200 cm flashes the screen red and ragdolls `lib.getMainPlayer`; while the game is paused its begin play's loop presses the left mouse button on player controller 0 (the wiki's "clicks resume"); the roll itself wakes a sleeper (`issleep` -> `wakeUp`, the hint "escape"). `newBlueprint19_C` adds movement input toward player 0's pawn each tick, shows its mesh only while its own camera saw it, and at 100 cm calls `setGamePaused(true)` and mints `newBlueprint5_C` at the camera; without its `condition` flag its begin play ends the game (`lib.end`, `quitGame`). A copy on another peer would pause, ragdoll or quit that peer, so each peer rolls its own and no lane carries either (the two are matched by behaviour, as the capture holds no name for them) |
| dropped gun's Ariral | tick of `event_arirStealsGun_C` refused | the event actor is on both peers and ticks every frame; from dusk to dawn from day 24, with a `prop_arirGun_C` about, it mints `npc_arirGunStealer_C` in a ring about player 0, for a gun that is a world prop, so the host's tick rolls and is the stealer's spawn source (`coop/creatures/npc_world_enum`). The stealer is a `Character` with no allowlisted parent, so its class is a late allowlist entry (`kNpcAllowlistLate`: the install does not wait for it and it joins the walk when a spawn of it is first seen), and its copy is parked |
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
| winter bush ticker | spawn refused | its timer plants one bush on a random trace, and a bush is a `growingPlant_C`: the host's crosses as `PlantState` (`coop/world/plant_sync`, [events-and-weather.md](events-and-weather.md)), so a client's own would stand beside it under another key `[?]` |
| mushroom spore | hit refused | a spore's hit plants a `shroom_7` plant where it lands; a client's spores are the host's mirrors, and the host's plant crosses as `PlantState` `[?]` |
| outside church | herb roll refused | `trySpawnHerb` rolls the stolas herb while it rains; the host's herb crosses as `PlantState` `[?]` |
| plants, sprinklers, growth area | tick, the herb's `grown` and the sprinkler's watering refused | not spawners, but what changes a plant on its own: the host runs them and sends the result (`coop/world/plant_gate`); the growth area's glow, which its refused tick no longer spawns on a client, crosses as an EventCue born at the plant (`coop/world/event_cue_sync`) `[?]` |

### The killer wisp

A killer wisp comes from two spawners, both the host's: the pyramid's four, and the ambient one a ticker rolls on a navmesh point at night between
days 25 and 29 (`ticker_yellowWispSpawner_C`; up to four live at once). Both are spawn sources of the same lane, so they are enrolled and mirrored
alike, and everything below keys on the wisp's class, not its spawner `[RD]`: the drill showed an ambient wisp's birth and its mirror, and did not
bring one to a player.

The host's killer wisp always grabs and kills the host's own body: its verbs address the main player, never the target it acquired. The host lane
reads each wisp's attack state and classifies the target through the player registry; for a client victim it neutralises the false grab on the host
(a damage cancel while any wisp holds a client, the fatality chain stopped) and sends the victim a grab message, and the victim dies for real on its
own machine after the host's fixed delay (`coop/creatures/wisp_attack_sync`, `coop/creatures/wisp_tear_mirror`). The bodies go where the native
choreography puts them on every screen: the victim replays the capture against its own player and its wisp mirror, and the host and the third peers
hold the victim's puppet at the wisp (`coop/creatures/wisp_grab_hold`). The tear plays on every peer's wisp mirror.

### The Arirals

Which Ariral is the host's and which stays a player's is judged spawner by spawner from the bytecode: what is anchored in the world is the host's, what
reads one player's camera, bed or use stays that player's.

| Ariral | Spawned by | Judged | Why |
|---|---|---|---|
| shooter | `treehouse_C` (`trySpawnPigShooter`, `trySpawnTankShooter`), `boarInvasion_C` | host's | anchored at the treehouse and aimed at fire tanks, gray boars and murder kerfurs, never at a player: its sense and fire timers read `firetank_C`, `grayboar_C`, `murderKerfur_C` only |
| pig beater | `treehouse_C` | host's | its own graph names the nearest gray boar its target and re-picks one when it dies (`targetDestroyed_d`, 0.2 s after), and runs away when the nearest boar is 100 m off or none is left; every move, slap and door after that is the follower graph it inherits |
| slapper, from the eater sphere | `treehouse_C.checkSurroundings` | host's | the treehouse reacts to a prop_poo in its sphere, a world prop, at the treehouse's own `climb` point |
| gun stealer | `event_arirStealsGun_C` | host's | the event actor is on both peers; the stealer is made for a gun that is a world prop (`GetActorOfClass(prop_arirGun_C)`), walks to it by the gamemode's `arirgun` key and destroys it, and the ring it is minted in is only taken about player 0 |
| slapper, ATV prank | `fuelingArir_C` | a player's | `checkVisible` line-traces from the Ariral's eye to player 0's camera and acts (`runaway`, the gas can drop) only when that line is clear |
| slapper, crate | `prop_arirContainer_v2_C` | a player's | the use of the crate by one player: it shakes the opener's view, drops what he holds and punches him a moment later, and its slapper is made 100 m from the crate |
| slapper, stolen prop | `slapperSummoner_C`, a component on the props of the campsite | a player's | it reads player 0's camera against both the prop and its start point (50 m), and arms only while the gamemode's `slappers` count is zero and no boar war runs, so each peer's own copy of the component answers to its own player |
| slapper, sleep | `mainGamemode_C.wakeUp` | a player's | the sleeper's own bed (`maingamemode_c:bed`), `arirfeeding` and `arirhit`, read at his own wake |
| slapper, notepad | `prop_notebook_busterSpawner_C` | a player's | made in the notepad's own `actionOptionIndex` for whoever used it, with `buster` set: it batters what is in its way, doors and walls (the follower graph's hit loop, `door_c.hack`, `customWall_c.damage`), and then him |

The host's own slapper from the five player-bound spawners is not sent either: nothing but the owner's machine can run an Ariral that reads its
player, and no owner lane carries these `[RD]`. A client's own run of those spawners is its own, so the client's local slapper is not parked,
suspended or suppressed (the class is allowlisted through its parent, the follower, but its spawns are bytecode-internal, which the deferred-spawn
interceptor does not see).

On a client every one of the host's Arirals is a display copy: the follower, the pig beater, the slapper, the shooter and the stealer are parked with
no entry of their graph let through (`coop/creatures/npc_mirror_brain`; a pig beater's mirror refuses its own graph and the follower graph under it).
The follower's graph is where an Ariral walks to player 0, slaps, takes a held item, destroys a poo, and opens the doors and lockers in its way; the
shooter's registers an event (`lib::setEvent`, which a client must not raise for the host's) and arms the sense and fire timers; the stealer's walks
it to the gun, slaps and shoves what it touches, opens lockers and destroys the gun. A copy shows the host's pose and animation only.

What the Arirals throw or carry is props, and crosses on the prop lane when the host mints it: the gas can `fuelingArir_C` drops, the lantern and the
notepad the Lantern Prank leaves (`coop/dev/signal_drill`: the host's `prop_lamp_blinker_C` and `prop_notebook_busterSpawner_C` were broadcast and the
client made both; `arirBusterSpawner_C` destroys itself in its own begin play and its child actors outlive it, so the event arm makes no creature),
the gun the stealer destroys, and the props a slapper puts back (`stolenthing`). A shooter's shot is not a prop: it is an `arirgun_shoot_c`, an actor
whose begin play traces, damages what implements the object interface within about 10 m of the hit, pushes the physics bodies there and hurts a fire
tank. No lane carries it, and a client's copy of the shooter fires none, so the host's shots show on a client only in what they do (a boar's or a fire
tank's health by the hit lane, a prop's motion by the prop lane) `[?]`; a cue per shot would show the beam itself and is not built.

`[V]` `VOTVCOOP_EVENT_FIRE_DRILL=treehouseShooter:150` and `treehousePig:100` (each peer stands its own `treehouse_C` at the world's origin, since the
test save holds none, and the host stands the boars and calls `trySpawnPigShooter` itself): the host enrolled the shooter (two boars) and the pig
beater (one) each with `source 'treehouse_C'`, the client made each copy and refused its graph (`npc_ariral_shooter_C mirror entry 1318 refused`, the
pig beater's 403), and the client's treehouse had `checkSurroundings` refused on its timer and the two shooter rolls refused when the drill called them
with two boars about, which minted no shooter of the client's own. `[V]` `stealsGunTick:100` (the drill stands a `prop_arirGun_C` and runs the event
actor's own tick at 22:00 on day 24): the host enrolled `npc_arirGunStealer_C` with `source 'event_arirStealsGun_C'` in a ring about its own player, the
client's own event actor's tick was refused, and the client made the copy and refused its begin play. The treehouse's slapper (a poo in the eater
sphere) and the tank shooter (a fire tank near the treehouse) were not forced `[RD]`: neither a poo nor a fire tank was stood.

### Hunters go for the nearest player

The host runs every shared creature's brain, and a Blueprint that reads "the player" as player 0 hunts the host alone: a client standing beside it is
ignored. One that finds its prey by overlap, pawn sensing or AI perception already sees a client's puppet (a `mainPlayer_C` on the host). In short,
the zombie family, the gore slither, the fossilhound, the antibreather's sight, the Geom Octa and the Killer Wisp (its own pick, `wisp_attack_sync`)
already see puppets, and the Krampus, the tentacle ball, the ariral follower and slapper and the Grays' punch read player 0 and are converted here.
The funguy, the orb, the shooter and pig beater, the vent crawler, eggs, wisps, deer, walking trees, the kavotias and Wolfgang are not hunters of
players, and the per-player scares stay per player.

`coop/creatures/hunt_target` holds a row per class: the player-0 reads its Blueprint makes (the seams of `coop/creatures/served_player`:
`GetPlayerPawn`, `GetPlayerCharacter`, every `lib.getMainPlayer` the creature makes, and `GetPlayerCameraManager`, answered with the puppet's BODY, so
a row asks for it only when every use of the result is an actor call) and the range its own Blueprint tests, 0 when it has none. A pass every 250 ms
on the host measures every player against each live creature of a row and asks the one rule (`coop/creatures/hunt_target_rule`, the mannequin's rule
made general: KEEP the target until it is lost; LOSE it when dead, gone, without a body or beyond the creature's own range; ACQUIRE the nearest living
player within that range, the earlier slot on a tie; with none, the host, which is no record, so the Blueprint's own branch runs as before). A
client's slot is recorded in `served_player` and the creature's reads of "the player" answer that client's puppet. With no client in the world nothing
is chosen, so a host picked while alone is never kept against a client who comes later. The choice is made on the pass, not at a read, since these
Blueprints read the player in many places and none is a decision point the host can name; a creature that re-picks its prey by distance every tick
(the fossilhound, the zombie) is not a row.

| Creature | The reads answered | Its own range | The attack |
|---|---|---|---|
| Krampus (`npc_krampus_C`) | `GetPlayerPawn` (its stalking circle and chase, the jump, the fireball, the overlap timer), `getMainPlayer` (the line trace, the stab's target), `GetPlayerCameraManager` (the despawn's `GetDistanceTo`) | 7500 cm: its stalking circle is 5000 cm round the player and the navmesh puts it up to 1000 cm off it, so the circle and half again (a choice of this lane, the Blueprint tests none for the hunt; its despawn leash is 5000 cm) | the stab is `stabplayer.punch`; the copy on a client runs the whole Blueprint (not parked), so the client's own copy stabs the client |
| tentacle ball (`tentacleBall_C`) | `GetPlayerPawn` (the chase's move proxy, the pin-down's trace and pull), `getMainPlayer` (the pin-down's anim targets), `GetPlayerCameraManager` (a trace origin) | 15000 cm: its sense, a `SphereOverlapActors` of `0x466a6000` | the pin-down animation on the overlapped player, no damage |
| ariral follower and slapper (`npc_arirFollower_C`) | `GetPlayerPawn` (`getTarget`'s goal, the throw, `playerSwing`), `GetPlayerCharacter` (`playerSpd`), `GetPlayerCameraManager` (a trace origin) | none: the Blueprint tests none to follow | the slap and the throw; the family is parked, so a hit on a puppet is delivered as `PlayerHit` |

The ariral gun stealer is not a row: it goes for the gun while one stands, and its only player read, the goal when none is left, is at its begin play,
before a pass can serve it (it slapped the host at once with no read answered, then fled and ended `[V]`). Its slap is an overlap of its punch sphere
with `mainPlayer_C`, so it lands on whichever player it reaches.

The Grays' punch (`coop/creatures/hunt_gray`): a gray's contact or arrival calls `grayEventController_C::despawn`, which punches `lib.getMainPlayer()`
whoever touched the gray, so a client that touched one punched the host. The host's pre watch on `despawn` picks the player nearest the gray that made
the call (the controller when none did), records the controller as serving that puppet for the call, and the punch lands on the puppet, where
`player_damage` cancels it and `creature_hit::IsDelivered` delivers it to the client. The Grays' copies on a client never call the controller (its
`begin` and its overlap are refused there).

The dev switch `VOTVCOOP_HUNT_SKIP_HOST=1` (`hunt_skip_host`) leaves the host out of every candidate list, and `VOTVCOOP_HUNT_DRILL` spawns each
creature (`coop/dev/hunt_drill`): with both players at one spot, the only way to make "the client is nearer" true. `[V]` Each arm was run once with
the switch and once without: with it the host log shows `hunt_target: <creature> hunts slot 1`, `served_player` holds slot 1 for it and its
Blueprint's reads are answered with the puppet (the Krampus 90, the tentacle ball 209, the follower 8373); without it the creature is recorded against
the host and no read is answered. The follower closed from 600 cm to 265 cm of the puppet (261 cm in the host arm: at one spot the position cannot
tell the two players apart, the record and the answered reads do). The Grays arm called the controller's `despawn` with the switch: `hunt_gray: ...
its punch is served to slot 1`, `player_damage: canceled punch on a peer's puppet`, host `[HIT] sent verb=punch slot=1
creature=grayEventController_C`, client `[HIT] applied verb=punch`; without the switch the host was the nearer, nothing was served and nothing was
delivered. `[?]` The tentacle ball arm's creature is a lone ball, which falls through the floor (its follower actor holds it up), so the chase and the
pin-down were not staged; the Krampus's pick and its reads were shown, not a stab on a client; a gray's own overlap and arrival were not staged, the
drill calling `despawn` itself; the zombie's and the antibreather's idle reads were not touched.

### The cave creatures

The Antibreather (`antibreather_C`, a `Character`) and Wolfgang, the giant centipede (`centipedeHead_C`, a `Character` with
no allowlisted parent, so a late allowlist entry; his pups `wolfgangJr_C` are a subclass of his), are the host's: each is
anchored at the cave, so the host's one instance is sent to every client and kept as long as the host has it.

| Piece | What it is | Lane |
|---|---|---|
| `antibreatherSPawner_C` | a level actor on both peers, in the cave at about (-65300, -59800, 9000) | a spawn source (`coop/creatures/npc_world_enum`) narrowed to `antibreather_C` and `centipedeHead_C`; its 10 s roll refused on a client (`coop/world/spawn_authority`) |
| `wolfgangEggs_C` | an Actor (no prop, no save actor) that the spawner's `cencipede` stands at (-68546, -57290, 8346) when an `eggyWolfgang_C` is about; its `damageByPlayer` mints an aggressive antibreather at the egg | a spawn source for `antibreather_C`; the egg itself is not shown to a client (no lane carries it) |
| `centipedeSegmet_C`, `centipedeSegmet_jr_C` | the body: 30 segments (a pup's own) minted by the head's begin play, each an Actor with meshes and no gameplay | each peer's own, minted by the copy's begin play, which runs whole |
| `eggyWolfgang_C`, `parentWolfgang_C` | save actors (`actor_save_C`): the egg's timer, and the mark of a hatched brood | not carried; a joiner's save load makes its own |

**What starts the spawn.** Not a player. The spawner's sphere (`Sphere1`) overlap only plays the cave's ambience as a 2D sound, once, when a
`mainPlayer_C` enters (a puppet is one) and player 0 is not in the entry box; it spawns nothing. The roll is a 10 s loop in the spawner's begin play:
with the day cycle's `sun_height` within 0.5 of 0, no creature standing, the spawner's own render component not drawn for 0.2 s and a line trace from
the spawner to player 0's camera blocked, a 10 % draw leads to `cencipede` on a further 0.2 % draw (Wolfgang, or the egg when an `eggyWolfgang_C` is
about), or when a `prop_food_pumpkin_c` lies within 20 m; otherwise, on day 3 or later, it leads to an antibreather at the spawner's point. With the
sun further from 0 the same loop ends the creature, when player 0 is more than 50 m from the spawner, it more than 25 m from player 0, and it was not
drawn. So no client presence has to start it and no intent is needed: the host's loop is the only roll, and it reads the host's player 0 and camera,
so a creature can appear in a client's view, and be ended while a client is near it and the host is not `[RD]`. `[V]` The `caveWolfgang`, `caveEgg`
and `caveHead` drills (`coop/dev/cave_drill`) each saw the level's own spawner roll an antibreather on the host (`npc-sync[ex-spawn]: enrolled
'antibreather_C' ... source 'antibreatherSPawner_C'`) and the client make its copy.

**The host's one, shown to every client.** `[V]` `caveWolfgang` (the host calls the level spawner's own `cencipede`): the host enrolled `centipedeHead_C`
with `source 'antibreatherSPawner_C'`, the client made the copy, which stood with its 30 segments (30 `centipedeSegmet_C` live on both peers) and
refused the entries below. `[V]` `caveEgg` (the host stands the egg 60 m from its player and calls its `damageByPlayer`): `enrolled 'antibreather_C'
... source 'wolfgangEggs_C'`, the client made it. `[V]` `caveHead` (the engine's own spawn of the head, the npc interceptor's route): the same
mirror, 30 segments, the same refusals. A client's own spawner, stood at the world's origin with its `cencipede` called by the drill, was refused
(`spawn_authority[caveSpawner.loop]: client-refuse ... entry 3593`) and the client held no head and no antibreather of its own; its 10 s roll (`entry
30`) was refused too, the level spawner's among them. The entries a client's spawner graph ran were 2603 (begin play), 1992 and 2152 (its short
delays), 3069 (`block`) and 3333 (the sphere), none refused: the rock, the music trigger and the ambience stay each peer's own. A drill-stood spawner
called beside the host's player minted, in some runs, a head that was not live when read; the level's spawner made it at once, and the cause was not
found `[?]`.

**A parked copy** (`coop/creatures/npc_mirror_brain_table`). Both copies run their begin play, tick, a per-viewer check and their destroy, and refuse
every other entry of their graph. The antibreather's allowed entries are 9848 (begin play: the mesh on its spring arm, the animation instance, its
timers), 4489 (the tick: the walk blend and the yaw; its one decision, the punch, needs `player`, which only the refused sight sets), 11016
(`checkVisible`, the 0.25 s timer: camera 0 within 10000, and its event count) and 10320 (the destroy's `false`). Refused `[V]` on the rig: 10278
`wander`, 11021 the hogger sphere (1e8 damage to a gray boar or fossilhound that touches it), 11011 the door timer and 10669 the mushroom timer;
refused by the table: the sight, `punch`, `findBlindSpot`, `playSound` and the six move-to results. Wolfgang's allowed entries are 12599 (begin play:
his segments, five timers, the per-viewer loop), 12997 (the tick: the rope of segments), 463 (the loop's resume) and 15754 (the destroy, which ends
the segments). Refused `[V]`: 15 and 15846 (kick), 15851 (calc: the target, and the food it picks), 15856 (checkDoors), 16170 and 14242 (the mouth's
overlap, which destroys the food it touches and counts toward his egg). The offsets are the cooked 0.10.0 graph's, read from the pak (a stub's
`LocalFinalFunction(ExecuteUbergraph, IntConst)` names its entry, a Delay's latent resume the loop's); every number the drills saw agrees with them,
and 463 and 4489 were not seen to run `[?]`. A head born while a `parentWolfgang_C` stands mints eight pups in its begin play: a copy's own are
refused by a pre hook on the deferred spawn, armed from the copy's birth to the end of its begin play (`coop/creatures/centipede_pups`), and the host's
pups are enrolled (`centipedeHead_C` is a source for `wolfgangJr_C`) and mirrored `[RD]` (a save with a hatched brood was not forced). The head's `sub`
child actor (`subPawn_C`, a hidden `Pawn` that walks to player 0 for path points) runs its begin play on a copy and is not parked `[?]`. Both are per
viewer for the event count (`coop/world/creature_event_relay`): the host's relay skips them and a copy runs its own `checkVisible` or loop `[RD]`.

**Hurting a client.** The antibreather's pawn sensing has `bOnlySensePlayers` off (the class default writes it), so it sees any pawn: a client's puppet
is a `mainPlayer_C`, and the sight entry takes it as `player`, chases it and `punch`es it (`mainPlayer_C::punch`, 150 cm reach). On the host that
punch is refused on a puppet and, the class being parked, delivered to the client's own player as `PlayerHit` (`coop/creatures/creature_hit`,
`coop/player/player_damage`) `[RD]`: no run brought an antibreather to a player. Where it wanders and where it hides is the host's alone, since
`wander` aims near player 0 and `findBlindSpot` hides from camera 0's sight, so it does not avoid a client's view.

**The save-loaded case.** Nothing is adopted, and nothing needs to be: `npc_adoption` binds the twins of creatures a save holds, by key, and none of
these classes is a save actor (only `eggyWolfgang_C` and `parentWolfgang_C` are). The one save-time mint is `mainGamemode_C::loadObjects`, which stands
an antibreather behind player 0 for 60 s when the game instance's `antibreatherkickout` flag is set (nothing in the capture sets it, and a joiner's own
instance starts clear), so `mainGamemode_C` is a source row for `antibreather_C` too, and a client's own load mints none `[RD]`.

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

The radial menu (follow, idle, patrol, fix the servers, get the reports, fix the transformers) is relayed host-authoritatively
(`coop/creatures/kerfur_command`): the host cancels the local dispatch on both roles and re-runs the game's own verb with the kerfur serving the peer
who asked (`coop/creatures/served_player`), so its reads of the player answer that peer: follow follows it, patrol circles it. A request waits while
the host has no body for its sender, is refused when that body is out of an arm's reach of the Omega, and a verb the game refuses changes nobody's
service. Get the reports reads the asker's hand and destroys the disc it takes, so the press carries the floppy disc the client holds, as its record,
with the command (`coop/creatures/kerfur_command_wire`, `coop/creatures/kerfur_disc_carry`): the host builds a copy of that disc in the puppet's hand
for the verb's one call, the game's own verb reads and destroys it, and the host answers that the disc was taken, on which the client's own machine
destroys its disc if it is still the one it held at the press. The copy is never published. A disc the Omega already holds is not carried, nor is a Wh
disc (the game's own hint answers); a press with no disc gets the game's refusal hint, which the host routes to that client
(`coop/requester_feedback`). A command the host refuses before running it, or holds, is answered to its requester as `KerfurCommandAnswer`
(`coop/creatures/kerfur_command_answer`), worded there in the player's language. A player's pick, and a drill's reflected call, are caught at one seat,
the script gate's watch on the Omega's `actionName` (`coop/creatures/kerfur_command_seat`): a client cancels a relayed verb there and asks the host,
the host hands the kerfur back to its own player there. At a join a save-loaded kerfur prop is adopted by class and nearest pose once the load tail
settles (`coop/creatures/kerfur_prop_adoption`), and a kerfur that was off in the save but on by the time the joiner is ready has its stale off-prop
retired by an id the character's spawn carries (`coop/creatures/kerfur_reconcile`). The robot's thirty skins are data-only subclasses, so the spawned
class is the skin; the same bodies serve as player skins on [players.md](players.md).

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
| The Poolwalker (`poolwalker_C`, an `ACharacter` with AI move-to, placed once in the tile `untitled_137` and in the tutorial tiles) is on no lane: its class is not in the 0.10.0 bytecode capture, so no function row can name it, and the tile is streamed in only while a player is near it, so each peer holds its own or none | `[V]` `walker` drill: `0 poolwalker_C held; class loaded: 0` on both peers at the base. A host-owned copy would be unsound (a client near the tile has an actor the host lacks) and it cannot be tried without moving a player; `[RD]` the asset's imports (`GetPlayerPawn`, `WasRecentlyRendered`, `CreateMoveToProxyObject`, `K2_GetRandomReachablePointInRadius`) |
| The Omega's take-object, pat, equipment, kill and sit-on-the-ATV verbs are not synced | `[V]` `coop/creatures/kerfur_command`, the verb table |
| The Omega's floppy state is not synced, so a relayed get-reports diverges on its result; its accessories and carried object likewise | `[V]` no lane under `coop/creatures` carries them |
| Two stalker classes are owned per peer (the eyer and the Shadow Figura); a copy of the Figura stands in its idle pose, since its movement component is parked and the walk animation reads velocity | `[V]` `coop/creatures/owner_entity_sync`, the member table; the pose is `[RD]` |
| The conversion is detected by a poll, five times a second, not at the verb | `[V]` `coop/creatures/kerfur_convert`; the lane reads the result rather than the call, because a peer's own conversion has already happened by the time any watch of ours runs |
| Three ambient spawner families (bp7, the bush and the tick) are neither mirrored nor refused, so each peer grows its own | `[V]` they appear in no lane under `coop/creatures` and in no row of `coop/world/spawn_authority` |
| A walking mannequin freezes on the gaze of the player it hunts only, and a client's look reaches the host a moment late, so it can slide on for that long. It goes for the nearest player within its range and keeps that player until it is lost (the player left, died, or is beyond the range), and the host's walker lands the hit, which runs on the victim's machine with its sound: the host refuses its own 2D copy of a hit on a peer. Its anger does not cross: a copy holds the host's `looking`, which is false while the walker is angry | `[V]` `prop_wMannequin_C`'s tick and `timer_damage`; `coop/player/player_damage` (the hit relay and the sound hush), `coop/creatures/walker_target` (its target, and the walker served as that player: `coop/creatures/served_player`), `coop/creatures/walker_state_host`, `coop/creatures/walker_gaze_host` |
| A dirt hole's own end is each viewer's: past 10 minutes, one its viewer has not had on screen in the last second and is not within 50 m of goes for that viewer, and a client's copy can go while the host's stands. No hole's end crosses: each peer's copy ends by its own viewer, the host's included | `[V]` `susDirtHole_C`'s begin play; `coop/world/ambient_fixture_sync` |
| A client's sus hole roll can be lost: the host judges the point (100 to 500 m from the player's body, on grass by its own trace, at most 4 of that player's holes standing, a burst of 2 then one in 300 s) and a refusal costs that roll, the next being 300 to 600 s away. A drone's camera can be far from the body, so its roll is refused | `[?]` `coop/world/sus_hole_intent` |
| A deer's dissolve is each viewer's: one a client walked up to is gone for that client and stands on the host until the host comes near. A walking tree halts and wanders by the host's view alone, so a client can watch one move | `[V]` `deer_C` and `walkingTree_C` read local player and render state; the deer mirror keeps its actor tick (`coop/creatures/npc_mirror`) and its pose drive refreshes the floor that tick reads `[?]` |
| The cave's creatures are the host's, and the roll that makes and ends them reads the host's player 0 and camera: a creature can appear in a client's view, or end while a client is near it and the host is not. The egg is not shown to a client, and a client's punch on it is not carried: a client's copy of an egg does not exist, so only the host's own hit wakes the egg's antibreather | `[RD]` `antibreatherSPawner_C`'s loop; `[V]` no lane carries `wolfgangEggs_C` |
| The antibreather's wander and its hiding read player 0 and camera 0 (the host's), though its sight sees puppets and chases the one it saw, and its punch on a client's puppet is delivered as `PlayerHit` | `[RD]` the sight entry, `punch`, `creature_hit::IsDelivered` |
| A host creature's first read of "the player", at its begin play, is the host's: the hunters' pass picks a target a quarter second after the creature appears, so a creature that reads once and never again (the ariral gun stealer with no gun left) keeps the host for that read, and the Krampus's `SetOwner` at its begin play is the host's. A creature with no range of its own (the ariral follower) takes the nearest player at any distance, and the zombie's sniff pull and the antibreather's idle wander still read the host | `[V]` the stealer slapped the host at once with no read answered; `[RD]` the rest, `coop/creatures/hunt_target` |
| The host's own camera coming near a deer ends it on the host, and that end crosses as any NPC end: the deer is gone for every peer, however far from the host | `[RD]` read from the deer's tick and the NPC end lane; no run measured it (the drill ends a deer on the host with a direct dissolve, not by the host's camera) |

## Code map

| Concept | Files |
|---|---|
| host-owned characters | `coop/creatures/centipede_pups`, `coop/creatures/npc_sync`, `coop/creatures/npc_mirror`, `coop/creatures/npc_world_enum`, `coop/creatures/npc_adoption`, `coop/creatures/npc_pose_host.cpp`, `coop/creatures/npc_pose_drive.cpp` |
| the engine wrappers | `ue_wrap/actors/kerfur`, `ue_wrap/actors/wisp` |
| per-peer creatures | `coop/creatures/owner_entity_sync` |
| the killer wisp | `coop/creatures/wisp_attack_sync`, `coop/creatures/wisp_grab_hold`, `coop/creatures/wisp_tear_mirror`, `coop/player/ragdoll_gate` |
| roaches | `coop/creatures/roach_sync` |
| the kerfur | `coop/creatures/kerfur_entity`, `coop/creatures/kerfur_convert` with its host and client halves, `coop/creatures/kerfur_form_assembler`, `coop/creatures/kerfur_command`, `coop/creatures/kerfur_command_seat`, `coop/creatures/kerfur_command_answer`, `coop/creatures/kerfur_command_wire`, `coop/creatures/kerfur_disc_carry`, `coop/creatures/kerfur_prop_adoption`, `coop/creatures/kerfur_reconcile`, `coop/creatures/served_player` |
| the kerfus | `coop/creatures/kerfus_lanes` with `kerfus_brain`, `kerfus_state`, `kerfus_intent` and `kerfus_follow`; `ue_wrap/actors/kerfus` |
| tests | `harness/autotest/autotest_kwisp_probe.cpp`, `harness/autotest/autotest_wisplane.cpp`, `coop/dev/kerfur_convert_drill` (a client turns a kerfur off and on beside a disc of its own), `coop/dev/kerfur_serve_drill` (a client's commands to the Omega run for that client), `coop/dev/kerfus_drill` (a kerfus a client turns on follows it) |
