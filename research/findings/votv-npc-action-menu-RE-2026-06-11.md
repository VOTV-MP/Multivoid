# VOTV NPC action menu (radial/list) — RE + coop mirror-sync seam design

Date: 2026-06-11. READ-ONLY RE pass for syncing NPC interaction-menu ACTIONS
(pet / follow / idle / patrol / turn off / take object / ...) across coop peers.

Sources: cooked-BP kismet disassembly via `research/bp_reflection/` tooling
(`_fn.py` / `_cfg.py` over kismet-analyzer JSON; new dumps generated this pass:
`p_kerfus.json`, `ui_UI.json`, `prop_kerfurOmega.json`,
`enum_interactionActions.json`), the CXXHeaderDump SDK
(`Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/*.hpp`), and the
coop sources (`src/votv-coop/`). Observability claims rest on the SETTLED
measured table in `research/findings/votv-inventory-pickup-seam-RE-2026-06-11.md`
section 3 (one MinHook on `UObject::ProcessEvent`; `EX_Local*` BP-script forms
are the only measured zeros; EX_CallMath / EX_FinalFunction / EX_VirtualFunction
native forms are measured-firing).

---

## 0. Executive summary

- The action menu is a TWO-CHANNEL interface protocol on `int_objects_C`
  (NOT `int_player_C`): `getActionOptions` builds the option list, then either
  `actionOptionIndex(Player, Hit, enum_interactionActions, Component)` (ENUM
  channel) or `actionName(Player, Hit, FString)` (NAME channel) dispatches the
  chosen action onto the target actor.
- **kerfurOmega uses the NAME channel** (10 string options; its enum-channel
  handler is a literal no-op). **p_kerfus (wheeled kerfus) and
  prop_kerfurOmega (the turned-off kerfur prop) use the ENUM channel.**
- **Every menu dispatch is `EX_Context + EX_LocalVirtualFunction` = BP-script
  interface dispatch = NOT observable by our ProcessEvent detour** (same
  measured-zero family as `playerGrabbed`, IDA-traced + smoke-zero). There is
  no PE-visible "action happened" choke point on either the caller or the
  target side.
- The workable seams are: (1) **poll-diff the 1-byte `State` field** on NPC
  mirrors (the menu's state verbs write it synchronously; the appliance-sync
  precedent), (2) **the PE-visible native calls INSIDE the handlers**
  (`BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` / `K2_DestroyActor`
  — all measured-firing; they carry `WorldContextObject == the NPC` so
  mirror-originated births/deaths are exactly attributable), and (3) the
  host-side **apply is a reflected ProcessEvent call of the SAME BP entry
  (`actionName` / `actionOptionIndex`)** on the authoritative actor, which runs
  the real handler with the real gates.
- Recommended protocol: `NpcAction` reliable (next free `ReliableKind = 49`,
  protocol v62) client->host with host relay for cosmetic replay, plus a
  **zero-growth `npcState` byte folded into `EntityPoseSnapshot`'s existing
  `_pad[3]`** (host-authoritative State stream; keeps client menus + poll-diff
  baseline correct).

---

## 1. The menu plumbing, end to end

### 1.1 The interface: `int_objects_C` (not `int_player_C`)

`CXXHeaderDump/int_objects.hpp`:

```
:59  void actionOptionIndex(AmainPlayer_C* Player, FHitResult Hit,
                            TEnumAsByte<enum_interactionActions::Type> Action,
                            UPrimitiveComponent* lookAtComponent);
:60  void getActionOptions(AmainPlayer_C* Player, UPrimitiveComponent* Component,
                           AActor* Actor, uint8 numberIn,
                           TArray<FString>& Options,
                           TArray<TEnumAsByte<enum_interactionActions::Type>>& options_enum,
                           TArray<FText>& optionsNamesOverlay,
                           uint8& Number, float& lookAtCenter);
:46  void ActionName(AmainPlayer_C* Player, FHitResult Hit, FString Name);
:36  void skipRadial(bool& Skip);
```

`int_player_C` (`int_player.hpp`) is the separate grab/use/lookAt interface
(`player_use` at :38, `lookAt` at :37, `kicked` at :33). Both interfaces are
implemented per-class by each interactable; `00000000npc.functions.txt` (npc_C
zombie base) has NO action functions — the menu is per-class, and among NPCs
only the kerfur family implements it.

### 1.2 `enum_interactionActions` — the ENUM channel vocabulary

Cooked names are `NewEnumeratorN`; the DISPLAY names live in the enum asset's
`DisplayNameMap` (extracted from `enum_interactionActions.json`, Exports[0].Data):

| value | label | | value | label |
|---|---|---|---|---|
| 0 | Grab | | 8 | **Activate** |
| 1 | Hold | | 9 | Sit |
| 2 | Collect | | 10 | Open |
| 3 | Put | | 11 | Close |
| 4 | **Use** | | 12 | Equip |
| 5 | Toggle | | 13 | Create |
| 6 | **Pat** | | 14 | Edit |
| 7 | Take | | | |

The UI renders enum options through
`GetEnumeratorUserFriendlyName(enum_interactionActions, value)`
(`ui_UI.buildActions` @2012).

### 1.3 Building the list — `mainPlayer.buildActionList` (1350 bytes)

`research/bp_reflection/_fn.py mainPlayer buildActionList`:

- @21: `lib_C::safeAsProp(lookAtActor)`; for a prop, `a := frozen || sleep`
  (@99); non-props (kerfurOmega) take the @1335 path, `a := false`.
- @201: `icast(lookAtActor)` to int_objects; @280:
  **`getActionOptions(self, lookAtComponent, lookAtActor, lookAtVerify, ...)`
  dispatched ONTO THE TARGET** (cross-object interface call).
- @634-838: clamps/resets `actionIndex` (`>= max_actionIndex` or the
  `settings.actionMenuIndex` option resets to 0).
- @962: `gamemode.playerInterface.buildActions(options_enum, options,
  optionsNamesOverlay, IsSimulatingPhysics||a, lookAtActor, lookAtComponent)` —
  hands both channels to the HUD widget.
- @1084: optional `lookatBox.SetBoxExtent(lookAtCenter)` (aim-assist box).

### 1.4 The HUD widget — `ui_UI.buildActions` (2470 bytes)

`ui_UI` is the `gamemode.playerInterface` widget (`ui_UI.hpp`; the only class
besides implementors that references `buildActions`).

- @97-153: `isNames := actions_names.IsValidIndex(0)` — **the NAME channel WINS
  whenever the target returned any strings**.
- NAME mode @186+: `names := actions_names`; builds one
  `ui_objectActionButton_C` per string (`SetStringPropertyByName(name)`).
- ENUM mode @1534+: `actions := actions_enums`; label =
  `GetEnumeratorUserFriendlyName` (@2012).
- @1079: `gamemode.mainPlayer.actionListAnimation(open)` (pure UI).

Widget fields (ui_UI.hpp): `actionSlots @0x0598`, `actions @0x05A8` (enum
TArray), `Names @0x05D8` (FString TArray), `isNames @0x05E8`,
`selectingAction @0x0656`.

### 1.5 Selection — `setActionIndex` / `fixActionIndex` / `selectedAction`

- `setActionIndex(int actionIndex)` (49 bytes): writes the field unless
  `altUse` is set (@0: `IFNOT(altUse) JUMP @19`; @19 assigns). mainPlayer
  fields: `actionIndex @0x0A98`, `max_actionIndex @0x0A9C` (mainPlayer.hpp:177-178).
- `fixActionIndex` (122 bytes): `actionIndex := (actionIndex + max) % max`.
- `selectedAction(...)` (1635 bytes) resolves the CURRENT pick:
  - @79: re-validates `lookAt` on the target (int_player icast).
  - @214: if `playerInterface.actions` non-empty -> ENUM result:
    `Output1 := actions[actionIndex]` (@640-743), `isSingle :=
    (len(actions)==1 || len(names)==1)`.
  - @959: else if `playerInterface.names` non-empty -> NAME result:
    `asName := names[actionIndex]` (@1385-1508), `Output1 := b0`.

### 1.6 Dispatch — `mainPlayer.useSelectedAction` (1128 bytes) — THE seam functions

```
@5:    selectedAction(Output1, asName, return, isSingle)
@60:   IFNOT(return) -> @500: buildActionList()   (stale menu -> rebuild)
@74:   IF gamemode.playerInterface.isNames:
@304:    icast(hitResult.HitActor) -> int_objects
@383:    >>> actionName(self, hitResult, asName) <<<        [NAME channel]
@439:    PlaySound2D('use')
       ELSE:
@748:    icast(hitResult.HitActor) -> int_objects
@999:    >>> actionOptionIndex(self, hitResult, Output1, hitResult.HitComponent) <<<  [ENUM channel]
@1064:   PlaySound2D('use')
```

Callers of `useSelectedAction` (mainPlayer ubergraph, `_mainplayer_uber_full.txt`):
@15642 (gated on `playerInterface.selectingAction` + `releaseEToUse` — the E
release while the list is open), @89512 / @89609 (`releaseEToUse` /
`isActionsOne` immediate-fire), @90369 (the hold-delay confirm chain @89970 ->
@90029 `selectedAction` -> isSingle), @114977. The plain E-press fallback path
is `useAction` (`_useaction_full.txt`): when the hit actor implements
int_player and nothing menu-ish applies it dispatches
**`player_use(self, hitResult)`** (@3472) and/or `pickupObject(hitResult)`
(@1645/@2620 for physics bodies). mainPlayer state fields used: `HitResult
@0x0744`, `lookAtComponent @0x0840`, `lookAtActor @0x0AA0`, `lookAtVerify
@0x0C69`, `altUse @0x0CEF`, `releaseEToUse @0x0E88`.

### 1.7 PE-visibility of the chain (the decisive facts)

Raw `$type` dump of the dispatch nodes (mainPlayer.json, this pass):

```
useSelectedAction: EX_LocalVirtualFunction VirtualFunctionName=actionName
useSelectedAction: EX_LocalVirtualFunction VirtualFunctionName=actionOptionIndex
buildActionList:   EX_LocalVirtualFunction VirtualFunctionName=getActionOptions
```

And kerfurOmega's internal verbs (kerfurOmega.json, scan over the uber):
`move`, `holdObject_kerf`, `makeMeow`, `dropKerfurProp`, `startKill` are all
self-dispatched `EX_LocalVirtualFunction`.

Against the measured observability table
(`votv-inventory-pickup-seam-RE-2026-06-11.md` section 3, ONE MinHook on
`UObject::ProcessEvent` 0x141465930):

| call | opcode form | our detour sees it? |
|---|---|---|
| `getActionOptions` / `actionOptionIndex` / `actionName` (player->target) | EX_Context + EX_LocalVirtualFunction (BP script) | **NO** (measured-zero family; `playerGrabbed` precedent IDA-traced + smoke-zero) |
| `useSelectedAction` / `buildActionList` / `selectedAction` (same-object) | EX_LocalFinalFunction / uber | **NO** |
| `InpActEvt_use_K2Node_InputActionEvent_41` (engine input -> mainPlayer) | engine ProcessEvent | **YES** (interactable_sync already PRE+POST-observes it — interactable_sync.cpp:264-274) |
| `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` (inside handlers) | EX_CallMath | **YES** (measured: npc-suppress cancel, npc_sync mirror, M2 watcher all fire on these EX_CallMath sites) |
| `K2_DestroyActor` (inside handlers) | EX_Context + EX_VirtualFunction (native by-name) | **YES** (npc destroy-sync PRE observer works in production) |
| `CreateProxyObjectForPlayMontage` (pat) | EX_CallMath | expected-YES (same form+family as measured BeginDeferred; **not individually measured — 2-min validation: register an INFO-log POST observer, pat a kerfur, expect one line**) |
| `CreateMoveToProxyObject` (move) | EX_CallMath | expected-YES (same caveat) |

**Conclusion: detect at native sub-calls + state polling; never at the
interface dispatch.**

---

## 2. The action sets

### 2.1 kerfurOmega (`AkerfurOmega_C : ACharacter` — the walking Kerfur-O; NPC pipeline)

`getActionOptions` (614 bytes): options_enum is set to an EMPTY array (@509
`options_enum := arr[0]`) — **pure NAME channel**. String sets (extracted from
the EX_SetArray literals):

- if `State in {b3,b4,b5}` (busy on a task): `['Turn_off']` + `'Equipment'`
  appended (@414 Array_Add) -> 2 options.
- else: `['Turn_off','Follow','Idle','Patrol','Fix_servers','Get_reports',
  'Fix_transformers','Take_object','Pat']` + `'Equipment'` -> 10 options.

`actionOptionIndex` exists but stubs to uber @16860 = bare `POP->ret` — a
**no-op** (so even sandbox-menu enum dispatches do nothing on a kerfurOmega).

`actionName` stubs to uber **@20350**. The switch (case-insensitive
`NotEqual_StriStri`) with full handler map — kerfurOmega field offsets from
kerfurOmega.hpp: `State @0x05C8` (TEnumAsByte<enum_kerfurCommand>, 0..6),
`kill @0x05E0`, `Animation @0x0678`, `holdObject @0x0680`, `sentient @0x07D8`,
`meow @0x0828`, `dropProp @0x0850` (TSubclassOf<Aprop_kerfurOmega_C>):

| string | handler @ | effect (bytecode-verified) |
|---|---|---|
| (entry gate) | @20350 | `IFNOT(kill) JUMP @20365`; if `kill` (murder mode) -> @20364 ret: ALL actions ignored |
| `turn_off` | @21844 | `dropKerfurProp()` -> see below: spawns `dropProp` prop + `K2_DestroyActor()` self |
| `follow` | @21808 | busy-gate (State in {3,4,5} -> ret @21807) else `State := b0; move(false)` |
| `idle` | @21588 | gate; `State := b1; move(false)` |
| `patrol` | @21368 | gate; `State := b2; move(false)` |
| `fix_servers` | @21134 | gate; `State := b3; move(false); dropObject()` |
| `get_reports` | @22058 | gate; `State := b4; move(false); dropObject()` |
| `fix_transformers` | @22292 | gate; `State := b5; move(false); dropObject()` |
| `take_object` | @22526 | gate; `holdObject_kerf(player, player.holding_actor)` (holding_actor @0x0A20 on mainPlayer) |
| `pat` | @22765 | gate; `Animation := true`; `CreateProxyObjectForPlayMontage(Mesh, kerfurOmegaV1_Skeleton_Montage, 1.0, 0, 'pat')` + 5 end-delegates (@22880-23159, the `Animation:=false` resetters live at @2502/@7915/@15398); `CharacterMovement.StopActiveMovement()` @23200; then JUMP **@7198**: `makeMeow()` (the meow SFX) + `meow += 0.02` + retrigger 0.15s; if `meow > 1.0` (@7305) -> `lib_C::getMainPlayer(...).ragdollMode(true,false,false)` @7395 — **the over-pet easter egg ragdolls the LOCAL mainPlayer** |
| `kill` | @21859 | `startKill()` (murder activation @23375+: `kill:=true`, `lib_C::setEvent`, face `angy:=1.0`). NOT in any base option list — **UNKNOWN which variant/flow offers the string** (murderKerfur itself is a separate `Aprop_C`; subclass `getActionOptions` overrides not exhaustively audited) |
| `equipment` | @23241 | `gamemode.objectViewer.assign(self, player, widget, rt, viewer)`; `upgradeUI := widget; viewer := viewer` — opens the accessory/upgrade 3D-viewer UI on the INTERACTOR'S screen. No busy-gate. |

`dropKerfurProp` (906 bytes, named fn):
- @5: `IFNOT(sentient) JUMP @57`; if `sentient` -> `Audio.Activate(true)` and
  RETURN (the sentient kerfur refuses to be turned off — no despawn).
- @62: if `hasFloppy` -> BeginDeferred(`lib_C::floppyFromType` class,
  WCO=self) + SetArrayPropertyByName('data') + FinishSpawningActor — drops the
  carried floppy as a prop.
- @388-768: `BeginDeferredActorSpawnFromClass(self, dropProp, transform)` +
  `FinishSpawningActor` -> cast `prop_kerfurOmega_C`, copies `sentient`
  (@847); @888 **`K2_DestroyActor()`** (spawn happens BEFORE the self-destroy).

`move(false)` (uber @19619): `IFNOT(Animation)` -> @19634 checks `server`
validity / `remoteControl` -> `CharacterMovement.StopMovementImmediately()`
(@19691) / movement bookkeeping. Benign on a tick-disabled mirror.

`holdObject_kerf` (4851 bytes, take_object): icast holdObject ->
`setPropProps(false,false,false,false)`; int_save `getData` -> BeginDeferred a
COPY of the held object's class (WCO=self, @416) + `loadData` + (one branch)
`player.putObjectInventory2(...)` + conditional `K2_DestroyActor` of the copy
(@860) — a serialize/teleport-into-kerfur-hands flow that spawns and destroys
actors. Multi-actor mutation; the most complex action.

### 2.2 p_kerfus (`Ap_kerfus_C : Aprop_corded_C` — the wheeled kerfus; PROP pipeline)

`getActionOptions` (320 bytes): **ENUM channel** —
`active==true -> options_enum = [8, 4, 6]` (Activate, Use, Pat);
`active==false -> [8]` (Activate). Strings empty. Field `Active @0x0460`
(p_kerfus.hpp:30), `possessLoc @0x04A4`, `energy @0x04C0`.

`actionOptionIndex` -> uber @9272 (traced):
- @9010 gate: `possessLoc == vec(...)` (the not-possessed sentinel) else ret.
- `b4` (Use) -> @8717: `IFNOT(active) ret; findBrokenServer()` (dispatch to fix).
- `b6` (Pat) -> @9247: `IFNOT(active) ret; meowAnim()` (cosmetic meow/anim).
- `b8` (Activate) -> @9198: `IF energy > 0: active := !active; upd(false)`
  (power toggle).

### 2.3 prop_kerfurOmega (`Aprop_kerfurOmega_C : Aprop_C` — the turned-off Kerfur-O prop)

`getActionOptions`: ENUM `[8]` (Activate). `sentient @0x0384`.
`actionOptionIndex` -> uber @29: `action == b8 -> spawnKerfuro()`.
`spawnKerfuro` (891 bytes): `BeginDeferredActorSpawnFromClass(self,
spawnKerfur, ...)` + `FinishSpawningActor` (the kerfurOmega CHARACTER — an
NPC-allowlist class) -> on success **`K2_DestroyActor()`** self (@767); on fail
`lib_C::addHint` (@786).

### 2.4 mainPlayer as a TARGET (puppets!)

`mainPlayer.getActionOptions` (142 bytes): returns ALL-EMPTY arrays;
`mainPlayer.actionOptionIndex` -> uber @113115 = `POP->ret` (no-op). **A
remote player's puppet (mainPlayer_C orphan) shows NO action menu and accepts
no enum actions — nothing to sync.** (The E-press on a puppet still routes the
int_player path: `lookAt` / `player_use` — out of scope here.)

### 2.5 The kerfurOmega skins

`kerfurOmega_0/_1/_2/_alien/_erie/...` (20+ subclasses, kerfurOmega_*.hpp) are
SKIN subclasses; npc_sync matches them via the SuperStruct walk
(`IsClassOrDerivedFromAnyAllowlisted`, npc_sync.cpp:171-178). Their
getActionOptions overrides were NOT exhaustively dumped — **UNKNOWN whether any
variant adds/removes options ('kill'?); the seam design below is
string-pass-through so unknown extra options forward unmodified.**

---

## 3. Coop infrastructure facts (what exists today)

- HOST authoritative NPCs: `coop/npc_sync.cpp` intercepts
  `BeginDeferredActorSpawnFromClass` (12-class allowlist incl.
  `NpcClass_KerfurOmega`, sdk_profile.h:724-737), allocates an `element::Npc`
  (eid), broadcasts `EntitySpawn`; POST observer binds the actor and fills the
  host reverse map `g_actorToNpcId` (npc_sync.cpp:136-137, actor* -> eid);
  `K2_DestroyActor` PRE observer -> `EntityDestroy` (npc_sync.cpp:279-292).
- CLIENT mirrors: `coop/npc_mirror.cpp` materializes wire spawns
  (bypass slot `MarkIncomingNpcSpawn`), `RegisterMirror`s the host eid, and
  **parks the mirror: `ue_wrap::puppet::DisableCharacterTicks` — actor tick
  (BP AI graph) + CMC tick OFF** (npc_mirror.cpp:305-309). Local NPC spawns are
  suppressed (interceptor). `TickClientNpcs` (npc_mirror.cpp:433-472) drives
  every mirror per net-pump tick via `MirrorManager<Npc>::Snapshot`.
- Pose stream: host `TickPoseStream` (npc_pose_host.cpp:79) reads each live
  Npc actor per tick -> `EntityPoseSnapshot` (protocol.h:1334-1349, 44 B): pos,
  yaw, speed, v39 kerfur `lookAt` (ReadKerfurLookAt, class-gated), v40
  `bodyYaw`, `stateBits` (bit0 inAir, bit1 hasLookAt, bit2 hasBodyYaw,
  **bits 3-7 free**), `_pad[3]` (**3 free bytes**).
- Identity: host actor->eid = `g_actorToNpcId`; eid->Element =
  `element::Registry::Get(eid)` -> `Element::GetActor()` (element.h:117).
  Client actor->eid: `MirrorManager<Npc>::Snapshot` + compare actor pointers
  (or extend the manager with a reverse map — Snapshot-walk is fine at NPC
  counts; the State poll below iterates the same snapshot anyway).
- Protocol tail: `kProtocolVersion = 61` (protocol.h:508); last ReliableKind =
  `ChatMessage = 48` -> **next free kind 49**.
- Per-peer puppets: `coop/remote_player.h` exposes the raw engine puppet actor
  (mainPlayer_C) per remote slot — the `Player` param for host-side applies.
- File sizes: npc_sync.cpp is 872 LOC (> 800 soft cap) — **new feature code
  must be its own TU** (`coop/npc_action_sync.{h,cpp}` suggested), not more
  npc_sync growth.

---

## 4. THE SEAM RECIPE

### 4.1 What happens today without sync (the bug being fixed)

A CLIENT clicking a menu entry on a kerfurOmega MIRROR runs the FULL handler
locally on the mirror via ProcessLocalScriptFunction (interface dispatch does
not care about authority): state verbs scribble mirror `State` (inert but
diverges the menu display), `turn_off` spawns a LOCAL `prop_kerfurOmega_C`
(client-owned -> PropSpawn broadcast = ghost prop on all peers) and destroys
the mirror locally (while the host NPC lives -> the next EntityPose drives a
dead actor), `take_object` spawns a local copy of the held item,
`pat` plays locally only. On `prop_kerfurOmega` Activate, the client's local
kerfurOmega spawn IS already swallowed by npc-suppress, but the prop's local
`K2_DestroyActor` still desyncs the prop. These are real divergence vectors
TODAY.

### 4.2 (a) DETECT a local player's menu action

No PE-visible dispatch exists (section 1.7), so detection is per-effect:

1. **State verbs (follow/idle/patrol/fix_servers/get_reports/fix_transformers)
   — poll-diff `State @0x05C8` on client mirrors.** Inside the existing
   `TickClientNpcs` mirror walk (zero extra iteration): keep the
   wire-authoritative State per mirror (from the npcState stream, 4.4); if the
   actor's live byte differs from the authoritative value, a local actor wrote
   it — on a tick-parked mirror the ONLY local writer is the menu handler
   (uber state writers outside actionName — @642, @1193, @4858, @18293,
   @19962, @36236 — are MoveTo-proxy delegates / task continuations that
   require host-side AI flows the mirror never starts; `checkDoor`, the one
   timer mirrors DO run, never writes State). Self-healing even if a stray
   writer existed: forward the diff, then re-snap the mirror byte to the
   authoritative value (the host gate rejects nonsense and the stream
   overwrites the scribble).
   This is the established appliance-sync mechanism (ue_wrap/appliance.h:10 —
   "BP-internal and bypass our ProcessEvent detour, so we never observe the
   switch — we poll").
2. **turn_off / take_object — intercept the PE-visible births/deaths.**
   Client-side `RegisterInterceptor` on `BeginDeferredActorSpawnFromClass`:
   param `WorldContextObject == a registered NPC-mirror actor` (dropKerfurProp
   @175/@538, holdObject_kerf @416, spawnKerfuro @322 ALL pass `self`) ->
   swallow (return true) + map (mirror eid, spawn class) -> semantic action:
   class == the mirror's `dropProp @0x0850` value (or a floppy class) =>
   `turn_off`; class == a held-object class => `take_object`. Plus a
   client-side `K2_DestroyActor` interceptor for registered mirror actors
   (veto local destroys; wire-driven destroys from `OnEntityDestroy` pass via
   a Mark/Clear bypass slot exactly like `MarkIncomingNpcSpawn`). This is the
   client authority boundary, not a crutch: mirrors must not give birth or die
   locally — only the wire may (MTA: client-side entities are server-lifetime).
   Dedupe the floppy+dropProp double-BeginDeferred by (eid, frame).
3. **pat — POST-observe `CreateProxyObjectForPlayMontage`** (EX_CallMath,
   expected-PE-visible — VALIDATE first, 1.7) with `Mesh` belonging to a
   tracked NPC (mirror on client, tracked actor on host) + montage section
   'pat'; OR fall back to polling `Animation @0x0678` rising-edge in the same
   tick walk. Either way the local montage+meow on the clicking client plays
   natively (instant feedback, correct over-pet target — see 4.5 risk 3).
4. **HOST player's own actions need detection too** (the host runs handlers
   natively, invisible the same way): the State stream (4.4) carries state
   verbs to clients automatically; `turn_off` flows via the EXISTING
   K2_DestroyActor PRE -> EntityDestroy plus the prop-side mirror of the
   dropped prop (4.4); `pat` uses the same montage-proxy POST observer
   (host-side, target = any tracked NPC) -> broadcast NpcAction{pat} so
   clients replay the montage.
5. **ENUM-channel citizens** (p_kerfus `Active @0x0460`, prop_kerfurOmega
   Activate): p_kerfus rides the PROP pipeline — same poll-diff shape on its
   `Active` byte if/when p_kerfus sync is wanted (phase 2; verify how p_kerfus
   mirrors are parked first — prop pipeline, not DisableCharacterTicks).
   prop_kerfurOmega Activate = a BeginDeferred of an ALLOWLISTED NPC class
   with WCO == a prop-mirror actor: already swallowed client-side by
   npc-suppress; add the forward (NpcAction{eid-of-prop, activate}) keyed off
   that existing interceptor fire + veto the prop's local K2_DestroyActor
   (prop mirror death-watch owns its lifetime).

### 4.3 (b) IDENTIFY the NPC cross-peer

- Client: mirror actor -> eid via `MirrorManager<Npc>` snapshot walk (or a
  small reverse map added to npc_mirror; the poll loop already holds both).
- Host: eid -> actor via `element::Registry::Get(eid)->GetActor()` (validate
  with `IsLiveByIndex` per registry.h:157-170); actor -> eid via
  `g_actorToNpcId` for host-side detection.
- Wire validation: `Registry::IsAllowedHostAllocatedEid` on the eid in a
  client->host NpcAction (NPC eids are host-range; registry.h:141-150).

### 4.4 (c) APPLY on the host + (d) what flows back

**Host apply = reflected ProcessEvent call of the SAME BP entry on the
authoritative actor** (the garage `acivae` / doorOpen precedent —
reflection::CallFunction builds the ParamFrame from the UFunction layout):

- State verbs + turn_off + take_object: call
  `actionName(Player, Hit, Name)` — Player = the requesting peer's puppet
  `mainPlayer_C*` (remote_player.h), Hit = zeroed FHitResult (0x88; no handler
  branch reads it), Name = the verb string. Running the REAL handler keeps the
  REAL gates: busy-gate (State in {3,4,5}), `kill` gate @20350, `sentient`
  refusal inside dropKerfurProp. NO re-implementation of game logic (RULE 1).
  - take_object phase-2 refinement: do NOT rely on `player.holding_actor` on
    a puppet (not maintained); instead call `holdObject_kerf(puppet,
    resolvedHeldActor)` directly, where the request carries the client's held
    prop eid and the host resolves its prop mirror. v1 may simply not forward
    take_object (see 4.5 classification).
- pat: do NOT call actionName on the host (the meow-accumulator easter egg
  would ragdoll the HOST's player — wrong target, risk 3). Host apply =
  cosmetic only: `CreateProxyObjectForPlayMontage(Mesh, montage, 'pat')` +
  `StopActiveMovement` via reflection (or skip the host visual in v1) and
  RELAY NpcAction{eid, pat} to the other clients, which replay montage-only on
  their mirrors.
- prop_kerfurOmega activate: host calls `actionOptionIndex(puppet, Hit,
  b8, comp)` (or `spawnKerfuro()` directly) on the host prop actor.

**Results that flow back AUTOMATICALLY via existing channels:**

| effect | channel |
|---|---|
| NPC movement change (follow/patrol/task walks) | EntityPose stream (v37+) |
| kerfur head/body | v39 lookAt / v40 bodyYaw |
| NPC despawn (turn_off) | K2_DestroyActor PRE -> EntityDestroy (existing) |
| kerfurOmega CHARACTER spawn (prop Activate) | BeginDeferred interceptor -> EntitySpawn (existing) |

**Results needing NEW wire pieces:**

1. `NpcAction` reliable, `ReliableKind = 49`, **protocol v62**:
   `{ uint32 elementId; uint8 verb; uint8 _pad[3]; uint32 auxEid; char name[24]; }`
   — verb enum {SetState0..5 / Pat / TurnOff / TakeObject / ActivateProp /
   NameString}; `name` carries the raw actionName string when verb ==
   NameString (forwards unknown variant options verbatim — subclass-proof);
   auxEid = held-prop eid for TakeObject. Client->host request; host applies
   (gated) then RELAYS to other clients ONLY for cosmetic verbs (pat) — state
   verbs need no relay (the State stream + pose stream carry the outcome).
2. **Host-authoritative State stream — zero wire growth:** put the live
   `State @0x05C8` byte into `EntityPoseSnapshot._pad[0]` (rename to
   `npcState`) + `stateBits bit3 = kEntityPoseBitHasNpcState` (kerfur-family
   gate like v39/v40). Read it in `TickPoseStream` next to ReadKerfurLookAt
   (npc_pose_host.cpp:111-133 pattern, one byte read off the actor already in
   hand); apply it in `Npc::SetTargetNpcPose` (snap, not interpolated). This
   keeps client menus (`getActionOptions` reads State) and the poll-diff
   baseline correct, and self-heals scribbles. sizeof stays 44 —
   static_assert unchanged.
3. The turn_off DROP PROP host->clients: `prop_kerfurOmega_C` (and the floppy
   classes) spawn host-side from kerfurOmega's EX_CallMath BeginDeferred; their
   `Init` is BP-internal (pinecone precedent) so **expected NOT to broadcast
   via Aprop_C Init-POST — add `prop_kerfurOmega_C` (+ `prop_floppyDisc_*`
   drop variants) to the M2 HostSpawnWatcher class list**
   (kAmbientPropSpawnMirrorClasses, sdk_profile.h:754-758) — UNKNOWN until
   smoke-tested whether the Init-POST path already catches them; verify before
   adding (avoid double-spawn; HasProcessedInit dedup exists in the watcher).

**Receiver-side cosmetic replay (pat):** on NpcAction{pat} call
`CreateProxyObjectForPlayMontage(mirrorMesh, montage, 'pat')` via reflection
(montage asset resolvable via FindObject — kerfurOmegaV1_Skeleton_Montage; or
re-dispatch `actionName('pat')` on the mirror and accept the local meow
accumulator, see risk 3).

### 4.5 Risks / edge cases

1. **Actions that destroy the NPC (turn_off) or spawn actors (take_object,
   prop Activate)** — covered by the interceptor boundary (4.2.2). Without the
   veto, the local prop ghost + dead-mirror pose writes are guaranteed
   (4.1). The K2_DestroyActor veto must NOT break `DrainClientMirrors` /
   `OnEntityDestroy` (both call K2_DestroyActor deliberately — use the bypass
   slot).
2. **Mirror timers run despite DisableCharacterTicks**: kerfurOmega arms
   `checkDoor` every 0.5 s at ReceiveBeginPlay (@24421-24444); it
   SphereOverlaps 50 cm for `door_C` and calls `doorOpen(false)` (@24027) — a
   client mirror parked next to a door fights the door sync TODAY (pre-existing,
   not introduced by this feature; the HostAuth door suppression may already
   mask it — verify during implementation, separate fix if not).
3. **Over-pet easter egg targets the LOCAL player** (`lib_C::getMainPlayer` ->
   `ragdollMode(true)` @7349-7395): host-applying actionName('pat') would
   ragdoll the HOST when meow crosses 1.0. Hence pat = cosmetic-replay only on
   host/other peers; the clicking client's own mirror run keeps the easter egg
   on the correct victim, and player-ragdoll already mirrors via the existing
   ragdoll sync.
4. **UI-only actions**: `equipment` opens the objectViewer on the interactor —
   LOCAL ONLY, no sync. (Actually EQUIPPING an accessory through that UI
   mutates kerfur accessory state — a SEPARATE follow-up sync, out of menu
   scope.)
5. **Menu display divergence pre-State-sync**: a client menu on a busy host
   kerfur shows 10 options instead of 2 until npcState lands — the host-side
   busy-gate still rejects wrong requests, so it is cosmetic-then-corrected.
6. **The `sentient` field on mirrors** (read by dropKerfurProp's refusal) is
   spawn-time state; EntitySpawn does not carry it — turn_off forwarded to the
   host runs the gate against HOST truth, so correctness holds regardless of
   the mirror's local copy.
7. **Protocol/version**: NpcAction must be version-gated (v62) — older peers
   drop unknown kinds; the planned version-gating work item already covers the
   reject-on-join UX.

### 4.6 Per-action classification (the deliverable table)

| action | channel | already mirrored today | needs explicit sync | class |
|---|---|---|---|---|
| Follow / Idle / Patrol | name | movement via EntityPose | request-forward + npcState byte | SYNC (light) |
| Fix_servers / Get_reports / Fix_transformers | name | movement via pose; world side effects (server fix etc.) are host-side AI -> flow via their own systems | same as above + dropObject side effect rides prop pipeline | SYNC (light) |
| Pat | name | nothing | NpcAction relay, cosmetic replay; local run stays | SYNC (cosmetic) |
| Turn_off | name | host: EntityDestroy already; drop prop NOT confirmed | request-forward + mirror-protect interceptors + HostSpawnWatcher class add | SYNC (full) |
| Take_object | name | nothing | complex (held-object resolve); recommend PHASE 2, veto-only in v1 (suppress local damage, no forward; the host gate makes the click a no-op) | DEFER |
| Equipment | name | n/a | none (interactor-local UI); accessory-state sync is a separate follow-up | LOCAL-ONLY |
| kill (hidden) | name | n/a | none (never offered in audited lists; murder flow is its own event) | LOCAL-ONLY / UNKNOWN trigger |
| p_kerfus Activate/Use/Pat | enum | prop pose pipeline | poll-diff Active + forward; PHASE 2 | DEFER |
| prop_kerfurOmega Activate | enum | client NPC-spawn already suppressed | forward + prop destroy veto (today it half-desyncs) | SYNC (full) |
| puppet (mainPlayer target) | — | empty option set, no-op handler | none | NONE |

---

## 5. Explicit unknowns / pre-implementation validation steps

1. `CreateProxyObjectForPlayMontage` PE-visibility: expected-YES (EX_CallMath
   family) but NOT individually measured — 2-minute observer-log validation
   required before relying on it for pat detection.
2. Whether `prop_kerfurOmega_C` / dropped-floppy Init-POST already broadcasts
   PropSpawn when spawned by dropKerfurProp on the host (pinecone precedent
   says NO -> HostSpawnWatcher addition) — smoke-verify to avoid double-mirror.
3. kerfurOmega skin subclasses' getActionOptions overrides (extra options such
   as 'kill') — not exhaustively dumped; the NameString pass-through forward
   covers them, but the option-list parity on mirrors is unaudited.
4. `enum_kerfurCommand` value 6 meaning (0..5 mapped above) — unused by the
   menu; unknown.
5. p_kerfus mirror parking details (prop pipeline) before extending poll-diff
   to `Active @0x0460`.
6. The mechanical route by which EX_CallMath/EX_FinalFunction native calls
   reach our ProcessEvent detour remains un-pinned in IDA (inherited unknown
   from the v58 doc); all reliance here is on measured precedents of the SAME
   forms.
7. Whether the existing HostAuth door suppression masks mirror `checkDoor`
   doorOpen(false) spam (risk 2) — verify in a doorway smoke.

## 6. File/module plan (for the implementer)

- NEW `coop/npc_action_sync.{h,cpp}` (npc_sync.cpp is 872 LOC — over the soft
  cap; do NOT grow it): the NpcAction wire receive/apply, the client State
  poll-diff (hooked into TickClientNpcs' existing walk), the mirror-protect
  interceptors (BeginDeferred WCO-gate + K2_DestroyActor veto with bypass
  slot), the pat montage observer + replay.
- `protocol.h`: ReliableKind::NpcAction = 49, NpcActionPayload, v61 -> v62,
  EntityPoseSnapshot `_pad[0]` -> `npcState` + `kEntityPoseBitHasNpcState`.
- `npc_pose_host.cpp`: one byte read (State) beside ReadKerfurLookAt.
- `npc_pose_drive.cpp` / `element/npc.h`: snap npcState on the mirror +
  expose the authoritative value to the poll.
- `ue_wrap/`: a tiny `kerfur_omega.h` reflected-offset wrapper (State/kill/
  Animation/sentient/dropProp reads; resolve via reflection with the @-offsets
  above as documented fallbacks — the base_window.cpp pattern). No gameplay
  logic in ue_wrap (principle 7).
