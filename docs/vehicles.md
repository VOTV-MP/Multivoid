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

A peer that does not author an ATV runs the rig natively, physics on and tick on, and is
corrected toward the authority on every packet (`coop/interactables/atv_corrector`): the
velocity is written, the position error is closed by a bounded corrective velocity, and past a
speed-scaled threshold, or when the error stops shrinking, the mirror is cut to the authority's
pose through the game's own teleport. This is MTA's vehicle shape, and the one thing a mirror may
not do is author collision damage: the seven hit delegates are intercepted on a peer that does
not own the rig's tick and dispatched with a zeroed impulse, so the notification runs whole and
only the damage dies (`coop/interactables/atv_hit_guard`). Cancelling the wheel delegates
outright cost a mirror its rig shape, a sag of thirty to forty centimetres measured on every run
until the cause was found.

### Two predicates

Who streams and whose machine runs the brain are different questions. Pose authority, the peer
driving or carrying the ATV, decides who streams; tick ownership, that peer or the host when
nobody authors, decides whose accumulators, wheel torque and collision damage are real. On the
wire the author is one slot and the seat another, because the seat is what the occupancy deny
reads, and a peer merely grabbing an ATV must not deny a seat nobody is in; a simultaneous mount
resolves to the lower slot. A peer may name only itself as author, and only the recorded author
may release; the release clears the author and nothing else, since nothing was frozen.

### The stream

While authored, the rig's pose, full rotation, linear and angular velocity stream about twenty
times a second, keyed by the ATV's save key; the host relays a client's stream. An idle ATV is
synced by the host at five hertz gated on change with a two-second keepalive floor, so a parked
vehicle costs one packet every two seconds. An ATV that appears at runtime, from the spawn menu,
has no save twin, so the host mints a synthetic key and announces it.

### Condition

The author's condition rides the same payload: per-wheel durability, dirt, fixes and type, the
spare's trio, body dirt, fuel and health (`coop/interactables/atv_condition_sync`). A mirror is
overwritten and re-derives its visuals through the game's own reducers on change edges, against a
baseline seeded from the actor rather than from the last packet: a zero baseline would break all
eight constraints on a settled rig, and a per-packet one would starve the dirt reducer forever.
Accumulators apply from any legitimate author; presence, which tyres are on and whether the
spare is, is consumed only from host-authored packets, because a client-authored eject ships a
mask whose paired wheel prop cannot travel, and applying it would turn a divergence into an item
loss the host persists. The mirror's own accrual is held at zero by the impulse neuter, so an
overwrite never races an irreversible act. The battery is an inserted prop's charge and rides
the prop lane.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the pose while driven or carried | the author | a twenty-hertz stream, relayed |
| the pose while idle | the host | five hertz on change, a keepalive floor |
| the seat | the occupancy arbiter | the lower slot wins a simultaneous mount |
| collision damage, the accumulators, wheel torque | the tick owner | a mirror's impulses are zeroed |
| the condition values | the author | overwritten on the mirror, re-derived on edges |
| tyre and spare presence | the host | consumed from host-authored packets only |
| a runtime ATV's identity | the host | a synthetic key, announced |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AtvState` | the author, relayed; the host when idle | key, pose, rotation, velocities, the seat, the author, the condition block, an adopt flag |
| `AtvRelease` | the author | authority lost, nothing else |
| `AtvSpawn`, `AtvDestroy` | the host to all | a runtime ATV's synthetic key and birth; its teardown |

## Late join

Every indexed ATV is snapshotted to the joiner with its pose, velocity, author and condition, so
an ATV airborne at the join arrives moving and lands, and the joiner's mirror is cut to the pose
on its first packet.

## Known limits

| Limit | Evidence |
|---|---|
| While driving, the mirror trails the author by two to three metres, and a wedged author leaves a static error the corrector does not close | `[V]` the driven runs; the measurement window still mixes the two |
| At an authority handoff both peers own the rig for about a second | `[V]` both verification runs |
| A client's tyre eject is refused by the arbiter, so the host keeps the wheel and the peers disagree until the eject intent lane exists | `[V]` by design, registered as a crutch |
| The pose stream rides the reliable lane, a stopgap; the sequenced unreliable datagram is owed | `[V]` the lane's own note |
| Not synced: the thirteen modules and every upgrade effect, lights, brake and turbo, honk, the radio, the map, repair, the kerfur passenger, and a remote driver's body is not seated on the ATV | `[V]` the gap list |
| A mirror's wheels may not be where its body is | `[RD]` a defect candidate, not yet measured |
| Nothing above has been verified by hand; every number comes from the autonomous rig | `[V]` the runs' archives |

## Code map

| Concept | Files |
|---|---|
| the lane | `coop/interactables/atv_sync`, `coop/interactables/atv_sync_internal.h` |
| the corrector and the hit guard | `coop/interactables/atv_corrector`, `coop/interactables/atv_hit_guard` |
| condition | `coop/interactables/atv_condition_sync`, `ue_wrap/devices/atv_condition` |
| the engine wrapper | `ue_wrap/devices/atv` |
| the seat | `coop/interactables/device_occupancy` |
| tests and probes | `coop/dev/atv_probe` (the drive arm), `coop/dev/atv_tire_probe`, `coop/dev/atv_eject_drill`; `python tools/mp.py smoke` with the ATV arm |
