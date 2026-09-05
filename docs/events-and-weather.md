# Events and weather

## Purpose

The shared world's own progression: the clock and the sky, the weather, the ambient spawners
and who is allowed to roll them, the story and scheduled events and how a client sees one fire,
the actors an event flies in, the walking pyramid, the base alarm, the emails and the daily
task. The rule under all of it: shared-world randomness is rolled once, on the host, and a
client never rolls a shared outcome.

## How it works

### The clock and the sky

The game's clock lives on one actor, the day-night cycle, and the sun is a pure function of it.
The host polls the clock and pushes it on a throttle and at a joiner's connect edge; the client
writes the three fields and keeps its own time scale at zero, so its scheduler never advances and
nothing it owns can fire on its own (`coop/world/time_sync`). The one exception is the shared
sleep: while everyone is in bed the client's clock runs at the accelerated scale for the phase
(the sleep gate is on [players.md](players.md)). An absolute clock snapshot also streams a few
times a second so a client's sun never steps. The star dome's random orientation and the
save-derived moon phase are pushed once (`coop/world/sky_sync`).

### Weather

One actor owns the weather: its scheduler timers for rain, lightning, fog, super fog and
permanent rain, a dozen state fields, and the mutator functions everything else calls. On the
host, observers on the five scheduler functions read the state after each fires, dedupe it by
hash and broadcast it on change and at a joiner's connect (`coop/world/weather_sync`). On a
client the same five scheduler bodies are cancelled before they run, so the client never rolls
"rain now", and a received state is applied through the mutator functions rather than field
writes, because the snow function alone has dozens of Blueprint listeners (`coop/world/weather_rain`).

Fog is not a flag: the enable flags are config gates, and the active fog is an actor (the rolling
fog controller, the super fog) that ramps the height-fog density over its own duration and
destroys itself; the host mirrors the actor's life (`coop/world/weather_fog`). A lightning
strike is a transient actor; the host observes its spawn and sends the location
(`coop/world/weather_lightning`). The red sky is a story event actor whose organic trigger is a
one-percent roll in the new-day handler on every peer, dispatched where no hook sees it, so the
host polls the state field for the edge and the client's own roll is killed at birth: every
new-day weather birth (red sky, black fog, rolling fog) funnels through the engine's
finish-spawning call, where a client-side catch destroys any birth the host did not command
(`coop/world/weather_redsky`, `coop/world/weather_event_births`).

Fireflies are the one peer-symmetric weather: the spawner rolls a ring around the local camera,
so every peer keeps its own and shares each spawn, and the union is fireflies near everyone
(`coop/world/firefly_sync`).

### Who rolls

A connected client ticks no shared-world spawner and rolls no shared-world spawn
(`coop/world/spawn_authority`), by one table of two shapes: the spawner's tick is parked
through the engine's own tick switch, re-parked once a second and restored when the session
ends, or the spawner's entry function is cancelled before its body. Shared-world content then
arrives only from the host through the character, world-actor and prop mirrors. A spawner
anchored to the player's camera (the pinecone drops) is the exception: each peer rolls its own
and the drops are shared peer-symmetrically. The remaining randomness is closed shape by shape:
a mirror where the host owns the roll and the state, an intent where a client's action makes
the host roll (a shop delivery time, a signal catch, a device claim), and a shared seed where
the game already uses a seeded stream. Cosmetic-local randomness (view bob, flicker, exhaust)
is left alone.

### Story and scheduled events

The scheduler is the save's own time step: it walks the event table and runs each due row
through the eventer, all inside Blueprints. The host cannot hook the fire, so it polls the
save's list of passed events once a second and broadcasts each new row
(`coop/world/event_fire_sync`). The client keeps its own copy of the walkable event list empty,
a one-integer write the game rebuilds unconditionally at every world load, so its scheduler
fires nothing even while its clock runs during a shared sleep. What a client does with a
received fire is a per-row policy kept in the code: rows whose outputs already ride a lane
(props, creatures, the ATV, sleep, the wisps, the cues, the devices) are not replayed, because
replaying them would deliver the effect twice; the level flips, story flags and cosmetic sounds
no lane carries are replayed through the same native verb; a random prank is never forwarded,
because the client would roll a different one. A replay is skipped when the client's own passed
list already carries the row, unless the host says the event is in flight. The developer menu's
event trigger dispatches through the same host path, so a forced event broadcasts like a
scheduled one.

The game keeps a registry of in-flight events: a reference count and the live event actors,
maintained by the actors themselves. The host polls it once a second and, at a joiner's ready
edge, sends one snapshot entry per in-flight event so the joiner replays it from the start with
the in-flight override (`coop/world/event_active_sync`). Events whose only trace is a one-shot
particle emitter (the meteor shower) are mirrored as cues: the host finds the live emitter,
broadcasts its identity and position, and a client spawns the same emitter
(`coop/world/event_cue_sync`).

### The actors an event flies in

The saucers, the mothership, the ariral ships, the sky objects, the jellyfish, the fire tank and
their kin are not characters, so the character lane cannot drive them. They have their own
mirror (`coop/world/world_actor_sync`): a second interceptor on the deferred-spawn call with a
name-matched allowlist disjoint from the character one, a full-rotation pose batch, a spawn
that carries scale and a class-interpreted birth blob (a coin is born with the points its
material comes from), a destroy broadcast, and a dead-retire in the host's pose walk for the
actors that destroy themselves at an event's end. A spawner that runs inside a Blueprint (the
pyramid's) is caught at the native function seam and gated by the spawning class. A heading
that lives outside the actor's rotation streams as an auxiliary yaw.

The walking pyramid rides those rails for its spawn, pose and despawn, the creature lane for
its four wisps, and the event lane for its identity; its own lane owns the one axis nothing
generic carries, the brain: the four state-writing timers are cancelled on the mirror, the
gather choreography is relayed, and the wisp it chases is streamed by identity so the mirror
runs the native chase look (`coop/creatures/piramid_sync`).

### The base alarm

One trigger actor is the base alarm: the siren, every red beacon, the ceiling flicker, the
basement grate and the in-flight registry. It is lit when the radar sweep hits an important
point (that is how an event lights it) or by the joke food, and stopped only from the radar
panel. The verb is idempotent and invisible, so both peers poll the active flag once a second:
the host broadcasts a transition, and a client that transitions first (its own sweep tripped,
its player pressed stop) sends its transition for the host to canonicalise
(`coop/world/alarm_sync`). The radar terminal's own beeps are per-viewer by design and have no
lane.

### Emails and the daily task

Every email producer funnels through one gamemode function into the save's list; the host owns
the append, and a receiver reproduces the whole thing in one reflected call, the row, the ding
at the laptop and the tab highlight, re-stamping the date from the synced clock; a deletion is
mirrored from either side (`coop/world/email_sync`). The daily task is host-authored, because
every live writer of it runs only on the host: the task creation on the frozen client clock
cannot, and the drone sale is suppressed on clients; the host polls a change hash and sends the
task state (`coop/world/daily_task_sync`). The rewards land in the shared balance, on
[devices.md](devices.md).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the clock, the sky | the host | pushed on a throttle; the client's scheduler frozen |
| rain, snow, fog, wind, lightning, red sky | the host | scheduler observed on the host, cancelled on the client; the client's births killed |
| fireflies | each peer | peer-symmetric union |
| ambient spawners | the host | parked or cancelled on clients; the camera-anchored ones per peer |
| a scheduled or story event | the host | observed by poll; replayed on the client per row |
| the in-flight registry | the game, on each peer | mirrored to a joiner as a snapshot |
| an event's actors | the host | the world-actor mirror |
| the pyramid's brain | the host | timers cancelled on the mirror; the gather relayed |
| the base alarm | whoever transitions | polled; the host canonicalises |
| emails, the daily task | the host | one reflected append; a change-hash poll |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `TimeSync`, `ClockPose` (stream), `SkyState` | the host to all | the clock; an absolute snapshot; the dome and the moon |
| `WeatherState`, `LightningStrike`, `RedSky` | the host to all | the state fields; a strike location; the red sky's edge |
| `FireflySpawn` | each peer, relayed | one spawn |
| `EventFire`, `EventSnapshot`, `EventCue` | the host to all; the host to one joiner; the host to all | a fired row; an in-flight event; an emitter cue |
| `WorldActorSpawn`, `WorldActorDestroy`, `WorldActorPose` (stream) | the host to all | an event actor's birth with scale and blob; its death; full-rotation poses |
| `PyramidGather` | the host to all | the gather commit |
| `AlarmState` | either role | the alarm's active flag |
| `EmailAppend`, `EmailDelete`, `TaskNewState` | the host to all; either role; the host to all | an email; a deletion; the task |

## Late join

The clock, the sky, the weather state and an active red sky are seeded at the joiner's connect.
The in-flight events arrive as snapshot entries and replay with the override; the live cues are
re-sent; every tracked event actor is re-spawned on the joiner; the pyramid's gather in flight
is re-sent after its mirrors exist; the alarm's current flag is sent unconditionally, and the
joiner runs the native trigger, which is idempotent against a save that already carried it.
Emails are seeded as a delta against the blob instant; the task state is sent at the ready
edge. A one-shot cue a joiner was not present for is missed, by definition.

## Known limits

| Limit | Evidence |
|---|---|
| A black fog the host rolls has no wire lane yet; the client's own rolls are suppressed | `[V]` the weather-birth seam's table |
| The lightning lane has never been seen received on a client in any log | `[?]` the lane exists; no receive line found |
| Several rolls are still per peer: the rare gamemode rolls (a one-percent forced quit, dreams, hall food), the server break-minigame variant, the loot content of mid-session containers and piles, the signal scramble and radio-tower shuffle | `[V]` the tracker's open rows; some measured, some inferred |
| The beehive, walking-tree, growing-plant, hole and leaf spawners are not yet censused for their anchor | `[?]` the spawner table's open rows |
| Trigger-volume fires (a bed event, a scare a player walks into) run per peer, as the single-player design intends | `[V]` by design, not a gap |
| Only the pyramid and the alarm have had the one-event-at-a-time pass; every other event rides the generic lanes on the per-row replay verdict in the code | `[V]` the events tracker |

## Code map

| Concept | Files |
|---|---|
| the clock and the sky | `coop/world/time_sync`, `coop/world/sky_sync`, `ue_wrap/world/daynightcycle`, `ue_wrap/world/skysphere` |
| weather | `coop/world/weather_sync`, `coop/world/weather_rain`, `coop/world/weather_fog`, `coop/world/weather_lightning`, `coop/world/weather_redsky`, `coop/world/weather_event_births`, `ue_wrap/world/directionalwind` |
| fireflies and the spawners | `coop/world/firefly_sync`, `coop/world/spawn_authority` |
| events | `coop/world/event_fire_sync`, `coop/world/event_active_sync`, `coop/world/event_cue_sync`, `coop/dev/event_trigger` |
| event actors and the pyramid | `coop/world/world_actor_sync`, `coop/world/world_actor_mirror`, `coop/creatures/piramid_sync` |
| the alarm, emails, the task | `coop/world/alarm_sync`, `coop/world/email_sync`, `coop/world/daily_task_sync`, `ue_wrap/world/email` |
| tests | `harness/autotest/autotest_weather.cpp`, `autotest_fog_probe.cpp`, `autotest_eventfire.cpp`, `autotest_eventforce.cpp`, `autotest_cueforce.cpp`, `autotest_alarmforce.cpp`, `autotest_piramidforce.cpp` |
