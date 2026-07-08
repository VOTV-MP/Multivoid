# COOP_WORLD_PROP_DIVERGENCE — client world props self-simulate their BP brain and drift

**STATUS: newly identified 2026-07-08 (concrete/cement `/qf`). Root code-verified; the live drift
is INFERRED (not yet runtime-observed on two peers). One confirmed instance (concreteBucket); the
generic fix is DEFERRED until rule-of-three is met.**

This is a cross-cutting architectural fact that was undocumented before 2026-07-08. Read it before
designing sync for ANY world prop whose Blueprint mutates its own state over time (drying, curing,
rotting/spoiling, growing, heating/cooling, count-decrementing).

## The root (one, code-verified)

A joining client **loads the host's save** (`coop/session/save_transfer.cpp` —
`CaptureLiveWorldToScratchSlot` → the joiner's native `loadObjects()` builds the world), so **every
keyed world prop is reconstructed as a real save-loaded `Aprop_C` with its FULL Blueprint brain**. A
network snapshot (`coop/props/prop_snapshot.cpp`) then reconciles by key (binds to the save-loaded
natives; host-only props fresh-spawn as mirrors).

For a **non-pile keyed world prop, the actor TICK is LEFT ENABLED** — nothing parks its brain (only
piles `native_pile_mirror.cpp:69`, puppets, vehicles, and event `WorldActor` mirrors disable tick).
Physics is suppressed on fresh mirrors (kinematic), but the **BP `ReceiveTick`/ubergraph is not**.

And **no per-prop BP-scalar state channel exists**: `PropSpawnPayload` (`protocol.h`) carries
identity + transform + physFlags; pose = transform; destroy = key. There is **no wire field and no
ReliableKind for arbitrary BP scalars** (`units`, `dry`, `temperature`, `ripeness`, growth). The
only scalar-state sync is bespoke **whole-subsystem** lanes (weather, the world clock/TimeSync,
kerfur on/off, window-clean, alarm, piramid choreography).

**Consequence:** a prop whose BP mutates via a **LOCAL per-actor accumulator** (e.g.
`concreteBucket dryTimer += DeltaSeconds`) self-simulates on **both** peers independently and
**diverges** over time. The wire never carries the mutating scalar, and the client's own brain runs
free.

## One root, two symptoms (the mirror-brain knob)

Whether a mirror's brain runs is a single design knob, and it yields two opposite failures:
- **Brain ON (the default for non-pile keyed props)** → the prop self-simulates → **diverges** from the host (each peer dries/rots/grows on its own clock).
- **Brain PARKED (piles)** → the prop **stalls** — it never progresses client-side unless the host drives it.

Either way the correct answer is the same: **the HOST must own the autonomous progression.** The
knob only decides which symptom you get if you don't.

## The proven precedent to extend

The **pile lane already does the right thing**: it parks the client brain (`native_pile_mirror.cpp:69`)
AND the host authors the progression/morph via the existing spawn/destroy lanes (e.g.
`actorChipPile_wetConcrete` cures host-side and the morph rides the pile lane). The fix for a brain-ON
prop is to **extend that pattern**: park its brain + host authors its progression via existing lanes,
plus (for props with a mid-life scalar that has no morph — like concreteBucket's `units`/`dry`) a
small **curated ON-CHANGE push** that drives the setter (`updStage`/`updDry`) on the mirror.

MTA precedent (`reference/mtasa-blue`, `CElementRPCPacket`/`setElementData`): sync a **curated**
per-element set **on change**, NOT an auto-stream of every scalar and NOT park-every-brain. This
validates the per-verb host-authoritative intent + the curated on-change push, and validates NOT
building a generic continuous channel.

## Scope discipline (why this is a class, not a one-off, but still deferred)

Candidate instances: drying concreteBucket (CONFIRMED local-accumulator), curing
customWall_wetConcrete (local-accumulator; brain-on UNMEASURED), rotting/spoiling food, growing
plants, heating/cooling items. **BUT divergence only occurs for a LOCAL per-actor accumulator** — a
mutation derived from a **synced global clock** (`getTimeSeconds`, day/night, weather) does NOT
diverge (the clock is already synced). So before counting an instance, **measure whether its timer is
a local accumulator or a synced-clock read.**

- Confirmed instances so far: **N=1** (concreteBucket). customWall a possible 2nd (unmeasured).
- **Do NOT** build a generic per-prop scalar channel or park every keyed-prop brain yet — rule-of-three
  (OPUS §11) is unmet, and park-every-brain has real blast radius (freezes harmless local cosmetics —
  idle anims, flicker — and breaks tick-dependent behavior in save-loaded natives).
- Fix the **confirmed instance** (concreteBucket) by extending the pile pattern; **document the class**
  (this file); generalize into a primitive only once 2-3 measured same-mechanism instances exist.

## First application

`docs/items/concrete.md` §3 — concreteBucket: park its brain + host-authoritative scoop (wallfixer
use-intent) + a 2-scalar curated on-change push (`units`→stage mesh, `dry`→material) + the units→0
`replaceProp` terminal rides the destroy+spawn lanes. NOT built; gated on a runtime divergence confirm
(G1) + the lane-propagation confirm (G2) + the wallfixer intent design (G3).

## Open measurement (before the generic primitive)

- Runtime-confirm the divergence: read `units`/`dry` on host vs client after ~10 min on two peers.
- Whether a periodic save-transfer re-sync papers over steady-state drift (only active co-present
  mutation would then matter).
- Per candidate prop: local-accumulator vs synced-clock-derived (decides if it's even in the class).
