# Puppet sounds RE -- footsteps / pick-up / throw (2026-06-11)

> **SUPERSEDED DETAIL 2026-07-04 (`1572149d`): the "chain measure" the
> receiver-geometry sections below reference NO LONGER EXISTS.** It raced
> mesh_playerVisible's BP construction on world-fresh clients (measured chain
> 0.00 -> latched meshOffsetZ_=-85 -> puppet sunk 85 + capsule-depenetration
> twitch; user hands-on 2026-07-04 16:48). meshOffsetZ_/meshOffsetYaw_ are
> retired (RULE 2): the puppet actor rides the wire pose VERBATIM (both ends
> mainPlayer_C = class-identical settled chains). The Mesh-slot RelLoc.Z:=0
> settle-write (par.1's footstep fix) REMAINS -- it is what makes the
> zero-offset invariant true. Everything else here (sounds RE, dispatch
> shapes) stands.

> **STATUS 2026-06-11 NIGHT -- PICK-UP CLOSED, USER-CONFIRMED ("finally it
> works"), after hands-on round 3 FALSIFIED this doc's grab-sound answer:**
> the audible E-grab feedback is NOT the per-material soft cue. The user
> reported "the sound is always the same, it's a unique single sound" -> an
> all-FUNCTIONS PlaySound scan (`_scan.py mainPlayer PlaySound` -- the
> original sweep covered only the UBER) found it: **`useAction` plays the
> FIXED SoundWave `use` via `PlaySound2D(use, 0.25, 1.0)` immediately after
> `pickupObject` (@2181; every successful E branch ends in a `use` play,
> every refusal in `use_deny`)** -- 2D, GRABBER-ONLY, hence structurally
> inaudible to peers. The E key chain: InpActEvt_use -> uber @101968 ->
> player_use / useAction(false) bracketed by playerGrabbed_pre/playerGrabbed;
> `pickupObject` itself is a 4-stmt uber trampoline with NO sound. The
> @100337 material-soft is a SECONDARY grab-variant thud (kept). FIX:
> `prop_sound::PlayUseClick` -- `use` spatialized at the grabbed prop on
> GRAB-IN, vol 0.5 (native 0.25 is an in-ear 2D figure; world source must
> survive att_default rolloff), the throw-whoosh synthesis pattern, played
> ALONGSIDE PlayGrabSound. ALSO that round: **prop_garbageClump_C derives
> from plain Actor, NOT prop_C** (SuperStruct import = Actor; the prop_ name
> lies) -> clump/pile grabs hit PlayGrabSound's IsDescendantOfProp bail ->
> NEW `ue_wrap::engine::GetActorRootPhysicalMaterial` (root comp ->
> GetMaterial(0) -> GetPhysicalMaterial(), all reflected) feeds the same
> physSound lookup for plain-Actor grabs; Fstruct_physSound layout
> dump-verified (0x98 bytes, soft @0x10 correct). Audit 0-CRIT (M-2: the
> root-material path ignores BodyInstance.PhysMaterialOverride + non-0 slots
> -- irrelevant for VOTV's single-material no-override grabbables).

> **STATUS 2026-06-11 EVENING -- ALL FOUR SHIPPED (v58) + smoke-PASS + audited;
> hands-on rounds applied two corrections to this doc:**
> 1. **Footsteps + run: SHIPPED per par.1/par.4 and USER-CONFIRMED WORKING.**
>    Mesh.RelLoc.Z one-shot settle at puppet spawn -> chain measure yields
>    meshOffsetZ_=0 -> actor at true capsule height (collision capsule no
>    longer floats +85); `DriveSprintWalkSpeed` (player-puppet-only, separate
>    from DriveCharacterMovement so NPC MaxWalkSpeed stays untouched). Volume
>    later user-tuned: votv_lib::CharacterStep grew a `volume` param,
>    kStepVolume=0.6 (puppet_footsteps.h).
> 2. **Throw: SHIPPED per par.3 and USER-CONFIRMED** (prop_sound::
>    PlayThrowWhoosh, swing spatialized, BOTH release branches;
>    clump_throw_sound deleted RULE 2 -- object_throw was a pre-RE guess).
> 3. **Pick-up CORRECTION (hands-on round 2 falsified par.2b's "populated at
>    prop init"):** a never-impacted prop's physSoundData cache is EMPTY (it
>    fills LAZILY) -- PlayGrabSound is now two-tier: cached soft_30 fast path
>    -> FRESH `lib_C::physSound(comp.PhysMat@0x0290)` ParamFrame call (the
>    exact native @100003 lookup incl. the row-miss return gate). Working
>    (logs: "grab cue (cache)" per grab).
> 4. **The user's "Half-Life pickup sound" = the INVENTORY blip (par.2's
>    inventory_Cue note), not the grab thud** -> shipped separately as v58
>    InventoryPickup=47 via a PlaySound2D POST observer; see
>    votv-inventory-pickup-seam-RE-2026-06-11.md (which also FALSIFIES the
>    "EX_CallMath bypasses ProcessEvent" claim for CDO-context
>    EX_FinalFunction calls -- only EX_Local* forms are invisible).
> Modules: coop/prop_sound.{h,cpp}, coop/inventory_pickup_sync.{h,cpp};
> hands-on for the blip PENDING.

**Question:** the remote-player puppet (mainPlayer_C orphan) produces NO sound for
(1) footsteps walking, (2) footsteps running, (3) object pick-up, (4) object throw --
even though our `coop/puppet_footsteps.h` stride emitter demonstrably dispatches
`lib_C::step` (host log shows the lazy resolve line exactly when a puppet walked).

**Method:** kismet bytecode disassembly (`tools/bp_reflect.py` + the offset-exact
`research/bp_reflection/_fn.py` / `_cfg.py` / `_disasm.py` drivers) of `lib_C::step`,
`lib_C::physSound`, the mainPlayer ubergraph (caller sites, grab chain, throw chain),
`comp_physicsImpact`, kerfurOmega's step path -- cross-checked against our own
receiver code (`remote_player.cpp`, `puppet.cpp`, `votv_lib.cpp`, `call.cpp`) and the
LIVE numbers in `votv-coop-host.log` / `votv-coop-client.log`.

Tooling note: `research/bp_reflection/lib.json` contains one invalid UTF-8 byte
(@5818019); a sanitized `lib_fixed.json` (json round-trip with `errors='replace'`)
was created next to it -- use `python research/bp_reflection/_fn.py lib_fixed step`.
`physSound` / `comp_physicsImpact` bytecode contains `EX_MapConst` which the shared
`size()` walker doesn't handle; offsets below were computed with a patched walker
(MapConst = 1 + 8 + 8 + 4 + entries + 1 terminator; validated against jump targets
@243/@338/@572 landing exactly on statement starts).

---

## TL;DR -- the four answers

| Sound | Native mechanism | Why the puppet is silent | Receiver-side dispatch |
|---|---|---|---|
| Footsteps (walk) | `lib_C::step` -> ground sphere-trace -> `physSound` datatable -> `SpawnSoundAttached(step cue, char root, ..., att_default, conc_footsteps)` | **Our `meshOffsetZ_=+85` actor-lift floats the capsule; step's trace bottoms out ~66 cm above the floor -> `bBlockingHit=false` -> the @940 gate returns before ANY audio.** Params are exonerated (kerfur control case). | Fix the puppet Z (zero the Mesh-slot RelLoc.Z at spawn, drop the actor-lift). The EXISTING `CharacterStep` dispatch then works unchanged. |
| Footsteps (run) | Same `step` call; volume = `FClamp(char.CMC.MaxWalkSpeed / speedVolume, 0.5, 2.0) * Volume`; sprint doubles `MaxWalkSpeed` (`updateSpeed` @676) | Same trace miss (one root cause for both) | Same fix; for the louder-when-running effect additionally write the parked puppet `CMC.MaxWalkSpeed` (default vs 2x default on the speed>400 edge) |
| Pick-up (E-grab) | mainPlayer uber @100337: `PlaySoundAtLocation(physSound(hit.PhysMat).soft_30, HitLoc + Normal*10, vol 0.5, pitch 1.0, att_default)` -- runs only inside the LOCAL grabber's input chain | The remote peer never executes the grabber's input chain (we apply grabs by state push) -- the sound is never dispatched for a remote grab | On GRAB-IN: read `prop.physicsImpact(@0x0230).physSoundData(@0x00C8).soft_30(+0x10)`; if non-null `PlaySoundAtLocation(soft_30, propLoc, 0.5, 1.0, att_default)` |
| Throw (LMB) | mainPlayer uber @16057 / @16826: `PlaySound2D(swing SoundWave, vol 0.8, pitch 1.05)` -- **2D, non-spatialized, audible to the THROWER ONLY by native design** | Even on the local machine this sound never exists in the world -- there is nothing to "miss"; it must be synthesized | On GRAB-OUT with release speed above a throw threshold: `PlaySoundAtLocation(swing, puppet head/hand, 0.8, 1.05, att_default)`. Plain drop/release stays SILENT (native parity). |

Nothing requires asset edits. All four are dispatchable through existing reflected
calls (`ProcessEvent` on `lib_C` / `GameplayStatics` CDOs + one component-field read).

---

## 1. lib_C::step -- full walkthrough + the silence root cause

`step(ACharacter* Character, float Z_offset, AActor* callActor, float Volume, float
Pitch, float speedVolume, UAudioComponent* AudioComponent, UObject* __WorldContext,
FHitResult& OutHit)` (CXX dump `lib.hpp:76`; cooked-asset property-name casings are
`character`/`volume`/`audioComponent` -- the LIVE FNames print capitalized because the
native engine registers those names first; zero `ParamFrame::Set: unknown param`
errors across all four votv-coop logs confirm every Set landed).

Disassembly (`_fn.py lib_fixed step`, 111 stmts / 5633 bytes). Flow-stack at entry:
`PUSH @5630` (the final RETURN).

### 1a. The trace (@285-@763)

```
@  285: CallFunc_MakeVector_ReturnValue := MakeVector(+0f, +0f, zoffset)
@  332: CallFunc_K2_GetActorLocation_ReturnValue := char.K2_GetActorLocation()
@  382: start := ActorLoc + (0,0,Z_offset)
@  428: CallFunc_Subtract_FloatFloat_ReturnValue := Subtract_FloatFloat(char.CapsuleComponent.CapsuleHalfHeight, 1.0f)
@  514/561: end := ActorLoc - (0,0, CapsuleHalfHeight - 1)
@  607: SphereTraceSingleForObjects(__WorldContext, start, end, 19.899999618530273f,
        objs={ObjectTypeQuery 0,1,9,5}, bTraceComplex=false, ActorsToIgnore=<empty local>,
        DrawDebugType=None, OutHit, bIgnoreSelf=true, ...)
@  763: PUSH @1125
@  940: IFNOT(bBlockingHit) POP        <-- ***THE GATE THAT SWALLOWS OUR CALL***
```

- Object types raw bytes `{0,1,9,5}` (EX_SetArray @5) = ObjectTypeQuery1
  (WorldStatic), 2 (WorldDynamic), 10 + 6 (two game channels). Floors/landscape are
  WorldStatic -> queried. Pawn is NOT queried (no self/other-player hits).
- `bIgnoreSelf=true` ignores `Cast<AActor>(__WorldContext)` (UE4.27
  `KismetTraceUtils.cpp ConfigureCollisionParams`) -- we pass `__WorldContext=puppet`,
  so the puppet ignores itself exactly like the native `self` call does.
- **No hit -> POP -> @1125 (`OutHit := trace hit; JUMP @5630`) -> RETURN.** No sound,
  no warning, no log. This is the ONLY total-silence gate in the function.

### 1b. Everything after the trace is pass-through (no controller/locality gate)

```
@  950: PUSH @1157
@  955: DoesImplementInterface(char, int_player_C)?      -- mainPlayer_C: YES (imports -109)
@ 1078:   char.playerStepped(OutHit)                     -- mainPlayer impl = uber @14917: BROADCAST playerSteppedOn (no-op w/o subscribers)
@ 1124: POP -> @1157                                     -- pass-through EITHER WAY
@ 1329: GetCollisionObjectType(HitComponent) == 20 ?     -- the WATER object channel
@ 1428-2403: water branch: wade_Cue (initial-overlap) / foot_water_Cue + eff_waterEnter, then POP -> @1125
@ 2404: (land branch) GetDisplayName(PhysMat) -> PrintString (no-op in Shipping)
@ 2846: physSound(PhysMat, __WorldContext, return, data) -- the material -> sound map (1c)
@ 2896: PUSH @3272;  @2901: PUSH @4092
@ 2906: DoesImplementInterface(callActor, int_objects_C)?
        - native mainPlayer call: callActor=null -> false -> POP -> @4092
        - OUR call: callActor=puppet, mainPlayer_C implements int_objects_C (imports -108)
          -> @3029-3166 volume calc -> @3212 callActor.stepped(volume, loc)
             (mainPlayer impl = uber @113070: bare POP -> no-op) -> @3271 POP -> @4092
        ***CONVERGES TO THE AUDIO EITHER WAY -- callActor non-null does NOT reroute the audio***
@ 4092: IsValid(AudioComponent)?  (null for both us and the native mainPlayer call)
        valid   -> @4135: comp.SetSound(data.step_2); comp.Play(0); isSnow param;
                   comp.SetVolumeMultiplier(clamp) -> POP -> @3272
        invalid -> @4559 (our path):
@ 5006:   SpawnSoundAttached(data.step_2_264EF62E..., char.K2_GetRootComponent(), 'None',
          HitLocation+offset, rot, b1=KeepWorldPosition, false,
          VolumeMultiplier = FClamp(char.CharacterMovement.MaxWalkSpeed / speedVol_loc, 0.5, 2.0) * volume_loc,
          pitch_loc, +0f, obj<att_default>, obj<conc_footsteps>, true)
@ 5411:   char.CharacterMovement.GroundFriction := (PhysMat.Friction * 4) ** 2
@ 5521:   spawned.SetBoolParameter('isSnow', gamemode.isSnowyFootsteps)  -> POP -> @3272
@ 3272: cast<mainPlayer_C>(char)?  -- kerfur: fails -> POP -> @1125.  Puppet: SUCCEEDS:
@ 3379:   getMainSave(__WorldContext).stats.steps += 1   <-- side effect, see 1f
@ 3741:   HitActor int_objects_C? -> HitActor.steppedOn(player, OutHit)
        -> POP -> @1125 -> RETURN
```

Key param semantics asked in the brief:

- **Volume does NOT read Velocity.** It reads `char.CharacterMovement.MaxWalkSpeed`
  (@3029 / @4334 / @4601 -- identical formula in all three paths):
  `FClamp(MaxWalkSpeed / speedVolume, 0.5, 2.0) * Volume`. The clamp floor is 0.5 --
  this term can NEVER mute the sound.
- **callActor**: just an `int_objects_C::stepped(volume, location)` notification to
  that actor before the audio; flow converges to the audio block regardless.
  mainPlayer_C's `stepped` handler is an empty uber stub (@113070 = `POP->ret`), so
  our `callActor=puppet` is a harmless extra no-op dispatch.
- **__WorldContext**: the trace world + bIgnoreSelf anchor + `getMainSave` /
  `getMainGamemode` context. Passing the puppet is correct.
- **AudioComponent**: non-null reroutes to the `comp.SetSound + Play` path (@4135);
  null (ours AND the native mainPlayer call) takes `SpawnSoundAttached`.

### 1c. lib_C::physSound -- the only other silence source

(`_disasm.py lib_fixed physSound` + patched-size offsets; jump targets @243/@338/@572.)

```
[ 0] @   0: IsValid(physMat)
[ 1] @  29: JUMPIFNOT @243
[2-4]       GetDisplayName(physMat) -> Conv_StringToName -> GetDataTableRowFromName(list_physSound, name, OutRow)
[ 5] @ 186: JUMPIFNOT @338  (row not found)
[6-8]       return := true; data := OutRow; JUMP @572
[ 9] @ 243: return := false
[10] @ 254: data := struct{}          <-- EMPTY: step_2 = soft_30 = ... = NULL
[11] @ 333: JUMP @572
[12-22] @338: data := MakeStruct{ step=foot_def_Cue, ..., particles=eff_woodDamage, staticSound=true }; return := false
[23] @ 572: RETURN
```

- **PhysMat NULL -> EMPTY data -> `SpawnSoundAttached(null)` -> engine returns null ->
  silent** (`step` never checks `physSound.return`).
- Unknown material name -> `foot_def_Cue` fallback (audible).
- For a correctly grounded character on the same floor as the local player the
  PhysMat is identical -> this branch is NOT the puppet's problem (but it IS the
  reason initial-overlap / penetrating traces would go quiet: overlap hits carry no
  face index -> null PhysMaterial).

### 1d. THE ROOT CAUSE -- the +85 actor-lift starves the trace

Receiver geometry (`src/votv-coop/src/coop/remote_player.cpp`):

- ApplyToEngine (~line 688): `puppetLoc = curPos_; puppetLoc.Z += meshOffsetZ_;
  SetActorLocation(actor_, puppetLoc)` -- the ACTOR (capsule) is lifted.
- Spawn chain measure (~line 212-229):
  `meshOffsetZ_ = srcChain - puppetChain = (-halfH) - (puppetMeshZ - puppetActorZ)`.

**Live numbers -- every puppet spawn in the current logs
(`Game_0.9.0n/.../votv-coop-host.log` + `votv-coop-client.log`, 4 spawns across 3
sessions):**

```
RemotePlayer::Spawn: chain measure (halfH-anchored, symmetric) -- halfH=85.00 srcChain=-85.00
                     puppet(meshZ=6149.49 actorZ=6319.49 chain=-170.00) -> meshOffsetZ_=85.00
```

So with floor at Z=0 and the source standing (capsule centre wire.z ~= 87):

| | actor.Z | trace start (@382) | trace end (@561) | sphere lowest reach | floor |
|---|---|---|---|---|---|
| local player | 87 | 87 | 87-84 = 3 | 3-19.9 = **-16.9** | 0 -> **HIT** |
| puppet (today) | 87+85 = 172 | 172 | 172-84 = 88 | 88-19.9 = **+68.1** | 0 -> **MISS by ~66 cm** |

-> `bBlockingHit=false` -> the @940 gate POPs -> RETURN. 100% silent on every call,
exactly matching the symptom (resolve line fires, no audio ever). `Z_offset` CANNOT
repair this: it shifts only the trace START (@285); the END stays anchored to
`ActorLoc - (CapsuleHalfHeight-1)` (@561), still ~66 cm short.

**Control case proving the params are fine:** the kerfur NPC mirror -- also
controller-less, also coop-driven -- is audible through the SAME function with the
SAME shape (kerfurOmega uber @34864, reached from `notify_kerfurStep.Received_Notify`
-> `int_anim_events::step(socketLoc)` -> kerfurOmega.step -> uber):

```
@34864: obj<Default__lib_C>.step(self, +0f, self, 1.0f, 1.0f, 400.0f, _, self, CallFunc_step_OutHit)
```

`callActor=self`, `AudioComponent=null`, `__WorldContext=self` -- i.e. OUR exact
pattern works for a non-possessed character whose actor sits at the true capsule
height. There is NO GetController / locality / possession gate anywhere in `step`
(full disassembly above -- the only casts are the three pass-through interface checks
and the stats-only `cast<mainPlayer_C>`).

### 1e. Why the actor is lifted at all -- and the root fix

The lift compensates a mesh chain the puppet never settles:

- mainPlayer_C authors `ACharacter::Mesh` (slot @0x0280) at RelLoc.Z = -85 and
  `mesh_playerVisible` (@0x04F8, attached to Mesh) at RelLoc.Z = -85 -> spawn-time
  world chain = -170 (the measured `puppetChain=-170.00`).
- The LOCAL player's BP tick then interpolates the Mesh slot's RelativeLocation to
  `(runLean, 0, 0)` -- mainPlayer uber:

```
@60155: GetVelocity -> VSize
@60224: X := VSize/(-25) - 25            (standing: -25; the FP body pull-back)
@60308: target := MakeVector(X, +0f, +0f)   <-- Z target is ZERO
@60355: VInterpTo(Mesh.RelativeLocation, target, worldDeltaSeconds, 5.0f)
@60484: Mesh.K2_SetRelativeLocation(ClampVectorSize(result, 0, 50), ...)
```

  -> settled local chain = 0 + (-85) = -85 (the measured `srcChain=-85`; also the
  documented "84 cm swing over the first ~2 s" in the v8 spawn comment -- that's this
  VInterpTo at speed 5/s converging).
- The puppet's actor tick is deliberately disabled (`puppet.cpp` ~line 431), so this
  write NEVER happens on the puppet; chain stays -170; we compensated at the ACTOR
  (+85) so the mesh renders at the right height -- and silently broke every
  ActorLocation-anchored mechanism, footsteps first.

**Fix (RULE 1, receiver-side, no game-file edits):** at puppet spawn, replicate the
single settled write the suppressed tick would have made -- set the puppet's
`ACharacter::Mesh` slot RelativeLocation Z to 0 (`K2_SetRelativeRotation` sibling
`K2_SetRelativeLocation`, the same reflected-call pattern already used for `lag_fl`)
BEFORE the chain measurement. The existing measurement then yields
`puppetChain = -85 -> meshOffsetZ_ = 0` automatically, the actor rides at the true
wire Z, and:

- component world transforms are IDENTICAL to today (mesh_playerVisible:
  (wire+85)-170 = wire-85 before vs wire+0-85 after; Mesh slot: wire either way) --
  zero visual change;
- `lib_C::step`'s native trace hits the floor -> **footsteps work with the EXISTING
  `CharacterStep` dispatch, zero changes to `votv_lib.cpp` / `puppet_footsteps.h`**;
- collateral fixes: the puppet's CAPSULE collision currently floats 85 cm high
  (knee-height walk-through, chest-height block) -- corrected; anything else reading
  the puppet's ActorLocation (proximity gates, AI hearing, future grab-distance
  checks) sees the true height;
- `GetHeadPosition` self-adapts (12 Hz head-bone-relative `headZOffset_` refresh,
  remote_player.cpp:854-862) -- only the pre-first-refresh default (+30) shifts for
  a few frames;
- optional exact parity: the local's settled Mesh RelLoc.X is -25 (the body
  pull-back); the minimal root fix is Z-only (X changes where the body renders by
  25 cm vs today's accepted visuals -- decide at implementation).

### 1f. Two benign side effects to be aware of (post-fix)

- `step` increments the OBSERVER's save `stats.steps` (+1 per puppet stride, @3379-
  @3511) because `cast<mainPlayer_C>(puppet)` succeeds -- the kerfur doesn't do this
  (cast fails). Not fixable at param level (it keys off the Character's class);
  accepted as a stats-only divergence.
- `step` writes the puppet's `CMC.GroundFriction` (@5411) -- the CMC tick is parked,
  nothing reads it; harmless.

### 1g. Native caller (mainPlayer uber) -- the gates our emitter mirrors

```
@70172: GetVelocity -> VSize;  @70241: > 10.0 ?            (move gate)
@70289: PUSH @70685
@70294: BooleanOR(inWater, onWater) -> @70332 SelectFloat(0.5, 1.0, ...)   (water 0.5x)
@70379: findFootLocation();  @70411: SelectFloat(2.0, 1.0, bIsCrouched)    (crouch 2x)
@70458: SelectFloat(0.75, crouchSel, input_run)                            (run 0.75x)
@70509: Vector_Distance(lastStep, foot) * factors -> @70647 step += ...
@70685: step > 150.0 ?  -> @70729 step := 0
@70752: MovementMode == 1 (Walking) || inWater || onWater || underwaterPlayer ?
@70929: IsValid(atv) ? -> @70972 POP (no footsteps while riding)
@70973: obj<Default__lib_C>.step(self, +0f, _, 1.0f, 1.0f, 400.0f, _, self, OutHit)
@71043: (speed <= 10 path) lastStep := findFootLocation()                  (re-prime)
```

Two more `step` sites reuse the same callee: @6505 (immediately after `doJump()` --
take-off scuff) and @104721 (the landing custom event, gated @104647 by a 1-element
PhysMat whitelist, then `Crouch(false)`). Once the Z fix lands, the wire's existing
`kStateBitInAir` edges can mirror jump/land scuffs with the same `CharacterStep`
call (optional polish; the stride emitter already self-recovers via its re-prime).

---

## 2. Pick-up sound

### 2a. Physical E-grab (the audible "pick up" in the world)

Chain (mainPlayer uber): grab custom event (@102953, takes the crosshair
`hitResult`) -> @103152 `sittingOn != HitActor` -> @103372 `HitComponent.
IsSimulatingPhysics` -> @103613 `grabbing_actor := HitActor` -> @102553 icast
int_player -> @102632 `prop.playerTryToGrab(self, collected)` (base `prop_C`
implementation is a stub returning false -- `_fn.py prop playerTryToHold` likewise) ->
NOT collected -> **@99610 the default grab path**:

```
@99610: PUSH @96240;  @99615: PUSH @100415
@99792: IsValid(hit.PhysMat) ?            (no physmat -> skip the sound, continue grab)
@100003: obj<Default__lib_C>.physSound(hit.PhysMat, self, return, data)
@100067: IFNOT(physSound_return) POP      (datatable row MISS -> NO grab sound -- the
                                           foot_def fallback sets return=false)
@100249: loc := hit.Location + hit.Normal * 10
@100337: PlaySoundAtLocation(self, data.soft_30_1E6AA0474C29728BFC6F82AF9108D822,
         loc, rot, 0.5f, 1.0f, +0f, obj<att_default>, _, _)
   -> POP -> @100415: prop.lookAt(...) -> heavy/hulkMode gates (@101036) -> grabHandle...
```

So the native pick-up sound = **the grabbed prop's physical-material "soft" cue, at
the grab point, volume 0.5, pitch 1.0, attenuated by `att_default`** -- a WORLD sound,
but it only ever runs inside the local grabber's input chain, which a remote peer
never executes. Hence silence for remote grabs.

`Hold Object` (@14067 site; called when `playerTryToHold` declines) is the
inventory-collect path -- `_fn.py mainPlayer "Hold Object"`: canBePickedUp /
beginHoldingObject (base prop uber @473 = `POP->ret`, no-op) / `addEquip` +
`K2_DestroyActor` -- **contains NO sound dispatch at all**. The inventory pickup
sound lives in `putObjectInventory2` @659:
`PlaySound2D(self, obj<inventory_Cue>, 1.0f, 1.100000023841858f, +0f, _, _, true)` --
2D, local-only by design (SoundCue `/Game/audio/effects/inventory_Cue`). (Note: the
uber's other `inventory_Cue` site @47064 is the DREAM-WAKE handler, not pickup.)

### 2b. Receiver-side dispatch (GRAB-IN edge, holder known)

Every `prop_C` carries `physicsImpact` (`Ucomp_physicsImpact_C*` @ **0x0230**,
`prop.hpp:8`) whose `physSoundData` (`Fstruct_physSound` @ **0x00C8**,
`comp_physicsImpact.hpp:9`) is populated at prop init with EXACTLY the struct the
grab sound reads: prop uber @489 `physicsImpact.init()` -> uber @5480
`makePhysSound()` -> `setPhysSoundData()` stmt[1-3]:
`physSoundData = lib_C::physSound(getComponentPhysmat(), self)` (with per-prop
overrides: `physmat_override` @0x02C0, custom break/damage effects). Struct layout
(`struct_physSound.hpp`):

```
0x00 step_2      USoundBase*     0x10 soft_30     USoundBase*   <-- the grab cue
0x08 impact_4    USoundBase*     0x18 damaged_7 / 0x20 destroyed_9 / 0x28 scrape_12 ...
```

**Recommended call** on the GRAB-IN edge for a remote-held prop mirror `p`:

1. `soft = *(*(p + 0x0230) + 0x00C8 + 0x10)` (guard both derefs; null -> SKIP --
   native parity with the @100067 row-miss gate).
2. ProcessEvent `UGameplayStatics::PlaySoundAtLocation` on the GameplayStatics CDO
   (the weather_lightning / firefly_sync precedent):
   `WorldContextObject = puppet (or the prop mirror)`, `Sound = soft`,
   `Location = prop mirror's ActorLocation` (native anchors at the crosshair hit
   +Normal*10 -- the prop's own location is the faithful receiver-side stand-in),
   `VolumeMultiplier = 0.5`, `PitchMultiplier = 1.0`, `StartTime = 0`,
   **`AttenuationSettings = att_default`** (`SoundAttenuation
   /Game/audio/misc/att_default` -- resolve once via FindObject; passing null would
   play the cue UNattenuated = audible map-wide).

Alternative without the two raw offsets: ProcessEvent `lib_C::physSound` with a
PhysMat read from the component -- strictly worse (needs the physmat anyway and
re-runs the datatable lookup the component already cached).

For inventory-collects by a remote player (prop vanishes into their inventory): the
native cue is 2D/local-only; if we want remote audibility, same PlaySoundAtLocation
shape with `inventory_Cue`, vol 1.0, pitch 1.1, at the puppet head.

---

## 3. Throw sound

### 3a. Native mechanism

Input entry `simulateLMB(pressed)` -> uber @15509 -> `input_fire := true; BROADCAST
input_LMB` -> gates: @15554 `animation`, @15569 `playerInterface.selectingAction`,
@15657 `activeInterface` -> @15701 `IsValid(grabbing_actor)`:

```
@15744: grabbing_component.GetCenterOfMass()
@15807: traceThrow(false, com, com, ..., velocity_fromCamera, ...)
@15894: throwShit(grabbing_component, velocity_fromCamera)
@16005: prop.thrown(self)                        (base prop_C: uber @465 = POP->ret, no-op)
@16043: dropGrabObject()                         (3-stmt stub, no sound)
@16057: obj<Default__GameplayStatics>.PlaySound2D(self, obj<swing>,
        0.800000011920929f, 1.0499999523162842f, +0f, _, _, true)
```

Second variant -- throwing the HELD/equipped item (@16129, `holding_actor` +
`input_drop || isDrawThrowPath`): traceThrow -> `simulateDrop` -> `droppedItem.
K2_SetActorLocation(camera)` -> `thrown(self)` -> @16794 `throwShit(rootComp,
velocity_fromCamera)` -> **@16826 the identical `PlaySound2D(swing, 0.8, 1.05)`**.

- `swing` = **SoundWave `/Game/audio/effects/swing`** (mainPlayer import -1160).
- `PlaySound2D` = non-spatialized, fire-and-forget on the LOCAL audio device
  (bIsUISound=true here). **Other players never hear a throw even in principle** --
  the receiver must synthesize a world sound.
- `throwShit` (`_fn.py mainPlayer throwShit`) is physics only:
  `SetPhysicsLinearVelocity(NewVel - fallVeloc)` + random angular velocity scaled by
  mass -- no audio.
- Plain release / placed drop is natively SILENT: `simulateDrop` (6 stmts) and
  `dropGrabObject` (3 stmts) contain no sound; the G-drop site @13858
  (`simulateDrop(false, true, true)`) plays nothing.

### 3b. Receiver-side dispatch (GRAB-OUT edge + release velocity)

The wire discriminator: native throws are keyed by the LMB INPUT, which we don't
stream -- but a throw imparts `velocity_fromCamera` (hundreds of cm/s) while a drop
just releases. On the GRAB-OUT edge with release speed `|v| >=` a throw threshold
(pick empirically, ~400-500 cm/s; below it stay silent for native drop parity):

ProcessEvent `UGameplayStatics::PlaySoundAtLocation` (GameplayStatics CDO):
`WorldContextObject = puppet`, `Sound = swing` (resolve the SoundWave once,
FindObject `/Game/audio/effects/swing`), `Location = puppet head/hand`
(`GetHeadPosition()` is fine -- native is camera-anchored 2D),
`VolumeMultiplier = 0.8`, `PitchMultiplier = 1.05`, `StartTime = 0`,
**`AttenuationSettings = att_default`** (same null-attenuation warning as 2b).

---

## 4. Run loudness -- same call, two knobs, no separate mechanism

Confirmed from bytecode -- fixing item 1 fixes running too:

1. **Volume**: `step` computes `FClamp(char.CharacterMovement.MaxWalkSpeed /
   speedVolume, 0.5, 2.0) * Volume` (@3029/@4334/@4601). The sprint state enters via
   `MaxWalkSpeed`, written by `updateSpeed` @676:
   `MaxWalkSpeed := grabsHeavy ? MaxWalkSpeedCrouched : (input_run && !exhausted ?
   defSpeed * 2 * Lerp(1, 1.25, agility/100) : defSpeed)` (exhausted = health<=25 ||
   sleep<=15 || food<=10, @46-@363). Walk ~350 -> 0.875x; run ~700-875 -> 1.75-2.0x.
   (@725-@772 also selects anim `spd` 350/600 on the same `input_run` -- a sprinting
   source streams speed ~600+, comfortably over our existing 400 run boundary.)
2. **Cadence**: the accumulator multiplies distance by 0.75 while `input_run`
   (@70458) -- longer strides when sprinting. Our `Stride` already mirrors this
   (`kRunFactor=0.75` over `kRunSpeedCmS=400`).

Puppet gap after the Z fix: its parked CMC keeps the class-default `MaxWalkSpeed`
(updateSpeed never runs -- actor tick disabled), so every step sounds walk-loud.
Recommended polish: the receiver already owns the parked CMC's fields (it writes
`Velocity` + `MovementMode` per tick); additionally write
`CMC.MaxWalkSpeed := (curSpeed_ > kRunSpeedCmS ? 2.f : 1.f) * <local default>`
(read the default once from the local player / CDO). No separate run/breath loop
exists on mainPlayer -- the only run-gated audio is this volume scaling.

---

## 5. Feasibility verdict

**All four sounds are dispatchable with zero asset edits**:

- footsteps walk + run: ALREADY correctly dispatched by `votv_lib::CharacterStep`;
  blocked solely by the +85 actor-lift (fix in our receiver: zero the Mesh-slot
  RelLoc.Z at spawn -> `meshOffsetZ_` measures 0; everything else is untouched);
- pick-up: one component-field read (`physicsImpact @0x0230 -> physSoundData @0x00C8
  -> soft_30 +0x10`) + reflected `PlaySoundAtLocation` with `att_default`;
- throw: reflected `PlaySoundAtLocation` of the `swing` SoundWave at the puppet on
  the velocity-gated GRAB-OUT edge (natively 2D thrower-only -- synthesis is the only
  possible projection, not a workaround);
- the kerfur control case (uber @34864) proves the whole `lib_C::step` path works
  for controller-less coop-driven characters once the capsule sits at the true
  height.

## Repro commands

```
python research/bp_reflection/_fn.py lib_fixed step            # 1a/1b
python research/bp_reflection/_fn.py lib_fixed physSound       # (sizes need the MapConst patch; see _disasm for logic)
python research/bp_reflection/_fn.py mainPlayer "Hold Object" throwShit updateSpeed stepOn
python research/bp_reflection/_fn.py prop playerTryToHold beginHoldingObject thrown
python research/bp_reflection/_disasm.py comp_physicsImpact setPhysSoundData makePhysSound getComponentPhysmat
python research/bp_reflection/_fn.py kerfurOmega step          # -> uber @34864
grep "chain measure" Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/votv-coop-*.log
```

Key bytecode artifacts: `research/bp_reflection/{lib_fixed,mainPlayer,prop,
comp_physicsImpact,kerfurOmega,notify_kerfurStep}.json`,
`_mainplayer_uber_full.txt` (blocks @6505, @13799-@14186, @15509-@16897,
@26680, @46875-@47193, @60155-@60536, @70172-@71074, @99610-@101169,
@102553-@103632, @104455-@104806), `_kerfuromega_uber.txt` (@34864).
