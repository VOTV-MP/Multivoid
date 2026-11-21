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
overwrite never races an irreversible act. The battery is the host's (below); each copy drains
its own between the host's states.

### Parts

A wrench or power tool unbolts a wheel, which leaves as a prop; a held wheel goes back into a
socket or the spare box, and the spare comes out of its box by the radial option. Each of these
changes presence, so on a client the body that would make the change is cancelled at the script gate
and asked of the host with `AtvPartIntent` (`coop/interactables/atv_part_intent`): `unscrewPanel`
for a looked-at wheel, `putTire` for a fit, `playerUsedOn` for the spare's stow, and
`actionOptionIndex` for the spare's take. A fit or stow destroys the wheel in the client's hand as
the body would and carries its scalars. The host judges the sender's world, the ATV by its key, that
the host itself syncs it, the reach, the tool or the wheel its hand held last (one use of a wheel per hold)
and the ATV's own state. An unbolt then runs the game's own `unscrewPanel` on the host's
copy with the look fields set for the call; a fit or stow writes the fields the body writes and runs
its reducer; the spare is born as a wheel prop beside the sender. The wheel's birth crosses as the
host's and presence follows in `AtvState`. A refused fit or stow gives the wheel back as a prop
beside the sender. The host has no copy of the wheel a client's hand held, so the scalars a fit or
stow carries are the client's claim, bounded at the receive edge (finite; durability 0..100, dirt
0..1, the repair countdown -1..3, the domain the condition read measured): a client can make a held
wheel pristine, the same trust as a module's fit. A refused act is refunded only when nothing of it
ran on the host's copy; one that ran in part is counted ran, said, and not refunded. The battery is not here: its fit and unscrew are the host's by `AtvRequest`
(below), and this lane lets a battery's `unscrewPanel` and `playerUsedOn` run for that one.
`coop/dev/atv_part_drill` (`VOTVCOOP_ATV_PART_DRILL=wheel`, both peers): five legs, each decided by an
event (the host's hand and counter lines, the peers' said tyre masks and wheel props) and never a
clock: an unbolt with an empty hand (the host refuses), two unbolts with a wrench and a fit of a held
wheel (the host runs each; both peers show the same mask and props), and a second fit forged on the
hold the first spent (refused, nothing given back). A bound says INVALID only, a FAIL or INVALID of
either peer is told to the other, and the host's `rig: VERDICT atv-part PASS` rests on its own
counters (ran 3, refused 2, refunded 0). Reds, each failing at its own leg: `atv_part_red=cancel_off`
(client), `atv_part_red=tool_judge_off` (host) and the host's `hand_spend_drill=hand_spend_no_latch`.
`[?]` built on this tree, not yet run; the spare's stow and take, and the reach, have no leg.

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
  once per item. Any refusal after that hand check (a duplicate, the sender out of reach or not alive, the ATV gone) is refunded
  with that module's prop, above the ATV or, when it is gone, above the sender.
  A copy that is adopted holding no state of the host's asks for it (request op 6), and the host answers with its state.
- **The battery.** `battery` is a charge, -1 with none fitted. A fit (`playerUsedOn` with a `prop_atvcarbattery_C` in
  hand) and an unscrew (`unscrewPanel`'s battery branch, which drops the battery as a prop with its charge) run on the
  client's copy and are asked of the host: op 5 with the charge the copy took, op 7 for the unscrew. The host fits only
  into an ATV with none and only from a hand that last held a battery (`no_item` otherwise, and nothing is refunded);
  once that check has passed, any refusal of the fit (`full`, out of reach, not alive, the ATV gone) is refunded with a
  battery prop at the request's charge, above the ATV or above the sender when it is gone; the host holds no number for the
  battery the sender held, so the charge is the sender's claim, kept within 0..100. It takes out only a fitted battery (`not_held` otherwise), and spawns the battery prop itself, at its own copy's
  charge and at the ATV's `Billboard4`, where the game's own unscrew spawns it; a spawn that fails refuses the request
  and changes nothing. The client's own copy of the unscrew spawns a battery too: it is recorded as it is born
  (`coop/props/prop_spawn_authoring`) and destroyed at the end of the unscrew body with no destroy sent, so the host's
  prop is the one every peer holds; a tire's branch is not touched. The host then writes `battery` and runs the game's
  `updBattery`. Every host state carries the charge with a flag that it read, a joiner gets it in its connect state, and
  a client writes it when it differs, with `updBattery`. The rig's ATV drill decides each step on events, counting
  battery props by actor: b1 a fit, b2 an unscrew (one new battery on every peer; a mirror's charge is decided when its
  save record lands on it), b3 a fit back, b0 a refused fit refunded, b4 a refused unscrew spawning nothing, b6 a forged fit refunding nothing, b5 the rejoined copy's charge.
  `[?]` built to the design, not yet run.
- **Lights, brake, turbo and the horn.** The syncer's copy writes them into its blocks' `stateBits` (brake 0x02, lights
  0x20, turbo 0x40, horn 0x80) at each block build. The host takes them from an admitted block (the block's sender must be
  the ATV's syncer) and runs the ATV's own light, brake and turbo bodies on its copy for each bit that changed; its re-send
  carries exactly these four to the other clients, which do the same on their copies. A client never applies them to an ATV
  it syncs. A change of any of the four makes the syncer's next block due at once, once per change: a held key sends nothing
  more than the stream's own pace. `[?]` built to the design, not yet run.
- **The horn is a level, as MTA's full keysync carries it** (`Shared/mods/deathmatch/logic/Utils.cpp:798` read, `:815`
  write: the pad's `ShockButtonL`). The honk key's two events on the seated copy (`InpActEvt_atv_honk_...`, watched at the
  script gate) set the level and a latch; the syncer's next block carries the level or the latch, and the block that carries
  a press clears the latch, so a tap between two blocks is still sent and no timer is involved. The level is not read off
  the copy's `input_alt`: the BP writes it only in the two key events and never clears it when the seat ends. A receiver
  that sees the level change plays the ATV's own horn press or release on its copy (the replay also toggles the copy's
  `input_alt` and the Fly module's flag, which are put back in the same frame). A joiner gets the level from the connect
  snapshot's block, which for an ATV a client syncs carries that client's last admitted level; a change of syncer forgets the
  level. It replaces an earlier edge request and its host-side table of presses.

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

### The car battery charger

The car battery charger charges an ATV battery, the loose one out of an ATV, while its cord is in and both clips sit
on it the right way round. Its clips and cord, and every loose ATV battery's charge, are the host's
(`coop/interactables/car_charger_sync`): the host sends a charger's row when they change and a battery's charge when
it moves (every 2 s, or at once past 5 points), and a client writes them and refuses its charger's tick, so the charge
and the spark are the host's. A client's clip move is seen against the host's last row and asked; the host sets it and
each battery's pins from its clips. A client's own cord event is put back to the host's.

### Who rides

One table says who rides an ATV: the host's. Every write of an ATV's seat, syncer and rider goes through
`coop/interactables/atv_seat_table`, the table's one writer, which announces each change to its subscribers on whichever
route made it: a judgement, a stamp, an answer, a departure, or the entry's own end (the ATV destroyed, the entry
replaced, the session over). The seat and the rider ride every `AtvState` the host sends, the rider as the kerfur's
element id (`riderEid`), so a peer learns who rides from the stamp and from nothing it watches itself.

The driver's own player is attached to the ATV and hidden while it drives, so its stream says nothing a body could sit
by. On every other peer, when the table puts a peer on an ATV, that peer's body is attached to the ATV once, at the
ATV's `playerHit`, the game's own driver seat where its sit puts its player, in the AnimBP's ATV seat pose (`isOnAtv`),
with its collision off so it cannot push the ATV. When the seat ends, the body is detached and the stream places it again
(`coop/player/puppet_seat`).

The kerfur passenger is the host's. The ATV's own sit asks every kerfur to come (`tryToOccupyCar`) and its dismount
sends the rider off, but on the host that runs only for the host's own player. When a client's seat begins, the host
runs the same steps on its copy at the seat's edge, not on a scan: it fills `allKerfuros` and asks each kerfur. When the
seat ends the rider gets off (`getOffCar`) and the rest turn back (`failCar`). The host's own seat is the game's. When
the ATV is destroyed under kerfurs it invited, those that were not seated are turned back by their own `failCar`; the
seated one is the game's, which dismounts it through its crash paths (`coop/interactables/atv_riders`). The kerfur that
sits tells the host's table (`coop/interactables/atv_rider_watch`, a watch on its `sitOnCar` and `getOffCar`), and the
stamp carries it from there.

On a client, the stamped kerfur's mirror rides the ATV the stamp names, by the kerfur's own `sitOnCar`, which attaches
it; the mirror's pose, state and movement stay the host's stream's, and when the stamp stops naming it the mirror is
unseated without a car (`coop/creatures/npc_ride`). A client's own game never decides a mirror's seat: its ATV's sit
would invite every kerfur of its world, mirrors included, and its dismount and crash paths would unseat or kill the
riding one, so `tryToOccupyCar` and `getOffCar` on a mirror are cancelled at the script gate on a client. `[?]` Built to
this design; no three-peer run has judged it yet.

The unseat of a mirror (`UnseatWithoutCar`) is `getOffCar`'s own steps without its car read, teleport and kill tail: detach,
collision on, `state` 0, `occupyCar` cleared, the AnimBP's seat pose off and the Mesh's absolute rotation restored. It sets
no movement mode and calls no `move(false)`: the mirror stays where it is and the host's stream owns where it goes next.

A stamp that reaches a client before its copy of the ATV is indexed is dropped whole, seat and rider with it (the vitals
are held and applied at the index, the table is not). The host's next block for that key carries both again, at most one
idle keepalive later, so a joiner that adopts its save's copy late sees the driver's body and the kerfur seated then.
The same holds when a client's copy is replaced or pruned while the host's lives on: the client's seat state ends with the
copy and returns with the next block. `[?]` Read from the code, not measured.

| Step | The host | A client and an observer |
|---|---|---|
| A client sits | the table takes the seat; the host's kerfurs are invited; the first to arrive sits and the stamp names it | the client's body sits in the seat on every other peer; the stamped kerfur's mirror sits on the ATV |
| The client moves the ATV | the kerfur rides with the ATV | the mirror rides with it, attached |
| The client gets off | the rider gets off, the rest turn back, the stamp names no rider | the body stands again; the mirror is unseated where it stands |
| The host sits | the game's own sit invites its kerfurs | the host's body sits in the seat; the stamped mirror sits on the ATV |

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
| lights, brake, turbo, the horn | the syncer | the syncer's copy's bools and the horn key's level ride its block's `stateBits`; the host applies each change and re-sends |
| the kerfur passenger | the host | its kerfur's own sit, called for a client's seat too on the seat's edge; the stamp names it and a client's mirror rides the ATV the stamp names |
| the driver's body in the seat | the host's seat table | each peer attaches a seated peer's body to the ATV once |
| tyre, spare and battery presence | the host | consumed from the host's own copy only; a client's unbolt, fit, stow, insert and spare take are asked of the host |
| a runtime ATV's identity | the host | its game key, announced |
| a car battery charger's clips and cord, a loose ATV battery's charge | the host | a row when they change, and a charge every 2 s while it moves; a client's clip move is asked |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AtvState` | the syncer (a client's to the host alone), re-sent by the host; the host when idle | key, pose, rotation, velocities, the seat, the syncer and the riding kerfur the host stamps, the host copy's trap, the syncer's brake, lights, turbo and horn, the condition block with its vitals ack, an adopt flag |
| `AtvSeat`, `AtvTakeover` | a client to the host | a sit in or leave of the driver's seat; a grab of the ATV |
| `AtvAnswer` | the host to the asking client | the verdict on one request, with the seat and the syncer the host holds |
| `AtvExplodeRequest` | a client to the host | the ATV the client syncs that exploded, by key, and whether the story event's trap fell in that frame; the host blasts at the ATV's own `tp` |
| `AtvPartIntent` | a client to the host | one piece of work on an ATV's wheels or spare: a wheel unbolted, a held wheel fitted or stowed as the spare, the spare taken; with the held wheel's scalars |
| `AtvSpawn`, `AtvDestroy` | the host to all | an ATV's key, class and pose; its teardown |
| `AtvSetComplete` | the host to a joiner | the count of distinct keys its connect snapshot named |
| `AtvHostState` | the host to all | an ATV's fuel, health, battery, modules, empty and broken with its state sequence; the answer to a request (to its sender alone: the request's id, the verdict, the copy's state) |
| `CarChargerState`, `CarChargerIntent` | the host to all; a client to the host | a car charger's clips (the battery each is on, its polarity) and cord, and a loose battery's charge and pins; a client's clips |
| `AtvRequest` | a client to the host | a refuel's gain, a repair, a module fitted or taken off, a battery fitted (its charge) or unscrewed (each with a request id), a copy's ask for the host's state |

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

The wheel and spare lane (`AtvPartIntent`) holds no state of its own, so a joiner needs two things
the other lanes already carry. Which tyres are on and whether the spare is on the ATV come in the
host's condition block of that ATV, whose presence a joiner takes from the host's own copy, a
connect snapshot's block included (`[V]` `coop/interactables/atv_condition_sync`, the presence
rule). The wheel props an unbolt left, or the spare a client took, lie in the world as host births,
carried to the joiner by the prop lane's connect snapshot like any other prop (`[?]` not run for a
wheel prop). An ask is an act, not a state: a client asks nothing before its world is announced, and
the host drops one that arrives before the sender's world is ready, so an unbolt or a fit made in the
join window is not queued.

## Known limits

| Limit | Evidence |
|---|---|
| While driving, the mirror trails the syncer: the correction is a bounded velocity bias, and the cut to the syncer's pose happens only past a speed-scaled error or when the error stops shrinking | `[V]` `coop/interactables/atv_corrector` |
| A client drives for one round trip before the host's answer can refuse its sit or grab; a refused sit dismounts, a refused grab drops, and the blocks it streamed meanwhile are dropped and counted, never applied | `[?]` `coop/interactables/atv_authority`, built to the design and not yet run |
| At a hand-back to the host the old syncer's last blocks may still be in flight; the host drops and counts them, and the stamp moves every client to the host's copy | `[?]` `coop/interactables/atv_authority`, `coop/interactables/atv_sync` |
| A wheel knocked off by damage on a client syncer's copy is not consumed, so the host keeps the wheel and the peers disagree | `[V]` `coop/interactables/atv_condition_sync`, by design |
| A client's wheel or spare work is refused while a client syncs the ATV (it is driven or carried, or not yet handed back): presence reaches the clients only from the host's own copy while the host syncs. The refused ask shows the client nothing but its wheel given back | `[?]` `coop/interactables/atv_part_intent` |
| An unbolt the host runs for a client is the game's own `unscrewPanel`, which ends with `resetUnscrew`; if the host's own player is mid-hold on that ATV's unscrew bar, the bar ends. A wrench or power tool also has no hold time here, so a client's unbolts of four wheels are limited by the lane's budget (a burst of 6, 2 a second) and not by the game's hold | `[?]` `coop/interactables/atv_part_intent`, `ue_wrap/devices/atv_parts` (the body's reset is read from the lens report, not re-derived) |
| The spare a client takes is born as a prop beside it, not into its hotbar; an unbolt the host runs for a client plays the game's 2D sounds on the host | `[?]` `coop/interactables/atv_part_intent` |
| The pose stream rides the reliable lane; a sequenced unreliable stream is the intended shape | `[V]` `coop/interactables/atv_sync` sends it reliable |
| Not synced: the radio | `[V]` nothing under `coop/interactables/atv_*` carries them |
| A client's refuel or repair is the rise its copy shows, which the host takes as is within the 100 ceiling and the reach; the gas can's own fuel after a pour is not carried. A refused take-off of a module the host does not have leaves its author's module in the world | `[V]` `coop/interactables/atv_host_sync` |
| A seated driver's head does not follow its look, and its flashlight and steps are still; a riding kerfur and the driver's body share the ATV, the driver at the ATV's `playerHit` and the kerfur at its `sitarrow`. A client's mirror rides the ATV the stamp names, not the nearest one | `[?]` `coop/player/puppet_seat`, `coop/creatures/npc_ride` |

## Code map

| Concept | Files |
|---|---|
| the lane | `coop/interactables/atv_sync`, `coop/interactables/atv_sync_internal.h`; `coop/interactables/atv_body` (this machine's own seat and carry against the table) |
| the seat and syncer table, and the host's judgements | `coop/interactables/atv_authority` |
| the corrector, the hit guard and the damage gate | `coop/interactables/atv_corrector`, `coop/interactables/atv_hit_guard`, `coop/interactables/atv_damage_gate` |
| condition | `coop/interactables/atv_condition_sync`, `ue_wrap/devices/atv_condition` |
| a client's part work | `coop/interactables/atv_part_intent`, `ue_wrap/devices/atv_parts`, `coop/dev/atv_part_drill` |
| the engine wrapper | `ue_wrap/devices/atv` |
| the seat | `coop/interactables/device_occupancy` |
| fuel, health, modules | `coop/interactables/atv_host_sync` |
| lights, brake, turbo, the horn (the block's bits taken and applied, the syncer's latch) | `coop/interactables/atv_stream_gates` (`TakeControls`) |
| the riders: the driver's body, the kerfur passenger | `coop/interactables/atv_riders`, `coop/player/puppet_seat`, `coop/interactables/atv_rider_watch`, `coop/interactables/atv_seat_table`, the mirror's ride in `coop/creatures/npc_ride` |
| tests and probes | `coop/dev/atv_probe` (the drive arm), `coop/dev/atv_tire_probe`, `coop/dev/atv_eject_drill`; `coop/dev/atv_gate_drill` (the row `atv_gate_drill=green\|<red>`: each act a player does at an ATV, every outcome decided by an event of `coop/interactables/atv_drill_events`); `coop/dev/atv_riders_drill` (the row `atv_riders_drill=on\|drive_control\|<red>`: three peers and a rejoin, the driver's body and the host's kerfur shown on every peer, a red for each owner and each seam of the riders); the smoke run with the ATV scenario enabled |
