# piramid2_C (walking pyramid event actor) + piramidSpawner_C — static bytecode RE

> **2026-07-04 LATE (same day): the mirror lane SHIPPED (`7b78b29a`, wire v97) — every
> "current mod coverage" / "gap" claim below (sections 0, 5, 7) describes the PRE-LANE state
> and is now historical.** As-built truth: docs/events/piramid.md (piramid2_C allowlisted,
> 'piramid' no-replay, piramidSpawner_C EX-caught, brain suppression + PyramidGather,
> WA dead-retire). One RE correction from the live runs: runTrigger's BeginDeferreds are
> EX_CallMath (PE-INVISIBLE) — section 5's "the existing B3b spawn-catch seam applies" was
> WRONG for this spawner. The bytecode facts (sections 1-4, 6) remain the durable ground truth.

Date: 2026-07-04. Sources: kismet-analyzer `to-json` of
`research/pak_re/extracted/VotV/Content/objects/piramid2.uasset` (fresh decode →
`research/bp_reflection/piramid2.json`, 113 exports; ubergraph CFG dump kept at
`research/bp_reflection/_piramid2_uber_full.txt`) + the pre-existing
`research/bp_reflection/piramidSpawner.json` + the live SDK dump
`Game_0.9.0n/.../CXXHeaderDump/piramid2.hpp` / `piramidSpawner.hpp` /
`piramid_sk_prot_Skeleton_AnimBlueprint.hpp` / `killerwisp.hpp` / `piramidTest.hpp`.
Evidence tags: [bytecode] = decoded Kismet statements (offsets are ubergraph byte
offsets in `_piramid2_uber_full.txt` unless a function name is given); [json] =
export/property tables in piramid2.json; [sdk] = CXXHeaderDump; [code] = our src
tree; [docs] = prior findings docs. Confidence: everything tagged [bytecode]/[json]/
[sdk] is read directly from data; inferences are marked.

---

## 0. TL;DR for the coop mirror

The pyramid is ONE plain `AActor` (`piramid2_C : AActor` [sdk piramid2.hpp:4]) that
moves itself by per-tick `K2_SetActorLocation` (NO CharacterMovement, NO nav agent,
NO root motion) toward host-random targets, hovers `10000*scale` units above a
per-tick ground trace, and walks its three legs 100% PROCEDURALLY in its AnimBP from
its own world motion. **Position/rotation are host-random (must be wire-mirrored);
everything else (legs, foot-plant sounds/particles/shakes, ping, beams) derives
per-viewer from the mirrored transform + the mirrored killerwisps.** It is NOT in
the B3b `kWorldActorAllowlist` today (only `piramidSubpawn_C` is — a different,
piramidTest-owned class) [code sdk_profile_names.h:177-194]. The event row
`piramid` IS in the client replay allowlist (`kReplayRows`
[code event_fire_sync.cpp:99]), which arms the client's own trigger box —
walk-in would spawn a CLIENT-LOCAL pyramid + 4 client-local killerwisps
(dup-class gap; see §7).

---

## 1. Role, components, variables

`piramid2_C` IS the whole walking pyramid: mesh + legs + sounds + lights + beams +
radar dot, one actor. Superclass `Actor` [json ClassExport SuperIndex; sdk].

Components (SCS templates, all `_GEN_VARIABLE` in the export table [json exports
31-109]; instance property offsets [sdk piramid2.hpp]):

| component | class | purpose (from bytecode use) |
|---|---|---|
| `DefaultSceneRoot` @0x348 | SceneComponent | root |
| `mesh` @0x328 | SkeletalMeshComponent | `piramid_sk_prot1` skeletal mesh, AnimClass `piramid_sk_prot_Skeleton_AnimBlueprint_C`, RelativeLocation Z **-10000** (mesh hangs 100 m below the actor root — the root floats, legs reach down) [json mesh_GEN_VARIABLE] |
| `movementVector` @0x330, `Arrow` @0x338, `lookDir` @0x268 | ArrowComponent | movement heading (fwd vector = walk direction), duplicate heading arrow, wisp-scan facing |
| `lookat` @0x260 | BillboardComponent | world-space look-target marker; fed to `anim.lookAtLoc` every tick |
| `p_L` @0x278, `p_R` @0x280 | BillboardComponent | left/right "hand" beam origins (wisp-side beam param) |
| `s1`/`s2`/`s3` @0x2A0/0x270/0x2A8 | BillboardComponent | leg/foot reference points (step positions) |
| `step_{close,med,far,sfar}{1,2,3}` @0x2B8-0x310 | AudioComponent (12) | per-leg foot-plant sound at 4 distance rings (`piramid_step_close/med/far/sfar_Cue` [json NameMap]) |
| `piramidPing` @0x2B0, `piramidPing_spooky` @0x228 | AudioComponent | 30 s ping (`piramidPing_Cue`); spooky variant (Halloween; NameMap includes `stockCreepyHalloweenEvilLaughSoundEffect`) |
| `effPing` @0x238 | ParticleSystemComponent | `eff_piramidPing` burst with the ping |
| `effStomp_1/2/3` @0x248/0x240/0x250 | ParticleSystemComponent | `eff_piramidStomp` per-leg stomp dust |
| `eff_cons_L/R` @0x288/0x290 | ParticleSystemComponent | `eff_piramidSucc` gather beams (left/right arm → wisp) |
| `PointLight` @0x318, `SpotLight` @0x320 | light components | lit pyramid; recolored orange in Halloween [bytecode @318-505] |
| `CameraShakeSource` @0x298 | CameraShakeSourceComponent | `piramidPingShake_C`, Inner 1000 / Outer **500000** (5 km — map-wide), bAbsoluteScale [json CameraShakeSource_GEN_VARIABLE] |
| `Sphere` @0x340 | SphereComponent | collision only — NO overlap events bound anywhere in the bytecode |
| `pumpkin` @0x230 | StaticMeshComponent | `jackoPumpkin_default`, hidden by default, shown in Halloween mode |
| `radarPoint` @0x258 | comp_radarPoint_C | shows on the player radar/map |
| `mov` @0x388, `Timeline_0/1/2` @0x378/0x368/0x358 | TimelineComponent | see §2/§4 |

Blueprint variables [json LoadedProperties; offsets from sdk piramid2.hpp]:

| var | type | off | meaning (bytecode-derived) |
|---|---|---|---|
| `anim` | piramid_sk_prot_Skeleton_AnimBlueprint_C* | 0x390 | cached AnimInstance; actor writes `anim.piramid2=self`, `anim.lookAtLoc`, `anim.armUp` |
| `speed` | float | 0x398 | walk speed, default **1500** [json CDO] |
| `isWalking` | bool | 0x39C | walking latch |
| `walkSpot` | FVector | 0x3A0 | current walk target point |
| `multiplyWalk` | float | 0x3AC | 0..1 walk-blend from the `mov` timeline (10 s ramp [json mov_Template]); also scales turn rate |
| `walkActor` | AActor* | 0x3B0 | walk-target actor (overrides walkSpot) |
| `wispTarget` | killerwisp_C* | 0x3B8 | current gather victim |
| `relLook` | FVector | 0x3C0 | idle look-around target (random) |
| `wispGathered` | bool | 0x3CC | set after a wisp is consumed |
| `gathering` | bool | 0x3CD | gather choreography in progress (freezes movement/AI timers) |
| `p` | float | 0x3D0 | Timeline_0 alpha copy |
| `suc` | float | 0x3D4 | suction strength (Timeline_1 alpha × 2) |
| `spawner` | piramidSpawner_C* | 0x3D8 | **CPF_ExposeOnSpawn**; set by the spawner at deferred spawn |
| `foliage` | InstancedFoliageActor* | 0x3E0 | added to `ignore` for the ground trace |
| `ignore` | TArray<AActor*> | 0x3E8 | trace-ignore list (foliage + `gamemode.borders`) |
| `speed_modify` | float | 0x3F8 | `scaleX * speed` (CDO 2500) |
| `gamemode` | mainGamemode_C* | 0x400 | cached via `lib_C::getMainGamemode` |

Functions [json exports 1-28]: ubergraph, `CustomEvent` (ping), `ReceiveDestroyed`,
`changeLook`, `seeWisps`, `randLoc`, `checkIfReached`, `moveAnim(bool)`,
`ReceiveBeginPlay`, `ReceiveTick`, 5 montage-proxy delegates
(`OnCompleted/OnBlendOut/OnInterrupted/OnNotifyBegin/OnNotifyEnd_A01F...`),
4 timelines × Update/Finished, `step(int Selection)`, `walkTo(FVector walkSpot,
AActor* walkActor, bool skipProjection)`, `walkSpotFunc() -> FVector`,
`scanWisps(killerwisp_C*& out)`, `isInside() -> bool`.

## 2. Movement — how it walks

**No AI controller, no navmesh pathing (only a point projection), no root motion.
It is a self-ticking SetActorLocation actor.** `PrimaryActorTick.bCanEverTick=True`
[json CDO].

`ReceiveTick` → ubergraph @2140 [bytecode piramid2.ReceiveTick]. Tick chain
(PUSH stack @2287-2312 executes @2312-then-@4488, @4104, @3655, @4588, @5133):

1. **speed_modify** = `GetActorScale3D().X * speed` — recomputed EVERY tick @2140.
2. **Terrain-follow hover** @2312-3654: `SphereTraceSingleForObjects` from
   `loc` straight down `15000*scaleX`, radius `500*scaleX`, against
   `lib_obj::obj_static` object types, ignoring `ignore` (foliage + borders); if
   hit: target = `hit.Location + (0,0,10000*scaleX)`, then per-axis `FInterpTo`
   (Z speed 5, X/Y speed 15) → `K2_SetActorLocation(sweep=false)`. So the actor
   root floats 10000*scale (= **20000 units at the event's scale 2**) above the
   ground and eases over terrain.
3. **Forward march** @4104-4487: if `!gathering`:
   `loc += movementVector.ForwardVector * speed_modify * dt * float(isWalking)` →
   `K2_SetActorLocation`. (isWalking=false ⇒ multiplier 0 ⇒ stands still.)
4. **Turning** @4588-5132: if `multiplyWalk > 0`: `RInterpTo` the
   `movementVector` (and `Arrow`) world rotation toward
   `FindLookAtRotation(loc*(1,1,0), walkSpotFunc()*(1,1,0))` (yaw-only math) at
   interp speed `Lerp(0, 1, multiplyWalk)`. The `mov` timeline (length 10 s
   [json mov_Template]) ramps `multiplyWalk` 0→1 on walk start
   (`moveAnim(true)` → `mov.PlayFromStart` @6271) and 1→0 on stop
   (`moveAnim(false)` → `mov.ReverseFromEnd` @6238); when the reverse finishes,
   `walkActor := null` @8155→@7405.
5. **Look-at** @3655-4103: `lookat` component eases toward `wispTarget`'s location
   (VInterpTo speed 1) if a wisp is targeted, else toward random `relLook`
   (relative). @4488: `anim.lookAtLoc := lookat.WorldLocation` every tick.
6. **Beam drive** @5133-5620: if `wispTarget` valid, per-tick particle params:
   own `eff_cons_L/R` vector param `target` = `wispTarget.center` location; the
   WISP's `eff_L/eff_R` param `loc` = own `p_L`/`p_R` locations (killerwisp fields
   `eff_L/eff_R` @0x4C8/0x4D0 [sdk killerwisp.hpp:7-8]).

**Walk-target selection** (`walkSpotFunc` [bytecode fn]): priority
`wispTarget.location` > `walkActor.location` > `walkSpot` (stored point).

**`walkTo(spot, actor, skipProjection)`** [bytecode fn]: no-op if already
`isWalking`; else sets `isWalking=true`, `walkActor=param`,
`walkSpot=param` (projected via `K2_ProjectPointToNavigation` with QueryExtent
(1000,1000,100000) unless skipProjection), then `moveAnim(true)`.

**AI (timers, all bound in BeginPlay** @5713-5956 [bytecode]):

| timer | period | handler | behavior |
|---|---|---|---|
| `seeWisps` | 1 s | @7905 | if `!gathering && wispTarget==null`: `scanWisps()` → `wispTarget`; if found, `walkTo(wispTarget)` @633 |
| `checkIfReached` | 1 s | @6323 | if walking & !gathering: 2D dist(loc, walkSpotFunc()) ≤ **10000** ⇒ arrived. If `spawner` valid AND `gamemode.killerWisps` EMPTY → `lib_C::progressAdvancement('piramid')` + `K2_DestroyActor()` @7321 (THE END). Else → stop (`isWalking=false`, `moveAnim(false)`) and, if `wispTarget` valid, start the GATHER (§4) @6733-6798 |
| `randLoc` | 5 s | @7417 | if idle (!isWalking, !gathering): spawner valid? — wisps remain ⇒ `walkTo(RandomPointInBoundingBox(center (0,0,1000), extent (30000,30000,0)))` @7582 (RANDOM); no wisps ⇒ `walkTo(spawner.walkFinish.location, skipProjection=true)` @7662 (exit march). Spawner NULL? — `isInside()` ⇒ random point again; else ⇒ `walkTo(GetPlayerPawn(0).location)` @7803 (**walks at player 0**) |
| `changeLook` | 1 s | @8011 | if !gathering: `relLook := RotateAngleAxis((10000,0,10000), RandomFloatInRange(-45,225), (0,0,1))` (RANDOM idle head wander) |
| `CustomEvent` | 30 s, first 15 s | @8413 | PING: `piramidPing.Activate` + `effPing.Activate` + `CameraShakeSource.Start()` |

`scanWisps` [bytecode fn]: iterates `gamemode.killerWisps`; a wisp qualifies iff
dist ≤ **100000** AND `dot(dirToWisp, lookDir.Forward) > 0` (front hemisphere) AND
`isInside()` (self within the ±70000 box around world origin
[bytecode piramid2.isInside]); picks the nearest via `lib_C::closestActor`.

**Determinism verdict**: position is NOT deterministic from spawn+time. Random
inputs: `randLoc`'s `RandomPointInBoundingBox`, `changeLook`'s random look, and the
wisp positions themselves (which drive both the chase target and arrival). Host
must stream the transform.

**The legs** are NOT in this actor's bytecode at all: the AnimBP
`piramid_sk_prot_Skeleton_AnimBlueprint_C` [sdk] is a procedural walker — per-leg
`legOrig/legLoc/legLerp/legFin_1..3`, `moving_1..3`, `StepSize/stepSpeed/
stepHeight(+_modify)`, foot-hit rotators, 3 CCDIK + LookAt + ~30 ModifyBone nodes,
spring states — all computed from the mesh's world transform each frame. It holds
back-refs `piramid2` (@0x3B30, written by the actor @5957) and `piramid`
(ApiramidTest_C @0x3AD8, the OTHER pyramid class §6). ⇒ **a transform-mirrored
client copy animates its own legs correctly for free.**

## 3. Lifecycle

- **Scheduler row** `piramid` (list_events idx 40, day 30, "18:00", story-scripted
  [docs votv-event-system-RE-2026-06-13.md:101]) → eventer `runEvent('piramid')`
  → activator `trigger_TBAc_piramid` (keyed `event_piramid`) ARMS the level volume
  `TB_event_piramid`; a player walking into the box fires the chain head
  **`piramidSpawner_2`** (level-placed `piramidSpawner_C` instance)
  [docs votv-event-trigger-graph-RE-2026-07-03.md:52]. So the pyramid does NOT
  appear at fire time — it appears when someone walks into the armed volume.
- **`piramidSpawner_C : actor_save_C`** [json piramidSpawner.json; sdk
  piramidSpawner.hpp] — components `walkFinish` + `walkLeave` billboards (route
  markers); vars `piramid -> piramid2_C`. Its `runTrigger` → ubergraph @840
  [bytecode piramidSpawner]:
  1. loop ×4 (Temp ≤ 3 @660): deferred-spawn **`killerwisp_C`** at
     `RotateAngleAxis(MakeVector(RandomFloatInRange(40000,69000), 0, 28000),
     RandomFloatInRange(0,360), Z)` — 4 wisps on a random ring 40-69 km out at
     z 28000 (the finish-transform re-rolls the randoms; both are random anyway).
  2. `gamemode.getObjectFromKey('wispSpawner').K2_DestroyActor()` @713 — kills the
     ambient wisp spawner for the save.
  3. @850: `BeginDeferredActorSpawnFromClass(piramid2_C, transform(own location,
     rot 0, **scale (2,2,2)**))` + `SetObjectPropertyByName('spawner', self)` +
     `FinishSpawningActor`; stores `piramid`.
  (`cordUnplugged/cordPlugged/setActiveTrigger/...` are empty POP→ret stubs
  @836-839 — actor_save_C interface fillers.)
- **BeginPlay** (`piramid2` ubergraph @5698→@15): loop-wait for
  `lib_C::getMainGamemode` (0.2 s Delay retry); **Halloween variant** @233: if
  `gamemode.gameInstance.gamemode == 5` (byte 5 = halloween mode [docs
  votv-spawnmenu-storymode-RE-2026-06-14.md:25]) → pumpkin visible, ping :=
  spooky cue, Spot/Point lights → orange (1, 0.076, 0.01), mesh material 0 :=
  `inst_goregibs_organsSK` (gore). Then bind the 5 timers (§2 table), cache+wire
  the AnimInstance (`anim.piramid2 := self` @5957), then @5991: `foliage :=
  GetActorOfClass(InstancedFoliageActor)`, `ignore += foliage`,
  `ignore += gamemode.borders`, **`lib_C::setEvent(true, false, self)`** —
  registers in the native active-events registry.
- **END**: `checkIfReached` @7321 — reached target while `spawner` valid and
  `gamemode.killerWisps` empty ⇒
  **`lib_C::progressAdvancement(name'piramid', false, self)`** (story-struct write;
  the scheduler separately appends `passEvents 'piramid'`) + `K2_DestroyActor()`.
  No other saveSlot writes in this class (no points, no forceObjects; the spawner's
  `wispSpawner` destroy in step 2 above is the other durable world change).
- **ReceiveDestroyed** @8373: **`lib_C::setEvent(false, false, self)`** —
  deregisters. (Matches the registry census
  [docs votv-active-events-registry-RE-2026-07-04.md:77].)
- Timed end: NONE. It ends only by reaching `walkFinish` with all wisps consumed.
  With `spawner == null` (e.g. a mirrored copy) the destroy branch is unreachable
  — it wanders/gathers forever.

## 4. Actions beyond walking — the wisp GATHER choreography

Trigger: `checkIfReached` arrives with `wispTarget` valid @6733:
`isWalking=false; moveAnim(false); wispTarget.gather(null, self)`
(killerwisp UFunction `gather(ApiramidTest_C*, Apiramid2_C*)`
[sdk killerwisp.hpp:65]; the wisp's own BP then freezes/gets pulled — wisp field
`gathered` @0x619 [sdk]); `gathering=true`; play montage
**`piramid_sk_prot1_Skeleton_Montage`** slot `gather` via
`CreateProxyObjectForPlayMontage` @6847 with all 5 delegates bound.

Montage notify names drive the sequence (switch @815 [bytecode]):
- **`begin`** @1011: activate own `eff_cons_L/R` + the wisp's `eff_L/eff_R` beams;
  `Timeline_0.PlayFromStart` (3 s [json]; update @1895 writes particle float param
  `beamAlpha = a*3` on both beams; copies to `p` @1620); then
  `Timeline_1.PlayFromStart` (update @1825: `suc := a*2`; finished @8229:
  `suc := 0`).
- **`gather`** @1274: deactivate the wisp's beams; `Timeline_0.ReverseFromEnd`
  (its finished-reverse path @1694 deactivates own beams + `wispTarget.center`).
- **`del`** @1423: **`wispTarget.K2_DestroyActor()`** (the pyramid CONSUMES the
  killerwisp), `wispTarget := null`, `wispGathered = true`,
  `Timeline_2.ReverseFromEnd`.
- Montage Completed/BlendOut/Interrupted @1570: `gathering=false`,
  `wispTarget=null` (releases the AI freeze).
- `Timeline_2` (update @8253): `anim.armUp := Ease(0→1, InOut exp 2)` — arm raise;
  played forward at gather start @7271, reversed at `del`.

Foot-plant FX: **`step(Selection 0|1|2)`** [bytecode piramid2.step] — per leg N:
activate the 4 distance-ring step AudioComponents + `effStomp_N`, then
`PlayWorldCameraShake(piramidStepShake_C, at step_closeN location,
inner 1000*scaleX, outer 200000*scaleX, falloff 8)`. Caller: not in this asset —
`UpiramidStep1/2/3_notify_C : UAnimNotify` classes exist [sdk
piramidStep{1,2,3}_notify.hpp], i.e. anim-notifies on the walk animation fire
step(N) through the AnimBP's `piramid2` ref (MEDIUM confidence — the AnimBP
.uasset is not extracted; the notify classes + the `anim.piramid2=self` wiring
make any other route unlikely).

Ping (every 30 s): `piramidPing` cue + `eff_piramidPing` + `piramidPingShake_C`
from the CameraShakeSource (outer radius 500 km ⇒ effectively map-wide shake).

**No sub-actor spawns from piramid2 itself** (the 4 killerwisps come from the
SPAWNER, once, at trigger time). No player interaction: no overlap bindings, no
damage, no abduction in this class; player proximity matters only as shake/sound
attenuation, and as the walk target in the no-spawner fallback @7803.

## 5. Sync-relevant verdict — externally visible state axes

Classification: (a) deterministic from mirrored inputs — replaying/deriving on the
client converges; (b) host-random/host-stateful — must cross the wire; (c)
per-viewer cosmetic.

| axis | class | notes |
|---|---|---|
| spawn moment + identity | **(b)** | spawner runTrigger fires on host walk-in; deferred spawn via `BeginDeferredActorSpawnFromClass` ⇒ the existing B3b spawn-catch seam applies. Scale (2,2,2) is constant |
| position (X,Y) | **(b)** | random targets (`randLoc`), wisp-chase, player-chase fallback — host must stream |
| position (Z) | **(a)** given X,Y | pure function of the ground trace + 10000*scale hover; client re-derives (or just take the wire Z — both fine) |
| rotation (yaw) | **(b)** (or (a) from streamed pos deltas) | RInterpTo toward walk target; cheapest correct: stream full rotation with the transform. **SUPERSEDED 2026-07-05 (as-built truth):** the "(a) from pos deltas" option was BUILT and LIVE-REFUTED — the heading keeps RInterpTo-easing toward walkSpotFunc for up to 10 s AFTER motion stops (the mov ramp-down), invisible to deltas; and "stream full rotation" is subtler than written: the ACTOR rotation never changes (root yaw stays 0 — live [WA-TRACE] evidence), the visible heading is the movementVector/Arrow COMPONENTS' world rotation. As-built = v100 `WorldActorPoseSnapshot.auxYaw` streams the component yaw (`75e5ab10`) |
| leg animation | **(a)** | 100% procedural in the AnimBP from world motion — free on a pose-mirrored copy |
| foot-plant sounds/stomp particles/step shakes | **(a)/(c)** | anim-notify-driven from the (procedural) walk anim; attenuation per-viewer |
| 30 s ping (sound+particle+shake) | **(a)-ish** | pure timer from BeginPlay; drifts vs host only in phase, no gameplay meaning. Acceptable per-peer; wire only if phase-lock is wanted |
| `isWalking` / `multiplyWalk` (stand vs march blend) | **(b)** | flips on host AI decisions; needed so a client copy that ISN'T running its own AI stops/starts correctly (if the client copy runs its own BP timers, it will fight the mirror — see §7) |
| gather choreography (montage, armUp, beams, suc) | **(b)** trigger, **(a)** playout | the START (which wisp, when) is host AI; once `wispTarget` identity + gather-start cross, the beams' per-tick params derive from the two mirrored actors' positions IF the client copy runs its tick with the same `wispTarget` set |
| wispTarget identity | **(b)** | must map host killerwisp → client's mirrored killerwisp (killerwisp_C is ACharacter, already host-authoritative via the NPC pose mirror [code wisp.h:7, ambient_spawner_suppress.cpp:69]) |
| wisp consumption (`del` → wisp destroy) | **(b)** | host destroys its wisp → NPC mirror despawns the client copy already; the client's piramid2 copy must NOT also locally destroy a wisp (double-destroy) |
| Halloween variant (pumpkin/lights/gore/spooky ping) | **(a)** | derived from `gameInstance.gamemode == 5`, same on both peers |
| lookat / relLook head wander | (c) | random idle look; cosmetic, invisible desync |
| radar dot | **(a)** | `comp_radarPoint_C` follows the actor — free with the transform |
| END (destroy + `progressAdvancement('piramid')`) | **(b)** | host-only decision; story write must stay HOST-ONLY (client copy has `spawner==null` ⇒ can never reach the @7321 branch — structurally safe [bytecode @6598]) |
| spawner side effects (4 killerwisp spawns, `wispSpawner` destroy) | **(b)** | host-authoritative; must NOT replay on client (§7) |

**Current mod coverage** [code]: `piramid2_C` is NOT pose-mirrored — the B3b
transform mirror allowlist `kWorldActorAllowlist` has `piramidSubpawn_C` only
(that's piramidTest's child pawn, §6) [code sdk_profile_names.h:193]. The event
rows `piramid`/`piramid_sig` are in `kReplayRows` (client replays the ARM)
[code event_fire_sync.cpp:99,103] and in the dev F1 menus
[code event_trigger.cpp:74,119; event_force.cpp:44].

## 6. `piramid` vs `piramid2` vs `piramidTest` vs `piramid_sig`

- **`piramid` (list_events row 40)**: the walking-pyramid EVENT. runEvent arms
  `trigger_TBAc_piramid` → `TB_event_piramid` volume → `piramidSpawner_2.runTrigger`
  → 4× killerwisp_C + 1× **piramid2_C** (scale 2). [docs event-trigger-graph:52;
  bytecode piramidSpawner]
- **`piramid2_C`**: the event actor RE'd here. Plain AActor.
- **`piramidTest_C`**: a SEPARATE `ACharacter` implementation of the same visual
  (same AnimBP via its `piramid` back-ref slot [sdk AnimBP.hpp:108], same
  step/beam/ping component roster, plus a `subPawn` ChildActorComponent of
  `piramidSubpawn_C : APawn` (Sphere + FloatingPawnMovement) [sdk
  piramidTest.hpp, piramidSubpawn.hpp]). Referenced by `list_props`
  (sandbox spawn menu) and by killerwisp (`piram` @0x620, `gather` param 1); NOT
  spawned by the event chain. This is the possession-risk class the B3b comment
  routes to "B3a/verify" [code sdk_profile_names.h:176].
- **`piramid_sig` (row 41)**: NO actor — `trigger_forceObject` appends the
  `piramid_sig` signal to `saveSlot.forceObjects` (SETI pool), like the
  arirSat signals [docs event-system-RE:102,521]. Already replay-covered
  [code event_fire_sync.cpp:103].
- Rewards: the only durable writes of the whole `piramid` event are
  `progressAdvancement('piramid')` (+ scheduler `passEvents`), the
  `wispSpawner` destroy, and the killerwisp removals. No prop_obelisk-style
  physical reward object is spawned [bytecode — exhaustive over both assets].

## 7. Coop design notes / open gaps (for the mirror work, not built here)

1. **Dup-class gap (real today)**: `piramid` ∈ `kReplayRows` ⇒ the client's
   `TB_event_piramid` box arms too. A CLIENT walking in first fires the client's
   `piramidSpawner_2.runTrigger` → client-local pyramid + 4 client-local
   killerwisps + client-local `wispSpawner` destroy. `ambient_spawner_suppress`
   only gates `ticker_yellowWispSpawner_C.ReceiveTick`, not this path
   [code ambient_spawner_suppress.cpp:76]. The wisp lane's spawn-catch enrolls
   HOST spawns; a client-side runTrigger is client-authoritative and unmirrored.
   Root fix direction: make `piramidSpawner_C.runTrigger` host-only (client-side
   cancel interceptor on the class function + host wire-fire), same shape as the
   other host-authoritative event actors.
2. **Mirroring the pyramid**: it satisfies every B3b precondition — plain AActor,
   BeginDeferred spawn, transform-only visual truth, procedural anim. Adding
   `piramid2_C` to `kWorldActorAllowlist` gives spawn+pose+despawn. BUT unlike the
   UFOs, the client copy RUNS ITS OWN AI (BeginPlay timers + tick march) and
   OWNS state writes (`wispTarget.gather`, wisp `K2_DestroyActor` at `del`,
   `setEvent`): the client copy will fight the pose drive (its tick SetActorLocation
   vs the mirror's — mirror wins at cadence but jitters) and can double-destroy
   mirrored wisps. A correct mirror needs the client copy's autonomy cut at the
   proper seams (e.g. client-side cancel of its timer handlers/`walkTo`, or
   gather-start driven by wire replay of `checkIfReached`'s decision), with the
   gather choreography replayed via `wispTarget` + montage — the per-tick beam
   params then converge locally (§5).
3. `setEvent(true/false)` on the client copy is DESIRABLE (active-events registry
   / no-pause invariant, [docs votv-active-events-registry-RE-2026-07-04.md]) —
   whatever suppression is built must keep the registry writes.
4. Late-join (DEVS_GAUNTLET): the EventSnapshot lane for `piramid` needs: pyramid
   transform + isWalking/multiplyWalk + remaining killerwisps (already covered by
   the NPC lane's join answer) + `gathering/wispTarget` if mid-gather.
   [RESOLVED 2026-07-05 v98 `e865b7f2`, differently than sketched: transform rides
   the WA connect snapshot (isWalking stays false on mirrors BY DESIGN — brain
   suppressed); mid-gather is a lane-local join-edge PyramidGather re-send
   (`piramid_sync::QueueConnectBroadcastForSlot`), not EventSnapshot fields; the
   generic EventSnapshot carries 'piramid' but the verdict table skips it as
   lane-owned. COOP_EVENT_JOIN.md 3.4.]

## Decode provenance

- `research/bp_reflection/piramid2.json` — generated this session:
  `tools/kismet-analyzer/kismet-analyzer-e8982e9-win-x64/kismet-analyzer.exe
  to-json research/pak_re/extracted/VotV/Content/objects/piramid2.uasset >
  research/bp_reflection/piramid2.json` (stdout capture; json.load clean, no
  _fixjson needed; 113 exports).
- Function dumps: `python research/bp_reflection/_fn.py piramid2 <fn>` (run from
  repo root — _cfg.py hardcodes the `research/bp_reflection/` relative path).
- Ubergraph: `python research/bp_reflection/_cfg.py piramid2
  ExecuteUbergraph_piramid2` → `_piramid2_uber_full.txt` (308 stmts, 8538 bytes,
  101 blocks — offsets cited above).
- piramidSpawner.json pre-existed; its ubergraph decoded this session (43 stmts).
