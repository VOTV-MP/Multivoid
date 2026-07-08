# concrete / cement / bucket — depletable + curing world items   (STATUS: RE DONE + DESIGN, NOT BUILT)

RE DONE 2026-07-08 (static bytecode, cited). Design ratified via the `/qf` thread (R1-R6 + a
code-verified architecture probe). **NOT BUILT — this session's output is documentation.** The user
grouped "concrete cement bucket things" as one N/N ask; measurement splits them by ownership, and the
world-placed member (concreteBucket) turned out to be the first confirmed instance of a broader
undocumented class — see `docs/COOP_WORLD_PROP_DIVERGENCE.md`.

## 1. Native behavior (ground truth, cited)

### cementBag (`prop_cementBag_C` : `prop_C`) — a HELD tool
- `units` int, CDO **50/50** (`maxUnits` 50), `name="cement"`. Count shown only as text (no per-unit mesh).
- **HELD** (`playerHandUse_LMB`/`RMB`). **LMB on a water bucket** (`canConcrete`): `amount = Round(bucket.height/maxHeight*20)`; `lib.replaceProp(bucket → 'concreteBucket')` + set the new bucket's `units` + destroy the old bucket; `units -= amount`; at `units<=0` → `K2_DestroyActor(bag)`. **LMB/RMB on another cementBag** → `transferUnit` (±1). `crafted()` → `units := 0`.
- Save: `units` round-trips. `bitten`/`getData`/`loadData` inherited.
- **=> client-owned HELD tool; its world-visible RESULT is a water bucket MORPHING to a concreteBucket** (`replaceProp` = `spawnPropThroughGamemode` + `K2_DestroyActor`).

### concreteBucket (`prop_concreteBucket_C` : `prop_C`) — a WORLD-placed depletable + curing prop
- `units` int, CDO **20**; `stage = (units-1)/5` → **4 fill meshes** (`updStage` `SetStaticMesh`); `dry` bool → **wet/dry material** (`updDry` `SetMaterial`); `dryTimer` float.
- **DRYING:** `ReceiveTick` `dryTimer += DeltaSeconds`; at **>600 s → `dry=true`** (a **LOCAL per-actor accumulator**, not a synced-clock read).
- **SCOOP:** `takeConcrete(player, FHitResult aim-hit)` — overrides `lookAt` (= prop_C's **aimed world-interact**, NOT held). If `dry` → blocked; else `units-1` + `updStage`; **at `units<=0` → `replaceProp(self, 'bucket')`** (morph back to an empty bucket). **Called BY `prop_wallfixer`** (a held tool: aim at a wall → scoop from a nearby bucket → fix the wall).
- Save: `units` + `dry` round-trip (`getData` ints[0]+bools; `loadData` restores + `updStage`/`updDry`).
- **Two distinct terminals + two mid-life scalars [MEASURED]:**
  - **units→0** = `replaceProp` = **destroy+respawn morph** → a real lane event (rides the destroy+spawn lanes, like piles).
  - **dry-out (600 s)** = `updDry` **in-place `SetMaterial` on the SAME actor** → **not a morph**.
  - **mid-life units 1..19** = `updStage` **in-place `SetStaticMesh` on the SAME actor** → **not a morph**.
- **=> host-owned WORLD prop; `units`+`dry` have world-visible in-place expression; drying is a host-authoritative LOCAL-accumulator timer.**

### wet-concrete PRODUCED actors (the pour/cure outputs)
- `customWall_wetConcrete_C` : `customWall_C` — `ReceiveTick dryTime += DeltaSeconds`; at `>timeToDry` (size-scaled) → morph to `customWall_C` (`wallType "concrete_0"`). Rides the **wallbuilder lane**. Brain-on? **UNMEASURED** (possible 2nd divergence instance).
- `actorChipPile_wetConcrete_C` : `actorChipPile_C` — `ReceiveTick dryTimer>120 s` → spawn `dryConcreteStone` + `K2_DestroyActor`. Rides the **pile lane**, whose mirrors **park the brain** (`native_pile_mirror.cpp:69`) → the HOST drives the cure and the morph propagates via the pile lane → **already correct**.
- `prop_garbageClump_wetConcrete_C` : `prop_garbageClump_C` (0 own fns) — spawned by the concreteBucket pour path (`units-=5`). garbageClump/pile lane.
- Spawned **by name via datatables** → they inherently ride the existing wall/pile lanes.
- `prop_sponge_bucketPour` = OUT OF SCOPE (a water/wash sponge). `prop_bucket` = the water/alc/soap liquid bucket (cementBag's precursor target).

## 2. Sync split (by ownership — the "generalized N/N" answer, part 2)

There is NO single lane. Each maps to a known axis by the **held-vs-world** measurement (same method as food):

| item | ownership | pattern |
|---|---|---|
| **cementBag** | HELD (client-owned) | the **rock-drop held→world authoring** — its `replaceProp` result morph (water bucket→concreteBucket) is an instance, not a new mechanic |
| **concreteBucket** | WORLD (host-owned) | **host-authoritative** — see §3; the confirmed instance of the world-prop divergence class |
| **actorChipPile_wetConcrete** | WORLD, pile lane | already correct (parked brain + host-driven cure/morph rides the pile lane) |
| **customWall_wetConcrete** | WORLD, wallbuilder lane | possible 2nd divergence instance — brain-on UNMEASURED |

## 3. Coop design (concreteBucket — DESIGN, gated, NOT built)

concreteBucket is the first confirmed instance of the class in `docs/COOP_WORLD_PROP_DIVERGENCE.md`
(a save-loaded client world prop runs its own BP brain and self-simulates state the wire never
carries). The fix **extends the proven pile-lane pattern** (park the client brain + the host authors
progression via existing lanes), plus one genuinely-new bit:

1. **Park the concreteBucket mirror's brain** (`SetActorTickEnabled false`, like piles) so it doesn't self-run `dryTimer` and diverge. (Scoped to this prop, NOT all keyed props — blast radius.)
2. **Host-authoritative SCOOP** — the client's scoop is a per-verb **use-intent** to the host (via the **wallfixer use-intent**: intercept the wallfixer's use, host runs `takeConcrete`+`fix`; do NOT intercept `takeConcrete` directly = phantom-scoop when no bucket is nearby).
3. **A 2-scalar curated ON-CHANGE push** (`units`→stage mesh, `dry`→material): the in-place mid-life expression + the dry-out have **no morph**, so they need a small host→client push that drives `updStage`/`updDry` on the mirror. (Curated on-change, per MTA `setElementData`/`CElementRPCPacket` — NOT a generic continuous scalar channel.)
4. **The units→0 terminal** (`replaceProp` destroy+respawn) rides the existing destroy+spawn lanes (host authors it, like a pile morph).
5. **Late-join** is free-ish: `units`+`dry` persist in the host save, which the joiner loads (§ `save_transfer`), then `loadData` runs `updStage`/`updDry`.

**Do NOT** build a generic per-prop scalar channel or park every keyed-prop brain — rule-of-three is
unmet (concreteBucket confirmed N=1; customWall a possible 2nd) and park-every-brain has blast radius.

## 4. Verification / gates (what's measured, what's not)

- **MEASURED (code):** all of §1; join save-loads the host world (real brained natives); tick left on for non-pile keyed props; no per-prop BP-scalar channel (`protocol.h`); `replaceProp` = `spawnPropThroughGamemode`+`K2_DestroyActor`; MTA curates on-change; `actorChipPile` cure is tick-driven but brain-parked (host-driven).
- **INFERRED (NOT runtime-observed):** that two peers' concreteBucket `units`/`dry` actually **drift apart** live; that `customWall_wetConcrete`'s brain is on.
- **BUILD GATES:** G1 runtime-confirm the divergence on two peers; G2 confirm the host morph/progression propagates on the lane for a parked-brain prop; G3 the wallfixer use-intent design; G4 (cementBag) confirm `spawnPropThroughGamemode` reaches `FinishSpawningActor`/`host_spawn_watcher`; + a user MVP call (live per-scoop mirroring vs save+late-join only — a co-watch edge, play-pattern-dependent).
- **NOT BUILT.** cementBag lands with the deferred rock-drop authoring; concreteBucket build is gated on G1-G3 + the MVP call.
