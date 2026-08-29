# The CRUTCH REGISTER — what we built the wrong way, and what the right way is

*[↑ docs index](README.md)*

**Created 2026-08-29 on USER DIRECTIVE:** *"THE CURRENT ATV IMPLEMENTATION IS MADE OF CRUTCHES -
WHICH IS ANTI-RULE 1. Needs proper design and fix. Also make a doc of current crutches - put atv
there and also put piles there - because interaction with piles is as crutchy as atv today -
creating a pile object attachmed etc - full of crutches - that's on the list after atv."*

This is the standing register of **subsystems we shipped in a crutch shape**. It exists because
RULE 1 is easy to honour on a new feature and easy to lose across a year of increments: each
individual commit looked like a targeted fix, and the accumulation is a crutch. A register makes
the accumulation visible, which no single diff can.

**Evidence tags.** **[V]** measured from the current tree / bytecode (cited) · **[RD]** derived from
measured facts, not observed at runtime · **[A]** asserted by an earlier doc, not re-verified ·
**[?]** unmeasured.

---

## 0. What counts as a crutch here

From `CLAUDE.md` RULE 1: *"When you find yourself adding a workaround (filter, skip-if, suppress-X,
catch-and-ignore), STOP. That is a crutch. Identify the root cause and fix it at the point where the
actual problem is."*

This register uses a slightly wider test, because the two entries below are not `skip-if` lines —
they are **architectures** that were shaped by an obstacle instead of by the problem:

1. **It neutralises or replaces the engine entity instead of driving it.** Principle 3 says our class
   owns network state and the **engine** class keeps rendering / animation / physics, joined by a
   pointer. A design that freezes the engine entity into a corpse, or swaps it for a fake, has
   abandoned that split.
2. **It spawned a second mechanism to compensate for the first.** A crutch that needs a crutch is the
   diagnostic signature — the compensating mechanism is load-bearing evidence, not a detail.
3. **Its stated reason has since been measured false, and the code stayed.** RULE 2 says the old
   thing goes when its reason retires.
4. **Two implementations of one concept compile together.** Also RULE 2.

A thing is **not** a crutch merely because it is incomplete, deferred, or unverified. Missing work is
missing work; this register is for work that was *built in the wrong shape*.

---

## 1. Register

| # | Subsystem | Shape of the crutch | Status | Order |
|---|---|---|---|---|
| C1 | **ATV** (`coop/interactables/atv_sync`, `ue_wrap/devices/atv`) | The mirror is a frozen corpse; the rig's whole purpose is deleted to make it hold still | **DESIGN CONVERGING** (2026-08-29) | **FIRST** |
| C2 | **Trash piles / clumps** (`coop/props/trash_proxy`, `native_pile_mirror`, + ~9 sibling modules) | The mirror is a FAKE actor, which broke aim, which grew a parallel aim system; two mirror implementations now coexist | **OPEN** | **SECOND** (user: *"that's on the list after atv"*) |

---

## C1 — the ATV

**RE of record:** `docs/vehicles/ATV.md`. **Design in progress:**
`research/findings/vehicles/votv-ATV-full-sync-DESIGN-2026-08-29.md`.

### What ships today
A mirrored ATV is `PrepareMirror`'d — `SetSimulatePhysics(false)` **on the root only**,
`SetActorTickEnabled(false)`, `SetActorRootNotifyRigidBodyCollision(false)` — and then
`DriveMirrorTransform`'d with `SetActorLocation` + `SetActorRotation`, again root-only, from a
**reliable** ~20 Hz `AtvState` stream interpolated by a `LerpWindow`. Pose is the *only* thing
synced.

### Why it is a crutch, not just an incomplete feature

**[V] It deletes the reason the entity exists.** `AATV_C` is a constraint rig: four wheels as
independent rigid bodies on `sus_FL1/FR1/BL1/BR1` suspension and `ax_*` axle constraints. Freezing
the body deletes suspension travel, weight transfer and body pitch — the entire output of that rig.
And it does not even freeze cleanly: **the wheels are never parked**, so four live rigid bodies hang
off constraints attached to a parent that teleports ~20×/s. Steering is frozen too, because
`setFrontWheelsOrientation` is only called from tick `[V]`, and there is no drive torque because
`applyWheelTorque` is also tick-only `[V]`. The result is a rigid block sliding along a lerped path
with locked wheels — `[RD]`, never observed, which is itself the point.

**[V] The root-only park is a whole class of defect, not a detail.** `PrepareMirror` parks the root;
the ATV binds **seven `ComponentHitSignature` delegates** (`mesh`, `car1_Capsule`, `backWheel_R`,
`backWheel_L`, `frontWheel_R`, `frontWheelRoot`, `backWheelRoot`) plus `OnActorHit` → `hitActor`.
Delegates are dispatched by the collision system, not by tick, and neither traced handler gates on
`isDriven` or on being local — so **six of the seven already fire on every mirror in shipped b145**
and mirrors author their own damage. The same root-blindness lets a player grab a frozen mirror by
aiming at a **wheel**, because the grab gate reads `HitComponent.IsSimulatingPhysics()` `[V]` and the
wheels still simulate.

**[V] The transport was knowingly the wrong one.** The 2026-06-08 Phase-1 blueprint offered an
unreliable newest-wins datagram as option 1, marked **"RECOMMENDED"**, and a reliable stream as
option 2, marked **"acceptable only as a stopgap"**. The stopgap shipped. MTA sends both player and
vehicle puresync as `PACKET_RELIABILITY_UNRELIABLE_SEQUENCED` `[V]` (`CNetAPI.cpp:338,350`), and this
codebase already has that lane (`PropPose=8`, `EntityPose=32`, `OwnerEntityPose=95`).

**[V] Idle ATVs diverge in shipped code.** On receivers an unauthored ATV is physics-ON *and*
tick-ON, so `fuel` / `battery` / `dirt` accumulate independently on every peer — the exact
`COOP_WORLD_PROP_DIVERGENCE` shape, in the one lane that never got its progression owner.

**[V] Three wire bits are produced and never read.** `stateBits` bit0 `isDriven`, bit1 `brake`,
bit2 `grabbed` are written by `ReadPayload` and consumed nowhere; only bit3 `authored` is read.

**[V] A shipped identity lane rests on a premise that is false.** `atv_sync.cpp:98-101` states *"a
bought ATV is delivered ONLY on the host"*; no row in the 473-row `list_store`, and no craft recipe,
sells an ATV. The code's real predicate is *"a mid-session ATV not in the baseline set"* — broader
than its comment, and gated for RULE-2 deletion pending a runtime census.

**Never hands-on tested**, with a third-party field report open against it.

### The proper fix (direction settled 2026-08-29)
A **vehicle-sync subsystem in MTA's shape**, not more patches to a 692-LOC file: always-simulating
corrected mirrors (velocity + turn speed + error spread + a sync-time-context staleness gate,
`CNetAPI::ReadVehiclePuresync`), single-syncer election covering the idle case
(`CUnoccupiedVehicleSync`), an unreliable-sequenced pose datagram split cleanly from a reliable
mid-join seed and reliable discrete edges, host-canonical arrays for config
(`CVehicleUpgrades` ships `count + slot states` on join), and syncer-authored vitals with the arbiter
owning discrete writes. The decisive argument is not fidelity: **it deletes the freeze/unfreeze state
machine**, and nearly every defect found in this lane lives in that machine's transitions.

---

## C2 — trash piles and clumps

**Status: OPEN, next after C1.** No design pass has been run; the evidence below is a survey, not a
converged analysis.

### The crutch chain, in the order it was built
Each step is quoted from the modules' own headers `[V]`.

1. **The real mirror "died".** A client's mirror of a trash entity was a real `actorChipPile_C` /
   `prop_garbageClump_C`. Per `trash_proxy.h`, that BP *"runs its own ubergraph -> it self-morphs /
   self-destructs / is GC-eligible (unrooted) on its OWN schedule … Within ~10 s it goes NOT-LIVE"*
   → a visible dup.
2. **Crutch: replace the engine entity with a fake.** `trash_proxy` swaps it for a bare
   `AStaticMeshActor` *"we own"* — *"NO blueprint -> never self-morphs / self-destructs"*. This is
   principle 3 inverted: rather than keeping the engine class and parking its brain, the engine class
   is removed from the picture.
3. **The fake broke the game's aim, so a parallel aim system was built.** Verbatim: *"the client-grab
   AIM is a CAMERA-RAY CONE (`EidForAimedPileProxy`), NOT the game's interaction trace -- a bare
   AStaticMeshActor proxy can never be lookAtActor (the `int_player_C` filter)"*. **A crutch that
   needs a crutch.**
4. **It also lost collision, hover GUI, occlusion and rotation**, with the fix deferred: *"The
   FAITHFUL future collision … is a garbageCollider-analog SHAPE component on the proxy."*
5. **The stated reason was later measured FALSE.** `native_pile_mirror.h` `[V]`: an inertness probe
   on 2026-06-30 *"PROVED that death was GC (unrooted)"* — a rooted runtime native stayed live and
   inert for 60 s **with collision ON** and showed the native hover GUI. **The root cause was
   `AddToRoot` all along**; the entity never needed replacing.
6. **The fix was applied to half the concept.** `native_pile_mirror` restores a real rooted
   `actorChipPile_C` **for the pile form only**. *"The CLUMP form stays a bare proxy (it has a
   LifeSpan + autonomous re-pile-on-contact -> too live to keep as a native)"* — which is the same
   brain-parking problem this project solves everywhere else, declined once.

### What that leaves in the tree today
- **Two mirror implementations for one concept** compiled together (proxy for clumps, rooted native
  for piles) — RULE 2.
- **The parallel camera-ray-cone aim survives**, because the proxy it exists for survives.
- **~11 modules** in the pile/trash lane (`trash_proxy`, `native_pile_mirror`, `pile_spawn_bind`,
  `trash_pile_sync`, `trash_clump_pose_stream`, `trash_collect_sync`, `trash_grab_intent`,
  `trash_use_intercept`, `trash_channel`, `prop_stick_sync`, `garbage_sync`) — the module count is
  itself the accretion signature.

### The probable proper fix `[?]`
The same shape as C1: keep the **engine entity**, park its **brain**, drive it — i.e. finish step 6
by extending the rooted-native recipe to the clump form (parking its LifeSpan and re-pile-on-contact
the way every other lane parks a brain), then **retire the proxy and its parallel aim whole** (RULE
2). Not designed; C2 gets its own `/qf` pass after C1 ships.

---

## 2. The pattern both entries share

Both crutches are the same move: **the engine entity was inconvenient, so it was neutralised or
replaced, instead of being kept and driven.** The ATV freezes it into a corpse; the clump swaps it
for a fake. In both cases the compensating machinery (interp over a dead rig; a camera-ray aim
system) is larger than the fix would have been, and in both cases the original obstacle turned out to
be one property — actor tick for the ATV, `AddToRoot` for the pile.

**Look here FIRST:** when a mirrored engine entity misbehaves, the question is *which brain do I park*
— tick, delegates, LifeSpan, timers, GC-rooting — **not** *how do I stop being the engine entity*.
Principle 3 is the test: if our design no longer holds a pointer to the game's own actor doing the
game's own rendering and physics, the design has left the architecture.

---

## 3. Register discipline

- An entry is **added** when a crutch is measured, with file:line evidence — never on a hunch.
- An entry is **retired** only when the crutch is gone from the tree, not when a fix is designed.
  Retired entries move to a CLOSED section with the commit that removed them.
- A crutch that is deliberately kept needs a written retirement plan naming what it masks, the
  targeted fix, and the gating criteria (principle 4's transitional-crutch exception).
- New crutches found during unrelated work belong here immediately; the register is worth exactly as
  much as its completeness.
