# Events and weather

## Purpose

The shared world's own progression: the clock and the sky, the weather, the ambient spawners and who is allowed to roll them, the story and
scheduled events and how a client sees one fire, the actors an event flies in, the walking pyramid, the base alarm, the emails and the daily
task. The rule under all of it: shared-world randomness is rolled once, on the host, and a client never rolls a shared outcome. Creatures an
event mints are on [npcs-and-kerfur.md](npcs-and-kerfur.md); the Easter hunt on [easter-hunt.md](easter-hunt.md), the Erie plush on
[erie.md](erie.md) and the vending machines on [vending.md](vending.md).

## How it works

### The clock and the sky

The game's clock lives on one actor, the day-night cycle, and the sun is a pure function of it. The host streams its clock: the within-day time, the
absolute time and the day number. On a client, right before each tick of its world's cycle (the menu scene's own cycle is left alone), the lane holds
the cycle's rate at zero and writes the host's last sample in, so the client's clock advances only with the host's, and neither its own advance nor a
write on the client (its cheat menu's day buttons do nothing there) reaches a midnight. The midnight's shared outputs (the dish hash codes, the daily
task, the Bad Sun roll, the results mail) are the host's, and reach a client as one sample carrying the next day number `[V]` (`coop/world/time_sync`).
At that sample a client performs its own share of the rollover, the part each machine keeps for its own player: its profile's days lived, its count of
midnights since its world loaded (the insomniac achievement at seven), the day's music flags, and game mode 0's day-number achievements `[RD]`
(`coop/world/day_edge`).

A client's hour pulses still run, so the game's automatic 6 am drone order is latched there (the midnight that re-opens it daily is the host's
alone). The weather births and the event walk are held too (below); the sky eye is the host's (a client's own noon roll is refused) `[V]`, and so are
the jellyfish (a client's own 18:00 spawn is refused) `[V]` and the red sky (a client's own toggle is refused, start and end alike: `[V]` on a
reflected call, the noon's own route `[RD]`). The flesh rain is the host's roll and reaches a client as a cue `[V]`; the gifts run per peer (Known
limits). Game mode 5 resets the time of day every second on every machine; a client refuses its own copy of that loop at its once-a-second resume,
after the one pass its begin play runs, which the clock takes back `[V]`, and the mode's other flows run on: its needs restore `[V]`, its ambience
and spawners `[RD]`.

A sample is sent each time the host's clock has moved half a game minute, and at least twice a second: every 500 ms at the normal speed, where a
game minute holds about six, so a client's own minute and hour pulses, its sun, sounds, decals and weekday follow the host's minute for minute
`[RD]`. In the shared sleep (on [players.md](players.md)) one comes about every 80 ms, and a frame longer than that on either peer can merge two and
skip a minute's pulse `[RD]`; one night gave 44 of 45 `[V]`. The star dome's random orientation, the save-derived moon phase, the sky eye and the
Black Hole Sun's black-hole sky stream from the host once a second (`coop/world/sky_sync`) `[V]`.

### Weather

One actor owns the weather: its scheduler timers for rain, lightning, fog, super fog and permanent rain, a dozen state fields, and the mutator
functions everything else calls. On the host, observers on the five scheduler functions read the state after each fires, dedupe it by hash and
broadcast it on change and at a joiner's connect (`coop/world/weather_sync`). On a client the same five scheduler bodies are cancelled before they
run, so the client never rolls "rain now", and a received state is applied through the mutator functions rather than field writes, because the snow
function alone has dozens of Blueprint listeners (`coop/world/weather_rain`).

Fog is not a flag: the enable flags are config gates, and the active fog is an actor (the rolling fog controller, the super fog) that ramps the
height-fog density over its own duration and destroys itself; the host mirrors the actor's life (`coop/world/weather_fog`). A lightning strike is a
transient actor; the host observes its spawn and sends the location (`coop/world/weather_lightning`). The black fog and the rolling fog the clock
rolls funnel through the engine's finish-spawning call, where a client-side catch destroys any birth the host did not command
(`coop/world/weather_event_births`). The black fog rides the weather state: the host stamps a bit while its gamemode holds a `blackFog_C`, each client
runs the game's own `spawnBlackFog` once per host episode and ends its copy when the host's ends, and the eyes each peer's fog mints are that peer's
own, shown as copies elsewhere (see [npcs-and-kerfur.md](npcs-and-kerfur.md); `coop/world/weather_blackfog`) `[V]`
(`VOTVCOOP_EVENT_FIRE_DRILL=spawnBlackFog:560` on both peers: the host's state carried the bit from the spawn to the fog's end and the client ran its
own `spawnBlackFog` at the first state that carries it).

The red sky is a story event actor that the clock's hour pulse toggles at noon on every peer (a living one ends, else a one-percent roll starts
one), dispatched inside the Blueprint, where the script-body gate still sees the toggle: the host sends its red sky as each toggle leaves it, and a
client refuses its own and runs the host's through the same toggle (`coop/world/weather_redsky`). The Bad Sun is a story event actor too, but every
peer runs its own copy by the shared clock and the harm is local: its widget traces from the local camera and hurts the local player. The host owns
whether a copy exists and sends its state as `Spawn Bad Sun` and its copy's `remove()` return; a client's own `Spawn Bad Sun` is refused at the
script-body gate, and a client keeps exactly one copy: the host's present spawns one if none lives, and the host's absent destroys an inactive copy at
once and leaves an active one to fade by its own clock (`coop/world/weather_badsun`). `[V]` for the clock half (a client's own copy activates at
07:00), the wire half (a spawn, a refused client spawn, the end, a rejoin and a joiner's stale copy, each with its red) and the day cycle's midnight
route; the cheat menu's route is `[RD]`, its handler needing a Sandbox or flying world.

The anti-gravity event lifts props on the host alone, and the host streams each lifted prop on the driven-prop channel until it rests
(`coop/world/agrav_sync`), 128 props at a time, the nearest to a player first: a prop lifted while every slot is taken floats on the host alone, and
when the event ends the host sends its resting place, forty props a second `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=agrav:110:trigger_agrav_C`: of 959 props
lifted, 129 were streamed while they floated, 545 were sent their resting place, 273 had not moved and 12 were never sent). Fireflies are the one
peer-symmetric weather: the spawner rolls a ring around the local camera, so every peer keeps its own and shares each spawn (`coop/world/firefly_sync`).

### Who rolls

A connected client ticks no shared-world spawner and rolls no shared-world spawn (`coop/world/spawn_authority`), by one table of the bodies a client
must not run, each refused at the script gate by a watch on its class and function names while a client session runs; the table is on
[npcs-and-kerfur.md](npcs-and-kerfur.md). Shared-world content then arrives only from the host through the character, world-actor and prop mirrors.
A spawner anchored to the player's camera (the pinecone drops) is the exception: each peer rolls its own and the drops are shared peer-symmetrically.
The remaining randomness is closed shape by shape: a mirror where the host owns the roll and the state, an intent where a client's action makes the
host roll (a shop delivery time, a signal catch, a device claim), and a shared seed where the game already uses a seeded stream. Cosmetic-local
randomness (view bob, flicker, exhaust) is left alone.

The gamemode's night roll is the host's: a one-shot timer, re-armed every two to five minutes, that in a night from the fourth day rolls a
three-percent chance of a visitor, a Geom Octa four times in ten, else a night egg. A client's body of it is refused, which also ends its own
re-arm; the host's roll is a spawn source of the character lane, and the Octa or the egg reaches every client as a mirror `[V]` (the `octa_drill`
births). The Octa's attack on a player is relayed from the host ([npcs-and-kerfur.md](npcs-and-kerfur.md)).

### Story and scheduled events

The scheduler is the save's own time step: it walks the event table and runs each due row through the eventer, all inside Blueprints. The host sees
each fire at the verbs: script-body watches on the eventer's `runEvent` and `runSpecialEvent` broadcast every call whose body completed, whether a
scheduled row's, a dev-menu fire's, the game's own menus', or the prank case `summonArirPrank` actually picked (`coop/world/event_fire_sync`). The
client keeps its own copy of the walkable event list empty, a one-integer write held right before each tick of the clock's cycle, so its scheduler
fires nothing as the host's clock moves it. A client runs no eventer verb of its own: the game's event and cheat menus are refused there, and the one
eventer call it runs is the replay's own.

What a client does with a received fire is a per-row policy kept in the code. Rows whose outputs already ride a lane (props, creatures, the ATV,
sleep, the wisps, the cues, the devices) are not replayed, because that would deliver the effect twice; the level flips, story flags and cosmetic
sounds no lane carries are replayed through the same native verb. A prank's rolled case arrives as its own `runSpecialEvent` fire and meets the same
policy: a graffiti decal or a scare box's arm is replayed; a case whose output rides a lane or stays on the host is not. A replay is skipped when the
client's own passed list already carries the row, unless the host says the event is in flight; one whose world or eventer is not up yet is retried
each second. A special fired before a peer joins is not replayed to it. The developer menu's event trigger dispatches through the same host path,
refuses running clients, including a join in progress, and binds a queued trigger to the submitting session's start serial and world generation, so
restarting the same session object cannot carry an old request into the next run.

The game keeps a registry of in-flight events: a reference count and the live event actors, maintained by the actors themselves. The host sees each
event begin and end at `lib_C::setEvent` and, at a joiner's ready edge, sends one snapshot entry per in-flight event from the registry, so the joiner
replays it from the start with the in-flight override (`coop/world/event_active_sync`). A creature with no event row is not replayed: the host sends
the creature's count of running events whole, by element id, and a client applies it to its mirror's registration (`coop/world/creature_event_relay`)
`[?]`; the creatures whose event is computed from the local camera, render state or player 0 (the killer wisp, the fossilhound, the antibreather,
Wolfgang and his pups, the egg) keep their own per viewer. Events whose only trace is a one-shot particle emitter (the meteor shower) are mirrored as
cues: the host broadcasts the emitter's identity and position when the row's `runEvent` returns, a joiner's snapshot reads the live emitters from the
object index, and a client spawns the same emitter (`coop/world/event_cue_sync`). A cue no row announces is taken at its emitter's birth: the host
reads the particle component's template at the next tick and sends its place (the growth area's glow at the plant it fertilizes) `[?]`.

### Events that change the world or the sky

These results are not creatures, so no character or world-actor lane carries them: each is seen on the host and shown by the client's own
level-placed copy, and a client's own roll is refused (`coop/world/event_world_sync`, the rows of `coop/world/spawn_authority`). The first three
ride existing carriers; the rest are cues 130 to 141 of `coop/world/event_cue_sync` (141 is the Midas UFO, below; 160 is the Evil's ending).

| Event | What the game does | Carried as |
|---|---|---|
| Black Hole Sun (`earthTp`) | `newsky_C.tp`, a 106 s chain of latent delays on the sky: `setEvent(true)`, a 2D sound, at 20 s the black hole's light and `skyVisibility(true)`, three mails, a second sound at 86 s, all undone at 106 s | not replayed: a second call of `tp` stacks the event count and the mails, and a joiner's would start the chain over. The flip is `SkyStatePayload.blackHole`, once a second and at a joiner's world-ready, applied with the light's `SetVisibility` and the sky's own `skyVisibility`; the two sounds are cues 130 and 131 at the sky's own `PlaySound2D`; the mails ride the append feed. `[V]` (`earthTp` drill: a client's sky followed the host's within 2 s at both edges; `earthTpHold`: a client that joined while the sky was held black was sent `blackHole=1` in its connect snapshot and applied it 3 s after its play-ready) |
| Day-13 ship tower (`arirShip`) | the eventer's `runTrigger` calls the level-placed `arirShipAppear_C`'s `appear(true)`: a fade, a radar point, the event count; its tick hides it when a camera looks at it or the sun is up; the end of the forward fade starts the "outside" mails | the row stays unreplayed; the host's `appear(dir)` is cue 132 or 133 and a client's own tower runs its own `appear` (only on a change, so the count is not taken down twice); a joiner is told while it shows. A client's tower's mail loop (`a__FinishedFunc`) is refused, since the host's mails arrive on the append feed. A photo's reputation is bound by `runTrigger`, not `appear`, and stays the host's `[V]` (`arirShip` drill) |
| Lake Event | the gamemode's `ticker_lakeMonsert` (15 to 20 minutes, a moonlit night from day 7) calls the lake's `setActive`: mist, loop, splash, light and beam | cues 134 and 135 on the host's `setActive`, a joiner told while lit; the client's own roll is refused `[V]` (`lakeGlow` drill). The pull of the monster reads the player standing in each peer's own lake box and stays per player |
| Stolen Radio Tower | `ticker_radioTowerPoof` (hourly, 0.5 %) spawns `radiotowerPoof_C`, which hides the level's radio tower and its collision once the local camera has not seen it, and shows it again, destroying itself, once it has not seen it again | cue 136 on the host's actor; a client spawns its own, which runs on its own camera, as the vanish is made to be seen only by a player looking away; a joiner is told while the host's lives. A client's own roll is refused `[V]` (`radioPoof` drill; the hide itself was not reached within the drill's minute) |
| Red Phone | the downloading panel's `redPhoneEventTimer` rolls 2.5 % and calls `startRinging` (a ring every 3.5 s) until the local camera is within 350 of the panel | cue 137 on the host's `startRinging`; the client's panel starts its own ring, which ends when its own player walks up. A joiner is told when the panel's timer handle is live. A client's own roll is refused `[V]` (`redPhone` drill) |
| Gore Locker | `ticker_lockerHead` (hourly): 8 % a random locker gets `head`, which its next opening turns into a `lockerguy_C` scare; 1 % a `lockerCorpse_C` at a random locker whose begin play sprays gibs, a blood decal and an emitter | cue 138 for a locker that gets a head (placed by the nearest locker of the client within 250) and cue 140 for a corpse, which a client spawns itself at the host's place; a joiner is told of each head and each live corpse. A client's own roll is refused `[V]` (`lockerHead` drill). The host's gibs (22 props and a decal) did not cross as props; with the cue a client's own count rose by the same 22 and 1 `[V]` (`lockerCorpse` drill, two runs) |
| Meat Rain | the day cycle's hour roll (0.01 %) spawns `event_fleshRain_C` at the origin; it rains `prop_garbageClump_C` around the camera of player 0 | a client's own spawn is refused at the deferred-spawn seam by class (the roll sits in the day cycle's graph) `[V]`; cue 139 on the host's actor makes a client spawn its own, so each player is rained on at their own camera, the clumps of a client's own being its local props `[V]` (`fleshRain` drill) |

### The actors an event flies in

The saucers, the mothership, the ariral ships, the sky objects, the jellyfish, the fire tank and their kin are not characters, so the character lane
cannot drive them. They have their own mirror (`coop/world/world_actor_sync`): a second interceptor on the deferred-spawn call with a name-matched
allowlist disjoint from the character one, a full-rotation pose batch, a spawn that carries scale and a class-interpreted birth blob (a coin is born
with the points its material comes from), a destroy broadcast, and a dead-retire in the host's pose walk for actors that destroy themselves at an
event's end. A spawner that runs inside a Blueprint (the pyramid's, the jellyfish path's) is caught at the native function seam and gated by the
spawning class. A mirror stops whatever its class simulates with (the coin's sphere, the jellyfish's capsule), so the pose drive alone moves it
`[V]`; a heading that lives outside the actor's rotation streams as an auxiliary yaw.

The event dispatcher's own spawns are enrolled by output: the vent crawler and the egg invader on the creature lane, and on this lane the mothership,
the morning UFO, the vent knocker, the drop saucers (car, body, tank, pig), the salt pile and the Soltomia cleaning craft. A client's copy shows the
ship and runs none of its gameplay: a saucer's BeginPlay and its drop are refused there (the dropped car, body or tank is the host's, on the prop
lane) and the copy is told to deploy so it descends; a copy's own `setEvent` is refused, so a ship the host destroys first cannot leave the client's
event count raised; the mothership and the ariral ship keep their own tick, which is where they fly; the morning UFO's copy is shown once the host's
moves it (`coop/world/spawn_authority`, `coop/world/world_actor_mirror`). `[V]` for the births and refusals: `VOTVCOOP_EVENT_FIRE_DRILL` fired
`borgRozital`, `fallcar_0`, `graystank`, `morningGay` and `ventKnocker` on the host, and for each the client logged the mirror's birth and the
refusals named above; what a client sees on screen was not watched.

The salt pile has no body of its own to run and its heart crosses as a prop. The Soltomia craft's copy runs no begin play, overlap or destroy (the ATV,
door and prop lanes carry what the host's does). The craft is hidden until a body enters its box on the host, and leaves when its door is opened: the
host sends an EventCue at each edge, and a client shows its copy and starts its own squeak, particles and talk loop on the first, and runs its own
`opened` on the second (the leaving montage, its faces and lines, its end), with the montage's sponge refused there, the host's being a prop
(`coop/world/solto_sync`); a joiner is sent the first cue while the craft is at work `[RD]`. `[V]` `VOTVCOOP_EVENT_FIRE_DRILL=soltoClean:70`, which
calls the host craft's box overlap and `opened` itself: the client showed its copy, played its leaving, refused its own sponge and took the host's.

The gamemode's UFOs and the body are the host's. Four timer handlers of `mainGamemode_C` (`ufo_ball`, `ufo_boo`, `ufo_joel`, `ufo_pill`) each mint
one saucer (`ufo_ballfo_C`, `ufo_boofo_spawn1_C`, `ufo_joel_C`, `ufo_pillfo_C`) and `tickerBody` mints `theBody_C`. A saucer stands at its spawn point
and flies a component along its own route by its tick or a timeline, with a sound and a camera shake for the local player, then destroys itself, so
its copy keeps its own tick and each peer shakes its own camera. The host's roll is a spawn source of the world-actor lane (`mainGamemode_C`'s narrowed
outputs in `coop/creatures/npc_world_enum`), a client's handler is refused (`coop/world/spawn_authority`, which also refuses a copy's `lib.setEvent` and
a body copy's overlap, which would end it 5 s after a pawn touched it), and the host's end is the copy's end. A copy flies the host's route from the
host's spawn point and at its pace; where a class rolls its heading or wobble in its own begin play (`ufo_ballfo_C`, `ufo_boofo_spawn1_C`) each
peer's own roll is used, so the two saucers leave the same point on slightly different headings `[V]` (`coop/dev/gamemode_drill`: `gmUfoBall`, the
two `balls` components moved along different lines from one point; `gmUfoPill` and `gmUfoJoel`, the `ufo` rose through the same heights and
`point_a1` stood and moved at the same points). `ufo_joel` rolls only with a player at the radio tower's top, which a drill may not arrange, so the
drill enrolled the class by hand and the roll itself is `[RD]`.

When the last peer leaves, the world-actor lane drops its rows though the host's actors live on, so at a joiner's world-ready the host enrols each
live body or saucer again (`coop/world/world_actor_rejoin`, see "A joiner and the actors that already stand"). The body, which stays until a pawn
touches it, then reaches a client that joins later (the body rolled before the client's first life ended stood at the same point for its second), and
a held saucer was delivered at the second world-ready, where its copy flew and ended by itself `[V]`. The Midas UFO (`midasUfoTest_C`) is a
level-placed saucer on a spline, switched on by `setActive(true)` when `ufo_midas` runs its trigger: the host's switch-on is EventCue 141 (its progress
along the spline in `x`, so a joiner is shown it where the host's is), and a client runs its own flyer's `setActive(true)`, which its own tick carries
and ends. The test level holds no such actor, so the `gmUfoMidas` drill stood one on each peer with no spline: the cue was sent and the client's flyer
started in the same second `[V]`; the flight itself was not seen `[RD]`.

The Evil's ending is one moment for every player. The Evil (`theEvil_C`, minted by `calculate_comp` when the `evil` signal reaches the refiner's last
level) is a countdown on the host; when it ends, `checkforevil` looks for a copy of the signal on every drive, drive box and rack, in the desk's saved
signals, the save slot and the drone's sell box, and with one found the graph grants `ending_evil`, saves, switches a post process on, plays white
noise and mints `gameover_C`, whose trip to the menu a session refuses and answers with a revive (`coop/player/run_end_travel`). The host sends
EventCue 160 when its post process comes on, and a client runs the same graph on a stand-in `theEvil_C`, entered past the achievement and the save,
which stay the host's. The host's Evil reaches a client as a prop copy with `active` up, which would count down and judge that client's own drives, so
a client keeps it inert (`coop/world/evil_ending`) `[V]` (`evilEnd` drill, `coop/dev/gamemode_drill`: the host's forced Evil ended 6 s after it was
stood; the client played the ending on a stand-in in the same second, and both peers showed the game-over text, were refused the menu and revived).
Nothing is replayed to a joiner.

The base computer's signal spawns are the host's. A completed signal download runs the desk's `autoSave` (the save button's press runs the same
lines inline), which mints by the signal's name: `tardis` the Tardis (`tardis_C`, beside the base, its `delay` set to 60 before the finish), `trifo`
the TriFO (`spawn_trifo`: `ufo_trifo_C` 12 km below the origin), `shitduende` the shitting duende (`shitDuende_C`, at the level's
`basebathroomtoilet1`), `ker` a marker for the Kerfur Omega's sleep scare and, from the refiner's last level, `evil` the Evil and `deer` the carcass
trigger. The desk is a spawn source of the world-actor lane, narrowed to the first three (`analogDScreenTest_C` in `coop/creatures/npc_world_enum`):
each is announced where it stands, its copy lasts as long as the host's, and the host enrols each live one again for a joiner at its world-ready. A
client's console cannot fire any of it: its `autoSave` is refused (`coop/interactables/desk_sim_sync`), its save-family press is sent to the host
(`coop/interactables/desk_verb_intent`) and its `spawn_trifo` is refused (`coop/world/spawn_authority`) `[V]` (the `sigTardis`, `sigTrifo` and
`sigDuende` drills, `coop/dev/signal_drill`: the client called its own `autoSave` with the signal stood, and held none after).

A copy runs its own presentation and keeps its own tick (`coop/world/world_actor_mirror`): the Tardis waits its `delay`, fades in with its sound and
out 10 s after something enters its sphere; the TriFO spins, blinks and flies its `ufo` component up by a timeline; the duende dissolves when a clear
line runs from its eyes to the local camera, so a copy ends when its own peer's camera sees it. A copy runs none of the host's gameplay: the TriFO's
`powercontrol.solar()` (the blackout, which the light and power lanes carry) is refused when the call comes from a TriFO's own graph, and both the
TriFO's and the Tardis's `lib.setEvent` are refused (`coop/world/spawn_authority`). The Tardis is born with the seconds still to run on the host's
delay, in the world-actor spawn's birth bytes (`coop/world/signal_spawn_copy`), since the class default is another value: both appeared 60 s after the
spawn, and a joiner is born with 0 s once the host's has appeared `[V]`. The TriFO's copy flew the host's route, 18 km up over the base, and its
destroy followed the host's `[V]`; the duende stood at the toilet on both peers `[V]`, and as no camera had a clear line to it the dissolve was not
seen `[RD]`.

The Alien Cart, the Alien Catapult and Throwing Rocks are spawners the host's eventer places (`hillRoller`, `alienJump`, `rockThrow`), and what they
make that a client sees is props. `hillRollerSpawner_C` mints each of its mesh children as the prop its tag names (a pallet, four wheels, a spotlight
head and a paper alien in 0.10.0) when anything overlaps its box, pushes each at 8 m/s, nails them with `nail_C` and `nail_ax_C` and plays a rumble;
`alienJump_C` mints its planks and a paper alien when a clear line runs to player 0's camera and launches them with a scream; `rockThrower_C` (three,
at fixed points) throws a `prop_rock_C` every 2 to 6 s while player 0's pawn stands in its box. The props cross on the prop lane and are driven there
`[V]` (`coop/dev/signal_drill`: `hillRoller` announced seven props on the host and the client made them in the same second; `alienJump` eleven). A
client has no spawner and needs none, and each aims at the host alone: the box and the camera read are player 0's. The nails (physics constraints; none
stood on the client), the rumble and the scream have no lane. The rock throw was not run `[RD]`: its box overlaps only the host's player, which a drill
may not place (`rockThrow`: three throwers stood on the host, none on the client, no rock thrown). The Lantern Prank is the same shape.

The trash prank's flings are the host's: after each `trash` call the host coasts every prop the trasher's box threw and announces each clump it
spawned (`coop/world/trash_fling_sync`) `[V]` (`trashBase`: the flung props streamed and the clumps reached the client within the second). A bed event
replays on every peer, so each player's wake teleport is its own, and the bed it moves is the host's: a client's wake moves no bed, and the host streams
its move (`coop/world/bed_event_gate`); the host's move of the bed was not reached in the drill `[RD]`. The insomniac is rolled on the host and
mirrored with its graph parked (`ticker_insomniacSpawner_C` as a spawn source); it cannot be forced, so no run has shown one `[RD]`.

The boar war's controller runs on the host alone. A boar is a prop with a brain: it crosses on the prop lane, the host claims every boar the gamemode
lists into the driven-prop stream (`coop/creatures/boar_sync`), and a client's copy runs its begin play, its hover tick and its destroy and no other
entry of its graph (`coop/world/copy_graph_gate`); a client's night boar spawner is refused. A client's swing at such a copy (a boar, a fire tank, a
kavotia, an igetis) is cancelled there and sent as a hit intent, which the host judges and applies with the actor's own `addDamage`
(`coop/creatures/hit_target_creature`) `[V]` (`coop/dev/boar_hit_drill`: three axe swings applied, the host's boar from 493 health to 193; the other
three classes take the same path and were not swung at `[RD]`). The fire tanks and the shooters are mirrored with the controller as their spawn
source, and the saucer over the treehouse rides `WeatherState.flags2` (`coop/world/ufo_shield_sync`) `[V]` (`VOTVCOOP_EVENT_FIRE_DRILL=boarwar:150`:
nine boars streamed and gated on the client, its saucer set active, two shooters mirrored; no fire tank spawned and the saucer's leave was not reached
`[RD]`).

The Day-44 gray tank (`graystank`) drops its box from a saucer that waits hidden for a body in its box. The box (`firetankDeploy_C`, a plain actor no
lane carries) falls, then unfolds in its own `spawnParts` into the fire tank and its scrap props; the box is a spawn source narrowed to the tank, the
tank is a world-actor mirror gated as the boar war's tanks are, and a client's own unfolding is refused (`coop/world/spawn_authority`,
`coop/world/copy_graph_gate`; a client never has a box to unfold, so that refusal has not been exercised `[RD]`). `[V]`
`VOTVCOOP_EVENT_FIRE_DRILL=graystankUnfold:260` calls the saucer's `forceDeploy` and the box's `spawnParts` itself (the event alone left the saucer
hidden for 330 s): the host enrolled the tank, the client made its copy and refused its graph, and the scrap props reached the client; the box is not
shown there. The boar war's other source is the fossilhound event (`event_fossilBoarWar_C`, a 2.5 % roll in the day cycle's midnight from day 39 on
`[RD]`), which throws out a fossilhound (a character-lane spawn source, as the ticker's is, see [npcs-and-kerfur.md](npcs-and-kerfur.md)) and a boar
that joins the gamemode's list and is claimed by `boar_sync` `[RD]`. The event cannot be forced, so no run has shown either.

Some of what the gamemode and the desk mint reads one player's camera, bed or use alone, and stays per player, unmirrored `[RD]`: the Red Skeleton
and the Walker (`CustomEvent_4`, a 1 in 65536 roll every 300 s, and `CustomEvent_7`, every 21600 s: each walks to player 0's pawn and, on contact,
ragdolls, pauses or quits the game of the machine it runs on, so a copy on another peer would do that to the wrong player); the Gray cloaking panel's
falling light (`skyFallingEvent_C`, which attaches to player 0's camera manager, flashes that screen and punches the main player); the Zombie Deer's
ghost (`deer_ded_C`, minted 5 to 10 minutes after the `deer` signal's carcass prop `prop_deaddeer_c` stands, at night, and pathing to player 0's
camera); and the Kerfur Omega's sleep scare (the `ker` signal's marker, consumed only while the local game mode is asleep, so it is the host's sleep
and the Omega is already a host-owned character on the npc lane). Each peer's own trigger or roll fires them for its own player, as the game does; the
join's world walk skips the ghost and the falling light (`coop/creatures/npc_world_enum`, and the table below), whose copies would have chased or hung
on the joiner. See [npcs-and-kerfur.md](npcs-and-kerfur.md); none of the rolls can be forced by an event name.

The Rozital hole's controller fires once a player walks into its box. The host's fire mints the room under the hole (a world-actor copy), the device,
the grate and the beacons (props) and the patrol controller, whose kavotias and igetis are mirrored with their graphs gated; a client's own controller
is refused its overlap `[V]` (`rozitalHole`: the client made the room, the device, the beacons and forty kavotia copies, each kavotia's graph refused;
the patrol's movement was not observed `[RD]`). The obelisk falls when a player walks into the host's armed box: the host's `obelisk_C` is a
world-actor copy on a client (the level's `trigger_spawnProp_C` as its spawn source), which runs the fall on its own machine (the sounds, the shakes and
the punch of its own player), while the props the fall mints are the host's, on the prop lane, and the copy's own are refused at the deferred spawn
(`coop/world/obelisk_copy`) `[V]` (`obelisk` with its level box forced: 71 of the client's mints refused, and the host's obelisk prop and seventy
rocks arrived). A save actor a joiner loaded itself (the obelisk, the hole's room, the salt pile, the Soltomia craft) is adopted as the copy when the
host announces it within three metres: `[V]` for the salt pile, `[RD]` for the others, as no run joined during their events.

The walking pyramid rides those rails for its spawn, pose and despawn, the creature lane for its four wisps, and the event lane for its identity; its
own lane owns the one axis nothing generic carries, the brain: the four state-writing timers are cancelled on the mirror, the gather choreography is
relayed, and the wisp it chases is streamed by identity so the mirror runs the native chase look (`coop/creatures/piramid_sync`).

The vent crawler (day 5) is a character, so the creature lane mirrors it and refuses its BeginPlay and box overlap on a client. The crawl is the
host's: the first body in the crawler's box, the host's player or a client's puppet, starts it, and its pose, the doors' moves and the balcony keypads
reach a client on their own lanes. What the crawler's graph plays itself has no lane: the vent steps, the metal break and its burst, the hanging vent's
swing and the signal room door's power. The host sends one EventCue (id 102) at the box overlap and the client plays all of it on the crawl's own
schedule, so the vent stops where the host's does (`coop/world/vent_crawler_sync`) `[V]` (`harness/autotest/autotest_vent_crawler.cpp`: a client a
minute past its join starts the crawl with its own player; its door closes and loses its power, 23 sounds and the burst play, and the doors, keypads
and vent end as the host's).

### The level's wandering fixtures, the Magnet Skeleton and the Basalt Pillars

Four actors are placed in the level, so every peer's world holds its own, and each decides for itself where it stands: the Cursed Church (`church_C`,
whose tick rolls a point of the map and moves there, ringing its bell seven times, while nobody of that machine sees it), the Telephone Booth
(`teleBooth_C`, hidden until its two-hour timer shows it and, one time in ten, moves it), the Siren Pole (`sirenPost_C`, whose tick walks it a step a
second while it was not rendered for a minute) and the Hallway White Door (`hallwayDoor_C`, destroyed by its begin play unless the save was loaded at
03:33). The host's are the world's (`coop/world/level_fixture_sync`): once a second it sends each one's pose, whether it shows and whether it lives, by
level name, on `AmbientFixtureState` with the family `kLevelFixture`; a client refuses the three moves and the door's begin play
(`coop/world/spawn_authority`) and places its own copy where the host's stands, but not while the copy is on screen (`WasRecentlyRendered`, 0.2 s), as
the game moves these only unseen; the church's copy rings its own `bell()` when it takes a place more than 10 m off. A joiner is sent every one and a
census end, after which its white door is destroyed when the host named none. `[V]` (`levelGaps` drill, `coop/dev/level_fixture_drill`: the client's
church, booth and pole followed the host's two 15 m moves, the bell call returned 1 both times, the booth was shown when the host's was, and the
client's door was destroyed as the host held none); `[RD]` for the wait while on screen and for a door the host holds, as neither arose.

The Magnet Skeleton is run by the base computer, so by the host alone: the `process` signal's branch calls `runTrigger` on the level's
`trigger_bloodSkeleton_C`, whose spawner stands the bones and the magnet (props, which cross as props) and the blood stain (`decal_skeletonBlood_C`,
a save actor that is no prop), which rides the world-actor lane from the spawner's source row `[V]` (`levelGaps`: one stain on each peer after the
host's trigger ran).

The Basalt Pillars are the host's field (`coop/world/basalt_pillar`). The buried basalt (`prop_hexpillar_C`) mints a `hexpillar_C` when dug; a
pillar's tick raises it (its `a`, shown by `upd()`) and seeds a neighbour 150 cm off towards player 0. Each mint rides the world-actor lane (a
joiner's own save holds the standing ones, adopted as the copies); a client's pillar never ticks (`spawn_authority`), and the host sends each
pillar's `a` when it moved by 0.005, on `AmbientFixtureState` with the family `kPillarGrowth`. A client's dig is refused on its own copy of the prop
and asked of the host on `PropIgniteIntent` with the verb 2. `[V]` (`levelGaps`: the host's dig gave one pillar on each peer and both read `a` 0.5
after the host wrote it; the client's dig left its buried copy standing, the host ran it, and both held two pillars and no buried basalt); a pillar
seeding a neighbour is `[RD]`, as none did in the drill's 100 s.

Six more classes ride the same lane (`kRows`): the White Door (`TheDoor_C`, a 3600 s `tp()` chain and a 10 to 15 s `open()`/`close()` roll), the
Rorschach decal (`rorGraf_C`, `move()`), the Ghost Log (`log_C`, a tick roll and a 10 s `check`), the Rubber Duck (`prop_duc_C`, a 60 to 300 s resume of
its ubergraph, entry 137), the base's mannequins (`prop_mannequin_C`, moved by `mannequinTp_C`'s tick) and the footprints (`fprints_C`). A client's tp,
`move`, ticks, `check`, duck entry 137 and the shrimp eater's 15 s `eat` timer are refused (`spawn_authority`); `open`/`close` are refused unless the
lane's own call made them. The door's state is the first pad byte of the payload (1 closed, 2 open) and the copy is driven to it by the game's own
`open()`/`close()`; the near-sphere slam is left to each copy. The duck and the mannequins are props whose generated names differ per peer, so the lane
names them by their interactable key. `[V]` (`gapDoor` drill, `coop/dev/level_prop_drill`: the client's copy was driven open, closed, 1500 cm and open
again at the host's own moments, and its own rolls were refused; `gapWander`: the decal, the log, the duck and one mannequin followed the host's 1500 cm
writes, though the duck copy stood 1506 cm away when the host's write arrived, so the prop lane had not carried that move); the 3600 s chain did not run
in the drill `[RD]`.

The footprints' `show()` is a DoOnce; a client's call is refused and sent as `PropIgniteIntent` verb 3, which the host runs
(`level_fixture_sync::TakeIntent`); the shown byte reaches every peer `[V]` (`gapPrints`: the host ran the client's call the same second, the client's
copy showed 3 s later and followed the host's later hide). `shrimpEaterMaster_C`'s timer was refused on the client, and the host's `eat()` (which
returns early unless the reputation tier, the day index, the shrimps' temperature and the nearest-to-`lastloc` rule allow it) took the shrimp props
from 2 to 1 with 1 empty pack in the same second, which the client followed `[V]` (`gapShrimp`, which satisfies each gate and restores the day clock).

A door's `jammed` flag (`door_C::jam(true)`, run by the Reactor Restart's `trigger_jamDoor_C` and by the Soltomia craft; it closes the door and, with
`autocancel`, unjams it 300 s later) is not on the door lane. The host sends each door whose flag changed, once a second, on `AmbientFixtureState`
with the family `kDoorJam` (`id` a hash of the level name, `x y z` its place, `pitch` 1 or 0), the client writes the flag on the door of that name
within 50 cm, and a joiner is sent every jammed door (`coop/world/door_jam_sync`); `trigger_jamDoor_C::runTrigger` is refused on a client. `[V]`
(`jamDoor` drill, `coop/dev/vending_drill`: the host ran `runTrigger`, its two doors went jammed, the client's copies followed a second later and its
own `runTrigger` was refused); the real `event_paperGray` row that arms the trigger was not run `[RD]`.

### A joiner and the actors that already stand

The world-actor lane drops every row when the host's last client leaves (`world_actor_sync::OnDisconnect`, run by `DisconnectAll` at the "all peers
gone" edge of the pump and at the end of a session; a client leaving while another stays runs only the per-slot cleanup), though the host's actors
live on, and its connect snapshot sends only rows it still holds. So at a joiner's world-ready `coop/world/world_actor_rejoin` walks the live
instances of every class of `kWorldActorAllowlist` in the loaded world (the object index, not an array walk) and enrols each one the lane does not
hold, which sends it to every peer (the character lane's own walk is `npc_world_enum::RegisterExistingWorldNpcs`). A joiner is owed the host's actor
unless it already has an actor of its own for it, or the actor reads the one player whose machine runs it:

| class | at a join | why |
|---|---|---|
| `rozitBorg_C`, `arirShip_C`, `jellyfish_C`, `morningUfo_C`, `kocker_C`, `superEgger_C`, `igetis_C` | enrolled | host-made by an event or a spawner (`trigger_eventer_C`, `ufoAlienDropper_C`, `jellyfishPath_C`, `kavotiaPatrolController_C`); no save interface in the capture, not in a level |
| `theBody_C`, `ufo_ballfo_C`, `ufo_boofo_spawn1_C`, `ufo_joel_C`, `ufo_pillfo_C` | enrolled | the gamemode's mints `[V]` |
| `tardis_C`, `ufo_trifo_C`, `shitDuende_C` | enrolled | the desk's mints `[V]` |
| `baocoin_C` | enrolled | the sell gun's coin: a plain `AActor`, never a save actor, and it lies until collected |
| `piramid2_C` | enrolled | the piramid lane's replay needs the joiner's mirror (`piramid_sync` runs after this walk) |
| `obelisk_C`, `holeBottom_C`, `saltpile_C`, `soltomiaCleaning_C` | enrolled | save actors; the joiner's own load holds one and the mirror adopts it within 3 m (`AdoptsSaveTwin`), so the row also makes the host's later destroy reach it; `[V]` for the salt pile |
| `ufoDropper_body_C`, `_car_C`, `_tank_C`, `_pig_C`, `firetank_C` | skipped | save actors (`ufoDropper_C : actor_save_C`; `firetank_C` implements the save interface itself): the blob the joiner loads carries a standing saucer or tank, and no twin adoption exists for these |
| `skyUfo_C`, `tentacleBallsFollower_C`, `skyFallingEvent_C`, `piramidSubpawn_C` | skipped | level actors and triggers nothing spawns (`skyUfo_C`, `tentacleBallsFollower_C`, whose pack belongs to the creature lane), the child of the sandbox `piramidTest_C`, and `skyFallingEvent_C`, which is bound to one player: it attaches to player camera manager 0 and punches `getMainPlayer` |

Each class with a standing instance logs `world-actor[join-enrol]` with the count and the number enrolled now, and a class that carries `skipSave` (a
save actor) with no twin adoption logs a warning, since the table's reading of the capture is then wrong for it; the classes with no functions in the
capture (`ufoDropper_body_C`, `_car_C`, `ufo_boofo_spawn1_C`, `piramidSubpawn_C`) rest on their siblings' parent calls and that warning `[RD]`. Run on a
`rozitBorg_C` and a `morningUfo_C`: the host fired the event and held the actor, the client was killed and started again, the host logged `drained 1
WorldActor element(s)` at the client's leaving and `join-enrol ... 1 enrolled now` at the second world-ready, and the client counted 0 before it and
exactly 1 after, a copy, never a second `[V]`. For the salt pile the client's own load held a `saltpile_C`, the host's row adopted it, and the count
stayed 1 `[V]`.

### The base alarm

One trigger actor is the base alarm: the siren, every red beacon, the ceiling flicker, the basement grate and the in-flight registry. It is lit when
the radar sweep hits an important point (that is how an event lights it) or by the joke food, and stopped only from the radar panel. The verb is
idempotent and invisible, so both peers poll the active flag once a second: the host broadcasts a transition, and a client that transitions first (its
own sweep tripped, its player pressed stop) sends its transition for the host to canonicalise (`coop/world/alarm_sync`). The radar terminal's own
beeps are per-viewer by design and have no lane.

### Emails and the daily task

Every email producer funnels through one gamemode function into the save's list; the host owns the append, and a receiver reproduces the whole thing
in one reflected call, the row, the ding at the laptop and the tab highlight, re-stamping the date from the synced clock; a deletion is mirrored from
either side (`coop/world/email_sync`). The daily task is host-authored, because every live writer of it runs only on the host (its creation is part of
the midnight a client's clock never reaches, and the drone sale is suppressed on clients). The host polls a change hash and sends the task state, whole
to a joiner at its world-ready (`coop/world/daily_task_sync`); a receiver that cannot apply a task yet keeps the latest and retries each second, and
drops it if its world changes first; the writes set the task's state and pay no reward. The rewards land in the shared balance, on
[base-computer.md](base-computer.md).

### Plants

A `growingPlant_C`, a player's planted seed or a winter bush alike, is a saved actor that a tick grows on its water, sun and rain. The host runs every
plant and sends `PlantState` with its row, place and values: at once on an edge a viewer sees (ripening, death) and about every 5 s while it changes
slowly (`coop/world/plant_sync`). A client's copy does not tick: its tick, the herb's `grown` and a sprinkler's watering are refused at the script gate
(`coop/world/plant_gate`), and so are the spawners that plant (`coop/world/spawn_authority`), so any change on it was that client's player, and it goes
to the host as `PlantEdit`, naming only the values that changed. The host judges each edit (`coop/world/plant_edit_gate`): the key, the row and class,
the sender's reach, the pot a planting names and the bounds of the values, at a bounded rate per client; it answers a refused edit of a plant it holds
with that plant's state. A plant is found by its save key, so what a join's load brought is already the host's, and a client's edits wait until its
join has settled. A client's harvest leaves the host's plant empty and the item with the harvester. Tests: `harness/autotest/autotest_plant_state.cpp`.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the clock, the sky | the host | streamed; the client's clock held at zero rate |
| rain, snow, fog, wind, lightning | the host | scheduler observed on the host, cancelled on the client; the client's births killed |
| the red sky | the host | its toggle sent from the host as it returns, refused on the client |
| the Bad Sun's existence | the host | its spawn and its copy's `remove()` sent as they return, a client's own spawn refused; each peer's copy runs by the shared clock and its harm is local |
| fireflies | each peer | peer-symmetric union |
| ambient spawners | the host | refused on clients at the script-body gate; the camera-anchored ones per peer |
| a scheduled or story event | the host | observed at its verb; replayed on the client per row |
| the in-flight registry | the game, on each peer | mirrored to a joiner as a snapshot |
| an event's actors | the host | the world-actor mirror |
| the pyramid's brain | the host | timers cancelled on the mirror; the gather relayed |
| the base alarm | whoever transitions | polled; the host canonicalises |
| emails, the daily task | the host | one reflected append; a change-hash poll |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `ClockPose` (stream), `SkyState` | the host to all | the clock and the day number; the dome, the moon and the eye |
| `WeatherState`, `LightningStrike`, `RedSky`, `BadSun` | the host to all | the state fields; a strike location; the red sky's edge; whether a Bad Sun exists |
| `FireflySpawn` | each peer, relayed | one spawn |
| `EventFire`, `EventSnapshot`, `EventCue` | the host to all; the host to one joiner; the host to all | a fired row; an in-flight event; an emitter cue |
| `WorldActorSpawn`, `WorldActorDestroy`, `WorldActorPose` (stream) | the host to all | an event actor's birth with scale and blob; its death; full-rotation poses |
| `PyramidGather` | the host to all | the gather commit |
| `AmbientFixtureState` (families `kLevelFixture`, `kPillarGrowth`, `kDoorJam`) | the host to all | a level fixture's pose, shown and alive, by its level name; a Basalt Pillar's height by its element id; a door's `jammed` |
| `PropIgniteIntent` (verbs 2, 3) | a client to the host | a dig of the buried basalt, by the prop's key; the footprints' `show` |
| `AlarmState` | either role | the alarm's active flag |
| `EmailAppend`, `EmailDelete`, `TaskNewState` | the host to all; either role; the host to all | an email; a deletion; the task |

## Late join

The clock streams from the connect, so a joiner's first sample once its world exists sets its time and day number; the sky (its eye included), the
weather state, the red sky's state and whether a Bad Sun exists are seeded at the joiner's world-ready (the join blob already carries a saved Bad Sun
copy, and the seed settles it against the host's state). The in-flight events arrive as snapshot entries and replay with the override; the live cues
are re-sent; every tracked event actor is re-spawned on the joiner, after the standing ones the lane dropped are enrolled (`world_actor_rejoin`); the
pyramid's gather in flight is re-sent after its mirrors exist; the alarm's flag is sent unconditionally, and the joiner runs the native trigger, which
is idempotent. Emails are seeded as a delta against the blob instant; the task state is sent at the ready edge. A one-shot cue a joiner was not
present for is missed, by definition.

## Known limits

| Limit | Evidence |
|---|---|
| The flesh rain, the red phone, the stolen radio tower, the lake glow and the locker head and corpse are the host's roll and reach each peer as a cue (above); what is per peer is what each reads off its own player: the rain's clumps fall about each player's camera and a client's are its own local props, the tower vanishes and the phone stops ringing for the player who looks away or walks up, the lake pulls the player in its box. The Black Hole Sun's mails and the ship tower's reputation are not part of this | `[V]` the drills named in the table above |
| A head's locker opened by the host's reflected `open` left a client's copy unopened in the drill, so the scare shows on the peer whose locker runs its own `open` | `[V]` `lockerHead` drill: the client's head was still set 20 s after the host's open |
| A decorated Christmas tree spawns its gifts on each peer whose player sleeps through midnight, and a client's gifts are its own | `[RD]` the tree's own check of the local player's sleep at hour 0 |
| Several rolls are still per peer: the rare gamemode rolls (the one-percent forced quit), the server break-minigame variant, the underground loot mounds, the signal scramble and the radio-tower shuffle | `[V]` no lane under `coop/world` carries them; `coop/interactables/garbage_sync` names the mounds |
| The flora spawner is not refused on a client, so each peer rolls its own (the deer, walking trees, dirt holes and beehives are the host's, `coop/creatures/ambient_creature_sync` and `coop/world/ambient_fixture_sync`, and so are the walking mannequins, whose spawner is a row of `coop/world/spawn_authority`) | `[V]` no row of `coop/world/spawn_authority` names it; see "Which spawners a client refuses" in [npcs-and-kerfur.md](npcs-and-kerfur.md) |
| A plant runs on the host's sun, rain and view of the world alone: a client refuses the bodies that change a plant on their own (`coop/world/plant_gate`). A fruit a client's toolgun forces on a plant that bears fruit as props stays on that client, as the prop it spawns does not cross; the host grows its own at its next ripening. What a client's copy shows between two gradual sends can be up to 5 s old | `[V]` `coop/world/plant_sync`; `tool_plantFruit_C`; `[?]` `coop/world/plant_gate` |
| Rain falls inside the base for a player standing indoors, the host included: the game's own effect, the same in single player. The rain emitter follows the player's camera and spawns its drops around it, below the roof's rain-collision mesh, which stops only drops falling onto it from above | `[V]` `coop/world/weather_rain` never moves the emitter or the camera; `[RD]` the decompiled day-night cycle's emitter move |
| A client that joins during a vent crawl gets the crawler from the join census but not the crawl's sounds, the vent's swing or the door's power, which start from the crawl's one cue; the crawl was forced through the eventer's own verb, not reached by the day 5 schedule | `[?]` `coop/world/vent_crawler_sync` |
| A client's hit on a Basalt Pillar breaks only its own copy; the Siren Pole's heading and the pillars' seeding read the host's player alone | `[RD]` `hexpillar_C`'s damage goes to its own `physicsimpact`; the seed blends towards `GetPlayerCharacter(0)` |
| The Easter hunt, the Erie plush and the vending machines are the host's and are on [easter-hunt.md](easter-hunt.md), [erie.md](erie.md) and [vending.md](vending.md). The Erie lane has not yet been run in a two-peer session | `[?]` see [erie.md](erie.md) |
| Trigger-volume fires (a bed event, a scare a player walks into) run per peer, as the single-player design intends | `[V]` by design, not a gap |
| Only the pyramid and the alarm have had the one-event-at-a-time pass; every other event rides the generic lanes on the per-row replay policy | `[V]` `coop/world/event_fire_sync`, the replay allowlist |

## Code map

| Concept | Files |
|---|---|
| the clock and the sky | `coop/world/time_sync`, `coop/world/day_edge`, `coop/world/sky_sync`, `ue_wrap/world/daynightcycle`, `ue_wrap/world/profile`, `ue_wrap/world/event_list`, `ue_wrap/world/skysphere` |
| weather | `coop/world/weather_sync`, `coop/world/weather_rain`, `coop/world/weather_fog`, `coop/world/weather_lightning`, `coop/world/weather_redsky`, `coop/world/weather_badsun`, `coop/world/weather_event_births`, `ue_wrap/world/directionalwind`, `ue_wrap/world/bad_sun` |
| fireflies and the spawners | `coop/world/firefly_sync`, `coop/world/spawn_authority` |
| events | `coop/world/event_fire_sync`, `coop/world/event_active_sync`, `coop/world/creature_event_relay`, `coop/world/solto_sync`, `coop/world/event_cue_sync`, `coop/world/vent_crawler_sync`, `coop/world/event_world_sync`, `coop/world/signal_spawn_copy`, `coop/dev/event_trigger`, `coop/dev/world_event_drill`, `coop/dev/signal_drill` |
| event actors and the pyramid | `coop/world/world_actor_sync`, `coop/world/world_actor_mirror`, `coop/creatures/piramid_sync`, `coop/world/copy_graph_gate`, `coop/world/obelisk_copy` |
| what an event moves | `coop/world/event_lanes`: `coop/world/agrav_sync`, `coop/world/trash_fling_sync`, `coop/creatures/boar_sync`; `coop/world/ufo_shield_sync`, `coop/world/bed_event_gate` |
| the level's fixtures, the pillars, the jammed doors | `coop/world/level_fixture_sync`, `coop/world/basalt_pillar`, `coop/world/door_jam_sync`; the drills `coop/dev/level_fixture_drill` (`levelGaps`), `coop/dev/level_prop_drill`, `coop/dev/vending_drill` (`jamDoor`) |
| the alarm, emails, the task | `coop/world/alarm_sync`, `coop/world/email_sync`, `coop/world/daily_task_sync`, `ue_wrap/world/email` |
| tests | `harness/autotest/autotest_weather.cpp`, `autotest_fog_probe.cpp`, `autotest_eventforce.cpp`, `autotest_alarmforce.cpp`, `autotest_piramidforce.cpp`, `autotest_vent_crawler.cpp`; the event drill, `coop/dev/event_drill`; the world roll drill, `coop/dev/world_roll_drill` |
