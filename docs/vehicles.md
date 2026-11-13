# Vehicles

## Purpose

The ATV, the game's one vehicle: a five-body physics rig with a seat that takes the player's
pawn, a parts economy of modules, tyres and a spare, and accumulators for fuel, dirt and health.
How the peer driving it and the peers watching it share one rig, why a watching peer runs the
rig natively instead of freezing it, what condition travels, and what is still open.

## How it works

### The rig, and why a mirror is not frozen

The ATV is not an engine vehicle: its root body simulates physics and its four wheels are
separate simulating bodies held by constraints, so the vehicle's visible output is suspension
travel, and one actor transform is not the pose. The first lane froze a mirror and teleported its
root twenty times a second, dragging four constrained bodies behind it: measured suspension
travel of thirty centimetres against a native two to four, and a release that launched the other
peer's copy at a metre and a half per second. That lane was replaced whole
(`coop/interactables/atv_sync`).

A peer that does not sync an ATV runs the rig natively, physics on and tick on, and is
steered toward the syncer on every packet (`coop/interactables/atv_corrector`): the
velocity is written, the position error is closed by a bounded corrective velocity, and past a
speed-scaled threshold, or when the error stops shrinking, the mirror is cut to the syncer's
pose through the game's own teleport. This is MTA's vehicle shape, and the one thing a mirror may
not do is author collision damage: the seven hit delegates are intercepted on a peer that does
not own the rig's tick and dispatched with a zeroed impulse, so the notification runs whole and
only the damage dies (`coop/interactables/atv_hit_guard`). Cancelling the wheel delegates
outright cost a mirror its rig shape, a sag of twenty-five to forty centimetres, until the cause was
found.

### The seat and the syncer are the host's

Who sits and who writes the rig are different questions, and the host decides both
(`coop/interactables/atv_authority`); no machine decides either for itself. The seat is a claim: a
client's local sit sends `AtvSeat`, and the host judges it (the sender's world ready, the ATV known,
the sender alive, a pace, one seat at a time, the seat free, the sender within reach) and answers
every request with `AtvAnswer`. The host's own player is judged by the same table at its own tick,
without the reach test. The syncer is the peer whose copy writes the rig and runs the brain, the
accumulators, wheel torque and collision damage. It is the occupant while seated; unseated, it stays
with the peer that drove or carried the ATV until that peer is out of range (130 m) or silent for
five seconds, or the ATV has been at rest and uncarried for a second, and then the host takes it
back. A grab sends `AtvTakeover`, judged the same way and refused while the ATV is seated or another
peer carries it. A peer merely carrying an ATV syncs it but is not its occupant, so it never denies
the seat. A client acts at once on its local sit or grab and yields to the answer: a refused sit
dismounts through the game's own dismount, a refused grab drops. Every machine follows the host's
stamp, the seat and the syncer on each `AtvState` the host sends. `[?]` Built to this design; no
two-peer run has judged it yet.

### The stream

While synced by a peer, the rig's pose, full rotation, linear and angular velocity stream about
twenty times a second, keyed by the ATV's game key. A client's stream reaches the host alone: the
host applies only its syncer's blocks, paced, and re-sends each to the other clients with its stamp.
An idle ATV is
synced by the host at five hertz gated on change with a two-second keepalive floor, so a parked
vehicle costs one packet every two seconds. An ATV that appears at runtime, from the spawn menu,
has no save twin, so the host announces it under its game key.
The client parks a valid announcement until gameplay, the discovery index and load quiescence
agree, stamped with the world it arrived in: a world change drops it, the new world's own replay
bringing it again, and a spawn that fails in a ready world is final, never retried.
`[?]` This readiness path still needs a two-peer join and travel test.

### Condition

The syncer's condition rides the same payload: per-wheel durability, dirt, fixes and type, the
spare's trio, body dirt, fuel and health (`coop/interactables/atv_condition_sync`). A mirror is
overwritten and re-derives its visuals through the game's own reducers on change edges, against a
baseline seeded from the actor rather than from the last packet: a zero baseline would break all
eight constraints on a settled rig, and a per-packet one would starve the dirt reducer forever.
Accumulators apply from the syncer, clamped to the game's domains when it is a client; presence,
which tyres are on and whether the spare is, is consumed only from the host's own copy, because a
client syncer's eject ships a mask whose paired wheel prop cannot travel, and applying it would
turn a divergence into an item loss the host persists. The mirror's own accrual is held at zero by the impulse neuter, so an
overwrite never races an irreversible act. The battery is an inserted prop's charge and rides
the prop lane.

### What the host owns beyond the rig

The rig's pose stays with the peer driving it. Everything else the ATV keeps is the host's copy's, and a client submits
its own acts and takes the host's state (`coop/interactables/atv_host_sync`):

- **Fuel and health.** A client's refuel (a gas can's `fuelUp`) or repair (a toolbox's `toolboxFix`) runs on its copy,
  where the can drains and the toolbox is spent, and is asked of the host as a request with an id. The host's judge
  (`coop/interactables/atv_host_judge`) serves each sender's requests from a queue of eight at four a second; a request
  whose sender's world is not ready on the host, or whose hand state the host still owes, waits at the head until the
  slot's world-ready edge or its applied hand state releases it. It judges that the sender is alive and within 8 m of the
  ATV or seated in it, that a refuel's sender holds a gas can and the tank has room, and that a repair's sender last held
  a toolbox (ten uses of one hold) and the health is under 100; a tank or health already full is refused as `full`. It
  then writes its
  own copy as the verbs' own bodies do (fuel up and `empty` cleared; health to 100, `brokenn` cleared, the game's
  `updHealth`), publishes, and answers the sender alone with the verdict, the copy's state and its sequence. A client
  driver's blocks only lower them: the host takes the lower of its value and the block's, from a block whose `ackSeq` is
  the ATV's current state sequence (the newest host state whose vitals the driver applied; any other block's vitals are
  ignored), then runs the ATV's own `checkIfRunout`. The host's own values and sequence ride every block it sends or
  re-sends, and a client takes them from those blocks.
- **Modules.** A fit (`playerUsedOn` with an upgrade in hand) and a take-off (`upgradeTake`) run on the client's copy and
  are asked of the host the same way, by the module's value. The host fits one its copy does not hold and takes off one
  it does, writes its `Modules` and runs `updUpgrades`. A fit's module must be of the class the sender's hand last held,
  once per item. A fit refused as a duplicate, after the hand check passed, is refunded with that module's prop at the ATV.
  A copy that is adopted holding no state of the host's asks for it (request op 6), and the host answers with its state.
- **Lights, brake and turbo.** The syncer's copy writes them into its blocks' `stateBits` (brake 0x02, lights 0x20, turbo
  0x40) at each block build. The host takes them from an admitted block and runs the ATV's own light, brake and turbo
  bodies on its copy for each bit that changed; its re-send carries exactly these three to the other clients, which do the
  same on their copies. A client never applies them to an ATV it syncs. `[?]` built to the design, not yet run.
- **The horn.** The ATV's syncer's press and release go to the host, which judges that the sender syncs the ATV (at eight
  a second, a release never refused), plays the ATV's own horn on its copy (the replay also toggles the copy's `input_alt`
  and the Fly module's flag, which are put back in the same frame) and sends it to every other peer. A horn a client
  holds down is released by the host when its seat ends, its stream goes silent or the table clears.

The client's side is `coop/interactables/atv_gate` and the open-request gate of `atv_host_sync`. The acts are watched at
ATV_C's own functions through the script gate: on a client `fuelUp` (from the can's `gascanFuel`), `toolboxFix` (once per
hold, the call that took the health to 100), `playerUsedOn` (a module added) and `upgradeTake` are asked of the host with
what the copy's verb did; a `fuelUp` that does not come from `gascanFuel`, the fuelbox's overlap with a can lying in the
world, is refused on a client, the host's copy being its author. On the host the same functions' ends call the publish
point. While any request of this client's on an ATV is unanswered (4 at most; a 5th act is not sent and said once) the
gate on that ATV is shut: a state or a block does not overwrite its vitals and modules and no sequence is recorded, so the
syncer's blocks ack an older state and the host ignores their vitals. An answer removes its id, and the state it carries is
applied when it closes the last; the ATV's destroy, the session's end and a world change clear the set. A take-off the
host refused leaves the client the module its own call handed over, which is destroyed at the answer. A state for an ATV
this client holds no copy of yet is kept, the newest of 8 ATVs, and applied at the copy's adoption.

The host publishes an ATV's state at one point: when a write to its copy changed the fuel, health, modules, `empty` or
`brokenn`, the ATV's state sequence is bumped and the state goes to every client, with no timer; a joiner is sent every
ATV's state at its ready edge. The map is a display of the ATV's own place, and its module is a module. The padlock and
the crowbar have no body on the ATV. `[?]` built to the design, not yet run.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the pose while driven or carried | the syncer | a twenty-hertz stream; a client's reaches the host, which re-sends it stamped |
| the pose while idle | the host | five hertz on change, a keepalive floor |
| the seat | the host | a judged request, answered, stamped to every client |
| the syncer | the host | assigned on a sit or a grab, taken back at rest, out of range or on silence |
| collision damage, the accumulators, wheel torque | the tick owner, the syncer's machine | a mirror's impulses are zeroed |
| every route to an ATV's damage and death (the two blast verbs, the explosion) | the tick owner | a copy that does not own the tick cancels them at the script gate; the owner takes every blast that reaches it |
| the story event's trap | the host | the host copy's `trap` rides the stamp (bit 4); the driven copy fires it |
| the condition values | the syncer | overwritten on the mirror, re-derived on edges |
| fuel and health | the host | a client's refuel or repair is a request the host judges and applies on its copy; a client driver only lowers them |
| the modules | the host | a client's fit or take-off is a request the host judges and makes on its copy |
| lights, brake, turbo | the syncer | the syncer's copy's bools ride its block's `stateBits`; the host applies each change and re-sends |
| the horn | the host, from the syncer | the press and release are requests the host judges; the horn plays on every other peer |
| tyre and spare presence | the host | consumed from the host's own copy only |
| a runtime ATV's identity | the host | its game key, announced |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AtvState` | the syncer (a client's to the host alone), re-sent by the host; the host when idle | key, pose, rotation, velocities, the seat and the syncer the host stamps, the host copy's trap, the syncer's brake, lights and turbo, the condition block with its vitals ack, an adopt flag |
| `AtvSeat`, `AtvTakeover` | a client to the host | a sit in or leave of the driver's seat; a grab of the ATV |
| `AtvAnswer` | the host to the asking client | the verdict on one request, with the seat and the syncer the host holds |
| `AtvExplodeRequest` | a client to the host | the ATV the client syncs that exploded, by key, and whether the story event's trap fell in that frame; the host blasts at the ATV's own `tp` |
| `AtvSpawn`, `AtvDestroy` | the host to all | an ATV's key, class and pose; its teardown |
| `AtvSetComplete` | the host to a joiner | the count of distinct keys its connect snapshot named |
| `AtvHostState` | the host to all | an ATV's fuel, health, modules, empty and broken with its state sequence; the answer to a request (to its sender alone: the request's id, the verdict, the copy's state); a driver's horn |
| `AtvRequest` | a client to the host | a refuel's gain, a repair, a module fitted or taken off (each with a request id), the syncer's horn |

## Late join

The joiner loads the host's save first, so it already holds the ATVs the save placed. Once its
save has finished loading, the host walks every ATV its world holds and announces each by key, then
closes the set with `AtvSetComplete`. The joiner adopts the copy it holds under that key and spawns
a mirror only when it holds none, so it ends with exactly one actor per host ATV. It then removes
every ATV the set did not name, such as one the host lost after the save was taken. Arrivals that
come before the joiner's world is ready wait in a park. A set the host cannot vouch for (an ATV
still loading, a send that failed) is withheld, which removes nothing; the host owes it and
retries. Each ATV's pose, velocity, condition and the host's stamp (the seat and the syncer) follow,
so an ATV airborne at the join arrives moving and lands, and the joiner's mirror is cut to the pose
on its first packet, even for an ATV a client syncs; after that it follows the syncer's blocks. A
joiner sends no request before its own world is ready: a sit or a grab it makes before then is held,
not yielded, and goes out at that point. A rejoin into the same slot is a departure and an arrival,
and the leaver's seat and syncerships were cleared at its departure. `[V]` the ATV spawn drill's
run, join, park and divergence arms, each red arm failing as designed (`coop/dev/atv_spawn_drill`).

## Known limits

| Limit | Evidence |
|---|---|
| While driving, the mirror trails the syncer: the correction is a bounded velocity bias, and the cut to the syncer's pose happens only past a speed-scaled error or when the error stops shrinking | `[V]` `coop/interactables/atv_corrector` |
| A client drives for one round trip before the host's answer can refuse its sit or grab; a refused sit dismounts, a refused grab drops, and the blocks it streamed meanwhile are dropped and counted, never applied | `[?]` `coop/interactables/atv_authority`, built to the design and not yet run |
| At a hand-back to the host the old syncer's last blocks may still be in flight; the host drops and counts them, and the stamp moves every client to the host's copy | `[?]` `coop/interactables/atv_authority`, `coop/interactables/atv_sync` |
| A client's tyre eject is refused by the arbiter, so the host keeps the wheel and the peers disagree until the eject intent lane exists | `[V]` `coop/interactables/atv_condition_sync`, by design |
| The pose stream rides the reliable lane; a sequenced unreliable stream is the intended shape | `[V]` `coop/interactables/atv_sync` sends it reliable |
| Not synced: the radio, the kerfur passenger, and a remote driver's body is not seated on the ATV | `[V]` nothing under `coop/interactables/atv_*` carries them |
| A client's refuel or repair is the rise its copy shows, which the host takes as is within the 100 ceiling and the reach; the gas can's own fuel after a pour is not carried. A refused take-off of a module the host does not have leaves its author's module in the world | `[V]` `coop/interactables/atv_host_sync` |

## Code map

| Concept | Files |
|---|---|
| the lane | `coop/interactables/atv_sync`, `coop/interactables/atv_sync_internal.h`; `coop/interactables/atv_body` (this machine's own seat and carry against the table) |
| the seat and syncer table, and the host's judgements | `coop/interactables/atv_authority` |
| the corrector, the hit guard and the damage gate | `coop/interactables/atv_corrector`, `coop/interactables/atv_hit_guard`, `coop/interactables/atv_damage_gate` |
| condition | `coop/interactables/atv_condition_sync`, `ue_wrap/devices/atv_condition` |
| the engine wrapper | `ue_wrap/devices/atv` |
| the seat | `coop/interactables/device_occupancy` |
| fuel, health, modules, the horn | `coop/interactables/atv_host_sync` |
| lights, brake, turbo (the block's bits taken and applied) | `coop/interactables/atv_stream_gates` (`TakeControls`) |
| tests and probes | `coop/dev/atv_probe` (the drive arm), `coop/dev/atv_tire_probe`, `coop/dev/atv_eject_drill`; `coop/dev/atv_gate_drill` (the row `atv_gate_drill=green\|<red>`: each act a player does at an ATV, every outcome decided by an event of `coop/interactables/atv_drill_events`); the smoke run with the ATV scenario enabled |
