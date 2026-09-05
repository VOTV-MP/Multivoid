# Props

## Purpose

Everything a player can pick up, carry, throw, drop, stick to a wall, put in a pocket or take out
of a container: how it is named across peers, where the mod catches its birth and death, who
owns it at rest and in a hand, and what happens to a prop whose own Blueprint keeps changing
it. Trash piles and clumps are a family of their own on [piles.md](piles.md); the item in a
player's hand is on [players.md](players.md).

## How it works

### Identity

A prop is `Aprop_C` or a subclass, and every keyed prop carries the game's own save key: a
string the prop's construction script mints as a fresh GUID when none is set, and the save
persists. The mod reads it and never overwrites it, with one exception: on a receiver, the
host's key is written into a mirror before its spawn finishes, so both peers hold the same
name. A save-loaded prop has the same key on both peers because both loaded the same bytes.
An element id rides alongside the key on every message (`coop/props/prop_element_tracker`,
`coop/props/prop_synth_key` for the families whose Blueprints mint no key).

### Where a prop is caught

Nothing about a prop is reflected as replication, so the mod watches four seams:

| A prop that | Is caught by |
|---|---|
| came from the save | the shared object scan at world start (`coop/element/object_scan_hub`) |
| was born by a spawner (mushrooms, pinecones, forage) | the prop's initialisation on the host; a client's shared-world spawners are parked (`coop/world/spawn_authority`) and the host mirrors what its own produce (`coop/props/host_spawn_watcher`) |
| was spawned by the spawn menu or extracted from a container on the host | the engine's finish-spawning call, because those births run their initialisation inside the Blueprint where no hook sees it |
| was destroyed | the engine's destroy call on either role: eaten, broken, picked up into a pocket (`coop/props/prop_lifecycle`); a prop that vanishes inside a Blueprint (the truck, culling, a lifespan) is caught by the host's death-watch and destroyed by id on every peer (`coop/props/registry_reaper`) |

A birth message carries the prop's class, key, id, transform, physics flags and, for classes
that keep one, the save scalar the prop was born with (a tape reel's progress), read by one
reader on every birth path so a mirror never starts from a class default.

### Who authors a prop

At rest, the host. A client never authors the existence of a shared-world prop: its own fresh
spawn of a keyed prop is not broadcast. Instead a client's actions become intents the host
performs (`coop/props/prop_drop_intent`):

- **Pick up.** The client's pickup destroys the world actor; that destroy crosses the seam, the
  host destroys its copy, and the key is parked, which records that the host holds no copy now.
- **Place.** The client's fresh spawn is caught at the finish-spawning seam and sent as a drop
  intent naming the parked key, the class and the transform; the host spawns it as the sole
  author and the mirror comes back down the ordinary birth broadcast.
- **Births the client cannot avoid.** A reel ejected from a caddy, a module or a drive taken from
  a rack, an item extracted from a container all materialise on the client first; they go
  through the same door, class-checked at the host, born asleep where the class needs it.

Any other keyed prop a client creates is dropped at that door and never reaches the host.

### Holding, throwing, dropping

While a prop is held, its holder owns it. The holder streams the held prop's world transform
every frame, unreliable and newest-wins (`coop/player/local_streams`). A receiver resolves the
prop by key on the first frame, turns its physics off, and follows the stream with a fixed-delay
snapshot interpolation that renders one interval behind the newest pose on its own clock
(`coop/props/remote_prop`, `coop/props/active_drive`). On release, a reliable message carries
the linear and angular velocity the body had at the instant of release: the game's throw is not
an impulse but the tracking velocity the physics engine accumulated while the player flicked the
camera, and a receiver that re-enables physics and hands that velocity back reproduces it. The
receiver also calls the prop's own thrown event, so the sound and the trail come from the
engine. A stream that stops for half a second, or a holder that switches to another prop, is an
implicit release. Grab and throw sounds that the game plays only for the local player are
synthesised on the receiver at the prop's position (`coop/props/prop_sound`); the pocket blip
is relayed the same way (`coop/items/inventory_pickup_sync`).

A mirror's physics and collision are set to what the game's own initialisation would have
produced on this peer (`coop/props/prop_wire_parity`); a fresh mirror starts kinematic while it
is remote-owned. A spawn or destroy the receiver applied is marked so the symmetric observer
does not broadcast it back (`coop/props/prop_echo_suppress`).

### Sticking to a wall

Wall-attachable props (the cameras) commit their stick inside the Blueprint; the one visible seam
is the commit entry, reached through a latent delay that the engine resumes through the
function dispatch. The sticking peer broadcasts the frozen state and the commit pose before its
own hold breaks, on the same ordered lane as the release so the order is structural; the
receiver re-poses the prop and dispatches the component's own force-stick, so the attach, the
effect and the destroy binding are the game's (`coop/props/prop_stick_sync`).

### Containers

A container's contents are not on the container: every container reads them from one global
per-peer array in the save object, addressed by an index the container holds, and every mutating
verb runs inside the Blueprint where no hook sees it. So the host authors the contents
(`coop/props/container_contents_sync`): one slice of its array per live container is sent at a
joiner's ready edge and on change, and an extraction on a client is a container-extract birth
through the intent door above. The open and closed state is a keyed-device channel like a
door's. The full break-and-spill behaviour, and the single-slot verbs, are reverse-engineered
and designed and not built.

### Props that change on their own

A joiner loads the host's save, so every keyed world prop is a real save-loaded actor with its
full Blueprint brain ticking on every peer, and no wire carries arbitrary per-prop scalars. A
prop whose brain advances a local accumulator (concrete drying, a wall curing, food rotting, a
plant growing) therefore simulates on each peer independently and drifts; a prop whose change
is derived from the shared clock does not. The rule is that the host owns autonomous
progression, in one of three shapes chosen per prop: park the brain and let the host author the
progression through the existing lanes (the pile shape); a host corrector that re-snaps a
deterministic, clamped accrual when the game keeps re-enabling the tick (the tape reel, at one
hertz); or an anchor, a start stamp every peer computes from, valid only while the rate is
constant ([ARCHITECTURE.md](ARCHITECTURE.md), where the authority is going). The reel corrector
is built; concrete and food are designed and not built.

### Deployables

Tools that leave a persistent actor behind (the grappling hook and rope, the nail gun, the wall
builder), timed explosives, the fishing rod and the physgun are reverse-engineered with a design
each and are not synced. Their shared shape, when built: the owner keeps its previews, montages
and ammo local; a commit becomes an intent to the host, either the spawn of a persistent actor
the host replays through the native entry point or an action on an existing host-owned prop;
the host mirrors the actor down; a late joiner gets it from the save; a player-attached phase
(climbing a hook, a cast) is an extension of the player's own stream.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a prop at rest: existence, transform, physics | the host | the birth broadcast; a client's births are intents |
| a held prop | the holder | a per-frame stream; the release carries the velocity |
| a thrown prop after release | each peer's physics, from the same velocity | no stream in flight |
| a prop's key | the game, once; the host's copy wins on a mirror | never rewritten on the owner |
| container contents | the host | slices of its object array; an extraction is an intent |
| a stuck prop | the sticking peer commits; every peer runs the native stick | one reliable message |
| a self-changing prop's progression | the host | park, corrector or anchor, per prop |
| a client's shared-world spawners | parked | shared-world content arrives only from the host |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PropPose` (stream) | the holder to all | the held prop's world transform, per frame |
| `PropSpawn`, `PropDestroy` | the host to all; a destroy from either role | class, key, id, transform, physics flags, the birth scalar; the key and id |
| `PropRelease` | the holder to all | the inherited linear and angular velocity |
| `PropDropIntent`, `ReelEjectIntent` | a client to the host | a place, or an unavoidable birth, for the host to author |
| `PropStickState` | the sticking peer to all | frozen or static, and the commit pose |
| `PropSnapPos` | the host to one joiner | a position correction for a save-authoritative prop moved in the join window |
| `ContainerState`, `ContainerContents` | the presser; the host | open or closed; one slice of the host's object array |
| `InventoryPickup` | each peer, relayed | the pocket blip, so others hear a pickup |

## Late join

The join page owns the mechanism: explicit deletes for the props the joiner's save had and the
host's world no longer has, then the snapshot bracket with one spawn per live keyed prop
(adopted by key, created when missing, transform converged), then position corrections for what
the host moved during the window, then the membership sweep that removes the locals the host
never claimed. Container slices are sent per live container at the ready edge. A stuck prop
reaches a joiner through the save, which carries the frozen state. A prop held by someone at
the moment of the join is resolved on its first streamed frame.

## Known limits

| Limit | Evidence |
|---|---|
| Two peers grabbing the same prop both stream it; nothing assigns the prop to one holder, so receivers follow whichever stream is newest | `[?]` no claim exists in the held-prop lane |
| A keyed prop a client creates outside the intent door (a place after a pickup, the whitelisted births, a container extract) never reaches the host, and nothing logs it | `[V]` the door's gate |
| Concrete, food and every other local-accumulator prop still drift between peers; only the tape reel has its corrector | `[V]` the class is measured; the fixes are designed |
| Volume and mass are re-derived per peer on a container extraction, so they can differ | `[V]` log |
| The deployables (hook, rope, nail gun, wall builder, explosives, fishing rod, physgun) are not synced; a nail or a wall placed by one peer reaches the others only through the save at their next join | `[RD]` the actors are not props and no lane catches them |
| A pose can arrive milliseconds before the spawn that names its prop; that is a race, not a defect, and a ledger tells the two apart instead of warning per packet | `[V]` field log |

## Code map

| Concept | Files |
|---|---|
| the birth and death seams | `coop/props/prop_lifecycle`, `coop/props/host_spawn_watcher`, `coop/props/prop_element_tracker`, `coop/props/prop_synth_key`, `coop/props/registry_reaper`, `coop/props/prop_echo_suppress` |
| the receivers | `coop/props/remote_prop` (held), `coop/props/remote_prop_spawn` (birth: adopt, converge, create), `coop/props/prop_fresh_spawn` (the materialiser), `coop/props/prop_wire_parity`, `coop/props/active_drive`, `coop/props/prop_sound` |
| a client's intents | `coop/props/prop_drop_intent` |
| the stick | `coop/props/prop_stick_sync` |
| containers | `coop/props/container_contents_sync`, `coop/items/save_record_wire`, `coop/interactables/interactable_sync` |
| the pocket blip | `coop/items/inventory_pickup_sync` |
| the join | `coop/props/prop_snapshot`, `coop/props/snapshot_census`, `coop/props/join_membership_sweep`, `coop/props/unresolved_pose_ledger` |
| the spawners a client must not run | `coop/world/spawn_authority` |
| tests | `harness/autotest/autotest_grab.cpp`; `python tools/mp.py smoke` |
