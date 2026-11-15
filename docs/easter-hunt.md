# The Easter hunt

## Purpose

The Easter hunt (the old well, the Ominous Easter Bunny, its twelve eggs, the bunny ears and the achievement), so the players
of one session share one hunt: one bunny, one set of eggs, one list of the eggs handed in. Story events in general are on
[events-and-weather.md](events-and-weather.md); the world-actor lane the bunny rides is on [npcs-and-kerfur.md](npcs-and-kerfur.md).

## How it works

In the season (15 March to 15 May, `mainGamemode.isEaster`) the well's zone (`easterZone_C`, level-placed) counts the carrots
thrown down it in its tick. At seven it destroys them, plays its rocket animation (`playAnimation`, about 16 s) and mints the
bunny (`ominousBunny_C`, a save actor). The bunny is talked to through six child talk objects (`tutorialHint_C`). The first
talk's end runs `fin`: an egg at each of the twelve `easterEggSpawn_C` points (`spawnEgg`, a `prop_easteregg_bunnyOwn_C`), the
bunny ears and a note thrown (`throwItems`). A later talk's end runs `CustomEvent`, which throws the ears and a note again. An
egg used on the bunny (`playerUsedOn`) is struck off its list; the last one sets `collectedEggs` and starts the closing talk,
whose end (`finishTalking_eggsCollected`) grants `eastercomplete`, explodes and destroys the bunny. A bunny destroyed before that
mints the angry one (`superHolyShitAngryBunny_C`) in `ReceiveDestroyed`.

Left alone, each of these would run on every peer for itself. The hunt is the host's (`coop/world/easter_hunt`, with the refusals in
`coop/world/spawn_authority`):

- **The well.** The zone's tick is refused on a client, so the host alone counts. The host's `playAnimation` is told to every
  client as `EventCue` 161 and the client calls `playAnimation` on its own zone. The animation's graph mints the bunny at its
  resume entry 1276: that resume is refused on a client and the graph's tail (entry 1456) is entered from the lane's tick, so the
  client's zone ends its animation as the host's does and mints nothing.
- **The bunny.** `ominousBunny_C` and `superHolyShitAngryBunny_C` are world actors of the host (allowlisted; minted by
  `easterZone_C` and `ominousBunny_C`; a loaded bunny is adopted as the save twin). The copy keeps its own tick, which only turns
  its head to the local camera. On a copy `spawnEgg`, `throwItems` and `ReceiveDestroyed` are refused, so it mints no egg, no
  item and no angry bunny; the host's eggs, ears and notes cross as props and its angry bunny as a world actor.
- **A client's talk and egg.** On a copy `fin`, `CustomEvent` and an egg's `playerUsedOn` are refused and sent as
  `PropIgniteIntent` verbs 4, 5 and 6 (the egg by its prop key, else its element id). The host runs the same call on its bunny;
  `fin` only while `spawnedEggs` is false, since each run mints a full set of eggs.
- **The talk offered.** Twice a second the host reads the bunny's `spawnedEggs`, `collectedEggs` and, of each talk object,
  `active` and shown, and on a change sends them as `AmbientFixtureState` family `kEasterBunny`. The copy writes the flags, calls
  `setActive` and sets hidden on its talk objects, so every player is offered the talk the host's bunny offers. When
  `collectedEggs` rises the copy starts its own closing talk.
- **The end.** The host's `finishTalking_eggsCollected` is told as `EventCue` 162; the client grants its own profile
  `eastercomplete`. The client's own closing talk's end is refused (the host's bunny ends the hunt and its destroy crosses).

The talks themselves are each player's own reading and are not sent.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the carrot count, the animation's start | the host | the client's zone tick refused; `EventCue` 161 |
| the bunny, the angry bunny | the host | world actors; the copy's mints refused |
| the eggs, the ears, the notes | the host | host props, sent by the prop lane |
| the eggs handed in, `spawnedEggs`, `collectedEggs`, which talk is offered | the host | polled 2 Hz, sent on a change; the copy writes them |
| a talk's end, an egg used | the host | the client's call refused and sent as `PropIgniteIntent` verbs 4 to 6 |
| the achievement | each player's own profile | granted at `EventCue` 162 |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `EventCue` 161, 162 | the host to all | the well's animation began; the closing talk ended |
| `AmbientFixtureState` (family `kEasterBunny` 5) | the host to all | `id` the bunny's world-actor id, pad bytes 0 and 1 the talk bits (two a talk object), pad2 byte 0 the flags |
| `PropIgniteIntent` (verbs 4, 5, 6) | a client to the host | the first talk ended; a later talk ended; an egg used (key, element id) |

## Late join

The bunny reaches a joiner as any world actor does, and its state is sent again at the connect
(`QueueConnectBroadcastForSlot`). A state that arrives before the copy waits and is applied when the copy stands.

## Known limits

| Limit | Evidence |
|---|---|
| The closing talk, the cue 162 and the achievement were never run: a drill that finishes the hunt would grant `eastercomplete` to the test machine's profile | `[RD]` `ominousBunny_C.finishTalking_eggsCollected`; the code is `easter_hunt::OnEndPre` and `OnCue` |
| The zone's own carrot count starting the animation was not run (out of season the tick does not count); the drill calls `playAnimation` on the host as the tick does | `[RD]` `easterZone_C.ReceiveTick` |
| A joiner whose save already holds a bunny (the save-twin adoption) was not run | `[RD]` `coop/world/world_actor_mirror` |
| The closing talk starts on a copy when `collectedEggs` rises, whoever handed the last egg in, so every player hears it; a player who does not read it to its end still gets the achievement with the host's | by design |
| The angry bunny's copy is driven by pose alone; what it does to a client's player is the host's actor's doing | `[V]` one angry bunny on each peer after the host's bunny was destroyed early |

What was measured, by the `easterHunt` drill on two peers (`coop/dev/easter_drill`) `[V]`: the host's `playAnimation` sent the cue, the client's
zone played its animation, its mint was refused and its tail ran; one bunny stood on each peer with six talk objects; the client's `fin` was
refused and run by the host (12 eggs, 1 ears and 1 note more on each peer); the client's egg was refused and run by the host (11 eggs on each);
the client's `CustomEvent` was refused and run by the host (2 ears on each); the talk bits followed each time; the host's bunny destroyed early
gave one angry bunny on each peer, the client's copy's own mint refused. No script error or fault was logged on either peer.

## Code map

| Concept | Files |
|---|---|
| the zone's cue and tail, the bunny's intents and state, the end's cue | `coop/world/easter_hunt`; the routing in `coop/dispatch/event_dispatch_prop_drive`, `coop/dispatch/event_dispatch_intent`, `coop/world/event_cue_sync`; the ticks in `coop/world/event_lanes`; the joiner's resend in `coop/session/connect_replay` |
| the refusals on a copy (zone tick, `spawnEgg`, `throwItems`, `ReceiveDestroyed`) | `coop/world/spawn_authority` |
| the bunnies as world actors | `ue_wrap/core/sdk_profile_names.h`, `coop/creatures/npc_world_enum`, `coop/world/world_actor_mirror` |
| the drill | `coop/dev/easter_drill` (`easterHunt`) |
