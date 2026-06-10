# Clump/pile cross-peer DUPE — DECISIVE root cause + correct fix (2026-06-08)

> **CORRECTION 2026-06-10 (bytecode re-verified, agent ac942efeea2b0376f):** §1(A)
> below is WRONG on both counts. `AundergroundGarbageSpawner_C` does NOT spawn
> chipPiles — it spawns `dirthole_item_C` buried-item mounds (keyed Aactor_save_C).
> The grabbable ground piles come from **`garbagePileSpawner_C`**, which is the
> OPPOSITE of what (A) claims: it spawns the real `actorChipPile_C` actors (not
> display meshes), is ONE-SHOT at level load, uses a **SEEDED RandomStream** with
> map-baked `spawnTrashAt[]`/`spawnTypes[]`/`seed` — so fresh-load pile PLACEMENT
> is map-deterministic, identical cross-machine. Cross-peer pile divergence is
> HOST STATE DRIFT (save round-trip via the keyless "Load Primitives" path,
> collection, converts) — not placement RNG. §1(B) and the v52 eid conclusions
> stand. See `votv-snapshot-adoption-root-causes-2026-06-10.md`.

Evidence base: live BP disassembly (`tools/bp_reflect.py`), CXX dumps, the shipped
code, AND a fresh smoke's runtime logs (`Game_0.9.0n[_copy]/.../votv-coop.log`
17:55:41-50). Builds on `votv-clump-ball-to-pile-conversion-RE-and-event-fix-2026-06-08.md`.

## 1. THE foundational fact: chipPiles are NOT co-located across peers (PROVEN)

Two independent, proven reasons:

**(A) Trash placement is per-game RANDOM (unseeded).** The grabbable ground piles
come from `AundergroundGarbageSpawner_C` — its spawn ubergraph uses the GLOBAL
unseeded RNG (`RandomPointInBoundingBox` + `RandomFloatInRange`, NOT the
`...FromStream` seeded variants) + `LineTraceSingleForObjects` +
`weightedRandomV2` for the loot type. So positions AND types differ every game.
(`research/bp_reflection/undergroundGarbageSpawner.json`; corroborated by
`votv-garbage-trash-interaction-RE-2026-05-27.md:54`.) — The separate
`garbagePileSpawner` IS seeded but only builds display MESH COMPONENTS, not
grabbable chipPile actors; not the source.

**(B) The client never receives the host's pile transforms.** In a real menu-mode
session the CLIENT fresh-boots a BLANK save (`engine::StartFreshGame(storyMode=true)`
via `harness.cpp` `BootStorySaveBlocking(forceFresh=true)`; blank = level defaults
only, `engine.cpp:377-378`). The HOST loads its own progressed save. The host's
piles are NOT broadcast as a connect-snapshot (chipPiles are not in the prop
snapshot path). `coop/garbage_sync.cpp::InstallSpawnerSuppressors()` IS shipped
(suppresses the client's own spawner) but does NOT mirror the host's existing piles
— so the client simply has fewer/none, still non-co-located.

**Runtime proof:** even with both peers on the SAME save file, the host's
position-consume matched a pile of the WRONG variant 278.7 cm away
(`CONSUME source chipPile ... at 278.7 cm (variant=3)` while the grabbed clump was
variant 8). With the client fresh-boot there is no counterpart at all.

=> `ue_wrap::prop::FindNearestChipPile` can NEVER reliably identify "the grabbed
pile" cross-peer. The whole position-consume model is unsound.

## 2. The exact multi-ball mechanism ("one pile -> multiple balls on the peer")

Dominant = **(b) the receiver's position-consume destroys the WRONG pile (or none),
so the grabbed pile's counterpart is never removed and the landed-pile/mirror adds on
top = net multiplication.** Two layered faults:

- **Fault 1:** the consume anchor is the HAND, not the pile. The 2026-06-08
  `RecordClumpSpawnOrigin` "origin anchor" fix (now REVERTED) tried to fix this but
  is gated on `freshSpawn` and the smoke proved that gate was FALSE for real grabs —
  broadcasts went out at the hand (Z≈6272) not the pile (Z=6232). So it never fired.
  (And per §1 it was moot anyway — anchoring perfectly still can't match a
  non-co-located pile.)
- **Fault 2:** even with a perfect anchor, position-identity is unsound (§1). The
  receiver's `FindNearestChipPile(loc,300cm)` (`remote_prop_spawn.cpp:444-456`)
  destroys the nearest of many non-co-located piles (24.9/278.7/90.2/124.3 cm, one
  variant-mismatched) and the `kFreshLanded` landed-pile spawns anyway — count grows
  per cycle.

Secondary = **(c)** landed-pile eid recycling collisions (smoke showed two different
landed piles broadcast with the same eid 34079; held-clump eid 35051 reused) — real
but a rapid-grab-loop artifact; (b) dominates. (a)/(d) are NOT the cause (every
watched clump's mirror WAS destroyed; no connect-snapshot pile replay).

## 3. The correct fix: eid-morph-in-place (Option A) — RETIRE the position-consume

Since piles are NOT co-located, the position-consume must GO (RULE 1+2). The grabbed
clump already has a cross-peer identity (its eid). Mirror the clump by eid ONLY and
morph the SAME entity ball->pile in place at the owner's convert edge — never
search/consume local piles.

**Detection (owner):** PRE observer on the clump's bound hit handler
`BndEvt__prop_garbageClump_StaticMesh_..._ComponentHitSignature__DelegateSignature`
(a real ProcessEvent-dispatched UFunction — bindable). Re-evaluate the byte-exact BP
gate (`delayOnHit==false && canConvert && holdPlayer-not-grabbing &&
Dot(Hit.Normal,Up)>0.75 && !IsSimulatingPhysics(Hit.Component)`; `prop_garbageClump`
ubergraph @2702->@3536). When it passes, broadcast `PropConvert{oldEid=clump eid,
newEid, pileClass, restingXform (≈ the clump's current GetActorLocation /
Hit.Location), chipType, landingVel}`. PRE not POST (POST = clump already torn down
by `K2_DestroyActor(self)`).

**Receiver (`remote_prop::OnConvert`, atomic):** (1) destroy the mirror ball by
`oldEid` (reuse the `OnDestroy` eid teardown `remote_prop.cpp:691-773`); (2) spawn the
pile via the existing `OnSpawn` `kFreshLanded`+`newEid` path. Ball vanishes the same
frame the pile appears.

**DELETE (RULE 2):** the `garbageClump` `FindNearestChipPile`-consume block
(`remote_prop_spawn.cpp:432-457`); `BroadcastLandedPileNear` + its 200cm search
(`trash_collect_sync.cpp:70-117`); the death-watch's pile-spawn (keep ONLY its
`PropDestroy` backstop for a clump that dies WITHOUT converting). **KEEP:**
`SetActorRootNotifyRigidBodyCollision(false)` + the PhysicsOnly release lock.
**Protocol:** add `ReliableKind::PropConvert` + `PropConvertPayload` (~56B) +
`event_feed` route.

## 4. OPEN sub-question to resolve FIRST next session (the re-grab case)

When peer A grabs a pile that B is mirroring (a previously-landed pile with an eid),
A's `playerGrabbed` destroys that pile (`actorChipPile_C::playerGrabbed` ub @3905 ->
`@2563 K2_DestroyActor`). For B to drop its mirror, that destroy must broadcast
`PropDestroy(eid)`. BUT the clump's morph-destroy is known to BYPASS the
`K2_DestroyActor` ProcessEvent observer (BP-internal dispatch — the reason the
death-watch exists). **Verify whether the PILE's grab-destroy is likewise
unobservable.** If it is, Option A also needs a way to propagate a grabbed mirror
pile's destroy by identity (e.g. a held-edge check: when the newly-grabbed clump's
source pile had an eid, emit `PropDestroy(thatEid)`); the position-consume was
(wrongly, by co-location assumption) standing in for this. This is the one piece the
position-consume was actually doing that the naive "just delete it" loses — must be
replaced by an identity-based destroy, not dropped.

## Files
- Unsound consume: `remote_prop_spawn.cpp:432-457`. Death-watch/landed-pile:
  `trash_collect_sync.cpp:70-117,199-225`. eid teardown to reuse:
  `remote_prop.cpp:691-773`; release lock `:495-525`. Client fresh-boot:
  `engine.cpp:314-389`, `harness.cpp:443`. Spawner suppressor (host piles still not
  snapshotted): `coop/garbage_sync.cpp`. RNG placement:
  `research/bp_reflection/undergroundGarbageSpawner.json`. Convert gate (byte-exact):
  `votv-clump-ball-to-pile-conversion-RE-and-event-fix-2026-06-08.md` §2/§4.
- The 2026-06-08 origin-anchor attempt was REVERTED (trash_collect_sync + prop_lifecycle).
</content>
</invoke>
