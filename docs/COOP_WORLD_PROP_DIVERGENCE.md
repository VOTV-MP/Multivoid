# COOP_WORLD_PROP_DIVERGENCE — client world props self-simulate their BP brain and drift

**STATUS: newly identified 2026-07-08 (concrete/cement `/qf`). Root code-verified; the live drift
is INFERRED (not yet runtime-observed on two peers). One confirmed instance (concreteBucket); the
generic fix is DEFERRED until rule-of-three is met.**

This is a cross-cutting architectural fact that was undocumented before 2026-07-08. Read it before
designing sync for ANY world prop whose Blueprint mutates its own state over time (drying, curing,
rotting/spoiling, growing, heating/cooling, count-decrementing).

## The root (one, code-verified)

A joining client **loads the host's save** (`coop/save/save_transfer.cpp` —
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

- Confirmed instances so far: **N=2** — concreteBucket (still un-built) and **wallunit_tapes
  (BUILT v114 `ba8ce297`, 2026-07-17)**. customWall a possible 3rd (unmeasured).
- **The v114 as-built shipped a SECOND transport for the class — the host CORRECTOR, not a park.**
  The wallunit's `upd()` re-applies `SetActorTickEnabled(active)` at every native verb AND every
  wire apply, so a client-tick park is UN-HOLDABLE without a site-list or a verb hook; because the
  accrual is RNG-free/deterministic/FClamp'd, host-owns-progression is delivered instead by a 1 Hz
  host exact-snap corrector (`ReelPose=40`, sawtooth <= 1 native increment;
  `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md` D2). **Pick per instance:** park (the L4 dish
  shape) when the sim is RNG-driven/un-boundable OR the tick stays off once killed; corrector when
  a native refresher keeps re-enabling the tick and the sim is deterministic+clamped.
- **Do NOT** build a generic per-prop scalar channel or park every keyed-prop brain yet — rule-of-three
  (OPUS §11) is unmet, and park-every-brain has real blast radius (freezes harmless local cosmetics —
  idle anims, flicker — and breaks tick-dependent behavior in save-loaded natives).
- Fix the remaining **confirmed instance** (concreteBucket) by extending the pile pattern OR the v114
  corrector shape (measure which per the pick rule above); generalize into a primitive only once a
  3rd measured same-mechanism instance lands.

## Not in the class: STATIC world-state (e.g. world rules) rides the save cleanly

The class is specifically about a per-peer copy that **mutates on its own** and drifts. **Static**
world-state — set once and never mutated — does NOT diverge even though it's also a per-peer copy:
loading the host save populates it and nothing changes it thereafter. The confirmed example is
**`Fstruct_gameRules`** (fall damage / difficulty / funny / custom content / seasons / the minigame
toggles): the runtime authority is the per-peer `mainGameInstance.gameRules`, but a joining client
boots from the host's live-captured save so the host's rules populate the client's copy, and there's
no mid-session rule editor to mutate it. So world-rules sync needed **no build** — it's host-authoritative
for free via the same save-load spine. **CONFIRMED** on a 2-peer smoke (client == host, 36 rules;
`research/findings/world-systems/votv-gamerules-settings-RE-2026-07-09.md` §4). The knob is: *static per-peer copy
seeded from the host save = fine; **mutating** autonomous per-peer copy (a local accumulator) = the
divergence class.*

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

## 2026-07-20 — the ANCHOR alternative, and the two measurements it now gates

`docs/COOP_SERVER_MODEL.md` §4-§5 proposes a cheaper answer than the curated on-change push for the
accumulator half: **an accumulator is never streamed, it is ANCHORED.** Store the start stamp once
(when the bucket was poured) and let every peer compute `dry` locally. Precedent: `[V]` MTA's
`CClock.cpp` is 58 LOC of pure formula from a stamp, with no tick at all.

It buys, for free: no stream, no possible divergence (one formula, one anchor), **late join solved**
(principle 8 — the joiner gets one stamp and is instantly correct, no snapshot cadence), and the
empty-server **freeze** (store elapsed at pause, re-anchor on resume). Parking the brain still
survives from the planned fix — otherwise the local accumulator fights the computed value.

**It is valid only if the accumulator's RATE is constant.** Hence the two measurements this document
now owns, in priority order:

1. **Is `dryTimer += DeltaSeconds` unconditional or gated?** Readable from bytecode; the cheapest
   measurement in the thread. If drying is slower in rain / faster indoors / temperature-coupled,
   anchoring fails and concreteBucket returns to a syncer whole. Generalises to a rule: **for every
   accumulator, measure the input set of its RATE, not just the value.** See
   `[[lesson-converges-for-free-needs-complete-input-readset]]`.
2. **Census of self-simulating props and the SHAPE of each mutation** (accumulator vs stateful
   machine). Exactly **one** accumulator is confirmed (concreteBucket); rule-of-three is not met, so
   "accumulators are anchorable" currently rests on a single instance. A stateful, non-linear
   progression would be the first member of a class anchoring cannot serve.

Measurement 1 decides whether the anchor scheme generalises beyond one example, and is cheaper than
everything else outstanding.
