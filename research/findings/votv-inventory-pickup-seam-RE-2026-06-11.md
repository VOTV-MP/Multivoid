# Inventory-pickup blip — the owner-side observable seam (2026-06-11)

**Question:** when a player collects an item INTO INVENTORY, `putObjectInventory2`
plays the Half-Life-style blip (`PlaySound2D(inventory_Cue, vol 1.0, pitch 1.1)`,
2D, collector-only). Find the ProcessEvent-observable seam that fires EXACTLY once
per inventory-collect on the collector's machine, so the owner can broadcast it and
peers can play the cue at the puppet.

**Answer (TL;DR): hook the blip itself.** The `@659` `PlaySound2D` inside
`putObjectInventory2` is dispatched `EX_Context(Default__GameplayStatics CDO) +
EX_FinalFunction(GameplayStatics.PlaySound2D)` — the SAME dispatch family as the
hands-on-proven PHC grab observers and the shipped BeginDeferred/FinishSpawning
observers. **POST-observe `UGameplayStatics::PlaySound2D`** and filter
`Sound == inventory_Cue && PitchMultiplier ≈ 1.1 && WorldContextObject == Local()`.
That predicate is true IFF the collect blip just played on this machine — a
game-wide census (below) shows zero other matches. Every collect path in the game
(26 call sites across 22 BP classes + 4 mainPlayer-uber sites) funnels through
`putObjectInventory2`, and the blip statement is the single success-path exit — so
the seam fires exactly once per successful collect and never on a failed one.

Method: kismet bytecode (`tools/bp_reflect.py` + `research/bp_reflection/_fn.py` /
`_cfg.py` drivers + raw UAssetAPI `$type` opcode reads), game-wide raw grep over
`research/pak_re/extracted/`, SDK signatures from
`Game_0.9.0n/.../CXXHeaderDump/{mainPlayer,Engine}.hpp`, and the project's measured
observer record (shipped features + captured logs). No source code modified.

---

## 1. `putObjectInventory2` end-to-end (task 1)

SDK signature (`CXXHeaderDump/mainPlayer.hpp:477`):

```cpp
void putObjectInventory2(class AActor* InputPin, bool noNotify, bool& return);
```

`InputPin` = the prop being collected. Call sites pass `noNotify=false` everywhere.
Disassembly (`_fn.py mainPlayer putObjectInventory2`, 47 stmts / 1294 bytes), with
the raw dispatch opcode of every call (read from `mainPlayer.json` `$type`):

```
@   0: PUSH @1291 (final RETURN)
@   5: noNotif := noNotify
@  24: PUSH @242
@  29: DoesImplementInterface(InputPin, int_player_C)        EX_CallMath
@  67: IFNOT -> POP (-> @242: not an int_player prop -> straight to collect)
@ 156: prop.canBeCollected(out ignore)                       EX_Context+EX_InterfaceContext+EX_LocalVirtualFunction   [BP, UNOBSERVABLE]
@ 202: IFNOT(ignore) POP -> @242
@ 212: (ignore==true) IFNOT(noNotif) -> @935 addHint("...") ; @226 return FALSE   <- refused-collect exit, NO blip
@ 242: lib_CDO.safeAsProp(InputPin, self, out isValid, out prop)   EX_Context(ObjectConst)+EX_LocalVirtualFunction    [BP, UNOBSERVABLE]
@ 306: InputPin.IsChildActor()                               EX_Context+EX_FinalFunction (native)
@ 348: child actor -> @362 JUMP @212 (hint/false)
@ 367: IFNOT(isValid prop) -> @829: InputPin.GetAttachParentActor() valid -> @212 (hint/false); else -> @1040
@ 381: prop.frozen ? -> @417 prop.awakeUnfreeze()            EX_Context+EX_LocalVirtualFunction                       [BP, UNOBSERVABLE]
@ 453: gamemode.propInventory.getInd()                       EX_Context+EX_LocalVirtualFunction                       [BP, UNOBSERVABLE]
@ 529: gamemode.playerContainer.propInventory.addObject(InputPin, ind, out return, out err)
                                                             EX_Context+EX_LocalVirtualFunction                       [BP, UNOBSERVABLE]
@ 645: IFNOT(addObject_return) -> @1160 (inventory FULL: hint @1190, return FALSE)  <- failed-collect exit, NO blip
@ 659: GameplayStatics_CDO.PlaySound2D(self, inventory_Cue, 1.0f, 1.100000023841858f, +0f, null, null, true)
                                                             EX_Context(EX_ObjectConst)+EX_FinalFunction (node -410)  [*** THE SEAM ***]
@ 719: InputPin.K2_DestroyActor()                            EX_Context+EX_VirtualFunction (native)                   <- the prop dies HERE
@ 755: gamemode.playerInterface.updateSlotInv()              EX_Context+EX_LocalVirtualFunction                       [BP, UNOBSERVABLE]
@ 813: return := TRUE -> @1291 RETURN
@1040: InputPin.K2_GetRootComponent().IsSimulatingPhysics()  EX_Context+EX_FinalFunction / +EX_VirtualFunction (native)
       simulating -> @1155 JUMP @453 (collect a non-Aprop_C physics actor); else -> @212 (hint/false)
```

Properties of the blip statement:

- **Single success-path exit.** `@659` is in one block, no loop reaches it twice;
  every refused/failed branch (`canBeCollected` ignore, child-actor, attached,
  non-simulating non-prop, `addObject` full) exits with `return=false` BEFORE it.
  Blip plays <=> collect succeeded <=> the prop was added + destroyed.
- `noNotify` does NOT gate the blip (it only suppresses the failure hint).
- The prop is destroyed at `@719`, immediately AFTER the blip — at PlaySound2D POST
  time the prop still exists (irrelevant for the recipe; the receiver plays at the
  puppet head, not the prop).

## 2. Why none of the BP-side calls are seams (task 2)

Every prop-side / cross-object BP call in the chain (`canBeCollected`,
`safeAsProp`, `awakeUnfreeze`, `getInd`, `addObject`, `updateSlotInv`, `addHint`,
and the callers' `playerTryToCollect` @13469/@13201, `ignoreSave` @81912, `asProp`
@82056, `playerTryToHold` @13703/@14005) is **`EX_Context +
EX_LocalVirtualFunction`** — the BP-script-callee form that routes
ProcessInternal-direct and NEVER reaches our ProcessEvent detour. This is the
playerGrabbed trap, IDA-traced + smoke-proven-zero in
`votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md` (§1.2: `execLocal*` ->
`CallFunction` sub_1414573C0 -> ProcessInternal sub_141302DC0; our detour is on
`UObject::ProcessEvent` 0x141465930 only, `game_thread.cpp` MinHook).

The native-callee calls in the chain (`IsChildActor`, `K2_GetRootComponent`,
`IsSimulatingPhysics`, `K2_DestroyActor`) ARE in the observable family (next
section) but are dispatched constantly game-wide for unrelated reasons — hooking
any of them would mean classifying every destruction/physics-query in the game.
The blip call is the only statement in the chain that is BOTH observable AND
semantically equal to the event we want.

`canBeCollected` is NOT a hover-prompt function: zero call sites in the mainPlayer
uber (the crosshair prompt is widget-driven); its only callers are
`putObjectInventory2` (@156) and prop-side overrides. Base `prop_C::canBeCollected`
= `ignore := false; return` (3 stmts) — collectable by default, overridden by
refuse-props.

## 3. Observability ground truth used (and the doc contradiction it settles)

Our detour: ONE MinHook on `UObject::ProcessEvent` (AOB,
`reflection.cpp` / `game_thread.cpp`); observers keyed on UFunction pointer.
Measured record in THIS binary:

| Dispatch form (raw `$type`) | Callee kind | Detour fires? | Evidence |
|---|---|---|---|
| `EX_Context + EX_FinalFunction` | native member/static | **YES** | `PHC.GrabComponentAtLocationWithRotation` (uber @94761, this exact form) — captured firing logs in `votv-physics-interaction-deep-re-2026-05-23.md:199-207` |
| `EX_Context + EX_VirtualFunction` | native by-name | **YES** | `PHC.ReleaseComponent` (uber @96019) — same captured logs; npc K2_DestroyActor PRE (NPC despawn sync works) |
| `EX_CallMath` | native static (GameplayStatics) | **YES** for `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` | every dumped BP spawn site is EX_CallMath (census over 147 dumps — incl. pineconeSpawner), yet npc-suppress client-cancel, npc_sync NPC mirror, weather_lightning mirror, M2 pinecone watcher (the smoke-observed world-origin artifact) and the FinishSpawningActor Q-menu watcher all demonstrably fire |
| `EX_Context + EX_LocalVirtualFunction` | BP script (cross-object/interface) | **NO** | `playerGrabbed` — IDA-traced + smoke-zero |
| `EX_LocalVirtualFunction` / `EX_LocalFinalFunction` (same-object) | BP script | **NO** | spawner `Init`, `runEvent`, `runTrigger` — smoke-zeros |

Settled contradiction: the firefly RE + events-catalog table claim "EX_CallMath
bypasses ProcessEvent" — that was **source-theory, never a measured zero** (the
firefly doc's wording is predictive: "would never fire"), and it is falsified as a
universal rule by the BeginDeferred record above (all those call sites ARE
EX_CallMath in the cooked bytecode). The only measured zeros are the
`EX_Local*` BP-script forms. Mechanical route for the native forms not re-pinned in
IDA here; the seam rests on the measured precedents (same form AND same class as
proven-firing hooks). Validation step for the implementer (2 min hands-on): register
the observer with an INFO log, collect one battery — expect exactly one line.

## 4. Every caller of `putObjectInventory2` (tasks 3+4 — what reaches the blip)

Game-wide census (raw grep over ALL extracted uassets for the FName + dump scan of
every hit): **no other engine path plays the collect blip; `putObjectInventory2` is
the single funnel.** All callers are `EX_LocalVirtualFunction` (BP->BP,
unobservable themselves — irrelevant, the blip inside is the seam).

mainPlayer uber sites (input mapping):

| Site | Entry / event | Condition |
|---|---|---|
| @13531 `putObjectInventory2(lookAtActor)` | `InpActEvt_drop` release (entry @12835; press = @14187 sets `input_drop` + hold-timer) — the **G key** | `hotkeyAction_swapR == true` (settings toggle "swap hold/collect"), nothing grabbed/held, crosshair on a prop, `playerTryToCollect` declined |
| @14153 `putObjectInventory2(grabbing_actor)` | same G chain | swapR variant while PHYSICS-GRABBING a prop |
| @82190 `putObjectInventory2(lookAtActor)` | **custom event `collectObject`** (entry @81721) | gates: `lookAtComponent.IsSimulatingPhysics`, `ignoreSave==false`, `asProp` valid, `propData.canHold==true` (else hint). No static BP caller exists game-wide (the cremator grep hit is its own `collectObjects`); dispatched dynamically (action-radial "Collect" / scripted) — caller-agnostic for us |
| @119289 `putObjectInventory2(FinishSpawningActor_RV)` | drop-equipped flow | unequip/hotbar move spawns the world prop then instantly re-collects it unless `drop_place || isRagdoll || drop_dontCollect` |

Other classes (all are "this interaction yields an item into your inventory", all
run inside the LOCAL player's input/interaction chains): `atm`, `groundHose`,
`growingPlant` (harvest), `hook`, `kerfurOmega::holdObject_kerf` (playable-kerfur
collect), `mainGamemode::RemoveEquipment` (unequip returns item to inventory),
`mainPlayer::addPropToPlayer` (scripted give), `prop_batteryCharger::putIn` (swap
out the charged battery), `prop_batts`, `prop_beebox`, `prop_box` (x2),
`prop_bucket`, `prop_butterchurn`, `prop_compostBucket`, `prop_container::extract`,
`prop_digcam`, `prop_drive`, `prop_driveRack::getDrive`, `prop_jar_honey::eat`,
`prop_torch`, `prop_vacuum`, `prop_vidcam::recharge`, `xmaslight`,
`ui_playerInventory` (x2 — TAB-UI collect actions).

**The silent sibling — `Hold Object` (take-in-HAND/equip, default G / R):** uber
@13765 (`grabbing_actor`) and @14067 (`lookAtActor`) when `hotkeyAction_swapR ==
false`, after `playerTryToHold` declines. Byte-verified: `Hold Object` and
`addEquip` contain ZERO sound dispatches. So for a battery/food/flashlight there
are two distinct verbs: **collect-to-inventory = blip** (always via
putObjectInventory2) and **take-in-hand = silent** (Hold Object); the seam captures
exactly the blip verb, by construction. (The E physics-grab is a third verb with
its own per-material sound — already mirrored by `prop_sound::PlayGrabSound`.)

## 5. False-positive analysis — every `inventory_Cue` reference in the game

Raw grep over ALL extracted assets: 10 referencing assets, fully classified:

| Source | Mechanism | Params | Observer sees it? |
|---|---|---|---|
| `mainPlayer::putObjectInventory2` @659 | PlaySound2D, WCO=**the collecting player** | vol 1.0 / pitch 1.1 | **YES — the target event** |
| `mainGamemode::putObjectInventory` @980 (legacy v1, own blip + K2_DestroyActor) | PlaySound2D, WCO=**the gamemode** | vol 1.0 / pitch 1.1 | matches Sound+pitch but **zero callers game-wide** (census: every caller calls v2) = dead code; additionally rejected by the `WCO==Local()` check |
| mainPlayer uber @47064 — DREAM-WAKE handler | PlaySound2D, WCO=player | vol 0.5 / **pitch 0.9** | rejected by the pitch gate |
| `customWall::fix` | PlaySound2D, WCO=the wall actor | vol 0.5 / **pitch 2.0** | rejected by pitch (and WCO) |
| `uicomp_playerInvSlot` / `uicomp_playerInvContainerSlot` | Button `PressedSlateSound` = inventory_Cue (Slate UI sound path) | n/a | **invisible** to ProcessEvent — TAB-UI slot clicks can never spam the wire |
| `prop_container` / `prop_dronesack` / `rope` | `Audio` component default `Sound` = inventory_Cue, played via UAudioComponent (native) | n/a | invisible |
| `npc_goreSlither_cheese` | alert/hurt AudioComponent templates | n/a | invisible |
| `kerfurOmegaV1_a2_pat` (anim) | anim-notify sound (native) | n/a | invisible |

So the predicate `Sound==inventory_Cue && WCO==Local() && 1.05f < pitch < 1.2f`
matches the collect blip and NOTHING else, game-wide. Correct silences (no blip,
no broadcast): refused collect (`canBeCollected` ignore -> hint), inventory full
(`addObject` false -> hint), Hold Object / addEquip (equip verb), plain E-grab,
drops. Note the broadcast mirrors EXACTLY what the owner hears — unequip-to-
inventory (`RemoveEquipment`, @119289) and machine-yield interactions
(charger/rack/ATM...) blip natively and will mirror; that is native parity, not FP.

## 6. The deliverable recipe (task 5)

**Owner side — register once (the host_spawn_watcher Install shape):**

1. `gsCls = R::FindClass(L"GameplayStatics")`;
   `fn = R::FindFunction(gsCls, L"PlaySound2D")` (SDK `Engine.hpp:11337`:
   `PlaySound2D(const UObject* WorldContextObject, USoundBase* Sound, float
   VolumeMultiplier, float PitchMultiplier, float StartTime, USoundConcurrency*
   ConcurrencySettings, AActor* OwningActor, bool bIsUISound)`).
2. Param offsets via `R::FindParamOffset(fn, L"WorldContextObject" / L"Sound" /
   L"PitchMultiplier")` (never hand-bake).
3. `GT::RegisterPostObserver(fn, OnPlaySound2DPost)`.

**Observer body (hot path: fires per BP 2D sound — keep it to pointer reads):**

```
sound = *(void**)(params + off_Sound);            if (!sound) return;
if (!R::NameEquals(sound, L"inventory_Cue")) return;     // allocation-free FName compare
pitch = *(float*)(params + off_Pitch);
if (pitch < 1.05f || pitch > 1.2f) return;        // excludes dream-wake 0.9 / customWall 2.0
wco = *(void**)(params + off_WCO);
if (wco != ue_wrap::engine::Local-player) return; // collector must be OUR local player
-> broadcast the reliable "inventory blip" event (sender slot is the identity; no
   prop payload needed -- the cue is collector-anchored; the prop's disappearance
   already rides the existing destroy sync from @719).
```

Thread note: this dispatch comes from input/UI chains = game thread, but keep the
`GT::IsGameThread()` guard shape if the body touches engine state (the
host_spawn_watcher pattern). Cost: 2-3 pointer reads + an FName compare on a
low-Hz UFunction — negligible.

**Collector locality / can a puppet fire it?** No. All 26 call sites execute
inside locally-dispatched chains (input events, local UI, the local player's
interaction handlers). The puppet is an unpossessed orphan (`GetController() ==
nullptr`) with actor tick disabled — it has no input stack, no UI, and nothing in
the game calls collect verbs on an arbitrary `mainPlayer_C` spontaneously; our mod
never reflects these calls at a puppet. The `WCO == Local()` check is belt and
braces (it also rejects the dead gamemode-v1 site whose WCO is the gamemode).

**Receiver side (already prescribed + existing infra):** on the blip event for
remote slot N, reflected `UGameplayStatics::PlaySoundAtLocation(WorldContext=
puppet, Sound=inventory_Cue, Location=puppet head (`GetHeadPosition()`),
VolumeMultiplier=1.0, PitchMultiplier=1.1, AttenuationSettings=att_default)` — the
exact `prop_sound.cpp` shape. The cue is resolvable on every peer without loading
concerns: it sits in `mainPlayer`'s import table (`/Game/audio/effects/
inventory_Cue`, SoundCue), so it is resident wherever a player exists; lazy
`FindObject` once, like the swing/att_default resolves. **No echo loop:** the
receiver dispatches `PlaySoundAtLocation` (a different UFunction), so the owner's
PlaySound2D observer never sees the mirrored play.

## 7. Repro commands

```
python research/bp_reflection/_fn.py mainPlayer putObjectInventory2      # section 1
python research/bp_reflection/_fn.py mainGamemode putObjectInventory    # the dead v1
python research/bp_reflection/_fn.py prop canBeCollected                # base impl
# raw opcode trees (the $type reads): inline python over mainPlayer.json, see the
#   statement indices: putObjectInventory2 stmt[8]=canBeCollected, [21]=addObject,
#   [23]=PlaySound2D (EX_Context+EX_FinalFunction node -410), [24]=K2_DestroyActor
grep -n "putObjectInventory2(" research/bp_reflection/_mainplayer_uber_full.txt   # 4 uber sites
# game-wide censuses:
grep -rl "inventory_Cue"       research/pak_re/extracted --include=*.uasset       # 10 assets
grep -rl "putObjectInventory"  research/pak_re/extracted --include=*.uasset       # 24 assets (all call v2)
grep -rl "collectObject"       research/pak_re/extracted --include=*.uasset       # mainPlayer + cremator(=collectObjects, false hit)
# proven-firing precedent for the seam's dispatch form:
#   research/findings/votv-physics-interaction-deep-re-2026-05-23.md:199-207 (PHC observers)
```

New dumps produced for this RE (gitignored as usual): `customWall`,
`npc_goreSlither_cheese`, `prop_container`, `prop_dronesack`, `rope`,
`uicomp_playerInvContainerSlot`, `uicomp_playerInvSlot`, `atm`, `groundHose`,
`hook`, `prop_batteryCharger`, `prop_batts`, `prop_beebox`, `prop_box`,
`prop_bucket`, `prop_butterchurn`, `prop_compostBucket`, `prop_digcam`,
`prop_drive`, `prop_driveRack`, `prop_jar_honey`, `prop_torch`, `prop_vacuum`,
`prop_vidcam`, `xmaslight`, `ui_playerInventory`, `cremator`.
