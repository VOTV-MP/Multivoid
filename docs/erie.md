# The Erie plush

## Purpose

The Erie plush (`prop_eriePlush_C`), its quest, and the live Erie it makes, in each form it takes: one thing for every player, the host's.
What is built, what a client's copy is refused and what is asked of the host, and what is left. Props in general are on
[props.md](props.md), the creature lanes on [npcs-and-kerfur.md](npcs-and-kerfur.md).

**The lane has not yet been run in a two-peer session.** It is written and it builds; two drills for it exist (`erieQuest` and `erieForms`,
`coop/dev/erie_drill`) and have yet to be run on two peers. So nothing here is `[V]`: a claim about the game's own Blueprint is `[RD]`, read
in the decompiled game, and a claim about what the lane does is `[?]`, written and not run. No run has granted the achievement, and none may: the
achievement step is unrun by design, as granting `liveerie` would write the test machine's profile.

## How it works

### The quest on the plush

`prop_eriePlush_C` is a `prop_food_C`: every peer holds a real copy with its whole Blueprint, as with any keyed prop. Its hit handler (the
bound hit event of its mesh) is the quest `[RD]`: with no `food` chosen a paper that hits it is eaten and sets `haspaper`, a pencil sets
`haspencil` (each runs `upd`, which shows the paper or pencil on the plush, and `write`); with both, `write` waits 4 s, picks a `food` from
a list, mints a note (`prop_notebook_paper_C`, its text the food's display name) and a pencil and throws them at player 0's camera. With
a `food`, a prop named for it that hits the plush is eaten (4 s of `munch`), `fullness` rises by 0.01 and `randomBoolWithWeight(fullness)`
decides `fuck`: the morph, which mints `eriePlushLive_C`, grants the achievement `liveerie` and destroys the plush. A super eater
(`prop_eriePlush_superEater_C`, which the Erie gun fires) eats any prop that hits it. Fire and microwaves end in `anger`
(`pains` over 120 mints `npc_angryErieFlesh_C`); an explosion of 10 or more on a plush with `templessthan0` set calls
`lib.replaceProp(..., "mytalkingerie")`. Two timers run on every copy: `lonetimer` (30 s: with no pawn within 25 m of the plush it counts, and past 5 counts it moves the plush
onto a prop near player 0; it also lets `pains` fall) and `eatcheesetimer` (30 s: one time in four, while nobody sees the plush, a food
lying unseen within 2.5 m of player 0 is moved to it, eaten with a sound and destroyed). None of the
plush's quest variables is in a save record of its own (the class declares no `getdata`), so a reload resets the quest `[RD]`.

The host owns the quest (`coop/world/erie_sync`):

- **The hit.** A client's copy runs no handler. The game's hit event on the copy is refused at the script gate; when the other actor is
  a paper, a pencil or a food (any prop, for the super eater) it is sent to the host as `PropIgniteIntent` verb 12 (`key` the plush's
  interactable key, `elementId` the other prop's id, which a local prop answers by the element tracker and a host-made one by the mirror
  reverse), at most once in 1.5 s for one pair. The host finds its own plush by the key and its own copy of the prop by the id, judges that the
  two stand within 6 m, and runs the same handler with them, so `haspaper`, `food`, `fullness` and the morph are only ever the host's.
  The alternative was to let the handler run on the peer whose physics produced the hit; that leaves the quest's state, its notes and the
  morph on whichever peer threw the paper, and no wire carries a plush's `food` or `fullness` `[RD]`.
- **What the host's quest makes crosses by the lanes it already has.** The eaten prop's destroy and the note and pencil `write` mints are
  host props born inside a Blueprint graph; they cross as props `[?]`.
- **The flags.** `haspaper` and `haspencil` are what the copy shows. The host sends them once a second when one changed, on
  `AmbientFixtureState` family `kEriePlush` (`id` a hash of the key, `_pad[0]` bit 0 the paper, bit 1 the pencil); the copy writes them and runs its own
  `upd`. A joiner is sent every plush with a flag set.
- **What a copy is refused** (`coop/world/spawn_authority`, a row each): `fuck`, `anger`, `write` (the mints), `lonetimer`,
  `eatcheesetimer`, `firedamage`, `microwave` and `exploded` (the verdicts that end in `anger` or `replaceProp`).
- **The achievement.** The host's call of `lib.progressAchievement` is watched; one for `liveerie` is the morph's. The call runs, and
  every client is sent an `EventCue` (id 170), on which it grants its own profile.

### The forms of the live Erie

The live Erie is not a prop: `eriePlushLive_C` is a `Character` that implements the prop interfaces (hold, use, save). Its good and evil
kinds are subclasses. It walks to navmesh points, dances, steals a prop within 10 m every 30 s (the prop is destroyed into its hands and
comes back when it throws it or ends), mints a bomb bottle and becomes the evil kind at `rep` -100, becomes the good kind at 100, and
is minted into the other forms: a pick up (the one action it offers, `actionOptionIndex` 7) makes the drag form and destroys it; fire makes
a drag form (the good kind) or a chip pile; a slice makes the chop. The forms are the host's actors on the lanes the game's other creatures use:

| Class | Lane | Why |
|---|---|---|
| `eriePlushLive_C`, `_good_C`, `_evil_C` | npc lane (`kNpcAllowlist`, a late entry: the install does not wait for the class) | a Character |
| `npc_angryErieFlesh_C` | npc lane (late entry) | a Character |
| `erieDrag_C`, `_good_C`, `_evil_C`, `erieChop_C` | world-actor lane (`kWorldActorAllowlist`, leaf names) | an Actor and an `actor_save_C`, no Character |

Every Blueprint function that mints a form is a spawn source (`coop/creatures/npc_world_enum`, one row per exact class: the plush, the
super eater, the three live kinds, the three drag kinds, the chop, the three held kinds, the cremator), so the host's mint is enrolled
whichever graph made it. A client's own mint of an npc form is cancelled at the spawn; the three refusals of a client's copy that would mint a form
the host's copy also mints (the held form's begin play, the drag form's tick that lands it, the chop's clothes timeline) are
`spawn_authority` rows. The npc forms' mirrors have their whole event graph refused (`coop/creatures/npc_mirror_brain`: the four rows
`eriePlushLive_C`, `_good_C`, `_evil_C`, `npc_angryErieFlesh_C`, each with no entry allowed), which parks the walk, the steal, the `rep`
timers, a pick up, a feed, fire and the end: the copy shows the host's pose and nothing else of the live Erie `[?]`. The copy's collision is switched off at its birth
(`npc_mirror_brain::SetUpPresentation`, as the live Erie's own begin play does) `[?]`.

### Pick up and feed

A client's pick up of a live Erie (action 7) and a feed (`playerUsedOn`: it destroys the prop in the player's hand) are entries of the
parked graph, so the copy refuses them and they are **not** sent to the host: the host's own `actionOptionIndex` hands the drag form to the
host's player (`pickupObjectDirect`), and a feed reads the player's held prop from the player it is given. Neither is a small, safe
forward. They are open (see Known limits).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the plush's quest (`haspaper`, `haspencil`, `food`, `fullness`, `pains`) | the host | a client's hit asked of the host; the two flags a copy shows sent |
| the notes, the eaten prop, the morph | the host | host props and a host mint; a client's copies refused |
| every form of the live Erie | the host | npc lane or world-actor lane; the copy is a pose with its graph parked |
| `liveerie` | each player's own profile | a cue from the host |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AmbientFixtureState` (family `kEriePlush`) | the host to all | a plush's key hash and its paper and pencil bits |
| `PropIgniteIntent` (verb `kVerbErieHit` 12) | a client to the host | the plush's key and the struck prop's element id |
| `EventCue` (id `kCueLiveErie` 170) | the host to all | the morph happened: grant `liveerie` |
| `EntitySpawn`, `EntityPose` and the world-actor messages | the host to all | the forms, as for any creature or world actor |

## Late join

A joiner is sent every plush that has a flag set at its ready edge, and the existing forms by the connect snapshots of the two lanes. The live
Erie and the drag form implement the save interface (`ignoresave` false: the game's own save writes their class, `rep` and transform), so a
joiner's own load makes a local twin of a saved one. The host's snapshot names a live Erie with no save key (its `key` reads `None`, so the
snapshot does not mark it save-persisted), so the joiner is sent a fresh mirror and the npc lane's ghost sweep ends the twin nothing claimed
(`coop/creatures/npc_adoption`) `[?]`: no run loads a save with a live Erie. The chop is not saved (`ignoresave` true), nor is the angry one (it declares no save functions).

## Known limits

| Limit | Evidence |
|---|---|
| A client cannot pick up or feed the host's live Erie: both are refused on the copy and not sent | `[RD]` `actionOptionIndex` 7 hands the drag form to the player it is given; `playerUsedOn` destroys that player's held prop |
| The live Erie's animation states (dance, steal, eat) are not sent; a copy walks by its pose's velocity only | `[RD]` the states are the Blueprint's `state`, `iscarrying` and montages, which its parked graph never runs |
| What a client's copy of the plush shows of the food (`foodm`, the 4 s of eating) is not shown; only the paper and the pencil are | `[RD]` `food` and `foodm` are not sent |
| The notes `write` throws are thrown at player 0's camera, the host's | `[RD]` `write` reads `getPlayerCameraManager(0)` |
| A client's own fire of the Erie gun, or its burying of a plush (`dirthole_C.processBuriedItem` mints `erieRise_C`, which mints a plush 30 to 60 s on), mints a prop or an actor on that client alone | `[RD]` no row refuses either; the prop door drops a client's keyed prop `[?]` |
| The Erie zone (`eriezone_C`, level-placed) and the rising plush (`erieRise_C`) run on each peer | `[RD]` not on any lane |
| The classes of the forms must be loaded on a client for a mirror to be made; no lane loads one | `[RD]` the plush's graph imports `eriePlushLive_C` and `npc_angryErieFlesh_C`, so a world that holds a plush has them; a mirror whose class is missing is dropped and said |
| The held form (`prop_erieHold_C`, minted by the player's inventory when a live Erie is placed) and the evil held form's hit (a fireball and a drag form) are not covered beyond the begin play row | `[RD]` |
| The achievement step is unrun by design: no drill may grant `liveerie` to the machine's profile | `[?]` `coop/world/erie_sync` |

The drills `erieQuest` and `erieForms` are the first runs to make. `erieQuest` is meant to show a client's plush copy refusing its hit handler and
asking the host, the host running it, the copy following the paper and pencil flags, and the note and pencil `write` throws crossing as props. `erieForms`
is meant to show a live Erie spawned on the host mirrored on the client with its graph parked, an `erieDrag_C` that lands as a live Erie enrolled by
the spawn-source row, an `erieChop_C` mirrored and its two live Erie enrolled, and (with `VOTVCOOP_ERIE_DRILL_MINT=1`) a client's own live Erie spawn
cancelled. They never call `fuck`, `anger`, `lib.progressAchievement` or the cue's handler, and the angry Erie is not spawned (its begin play hunts a
player). Not covered by either: the refusal of the held form's begin play, the drag form's tick and the chop's clothes timeline on a client, a
save-loaded twin, the angry Erie, the achievement cue `[?]`.

## Code map

| Concept | Files |
|---|---|
| the quest, the flags, the cue | `coop/world/erie_sync` |
| a copy's refusals | `coop/world/spawn_authority` (the `eriePlush.*`, `erieHold`, `erieDrag` and `erieChop` rows) |
| the npc forms' parked graphs | `coop/creatures/npc_mirror_brain`, `npc_mirror_brain_table` |
| the spawn sources and the allowlists | `coop/creatures/npc_world_enum`, `ue_wrap/core/sdk_profile_names.h` |
| the drills | `coop/dev/erie_drill` (`erieQuest`, `erieForms`) |
