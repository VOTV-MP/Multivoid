# VOTV melee (LMB attack with held prop) — damage path RE

> Static RE only (kismet bytecode via `research/bp_reflection/*.json` + `_cfg.py` flow tracer +
> CXXHeaderDump). No probes run. Per [[lesson-ex-callmath-invisible-to-processevent]] /
> COOP_DISPATCH_VISIBILITY.md: **a static `$type` CANNOT prove PE-visibility either way** — every
> visibility claim below is tagged with its evidence class; live catch probes must gate any build.
> Import indices resolved per [[lesson-bp-json-grep-resolve-imports]] (Imports[-idx-1]).
> Tooling note: `research/pak_re/size_ubergraph.py` gained `EX_MapConst`/`EX_SetConst` size handling
> (comp_physicsImpact ubergraph previously untraceable); validated computed total == header
> ScriptBytecodeSize (13184 == 13184).

## melee damage path (RE 2026-07-11)

### 0. TL;DR

- LMB = the `fire` InputAction (`DefaultInput.ini`: `ActionName="fire",Key=LeftMouseButton`).
- ONE attacker-side damage roller: `mainPlayer.attack` (script fn, called EX_LocalVirtualFunction
  from the ubergraph). It line-traces the arm reach, computes `finalDamage = weaponData.damage *
  physmat multiplier`, then fires up to three INTERFACE messages at the trace hit:
  `int_player_C::playerHitWith` (to the HELD item), `int_player_C::damageByPlayer` (creature verb),
  `int_objects_C::addDamage` (prop verb). All three are `EX_Context + EX_LocalVirtualFunction`.
- **The mannequin is NOT a creature class.** `Aprop_mannequin_C : Aprop_C` (thin: UberGraphFrame +
  ReceiveDestroyed/ReceiveBeginPlay only, `prop_mannequin.hpp`). The night-walking behavior is
  driven by a separate director `AmannequinTp_C : Aactor_save_C`
  (`checkRender(TArray<Aprop_mannequin_C*>&, bool& canTp)`, `mannequinTp.hpp`). Melee vs a
  mannequin therefore takes the GENERIC PROP chain (b == a).
- Prop health lives on the component: `Ucomp_physicsImpact_C.health @ 0x238`, resistance in
  `Fstruct_breakableProp damageData @ 0x208`. The write is
  `VictoryFloatMinusEquals(health, damage/damageResistance)` — an `EX_CallMath` into
  `VictoryBPFunctionLibrary` (UPARAM(ref) in-place mutation).
- Character NPCs (e.g. `npc_zombie`, base `A00000000npc_C : ACharacter`) implement `addDamage`
  per-class; the zombie also decrements via `VictoryFloatMinusEquals(health, damageTaken)` →
  `died()`. `kerfurOmega` implements both `addDamage` and `damageByPlayer`.
- **UE's native damage framework is NOT used**: zero hits for
  `ApplyDamage|TakeDamage|AnyDamage|PointDamage` in mainPlayer.json + prop.json +
  comp_physicsImpact.json. There is no engine-native TakeDamage seam to Func-patch on this path.
- The end-of-chain destroy (`owner.K2_DestroyActor()` at health<=0) and the gib spawns
  (BeginDeferred/FinishSpawning EX_CallMath) land on ALREADY-SHIPPED Func seams
  (prop_lifecycle destroy seam, host_spawn_watcher).

### 1. Attack entry point on mainPlayer (how LMB routes)

`DefaultInput.ini` (pak `VotV/Config/DefaultInput.ini`):
`+ActionMappings=(ActionName="fire",...,Key=LeftMouseButton)` — verified from the extracted pak.

- **Press**: `InpActEvt_fire_K2Node_InputActionEvent_58` (mainPlayer export #291) →
  `ExecuteUbergraph_mainPlayer` entry `@110229` → `@15523 input_fire := true` →
  `@15534 BROADCAST input_LMB` (multicast delegate, EX_CallMulticastDelegate) → gate chain:
  - `@15554 animation` → ret if in a scripted animation
  - `@15569 gamemode.playerInterface.selectingAction && releaseEToUse` → ret
  - `@15657 IsValid(activeInterface)` → ret (screen interaction; the SEPARATE hardcoded
    `InpActEvt_LeftMouseButton_K2Node_InputKeyEvent_1` `@26680` handles the widget click via
    `WidgetInteraction.PressPointerKey`)
  - `@15701 IsValid(grabbing_actor)` → E-grab THROW branch (`traceThrow` + `throwShit` +
    `int_player_C::thrown` + `dropGrabObject`) — not melee, returns
  - `@16129 IsValid(holding_actor) && (input_drop || isDrawThrowPath)` → held-item THROW branch
    (`simulateDrop` + `throwShit`) — not melee, returns
- **Melee continuation `@16898`** (three PUSHed segments, in execution order):
  1. `@16913-17734`: if crosshair `hitResult` (the persistent lookAt trace instance var) blocks AND
     HitActor implements `int_objects_C` → `playerUsedOn(self, hitResult, HitComponent,
     holding_actor, holding_name)` (the generic "player clicked on you holding X" verb)
  2. `@17916-18030`: if `holding_actor` implements int_objects →
     `holding_actor.playerHandUse_LMB(self)` (smart-item in-hand verb — nailgun/physgun etc.)
  3. `@18069-18604`: if lookAt HitActor implements int_objects → `canHit(noHit)` `@18544`
     (base `prop.canHit`: `noHit := false`, i.e. hittable by default; subclasses opt out).
     `noHit==true` → stop. Else fall to the swing block `@271`:
     `IsValid(weaponData.montage)` + `reload <= 0` + `weaponData.attack == true` →
     **`@387 CallFunc_attack_ReturnValue := attack(CallFunc_attack_hit, CallFunc_attack_OutputPin)`**
     [EX_LocalVirtualFunction, self-call]. Post: `@429/@725` if `combat` OR the swing HIT →
     `@443 BROADCAST swingMelee`; hammer special-case (`saveSlot.hold[0]` name vs `'hammer'`);
     `@821 playFPAnim(montage...)`; `@925+` frozen-water cosmetic branch.
- **Release**: `InpActEvt_fire_..._59` → `@110261` → `@18621 input_fire := false` →
  `BROADCAST input_LMB` → clear `timeLMBTimerHandle` → if holding_actor int_objects →
  `@18786 playerHandRelease_LMB(self)`.
- Hold-repeat rides `input_fire` + latent Delay re-entries (`@670 Delay(0)` loop, `@169/@194`
  blocks re-checking `input_fire && !input_rotate` → `@271`); `delayLMB`/`delayLMBtimer`
  (`@19000/@19108`) are the hold-E-style `playerUsedOn_delay` lane, NOT the melee repeat.
- `simulateLMB(pressed)` (export #205, BlueprintCallable → uber `@15509`) is the game's own
  programmatic LMB entry — enters the exact same @15523/@18621 flow.

### 2. attack() — the damage roll (mainPlayer, 3934 bytecode bytes)

```
[@5]    finalDamage := weaponData.damage_2          (Fstruct_weapon of the HELD item; set at updateHold)
[@41]   arm(0, out start, out end, out rot)         (EX_LocalVirtualFunction, self — reach endpoints)
[@87]   LineTraceSingle(start, end, ...OutHit)      (EX_CallMath — the MELEE reach trace, own trace,
                                                     NOT the crosshair hitResult)
[@223]  icast<int_player_C>(holding_actor)          (EX_ObjToInterfaceCast)
[@302]    holding_actor.playerHitWith(OutHit, self)   [EX_Context + EX_LocalVirtualFunction]
                                                     — notifies the HELD prop (the rock) it hit something
[@760]  Array_Contains(weaponData.matEff, PhysMat)  → [@1156] finalDamage *= weaponData.matEffDmg[idx]
[@1693] icast<int_player_C>(OutHit.HitActor)
[@1772]   HitActor.damageByPlayer(self, OutHit, finalDamage)   [EX_Context + EX_LocalVirtualFunction]
                                                     — the CREATURE damage verb (int_player.hpp:27)
[@2000] icast<int_objects_C>(OutHit.HitActor)
[@2184]   HitActor.addDamage(self, finalDamage, OutHit, CameraFwd * weaponData.force, false)
                                                     [EX_Context + EX_LocalVirtualFunction]
                                                     — the PROP damage verb (int_objects.hpp:75)
[@2637] lib_C.physSound + [@2919] PlaySoundAtLocation      (cosmetic)
[@3799] HitComponent.AddImpulseAtLocation(fwd * min(mass,1) * force * lerp(1,2,str), loc)
                                                     (physics shove, EX_VirtualFunction on the comp)
```
Both `damageByPlayer` AND `addDamage` are sent unconditionally when the hit actor implements the
respective interface — an actor implementing both gets both (base `Aprop_C` implements both but its
`damageByPlayer` is a NO-OP, uber @466; only `addDamage` does work).

### 3. Prop target chain (this IS the mannequin chain)

```
prop.addDamage(actor, damage, hit, impact, skipSetting)        [stub -> ExecuteUbergraph_prop(1046)]
  @1088  physicsImpact.impactDamage(damage, hit, actor, impact/100)
                                                     [EX_Context + EX_LocalVirtualFunction, cross-object
                                                      call onto the Ucomp_physicsImpact_C component]
comp_physicsImpact.impactDamage(...)                 [stub -> ExecuteUbergraph_comp_physicsImpact(10719)]
  @10719 unbreakable       -> ret
  @10416 isFireDamage := false; health > 0 ?
  @10471 damage / damageData.damageResistance > 1 ?  (small hits fully absorbed)
  @10570 impactDmg := impact
  @10652 VictoryFloatMinusEquals(health, damage/damageResistance, FloatOut)
         *** THE PROP HEALTH WRITE *** health @ 0x238 on Ucomp_physicsImpact_C (comp_physicsImpact.hpp:19),
         damageData = Fstruct_breakableProp @ 0x208 (hpp:18). EX_CallMath ->
         VictoryBPFunctionLibrary::VictoryFloatMinusEquals (import -117 chain), UPARAM(ref) in-place.
  @9697  owner icast int_objects -> owner.receivedPhyiscsDamage(damage/resist, hit)
         [EX_Context + EX_LocalVirtualFunction; base prop impl = NO-OP (uber @422); subclasses hook it]
  @9886  FloatOut <= 0 ?  -> BREAK cascade:
    @8387 destroyed sound  @8584 destroy particles (+size/vel InstanceParameters @7248..8219)
    @8761 damager-actor velocity preserve
    @9446 save_main.stats.object_broken += 1
    @9148 isFireDamage -> owner int_player broken_fire()
    @2559 GIB LOOP: per damageData.propGibs[i]: gamemode.spawnPropThroughGamemode(gibClass, xf, 1)
          + random angular/linear velocity  (spawnPropThroughGamemode internally BeginDeferred/Finish
          -> lands on the host_spawn_watcher Func seam class)
    @9288 owner.K2_DestroyActor()   [EX_Context + EX_VirtualFunction — PE-invisible BP-issued destroy;
          this is EXACTLY the class the shipped v106 prop_lifecycle UFunction::Func destroy seam
          catches (dispatch map row [V])]
```
Mannequin specifics: `Aprop_mannequin_C` adds no damage overrides (hpp: only
ReceiveDestroyed/ReceiveBeginPlay) — health/damageData values are per-instance SCS/CDO data (not
verified statically). Its death reaction hangs off `ReceiveDestroyed`.

### 4. Creature target chain (real Characters; zombie exemplar)

Base `A00000000npc_C : ACharacter` is bare (no health). Each NPC BP implements the verbs:

```
npc_zombie.addDamage(...)            [stub -> ExecuteUbergraph_npc_zombie(20166)]
  @20166 isDead -> ret
  @20181 damageTaken := damage; damageHit := hit; zombie_damage_Cue.Play
  @20291 attacking -> ret (no damage while it attacks)
  @21057 headArmor > 0 ? -> @21277 hitHeadArmor(loc);
         @21300 VictoryFloatMinusEquals(headArmor, damageTaken) -> <=0 -> headArmorDestroyed()
  @21410 else: VictoryFloatMinusEquals(health, damageTaken)   *** CREATURE HEALTH WRITE ***
         @21452 FloatOut < 0 -> @21501 died() -> bone-gib loop @6505
         (Prop_To_Object + BeginDeferredActorSpawnFromClass @6937 / FinishSpawningActor @7195,
          EX_CallMath -> Func-seam class)
```
`kerfurOmega` implements `damageByPlayer` (stub → uber 16899) AND `addDamage`. So "creature damage"
is per-class BP; there is NO shared native health system.

### 5. Dispatch type + PE-visibility per link (static; honest)

| # | Link | $type (this dump) | Visibility assessment | Evidence class |
|---|---|---|---|---|
| 1 | `InpActEvt_fire` press/release | native input -> UFunction | **VISIBLE** (PRE+POST) | [V-class] dispatch map rows for `InpActEvt_use`/`InpActEvt_drop` live-verified; `fire` itself not live-tagged yet |
| 2 | uber -> `attack` / `canHit` / `punch` | `EX_LocalVirtualFunction` (self) | INVISIBLE to PE **and** to a Func patch expected (ProcessLocalScriptFunction inline, map row "EX_Local* invisible to BOTH") | **$type [V] bytecode-confirmed 2026-07-14** (parsed `attack` ScriptBytecode, opcode #0 `arm` = EX_LocalVirtualFunction); PE-vs-Func visibility still live-gated per [[lesson-script-fn-invisible-to-func-patch]] |
| 3 | `attack` -> `playerHitWith` / `damageByPlayer` / `addDamage` | `EX_Context` + `EX_LocalVirtualFunction` (interface msg, cross-object) | same as #2 — INVISIBLE expected. The map already live-tags this exact SHAPE: device verbs (row "every screen/panel verb ... EX_LocalVirtualFunction (measured) INVISIBLE") and pile verbs (`playerGrabbed` etc.) | **$type [V] bytecode-confirmed 2026-07-14**: parsed `attack` ScriptBytecode — `playerHitWith` (op #2), `damageByPlayer` (op #12), `addDamage` (op #16) ALL `EX_LocalVirtualFunction`; the ONLY native calls inside `attack` are utilities (`LineTraceSingle`/`BreakHitResult`/`Array_*`/math, `EX_CallMath`+`EX_FinalFunction`) + physics/sound cosmetics — **no damage callee is native**. Parse self-checked vs §2 known-positive chain. So melee's damage layer is a VM-substrate (GNatives) consumer, NOT covered by the existing Func-patch |
| 4 | prop uber -> `physicsImpact.impactDamage` | `EX_Context` + `EX_LocalVirtualFunction` | same as #3 | [RD class] |
| 5 | `impactDamage`/zombie -> `VictoryFloatMinusEquals` | `EX_CallMath` (VictoryBPFunctionLibrary, import resolved -117) | PE-INVISIBLE; **Func-VISIBLE expected** — EX_CallMath is live-PROVEN caught by the `ufunction_hook` Func patch (K2_SetTimer census 2026-07-10, map row [V]) | [V-class mechanism, this fn unprobed] |
| 6 | health<=0 -> `owner.K2_DestroyActor()` | `EX_Context` + `EX_VirtualFunction` | PE-INVISIBLE, **Func-VISIBLE [V]** — the shipped prop_lifecycle destroy seam catches BP-issued K2_DestroyActor (live 2026-07-07) | [V] |
| 7 | gib/bone spawns | `EX_CallMath` BeginDeferred/FinishSpawning | Func-VISIBLE | [V-class rows 87/91] |
| 8 | `BROADCAST swingMelee` (@443) / `input_LMB` (@15534/@18632) | `EX_CallMulticastDelegate` | each BOUND listener is dispatched via ProcessDelegate -> **ProcessEvent -> VISIBLE per listener**; in-game binder precedent: `npc_arirFollower` EX_AddMulticastDelegate on `swingMelee`, `transformerMGPanel` on `input_LMB` | [RD engine + map "multicast Broadcast VISIBLE" class; unbind-from-C++ UNPROVEN [[lesson-bp-delegate-unbind-unproven-capability]]] |
| 9 | engine damage API (`TakeDamage`/`ApplyDamage`) | — | **ABSENT from the whole chain** (0 grep hits in the 3 assets) — no engine-native seam exists here | [V static, decisive for absence] |

### 6. Where health actually mutates

- **Props (incl. mannequin):** `Ucomp_physicsImpact_C.health` (float @ 0x238). Written by
  `VictoryFloatMinusEquals(health, damage/damageData.damageResistance)` at
  `ExecuteUbergraph_comp_physicsImpact @10652`, inside the `impactDamage` handler. Same library
  call also used by `fireDamage`/`exploded` handlers elsewhere in the same ubergraph (9 references
  in comp_physicsImpact.json).
- **Creatures:** per-class instance floats (zombie: `health`, plus `headArmor`), written by the
  same `VictoryFloatMinusEquals` (zombie uber @21415 / @21300) inside the class's `addDamage`
  handler; death via the class's `died()`.

### 7. Coop seam candidates (client-hits -> host-applies damage-intent lane)

1. **Attacker intent edge: PE observer on `InpActEvt_fire`** (press; POST). Evidence STRONG
   ([V]-class InpActEvt rows). Gaps: fires for throw/UI paths too; carries no target/damage —
   the lane must re-derive them (read `holding_actor` + its weaponData and either re-run the arm
   trace or read the uber persistent-frame `CallFunc_attack_hit`/`CallFunc_attack_OutputPin`
   after dispatch — frame-read is fragile). Best used as the TRIGGER, paired with own trace.
2. **Attacker hit edge: bind a handler to the `swingMelee` multicast delegate** (fires exactly on
   combat-mode swings OR any swing that HIT — the interesting subset). Broadcast dispatches every
   bound listener through ProcessEvent -> our detour sees it. Evidence MEDIUM: EX_CallMulticastDelegate
   confirmed @443; two shipped BPs already bind these delegates (so the mechanism demonstrably
   works in-game); BUT our C++-side bind of a BP dynamic delegate is an unproven capability
   ([[lesson-bp-delegate-unbind-unproven-capability]] — prove bind/unbind FIRST, build gate).
   Delegate carries no params — still edge-only; params via `hitResult`/holding read at fire time.
3. **Target-side mutation seam: `UFunction::Func` patch on
   `VictoryBPFunctionLibrary::VictoryFloatMinusEquals`** — the single choke point where BOTH prop
   health AND creature health decrement. Mechanism live-proven for EX_CallMath (K2_SetTimer census,
   [V]); the POST callback's `FFrame::Object` = the CALLING object (comp_physicsImpact instance /
   npc actor) = free target attribution. Caveats: (a) generic library fn — referenced by 34/278
   dumped BPs; run a live rate census FIRST (probe-don't-guess) though it is event-driven, not
   per-tick by design; (b) it is an OBSERVE seam (post-mutation), fit for host-authority
   telemetry/confirmation, not for cancelling a client-local application.
4. **Host APPLY verb: reflected ProcessEvent dispatch of `addDamage` (props/mannequin) or
   `damageByPlayer` (creatures implementing it) on the host's copy** — outbound reflected calls
   are always dispatchable by us (precedent: alarm `runTrigger`, map row [V]). Signatures:
   `addDamage(AActor* Actor, float Damage, FHitResult Hit, FVector impact, bool skipSetting)`
   (int_objects.hpp:75), `damageByPlayer(AmainPlayer_C* Player, FHitResult Hit, float Damage)`
   (int_player.hpp:27). Host-side apply then re-uses the ALREADY-SHIPPED destroy + spawn seams to
   fan out the consequences (K2_DestroyActor Func seam; gib spawns via spawn watcher).
   Note the Player param: a client's swing applied host-side would pass the HOST's mainPlayer (or
   null) — check per-target whether the param is read (zombie uses it for focus/aggro).

Recommended shape: client peer = trigger (1) + own damage roll mirror of `attack`'s math
(weaponData.damage * matEffDmg + force impulse) -> damage-intent packet (target eid, finalDamage,
hit essentials, impulse) -> host applies via (4); (3) as the later authority/anti-divergence seam.
Pre-build gates per the probe rule: live catch probe proving #2/#3 fire and #2's bind capability;
and decide the client-local suppression story (client's own `attack` still applies damage locally —
the same double-apply class as every mirrored verb; kill at the SOURCE by suppressing client-local
apply only after the host echo, or accept client-predicted apply + host reconcile).

### Sources

- `research/bp_reflection/mainPlayer.json` (exports #97/#224-225/#291-292 InpActEvt_fire, #341 punch,
  #415 canHit, attack fn; ExecuteUbergraph_mainPlayer offsets cited inline)
- `research/bp_reflection/prop.json` (addDamage stub -> uber 1046; damageByPlayer no-op @466;
  receivedPhyiscsDamage no-op @422; impactDamageCPP -> uber 1994 same physicsImpact.impactDamage call)
- `research/bp_reflection/comp_physicsImpact.json` (impactDamage -> uber 10719; health write @10652;
  destroy cascade @8387-@9288)
- `research/bp_reflection/npc_zombie.json` (addDamage -> uber 20166; health write @21415)
- CXXHeaderDump: `comp_physicsImpact.hpp` (health @0x238, damageData @0x208), `prop_mannequin.hpp`,
  `mannequinTp.hpp`, `int_objects.hpp`, `int_player.hpp`, `00000000npc.hpp`, `prop.hpp:186`
- pak `VotV/Config/DefaultInput.ini` (fire = LeftMouseButton)
- Flow tracing: `research/bp_reflection/_cfg.py` (+ the size_ubergraph.py EX_MapConst fix, validated
  against ScriptBytecodeSize)
