# Light and hot plate switches

A lamp's, a wall light's, a spotlight's, the beacon's and the hot plate's switch are the host's on every peer, with the
hot plate's heat level, and a client's press of one is asked of the host. The identity, birth and drop of the props
themselves are on [props.md](props.md).

## What the game does

The desk lamp (`prop_deskLamp_C`), the floor lamp (`prop_floorlamp_C`), the wall lights (`prop_lightIntWall1_C`, and
`prop_lightIntWall2_C`, which inherits it) and the hot plate (`prop_cookingPad_C`) flip their own `active` from their
radial action or their right click and run `upd`, which shows the light, the glow or the heat for `active` and the
cord's power together; the cord's plug and unplug run `upd` too, and a wall light also flips on a wire's pass. The hot
plate's switch is its action 8, and its action 5 steps its heat level `power` from 0 to 4 and runs `upd`; its tick cooks
the food on it while it is powered and on. The spotlight's head (`prop_spotlightHead_C`) flips `active` in
`toggleLight` and runs `upd`; the tripod spotlight (`prop_spotlight_C`) carries its head as a child actor, its
`lighhead`, which names the tripod back as its `tripod`. The beacon (`prop_beacon_C`) sets `isActive` through `open`,
which also lights its radar point. No lane carried any of these, so each peer's press was its own: a client's lamp lit
on its machine alone, and a client's hot plate heated nothing the host cooks.

## How it works

Every one of those bodies ends in `upd` or `open`, which the script-body gate watches by name, class-declared, at their
end (`coop/props/prop_switch_sync`). The host keeps the last state it sent per prop and sends each change
(`PropSwitchState`): the prop's element id, or a tripod head's tripod's with the head flag, whether it is on, and the
hot plate's level. A client keeps the last state it knows per copy, the copy's own at its bind or the host's last; a
change at the end of a body it did not run for the host's state is its player's press, which stays on its copy and is
asked of the host (`PropSwitchIntent`). The host judges the sender's reach (its body within 6 m of the prop) and up to
8 asks per half second per sender, then switches its own copy through the prop's own body (the fields written and `upd`
run, or `open(condition)`), whose end sends the state to every client. A refused ask, or one for the state the host
already has, is answered with the host's state to the asker alone, which puts its copy back. A client takes a state
through the same body, and holds the newest state of a prop whose copy is not bound yet until its bind.

## Who owns what

| State | Owner | How it crosses |
|---|---|---|
| a lamp's, wall light's, spotlight's, beacon's or hot plate's switch, and the plate's heat level | the host | each change sent; a client's press runs on its copy and is a request the host judges |
| the cord's power under it | the cord lane | each peer's `upd` reads its own cord's plug, which the cord lane carries |
| the food cooked on a hot plate | the host | its own food lane; the host's plate is on when any peer switched it on |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PropSwitchState` | the host to all, or a joiner or a refused asker alone | the prop's id (a tripod's, for its head), whether it is on, the plate's heat level |
| `PropSwitchIntent` | a client to the host | the same, as the client's copy reads after its press |

## Late join

At a joiner's ready edge the host sends every switched prop's state to that slot. One whose copy is not bound yet is
held, the newest per prop, up to 64, until its bind.

## Measured

`[V]` (a fork-only drill, three peers, on the fork's build of the same change): the host laid out a desk lamp, a
floor lamp, a beacon, a spotlight head, a powered hot plate and a tripod spotlight; slot 1's switch of each reached the
host's copy and its own stayed on; the host's switch of the five lights reached slot 1, and its step of the plate's
level reached it with the plate still on; a slot that joined after both found the plate on at level 1. Before it, a
client's switch left the host's lamps off.

## Known limits

The press's own sound plays on the presser's machine alone. The salt lamp's action is a lick, not a light; the UV light,
the neon sign and the motion light have no switch of their own and follow their cord.

## Code map

| Concept | Files |
|---|---|
| the switches | `coop/props/prop_switch_sync` |
