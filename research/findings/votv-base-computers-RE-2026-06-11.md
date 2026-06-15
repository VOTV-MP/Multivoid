# VOTV base computers / terminals — RE + coop occupancy & screen-state sync design

Date: 2026-06-11/12. READ-ONLY RE pass. Groundwork for next session's implementation.

> **STATUS 2026-06-12: PHASE 1 (occupancy) IMPLEMENTED — v63 `DeviceClaim=51`**
> (`coop/device_occupancy` + `ue_wrap/device_screen` + `prop_sound::PlayDenyClick`).
> Shipped per §2.3 (activeInterface poll), §3.1 (InpActEvt_use PRE aim-clear deny,
> v1 = whole-actor granularity), §3.2 (button_keypad_deny), §3.3 (host first-wins
> arbitration + reflected setActiveInterface(null) force-exit + disconnect release
> + connect-snapshot replay). Claim keys diverge from §5.2 deliberately: per-WIDGET
> identity ("desk"/"sat"/"radar"/"reactor"/"laptop" singletons + "tfm_<posKey>"/
> "arc_<posKey>") — §1's own shared-widget finding makes per-INSTANCE sat/radar
> keys unsound (all SAT consoles render the one umg_console child; two peers in
> "different" consoles would type into the same screen). Smoke-verified: offsets
> 0x7E0/0x744/0xAA0 resolve, 7/7 widget + 7/8 device classes (arcade lazy), deny
> gate installed both roles. Hands-on pending. **PHASE 2 (state mirror §4-5, kinds
> 52-56 + the sky-signal host-roller §5.4 + U1-U10 validation) NOT BUILT.**

User-decided design being served: (1) **occupancy** — ONE peer "inside" an
enterable device at a time; a second peer's E gets DENIED with the existing
deny/fail sound; (2) **RT screen contents mirror cross-peer** per RULE 1 (no
fake/empty screens) — a peer entering later sees the same screen state.

Sources: cooked-BP kismet disassembly via `research/bp_reflection/` tooling
(new dumps this pass: `analogDScreenTest`, `panel_SATconsole`, `panel_radar`,
`panel_reactor`, `panelBase`, `ui_consolesAtlas`, `ui_console`, `laptop`,
`ui_laptop`, `spaceRenderer`, `serverBox`, `transformerMGPanel`, `prop_arcade`,
`prop_portablePc`, `drone`); the CXXHeaderDump SDK
(`Game_0.9.0n/.../CXXHeaderDump/*.hpp`); pak asset census
(`research/pak_re/extracted`); coop sources (`src/votv-coop/`). PE-visibility
verdicts rest on the measured table in
`votv-inventory-pickup-seam-RE-2026-06-11.md` §3 (only `EX_Local*` forms are
measured-zero) + raw `$type` dumps made this pass.

---

## 0. Executive summary

- **The enterable-device census is CLOSED**: a pak-wide grep for callers of
  `mainPlayer.Enter Interface` yields exactly **8 device assets**:
  `analogDScreenTest` (the 4-screen main desk; only its COORDS panel enters),
  `panel_SATconsole` (command terminal), `panel_radar`, `panel_reactor`,
  `laptop` + `prop_portablePc` (both showing the ONE shared `gamemode.laptop`
  ui_laptop widget = "the meadow PC"), `transformerMGPanel` (map transformer
  minigame), `prop_arcade`. Nothing else in the game enters a 3D screen.
- **The enter/exit seam**: device `actionOptionIndex(Use)` →
  `player.Enter Interface(widget, …)` → `setActiveInterface(widget, zoom=true,
  3D=true)` writes **`mainPlayer.activeInterface @0x07E0`** (THE
  inside-a-device discriminator), freezes movement, swaps input mode + FOV.
  Exit = `setActiveInterface(null)` (widget ESC handlers / `ragdollMode`) which
  **BROADCASTs the `exitInterface` delegate @0x0D00** and restores everything.
- **Every call in the subsystem is `EX_LocalVirtualFunction`** (raw `$type`
  dumps, §2.4): Enter Interface, setActiveInterface, saveSignal, deleteSignal,
  addEmail, makeAnOrder, addSignal, spawnSignal — ALL invisible to our
  ProcessEvent detour. Detection is **state polling** (the appliance/economy
  precedent); apply is **reflected ProcessEvent calls of the same BP entries**
  (the garage/economy precedent). The ONLY PE-visible seam on the enter path is
  the engine input event `InpActEvt_use_K2Node_InputActionEvent_41`
  (production-proven: interactable_sync.cpp:264-300).
- **The busy/deny recipe**: PRE-observe `InpActEvt_use` on the local player; if
  the aim (`HitResult.Actor` / `lookAtActor`) resolves to a busy device, null
  the aim fields for the dispatch (restore in POST — the exact door-stress
  precedent shape) and play **`/Game/audio/effects/button_keypad_deny`** via
  `PlaySound2D(vol 0.5, pitch 1.0)` — byte-identical to the game's own
  save-deny dispatch (analogDScreenTest uber @23927). The game's own busy idiom
  (transformerMGPanel disables its `useBox` collision while occupied,
  @2345/@2774) is documented as the alternative.
- **Screen state is already SERIALIZED by the game**: the whole 4-screen desk
  marshals to `Fstruct_signalTableState` (0x3B8) via `gatherData`/`setData`
  (save/load uses it: mainGamemode.saveObjects @3573 / loadObjects @6241 →
  `saveSlot.analogPanelsData @0x08E8`). Emails, tasks, saved signals, orders,
  points all live in `saveSlot` → **the v56 save-transfer join already gives a
  joining client the full screen state**. The live sync = watermark-poll the
  few mutable arrays/structs + one keyed busy channel.
- **The one per-peer-RNG hole**: `spaceRenderer.spawnSignal` re-arms every
  20-60 s and rolls fully-random sky signals per peer (uber @3842/@4227,
  `addSignal` @148-586). Host must be the only roller; client spawner
  suppressed; signals mirrored at the result level (the wind/firefly category).
  Emails are funneled through ONE entry (`gamemode.addEmail`) whose rollers
  (daynightCycle tasks, drone sell→`lib_C::getResponse`) are host-side or
  busy-peer-side; mirror the resulting `saveSlot.emails` append.

---

## 1. CLASS CENSUS — every enterable computer/terminal

Census method: `grep -rl "Enter Interface" research/pak_re/extracted/VotV/Content/{objects,main,umg,test,misc}`
→ 8 device assets + mainPlayer (the implementation). This is the COMPLETE set
for the whole pak (tutorial variants excluded).

| # | class (.hpp / json) | base class | identity / key | screen widget → where it renders | enters? |
|---|---|---|---|---|---|
| 1 | `AanalogDScreenTest_C` (analogDScreenTest.hpp; dev name kept by the game) | `AActor` | **singleton** — `gamemode.analogPanels @0x0EC0` (resolved via GetActorOfClass, mainGamemode uber @18997); no save Key | ONE `Uui_consolesAtlas_C` (`Widget @0x0638`) drawn into `widget_RT @0x0650` via `widgetRender` (UWidgetComponent @0x0618); the FOUR screen meshes `screen_comp/coords/download/play @0x04D0-0x04E8` show material crops of that one RT; it ALSO pushes child widgets + materials to the SAT consoles (`consoles[i].widget := widget.umg_console`, uber @982/@1104) | only the **COORDS** screen (`button_coords` → Enter Interface @28958); play/download/comp screens are physical-button-operated, never entered |
| 2 | `Apanel_SATconsole_C` | `Aactor_save_C` (save **Key @0x0230**) | multiple placed; `root` flag → `gamemode.rootConsole @0x0848` (uber @572); array `consoles` on the desk/gamemode | `Uui_console_C` (`Widget @0x0298`) = the atlas's `umg_console @0x0600` child — shared screen, per-panel mesh `radarScreenMesh` gets the material (desk uber @1104) | YES — `actionOptionIndex(Use b4)` @1218 → @606 |
| 3 | `Apanel_radar_C` | `Aactor_save_C` (Key @0x0230) | placed instance(s); `radarElement := screens.widget.umg_radar` assigned from the desk broadcast (@309-328) | `Uui_radar_C` = the atlas's `umg_radar @0x0608` child | YES — `actionOptionIndex(Use)` @548 → @896 |
| 4 | `Apanel_reactor_C` (no .hpp in dump — class json: `panel_reactor_C : panelBase_C : AActor`) | `panelBase_C` | **no save Key** (panelBase is AActor); expected singleton (base reactor room) — VERIFY live | `ui_reactor_C` (rod control screen) | YES — uber @212: Enter Interface(widget,…,'reactor') + teleport to `stand` |
| 5 | `Alaptop_C` | `Aactor_save_C` (Key @0x0230) | the base laptop; widget is **`gamemode.laptop @0x0448`** (`Uui_laptop_C`) — assigned at boot (laptop uber @9898 `widget := gamemode.laptop`) | `ui_laptop_C` on `screen` UWidgetComponent (`screen.SetWidget(widget)` @4547); **ONE shared widget instance** for ALL physical laptops | YES — `actionOptionIndex(Use b4)` → checks → `enter(player)` @4472 |
| 6 | `Aprop_portablePc_C` | `Aprop_C` (**prop pipeline — has an eid already**) | buyable prop; child `AportablePcTop_C` carries the `screen` WidgetComponent | the SAME `gamemode.laptop` widget (`usePC` @158: `Enter Interface(gamemode.laptop,…)`; `gamemode.laptop.nearestActor := self` @312) | YES — own `usePC` fn (gated on `opened`) |
| 7 | `AtransformerMGPanel_C` | `AActor` | per-transformer instances across the map; no save Key — identity via owning transformer (VERIFY live: position/owner chain) | **per-instance** `Uuiwindow_transformerScreens_C` (`widgetInst @0x02A8`) | YES — `actionOptionIndex(Use)` @1834 → Enter Interface @2017 |
| 8 | `Aprop_arcade_C` | `Aprop_C` (prop eid) | buyable prop | per-instance `ui_arcade_invaders_C` (`scrWidge`) | YES — uber @830: `player.arcade := true` + Enter Interface @963 + teleport |

Screen-space interfaces that use the SAME `setActiveInterface` plumbing but are
**NOT devices** (no camera-zoom 3D screen; excluded from occupancy):
`mainGamemode.enterClipboard/openPropInv/enterDrawPaper/openPicker` (all
`zoom=false, 3D=false`).

**Explicitly NOT enterable** (checked): `serverBox` (zero Enter Interface /
setActiveInterface hits — the server minigames run on in-world widget
interaction, separate feature), `wallunit_console` (a static mesh only, 1
field), `droneConsole` (physical buttons only — already part of drone work).
`signalCam_C` (cameras) are not entered; their feeds render into the laptop's
CAMS tab locally from world state.

**Placed-instance counts are NOT settled from static dumps** — the partial
sublevel dumps each show ≤1 of each panel, but 59/20/17 sublevels REFERENCE
SATconsole/radar/desk (imports ≠ placements). VALIDATION: live GUObjectArray
walk (dev overlay) listing instances + Keys of classes #2,3,4,7 on a loaded
save. The BP code proves: desk = singleton (`GetActorOfClass`), SAT consoles =
plural (`consoles` array + `root` flag + `withinConsoles` counter), laptop
widget = singleton.

### 1.1 The shared-RT architecture (why mirroring STATE mirrors the pixels)

`ui_consolesAtlas` (167 fields) is one canvas with per-screen cover canvases
(`canvas_cover_comp/_console/_coords/_download/_play/_radar @0x0270-0x0298`,
`canvas_signalList @0x02A0`) — the desk draws it into `widget_RT` and each
physical screen mesh samples its own UV region (incl. the SAT console + radar
meshes, which receive a material from the desk at boot: uber @982-@1235
`consoles[i].widget := widget.umg_console; …SetMaterial(1, mat); …begin()`).
The laptop family shows the one `gamemode.laptop` widget. The transformer +
arcade carry per-instance widgets.

**Consequence (RULE 1 path):** every pixel on every screen is a pure function
of LOCAL widget state + the state fields below (§4). No texture streaming —
mirror the state, the RT mirrors itself.

---

## 2. THE ENTER/EXIT SEAM

### 2.1 Player-side core (mainPlayer.hpp + bytecode)

Fields (mainPlayer.hpp):

```
:103  UWidget* activeInterface;        // 0x07E0  <- THE discriminator (null = not inside)
:276  FmainPlayer_CExitInterface exitInterface;  // 0x0D00 multicast delegate
:295  bool wasActiveInterface;         // 0x0D70
:364  bool isActiveINterface3D;        // 0x0F05
:51   UWidgetInteractionComponent* WidgetInteraction; // 0x0628 (focus follows: uber @33053)
HitResult @0x0744 (FHitResult, 0x88; Actor weakptr at +0x68), lookAtActor @0x0AA0,
arcade @0x0930 (prop_arcade sets it true on enter)
```

`setActiveInterface(activeInterface, inString, zoom, sentBy, ignoreZoom,
isActiveINterface3D, showInterface, &return)` (mainPlayer.hpp:489; 1676 B):

- @669 `activeInterface := param` (the field write).
- valid widget: @974-1052 input mode GameAndUIEx (zoom) / UIOnlyEx (@1322-1400),
  mouse cursor on, @1180 `Camera.SetFieldOfView(settings.M_panelFOV…)`,
  @1278-1306 `simulate_useReleased(); interactedWithWidget(); return := true`.
- null widget (exit): @1405-1531 GameOnly input, cursor off, @1545 default FOV,
  **@1643 `BROADCAST exitInterface`**, `return := false`.
- @548: `icast(param)` → `intComs_unfocused()` on the widget being SET (a
  fresh-focus reset, runs on enter — NOT the exit notification).

`Enter Interface(activeInterface, LookAtLocation, camLocation, callFrom)`
(mainPlayer.hpp:495; 791 B): @352 `isLookAt := true`; @363
`setActiveInterface(widget,'',zoom=true,_,false,3D=true,show=false)`; success →
@416 `simClick(); CharacterMovement.SetMovementMode(b0)` (**MOVE_None — the
player is frozen while inside**); fail+not-sitting → @614 SetMovementMode(b3).
`callFrom` strings: 'enter console' / 'enter radar' / 'enter laptop' / 'enter
panel coordinates' / 'reactor' / 'enter prop arcade' (log-only).

**Exit triggers** (who passes null): widget key handlers — proven:
`ui_laptop.OnKeyUp` @363 `player.setActiveInterface(_, …)`; and
`mainPlayer.ragdollMode` @472 (**forced exit on ragdoll/death — the busy
release MUST cover this; the activeInterface poll does**). The desk coords
panel additionally resets itself via its own `intComs_unfocused` → uber @78564:
`master_spaceRenderer.move_* := false; controllingCoordinatePanel := false;
bEnableClickEvents := false`.

### 2.2 Device-side enter chains (bytecode-verified)

All four panel enters START at the device's `actionOptionIndex(Player, Hit,
Action, lookAtComponent)` — the int_objects ENUM channel (`Use` = b4; see the
npc-action doc §1 for the dispatcher plumbing `useSelectedAction` @999).

- **Desk / coords** (`ExecuteUbergraph_analogDScreenTest`, entry @18326):
  `player_using := player` (@18326, **0x1358 — device-side last-user field**);
  `ADDDEL player_using.input_E += …` (@18368); `component_button :=
  hit.HitComponent` (@18581); long dispatcher; `button_coords` match @28806 →
  @28858-29383: `Enter Interface(widget, screen_coords.loc, stand.loc, 'enter
  panel coordinates')`; `makeLookAt(...)`; `gamemode.bullshitWidgetClick(widget)`;
  **`controllingCoordinatePanel := true` (@29313, field 0x1410)**;
  `PlayerController.bEnableClickEvents := true`.
- **SAT console** (`ExecuteUbergraph_panel_SATconsole` @1218 → @606): `con();`
  **`widget.used := self`** (ui_console.used @0x0410 — records WHICH console
  panel is in use); gate `hit.HitComponent == Box` @825; @873-1217 `Enter
  Interface(widget, radarScreenMesh.loc, stand.loc, 'enter console')` +
  **`player.K2_SetActorLocation(stand.loc)` — the game TELEPORTS the player to
  the stand**; DelayFrames(3) → `widget.Enter()` (@96).
- **Radar** (`ExecuteUbergraph_panel_radar` @548): Use → component checks
  (`lever` = the alarm lever → trigger_alarm runTrigger @150-268; `panel` = the
  screen → @896-1184 `Enter Interface(radarElement, …, 'enter radar')` +
  teleport to stand). Radar modules (upgrades) install via the same dispatch
  @1185+.
- **Reactor** (`ExecuteUbergraph_panel_reactor` @212): `Enter
  Interface(widget, screen.loc, stand.loc, 'reactor')` + teleport to stand.
- **Laptop** (`ExecuteUbergraph_laptop` @2104+): Use(b4) → @2633 floppy/zip
  eject branch; @2700-3686 gates: keyboard + mouse child-actors stationary
  (|v|<5), upright (dot>0.5), within 150 cm; @3700 `Box.IsOverlappingActor(
  player)`; @3765+: clear all `portablePcTop_C` screens; `widget.genStore()`
  (@4435 — store list rebuilt on every enter); **`enter(player)`** (@4472) →
  (laptop.enter, 481 B): screenUI mode → AddToViewport + setActiveInterface
  (zoom=false), else @254 `Enter Interface(widget, screen.loc, lookAt_0.loc,
  'enter laptop')`; @169 `widget.bIsFocusable := true; widget.nearestActor :=
  self` (ui_laptop.nearestActor @0x0D80); then `widget.createRC()`,
  `stabilizeSeat()`, `screen.SetWidget(widget)` @4547, **`BIND exitLaptop;
  ADDDEL gamemode.mainPlayer.exitInterface += …` @4592-4615** (the laptop
  listens for the player's exit broadcast). `Activate`(b8) = lid open/close
  (`isOpened`, hiveBootup/hiveShutdown sounds @2275+).
- **Portable PC** (`prop_portablePc.usePC`): gate `opened`; `Enter
  Interface(gamemode.laptop, top.screen.loc, top.camLoc.loc, 'enter laptop')`;
  `gamemode.laptop.bIsFocusable := true; gamemode.laptop.nearestActor := self`.
- **Transformer panel** (`ExecuteUbergraph_transformerMGPanel` @1834):
  `Player := player` (field 0x02B0); `Enter Interface(widgetInst, …)` @2017;
  binds player scroll delegates + `exitInterface` @2245; **`useBox.
  SetCollisionEnabled(b0)` @2345 — the game's OWN busy idiom: the panel makes
  itself un-interactable while occupied**; exit handler @2523: unbind all +
  `useBox.SetCollisionEnabled(b1)` + `bEnableClickEvents := false`.
- **Arcade** (`ExecuteUbergraph_prop_arcade` @830): `player.arcade := true`;
  `Enter Interface(scrWidge, …)`; teleport to stand.

### 2.3 Occupied-by-WHOM fields (for the busy poll)

| side | field | offset | written | cleared |
|---|---|---|---|---|
| player | `activeInterface` | mainPlayer 0x07E0 | setActiveInterface @669 | same fn, null param (widget ESC / ragdollMode) |
| desk | `player_using` | 0x1358 | every desk `actionOptionIndex` @18326 | **never** (last-user marker — do NOT use as busy) |
| desk | `controllingCoordinatePanel` | 0x1410 | coords enter @29313 | coords exit @78784 (intComs_unfocused) |
| SAT console widget | `ui_console.used` | 0x0410 | console enter @620 | never (last-used panel ptr — resolves WHICH console instance) |
| laptop widget | `ui_laptop.nearestActor` | 0x0D80 | laptop/portable enter | never (last device) |
| transformer | `Player` | 0x02B0 | enter @1875 | not cleared; the collision gate is the real lock |
| player | `arcade` | 0x0930 | arcade enter @830 | arcade exit (uber, on exitInterface) |

**The robust local "inside which device" detector** = poll
`activeInterface @0x07E0` on the LOCAL player (one pointer read per net-pump
tick); classify the widget by class (`ui_consolesAtlas` → desk-coords;
`ui_console` → SAT console, instance = `ui_console.used @0x0410`; `ui_radar` →
radar (instance: the radar that assigned radarElement — singleton-expected,
VERIFY); `ui_laptop` → the meadow PC (one shared claim regardless of physical
device; `nearestActor` tells which body, cosmetic only); `ui_reactor` →
reactor; `uiwindow_transformerScreens` → transformer instance (widget→panel
backref — VERIFY field, else match by `widgetInst` pointer over instances);
`ui_arcade_invaders` → arcade prop (eid via prop registry)).

### 2.4 PE-visibility verdicts (the decisive table)

Raw `$type` dump of every relevant call site (this pass; tool: json walk over
`ScriptBytecode`):

| call site | opcode form | our ProcessEvent detour sees it? |
|---|---|---|
| `Enter Interface` (all 8 devices → player) | `EX_Context + EX_LocalVirtualFunction` | **NO** (measured-zero family) |
| `setActiveInterface` (laptop.enter, ui_laptop.OnKeyUp, gamemode helpers) | EX_LocalVirtualFunction | **NO** |
| `actionOptionIndex` / `getActionOptions` (menu dispatch) | EX_Context + EX_LocalVirtualFunction | **NO** (npc-action doc §1.7, same dispatcher) |
| `gamemode.saveSignal` / `deleteSignal` / `addEmail`; `laptop.addEmail/makeAnOrder/genStore`; `spaceRenderer.spawnSignal/addSignal/gatherSignal`; `lib_C.addEmail/getResponse/setTaskNew` call sites | ALL EX_LocalVirtualFunction | **NO** — the whole subsystem mutates invisibly |
| `InpActEvt_use_K2Node_InputActionEvent_41` (engine input → mainPlayer) | engine ProcessEvent | **YES** — production PRE+POST observers (interactable_sync.cpp:264-300; sdk_profile.h:999) |
| `exitInterface` delegate BROADCAST → bound `laptop.exitLaptop` / transformer handler | ProcessDelegate → ProcessEvent | **expected-YES** (delegate dispatch routes through ProcessEvent) — NOT individually measured; 2-min validation: INFO-log POST observer on `laptop.exitLaptop`, exit the laptop once. Not load-bearing (the poll covers exit). |
| OUR reflected calls INTO any of the above (apply path) | reflection::CallFunction → ProcessEvent | always works (garage `acivae` / economy `makeAnOrder` precedents) |

**Conclusion: detect by polling `activeInterface` + the state fields (§4);
deny at the `InpActEvt_use` PRE seam; apply via reflected calls of the game's
own entry points.**

---

## 3. THE BUSY/DENY RECIPE

### 3.1 The deny gate (recommended: player-side PRE-clear, the door precedent)

The door HostAuth fix (interactable_sync.cpp:200-230) established the shape:
*"the non-authority never advances state from its own simulation — our lever is
a field the BP already gates on, since we cannot add flags to assets"*. The
PRE observer clears the gate field for the body of ONE `InpActEvt_use`
dispatch; the POST observer restores it; every exit path restores (leak-heal at
next PRE).

For devices, the universal player-side gate is **the aim itself**:
`useSelectedAction` icasts `hitResult.HitActor` (field `HitResult @0x0744`,
Actor weakptr at +0x68 of FHitResult(0x88)) and `buildActionList`/`useAction`
read `lookAtActor @0x0AA0` (npc-action doc §1.6 — both are FIELDS read inside
the dispatch, after PRE runs). So:

1. PRE(`InpActEvt_use`): resolve the aimed actor (`lookAtActor`, fall back to
   `HitResult.Actor`); map actor → device claim key (§5.2); if **busy by
   another peer**: save + null `HitResult.Actor` (8 B at 0x0744+0x68) and
   `lookAtActor` (0x0AA0); remember in a single slot.
2. The native chain (useAction → … → actionOptionIndex → Enter Interface)
   no-ops: every icast fails on null. No enter, no grab-through, no side
   effects.
3. POST(`InpActEvt_use`): restore both fields FIRST (every early-return path —
   the door leak lesson, audit IMP-3), then play the deny sound (3.2), rate-
   limited by the existing 300 ms debounce shape (press+release double-fire,
   interactable_sync.cpp:257-268).

Notes:
- This gates ONLY E-presses aimed at a busy device — grabs/other interactions
  elsewhere unaffected (the clear lasts one dispatch).
- For the desk, the claim key must be **the coords sub-device**: gate only when
  the aimed COMPONENT is the coords cluster (`button_coords` /
  `screenbutton_switchToCoord1-3` / `screenbutton_bringCoord1-3/Cent` boxes —
  the coords occupant owns the dish aim; the play/download/comp buttons stay
  un-gated, their state rides §4 sync). Component resolve: `HitResult.Component`
  weakptr at +0x70. v1 MAY gate the whole desk if component mapping proves
  fiddly — document the choice in the PR.
- **Why not the game's own idiom** (transformer `useBox.SetCollisionEnabled(
  false)` mirrored on remote peers — the native shape): collision-off makes E
  do NOTHING silently (no lookAt text, no aim → our PRE can't even resolve the
  device → no deny sound). The user explicitly wants the deny SOUND, so the
  PRE-clear+sound is the design; the collision mirror remains the fallback if
  the PRE-clear shows any seam (e.g. WidgetInteraction clicks bypassing
  InpActEvt_use — see Unknown U3).

### 3.2 The deny/fail sound — exact asset + dispatch

The game's "save denied/failed" sound the user described is
**`button_keypad_deny`** — `SoundWave /Game/audio/effects/button_keypad_deny.button_keypad_deny`
(import table, analogDScreenTest.json imports 626/1024).

The canonical dispatch (the play-screen "save signal to laptop" deny —
`button_play_saveSig` handler, analogDScreenTest uber):

```
@23434: component_button == button_play_saveSig ?
@23546: sig := gamemode.savedSignals_0[play_selectIndex]
@23627: gate: sig.decoded >= sig.size  AND  sig.size != 0
   OK  @23778: gamemode.laptop.addSignal(sig)        (send to laptop signals tab)
  DENY @23927: GameplayStatics::PlaySound2D(self, button_keypad_deny, 0.5f, 1.0f, 0f, _, _, true)
```

The SAME asset is the keypad deny (`passwordLock` uber @413/@1456:
`SpawnSoundAttached(button_keypad_deny, …)` — 3D at the lock). The desk also
has a SECOND deny voice: the `deny` AudioComponent @0x0538 (Sound =
`buttonshort14`) used by `comp_uploadData` @35 when decode is active — NOT the
one the user described.

**Replay recipe for our deny:** reflected
`UGameplayStatics::PlaySound2D(WorldContext=mainPlayer, Sound=button_keypad_deny,
VolumeMultiplier=0.5, PitchMultiplier=1.0, StartTime=0, ConcurrencySettings=null,
OwningActor=null, bIsUISound=true)` — resolve the SoundWave via
`FindObject("/Game/audio/effects/button_keypad_deny.button_keypad_deny")`
(the weather_lightning / prop_sound::PlayInventoryBlipAt precedent —
EX_CallMath-shaped natives are callable through our reflection layer).

### 3.3 Claim/arbitration (who wins two simultaneous E-presses)

Host arbitrates (MTA shape — server-authoritative resource locks):

1. LOCAL peer poll detects `activeInterface` rising edge → classify → send
   `DeviceClaimRequest{key}` reliable to host **and optimistically stay in**
   (SP latency must not gate the common case — claims collide only in a
   sub-100 ms race).
2. Host keeps `key → slot` claim table. First request wins; broadcasts
   `DeviceBusy{key, slot, busy=1}`. A LOSING claimant gets
   `DeviceBusy{key, winnerSlot}` while its own slot ≠ winner → the loser's
   client **force-exits its own player**: reflected
   `setActiveInterface(null, "", true, null, false, isActiveINterface3D=true,
   false)` on the local mainPlayer (the ragdollMode exit precedent — the game's
   own forced-exit path) + play the deny sound. Sub-100 ms double-entry resolves
   to one occupant with at most a blink on the loser.
3. Falling edge (`activeInterface` → null, ANY cause: ESC, ragdoll, death) →
   `DeviceClaimRelease{key}` (+ the release state snapshot, §5.3). Host clears,
   broadcasts busy=0.
4. **Disconnect release**: host clears every claim held by the dropped slot on
   peer-remove (the roster/disconnect hook) and broadcasts busy=0 — a leaver
   never wedges a device. Host’s OWN claims clear on session end trivially.
5. Remote peers keep a local busy set; the PRE deny gate (3.1) reads it.

---

## 4. THE SCREEN-STATE MODEL per device

### 4.1 The desk (analogDScreenTest) — coords / download / comp / play

**The game already defines the canonical state blob**: `gatherData` (792 B)
packs 21 fields → `Fstruct_signalTableState` (0x3B8, struct_signalTableState.hpp):
DL_signalDownloadData (Fstruct_spaceObject 0x70), DL_SignalDownloadDLData
(Fstruct_signalDataDynamic 0x70), filter offsets/speeds (4×float), filter
actives (2×bool), DL_downloading, DL_PolarityDir, comp_maxLevel, comp_progress,
comp_data_0 (0x70), signalPlayData (Fstruct_signal_data 0x1C8), coord_signalData
(Fstruct_signal_spawn 0x2C), DL_resPercent, physMods[], backPlatesStates[],
backPlatesUnscrew[], remapVal, radarMods[]. `setData` (1305 B) unpacks +
refreshes (`updComp/updPolarity/...` chain). Save/load: `mainGamemode.saveObjects`
@3431 (`saveSlot.downloadPanelSignal := analogPanels.DL_SignalDownloadDLData`) +
@3573 `gatherData` → `saveSlot.analogPanelsData @0x08E8`; `loadObjects` @6241
`setData(saveSlot.analogPanelsData)`.

Live actor fields (analogDScreenTest.hpp, the same data un-marshalled):
`play_selectIndex @0x06C4`, `play_volume @0x06D0`, `active_play @0x06E8`,
`play_signalText @0x0700`, DL_* block @0x08F0-0x0A34 (`DL_resDetecPercent
@0x0970`, `DL_SignalDownloadDLData @0x0978`, `DL_downloading @0x0A2C`,
`DL_PolarityDir @0x0A30`, `active_download @0x0A34`), `active_coords @0x0A35`,
`coord_signalData @0x0A38`, `coord_coordLogText @0x0A68` (FString — the coord
log!), `coord_isPing @0x0A7C`, `coord_cooldown @0x0A80`, comp_* @0x0A90-0x0B41
(`comp_progress @0x0AA8`, `comp_data_0 @0x0AB0`, `comp_hecertext @0x0B30`,
`active_comp @0x0B41`), `canDL @0x1468`, `controllingCoordinatePanel @0x1410`.

Authority/persistence classification:

| state | lives | persisted? | sync need |
|---|---|---|---|
| download/comp/play/coords panel state | desk fields ⇄ `saveSlot.analogPanelsData` | YES (save-transfer join covers) | LIVE: poll-diff + broadcast deltas (host-relayed); the **`gatherData`/`setData` pair is the ready-made marshal** — call them via reflection to snapshot/apply the WHOLE desk in one struct (1 reflected call each side), or hand-pick the cheap fields for the periodic delta and reserve full setData for claim-release/join true-up. setData side effects = upd* refreshes (exactly what a mirror wants); it does NOT touch saveSlot. |
| saved signals list (the play screen list + `canvas_signalList`) | `gamemode.savedSignals_0 @0x0968` (⇄ saveSlot @0x0680 at save) | YES | LIVE: watermark-poll count (economy orders precedent); on append/delete broadcast the row; apply via reflected `gamemode.saveSignal(sig, New=true, checkOnly=false, q, false)` / `gamemode.deleteSignal(index)` — the game refreshes `Create Signal List` itself (saveSignal @1160) |
| drive slots (`obj_driveSlot_play/comp .drive @0x0250` on AdriveSlot_C) + drive contents (`prop_drive.Data_0 @0x0550`) | world props | drive data saved with prop | the drive PROP rides the prop pipeline already; the INSERTED state + `Data_0` swap (export @23016 / import @22379-22747 / comp upload `comp_uploadData` swap) needs a small keyed delta or rides the desk blob; VERIFY whether a mirror drive overlapping the slot port triggers mirror-side insertion (driveSlot BndEvt overlap) — if yes, suppress on mirrors |
| sky signals (the coords minigame targets) | `spaceRenderer.signals @0x0278` (+ ui_signal widgets) | NO — **pure per-peer RNG** (`spawnSignal` re-arm 20-60 s @3842; `addSignal` rolls pos/strength/freq/pol/obj `lib_C::getWeightedObject` @5-586; widget lifetime 120-240 s @1043) | **host-roller**: suppress client `spawnSignal` (interceptor on the timer re-arm is impossible — EX_LocalVirtualFunction; instead suppress at RESULT: client never rolls because WE null its timer? NO — RULE 1: the clean lever is replicating MTA's roller model: client's `addSignal` calls happen only from its own spawnSignal timer; we cannot PE-block it → **client-side: reflected-call `deleteSignal`-equivalent is absent; the workable suppression is wire-driving `signals` to match host (overwrite array + widgets)**. See §5.4 — the recommended mechanism is the same RESULT-mirror as firefly: host broadcasts each rolled `Fstruct_signal_spawn` (0x2C) + a periodic signals-array hash; client rebuilds its set (reflected `addSignal(vec)` then overwrite the rolled row fields w/ the wire struct, or full hand-build via WidgetBlueprintLibrary::Create — the firefly SpawnEmitterAtLocation precedent). VALIDATE the cheapest variant live (U5). |
| dish aim (`spaceRenderer.coords @0x0264`, FVector2D) + move flags | spaceRenderer | coords ride analogPanelsData? (coord_signalData yes; the AIM itself — VERIFY) | stream at ~2-5 Hz while the coords panel is claimed (KeyedScalar-like payload); remote spaceRenderer writes `coords` so dishes + a later enterer match |
| catch results (`gatherSignal` RandomBoolWithWeight @831, `coord_signalData`, coordLog) | desk + spaceRenderer | partially (coord_signalData in blob) | the BUSY PEER is the roller (one-at-a-time = the occupant's RNG is authoritative); results flow via the desk blob + savedSignals channel; `writeToCoordLog` text rides `coord_coordLogText` in the blob |

The desk ALSO exposes one-shot world effects from its dispatcher (drive eject
spawns `prop_computerBackPanel_*` via BeginDeferred @18955 + pickupObjectDirect;
radio/redphone; trifo spawn) — all PropSpawn-pipeline territory, already
covered by existing prop sync / M2 watcher patterns.

### 4.2 SAT console (panel_SATconsole + ui_console)

Screen state (ui_console.hpp): `LogText @0x02E0` (FString — the whole terminal
scrollback), `Command @0x0330` (current input line), `consoleLine @0x0340`,
`Active @0x0360`, `processing @0x0361`, `pingingServers @0x0368`,
`activeServer @0x0378`, `calI @0x03D8`/`calIng @0x03DC` (dish calibration
sweep), `used @0x0410` (the occupying panel), `servProgress @0x0418`.

- NOT save-persisted (a fresh widget each boot; the log resets) — the join
  needs nothing; parity for a later enterer = mirror `LogText` (+ `Active`)
  at claim-release; the in-progress typing (`Command`) is the busy peer's own.
- Console COMMANDS mutate world actors: dish calibration
  (`calibratteDish`/`tool_setDishCalibration` → dish_C state), server pings,
  status emails (`lib_C.addEmail` @12907/@15883/@18959 — ride the email
  channel §4.4). Those world effects are separate sync features (dish/server
  state) — out of scope here except the emails.
- MINIMAL mirror: `LogText` string in the release snapshot (chunk if > datagram;
  it can grow — cap to the last N KB, the UI only shows a screenful).

### 4.3 Radar (panel_radar + ui_radar) — DERIVED, claim-only

ui_radar renders moving world actors (`comp_radarPoint` carriers) from LOCAL
world state — which the existing sync already mirrors (players/props/NPCs).
View state (`Zoom @0x0350`, `Offset @0x0354`) is per-widget cosmetic;
include {zoom, offset} in the release snapshot or skip (classification:
claim-only). The MOAB/mortar control (`rocket @0x0390`, `loadingMortar
@0x03B8`, moabPoints) is a world-effect feature (out of scope; flag for the
events/weapons track). The radar lever (alarm trigger @150-268) is an
M3-category world trigger, not screen state.

### 4.4 The meadow PC (gamemode.laptop ui_laptop — base laptop + portable PCs)

Tabs (ui_laptop.hpp buttons @0x03F0-0x0430): adv(ancements) / cams / comps /
email / floppy / pics / signals / store / upgrades. State:

| tab/state | source of truth | persisted | RNG? | sync |
|---|---|---|---|---|
| **store catalog** | `generateStore` @5764→`list_store` DataTable rows + unlock gates (achievements, `save_main.storeItems`) | asset-static | **NO — deterministic** | none (regenerated on every enter @4435) |
| cart / order | `cart @0x0B10`, `makeAnOrder` | saveSlot.orders @0x0490 | n/a | **ALREADY SYNCED** (v49 economy OrderRequest; order list watermark-poll) |
| points | saveSlot.Points @0x0090 | yes | n/a | **ALREADY SYNCED** (balance_sync host broadcast) |
| **emails** | `saveSlot.emails @0x0118` (TArray<Fstruct_email>, 0x50/row: new, pfp(UTexture2D*), username enum, date FIntVector, topic FText, text FText) | YES | senders vary (below) | join: save-transfer; live: watermark-poll `emails.Num` → broadcast new rows → apply via reflected `gamemode.addEmail(item)` (gives the 'email' sound at the laptop + tab highlight for free — gamemode.addEmail @0-478). pfp: transmit the username enum + re-resolve the texture from `list_emailCharacters` row (don't ship texture ptrs). FText via ue_wrap/ftext_utils. |
| signals tab | `addSignal/removeSignal/playSignal` — rows are Fstruct_signalDataDynamic sent from the desk play screen (`gamemode.laptop.addSignal` @23859) | laptop-side list (VERIFY persisted array — likely rides saveSlot via its own array) | no | append/delete deltas alongside the savedSignals channel |
| tasks (hash-collection quests) | `saveSlot.Task @0x0128` (Fstruct_task 0x70: levels, dishesCode[], checkedDishes[], completed[], fails) + `taskNew @0x0CA8` (Fstruct_taskNew 0x48: sigRequired[], sigCompleted[], requiredDishes[], rewards) + `totalCompletedTaskParts @0x08A0` | YES | **task roll = daynightCycle RNG** (`createNewTask` @5691 → `lib_C::setTaskNew` @5714 + task emails @6671/@7094/@7900); progress via `lib_C::processTask`/`add_task` | host-roller: the host daynightCycle owns rolls (clock already host-auth, time_sync v36); client's own daynightCycle roll must be neutralized — the events-catalog scheduler verdict applies (sync at OUTPUT): mirror `saveSlot.taskNew/Task` structs on change + the task email rides the email channel. VALIDATE client-side daynightCycle behavior under synced clock (U6). |
| scientist/alien response mails | drone sell flow: drone uber @167 `lib_C::getResponse(sell_responseEmail, …)` (**RNG roll**: random char of 3 + random row from `list_charactersSignalResponse` topics/responses, getResponse @141-2098) → `addEmail` @251; special downloads: desk @4799-5068 (`looker_behind` → `list_signalEmailResponse_obsolete['special_the_evil']`, 5 s delay) | emails persisted | roll-RNG | the ROLLER is host-side already for the drone (drone is host-auth v48 + client drone tick suppressed). The desk special fires on the DOWNLOADING peer (the busy occupant) → its local `gamemode.addEmail` lands in ITS saveSlot → the email watermark channel carries it to everyone. Host-relay client-originated email rows (client → host → others) like OrderRequest. |
| cams tab | signalCam_C feeds render locally from world | n/a | no | none (world already mirrors) |
| files/floppy/pics/videos | inserted floppy/zip media (laptop_C drive hitboxes), photos | per-prop data | no | floppy/zip props ride prop pipeline; their CONTENT sync = separate feature (flag) |
| RC drone (`createRC`, inputRC_*) | the laptop's remote-control vehicle | n/a | no | out of scope (vehicle-category, ATV/drone precedent exists) |

### 4.5 Reactor panel (panel_reactor + ui_reactor)

The rod-control screen for base power. State = reactor rods/power (ui_reactor
+ the reactor actor). Power state is already in the power_sync orbit
(PowerPanelPayload exists, protocol.h:1699). Classification: **claim-only in
this feature**; verify during implementation that rod positions ride (or get
added to) power sync — separate channel if not (flag U7).

### 4.6 Transformer minigame panel (transformerMGPanel)

Per-instance minigame state (sine offset/freq/amp targets @0x0320-0x0328,
switches_states @0x02D8, isSineComplete/isRotatorsComplete/isSwitchesComplete
@0x0360-0x0362). The targets are rolled per attempt; the SOLVED result flows
into the transformer/power world state (power_sync territory). Classification:
**claim-only** (deny the second peer; the minigame in-progress is the
occupant's own; the result rides power sync — verify the transformer-fixed
edge is covered there, else flag).

### 4.7 Arcade (prop_arcade)

`ui_arcade_invaders` minigame — interactor-local entertainment;
`player.arcade @0x0930` set while playing. Classification: **claim-only**
(deny double-enter; no state mirror — RULE 1 "no fake screens" is satisfied
because a non-occupied arcade shows its idle screen from local widget state).

---

## 5. SYNC DESIGN SKETCH

Protocol tail today: `kProtocolVersion = 62` (protocol.h:509); last
`ReliableKind = LockerDoorState = 50` (protocol.h:1130) → **next free kind 51;
next version 63**. New module per the file-size rule: `coop/device_occupancy.{h,cpp}`
(busy channel + deny gate) and `coop/console_state_sync.{h,cpp}` (desk/console/
email/signal state) — interactable_sync.cpp (388) could host the busy adapter
but occupancy is not a keyed TOGGLE; keep it separate. ue_wrap additions:
`ue_wrap/console_desk.h` (analogDScreenTest field/fn wrappers incl. reflected
gatherData/setData), `ue_wrap/space_renderer.h`, `ue_wrap/laptop_ui.h` — no
gameplay logic in ue_wrap (principle 7).

### 5.1 New wire pieces (sketch)

| kind | dir | payload (≤ 228 B/datagram unless chunked) | notes |
|---|---|---|---|
| `DeviceClaim = 51` | client→host req / host→all broadcast | `{ uint64 deviceKey; uint8 slot; uint8 busy; uint8 _pad[2] }` | deviceKey = FNV of the key string (§5.2; wire_key_util precedent). Host arbitration §3.3. Include all current claims in the connect snapshot (join true-up). |
| `ConsoleDeskState = 52` | owner→host→all | the `Fstruct_signalTableState` essentials, hand-packed (~120-180 B without signalPlayData/byteImage; full blob on release/join via the bulk lane) | cadence: 1-2 Hz while the desk is claimed + edge-triggered on the cheap fields; full reflected `setData` apply on receivers (it runs the upd* refresh chain) |
| `DishAimState = 53` | coords occupant→host→all | `{ FVector2D coords; uint8 moveBits }` unreliable lane | 2-5 Hz only while coords claimed |
| `SkySignalSpawn = 54` | host→all | `Fstruct_signal_spawn` (0x2C) + ui lifetime float | host-roller; client spawner divergence handled per §5.4 |
| `EmailAppend = 55` | owner→host→all (host-relay) | `{ uint8 usernameEnum; FIntVector date; topic/text strings }` chunked like OrderRequest if long | apply = reflected `gamemode.addEmail` (re-stamps date from synced clock — acceptable; or write the array row raw + replicate the cosmetics) |
| `SavedSignalDelta = 56` | owner→host→all | add: Fstruct_signalDataDynamic serialized (strings + scalars + byteImage chunked over the bulk lane — the v56 save-transfer lane moves MBs, a few-KB image is trivial); del: index/id | apply via reflected `gamemode.saveSignal(...)` / `deleteSignal(idx)` |
| (reuse) | | task structs `taskNew`/`Task` raw-struct write on change | piggyback on ConsoleDeskState cadence or EmailAppend events |

(Exact kind numbering to be settled at implementation against the then-current
protocol tail; the table fixes the SHAPES.)

### 5.2 Device claim keys

| device | key string |
|---|---|
| desk coords panel | `"desk:coords"` (singleton via gamemode.analogPanels) |
| SAT console | `"sat:" + Key@0x0230` (actor_save FName; root console's Key — VERIFY non-empty on placed instances; fallback `"sat:root"` for gamemode.rootConsole) |
| radar | `"radar:" + Key@0x0230` |
| reactor | `"reactor:0"` (no save Key; singleton — VERIFY) |
| meadow PC | `"laptop"` — ONE claim for base laptop + every portable PC (they share the single `gamemode.laptop` widget; two peers in "different" laptops would type into the same screen — the shared claim is REQUIRED, not a simplification) |
| transformer panel | `"tfm_" + <owning transformer identity>` (VERIFY the panel→transformer backref field; fallback: position-hash key like the pile position-bind precedent) |
| arcade | `"arcade:" + propEid` (prop registry) |

### 5.3 Claim-release snapshot ("not frozen mid-minigame")

On falling edge the releasing peer sends, with the release: desk → full
reflected `gatherData` blob (authoritative final state incl. coordLog text);
SAT console → `LogText` tail + `Active`; radar → {zoom, offset} (optional);
laptop → nothing extra (emails/orders/signals channels already carried every
mutation live). Receivers apply (desk: reflected `setData`) so the NEXT
enterer — on any peer — sees exactly what the previous occupant left. This,
plus the live cadence, satisfies "the visible screen not frozen mid-minigame".

### 5.4 The sky-signal mirror (the one RNG-roller redesign)

Host = the only roller. Client-side the `spawnSignal` self-arm CANNOT be
PE-suppressed (EX_LocalVirtualFunction); options, in RULE-1 preference order:

1. **State-overwrite mirror** (recommended; matches the door HostAuth
   "field-the-BP-gates-on" shape): let the client roll locally BUT immediately
   reconcile — the net-pump compares `spaceRenderer.signals` against the
   host-authoritative set (hash in ConsoleDeskState cadence): remove
   non-authoritative rows (Array ops via reflection + `deleteSignal(i)` on
   spaceRenderer, which also despawns the ui_signal widget — fn exists, 16 fns
   on spaceRenderer) and inject missing host rows (reflected `addSignal(vec)`
   then overwrite the rolled fields of `signals[last]` + the paired
   `signals_a[last]` widget props via SetXPropertyByName — exactly how the game
   itself builds them @896-1719).
2. Timer kill: locate the client's pending `spawnSignal` timer via
   `K2_SetTimerDelegate` handle and clear it once per session
   (`UKismetSystemLibrary::K2_ClearTimerDelegate` via reflection) — then ONLY
   wire rows exist. Cleaner runtime, but the timer re-arms inside the BP each
   fire (@3819 BIND + @3880 SetTimer) — clearing once suffices ONLY if our
   injection path never re-triggers the BP's own re-arm (it doesn't —
   `addSignal` doesn't re-arm; only `spawnSignal` does). VALIDATE (U5).
   With the timer dead, option 1's reconcile becomes a cheap assert.
3. Catch-the-roll-and-replace: not possible (no PE seam).

The `gatherSignal` CATCH roll needs nothing: the coords occupant is the only
roller by occupancy design; results flow via desk state + savedSignals.

### 5.5 What rides existing channels (NO new work)

- Points: balance_sync. Orders/shop: v49 economy. Time/day: time_sync v36.
- Join screen-state: v56 save-transfer (`saveSlot.analogPanelsData`, `emails`,
  `Task/taskNew`, `savedSignals_0`, `orders`, `Points` all in the .sav).
- Drive props, portable PC props, arcade props: prop pose pipeline (eids).
- Power/reactor/transformer outcomes: power_sync orbit (verify edges, U7).
- World effects from console commands (dish calibration, server states):
  separate features; the events catalog M1/M2/M3 covers their triggers.

### 5.6 Classification summary (every device)

| device | busy claim | screen-state mirror | already covered |
|---|---|---|---|
| desk coords (signal catcher) | YES | YES — desk blob + dish aim + sky signals + coordLog | join via save-transfer |
| desk download screen | n/a (button-operated) | YES — desk blob (DL_*) | join via save-transfer |
| desk comp screen (signal refiner) | n/a | YES — desk blob (comp_*) + drive Data_0 swap | join via save-transfer |
| desk play screen (signal saver/playback) | n/a | YES — desk blob (signalPlayData, play_selectIndex) + savedSignals deltas; playback AUDIO at the desk = phase-2 spatial cue | join via save-transfer |
| SAT console (terminal) | YES | LogText tail at release (+ live optional) | world effects separate |
| radar | YES | optional {zoom,offset} | contents derive from world sync |
| meadow PC (laptop family) | YES (one shared claim) | emails/signals/tasks deltas; store deterministic | orders+points already synced |
| reactor panel | YES | claim-only here | power_sync (verify) |
| transformer panel | YES | claim-only | power_sync (verify) |
| arcade | YES | local-only | prop pipeline |
| serverBox monitors | NOT enterable | — | separate minigame feature |
| clipboard/propInv/paper/texturePicker | NOT devices (screen-space) | — | — |

---

## 6. Explicit unknowns / pre-implementation validation steps

- **U1 — placed instance counts + Keys**: live GUObjectArray walk for
  panel_SATconsole/panel_radar/panel_reactor/transformerMGPanel instances on a
  fresh save; confirm save Keys are set on placed panels (the `setKey` flow) —
  determines the claim-key fallbacks (§5.2).
- **U2 — exitInterface delegate PE-visibility** (expected-YES, not measured):
  POST observer on `laptop.exitLaptop`, exit once, expect one line. Nice-to-have
  only (poll already covers exit).
- **U3 — does any enter path bypass `InpActEvt_use`?** The widget-click path
  (`WidgetInteraction`, `bullshitWidgetClick`) and gamepad bindings: hands-on —
  while a device is wire-busy, try entering via every input route; if any
  bypasses the PRE gate, fall back to the transformer collision idiom
  (`useBox`-equivalent per device) as a second fence + keep the sound at the
  PRE layer.
- **U4 — FHitResult field offsets** (+0x68 Actor, +0x70 Component) — standard
  4.27 layout consistent with size 0x88; confirm once in IDA or by live read
  before shipping the PRE-clear.
- **U5 — sky-signal mirror variant**: validate reflected
  `K2_ClearTimerDelegate` kills the client `spawnSignal` re-arm for good
  (§5.4.2) vs the reconcile loop (§5.4.1); measure `signals.Num` divergence
  with both peers idle 10 min.
- **U6 — client daynightCycle under synced clock**: does the client roll its
  own `createNewTask`/task emails today (duplicate divergent tasks)? Run both
  peers to the task-roll hour; diff saveSlot.taskNew + emails. The events
  catalog flagged the same question for `settime` (M1 ⚠).
- **U7 — reactor/transformer result edges in power_sync**: confirm
  transformer-fixed and reactor rod state propagate; else add to power channel.
- **U8 — drive-slot mirror insertion**: does a mirrored drive prop overlapping
  `driveSlot.drivePort` fire the slot's BndEvt on the non-owning peer
  (double-insert)? Tick-suppression on prop mirrors may not stop overlap
  events.
- **U9 — `ui_laptop` signals-tab persistence**: find the array backing the
  laptop signals list (saveSlot field or widget-local) — determines whether
  the laptop-signal delta needs its own join true-up or rides save-transfer.
- **U10 — desk `setData` mid-session safety**: setData runs upd* refreshers;
  verify no side effect fights a LIVE claim (apply only when the desk is
  unclaimed or on the claim owner's authority).

## 7. File/precedent map (for the implementer)

- Busy gate + deny: `coop/device_occupancy.{h,cpp}` — PRE/POST pair shape from
  interactable_sync.cpp:200-300 (`OnUseInputPre`/`OnUseInput`, single-slot
  restore discipline, 300 ms debounce); deny sound via the prop_sound /
  weather_lightning reflected-PlaySound2D pattern.
- State sync: `coop/console_state_sync.{h,cpp}` — watermark polls (order_sync
  precedent), KeyedScalar-style streams (wind windTarget precedent), bulk lane
  for byteImage (save-transfer v56), reflected applies (garage `ApplyOpen`,
  economy `CommitOrder` ParamFrame building).
- Keys/identity: wire_key_util FNV; actor_save Key @0x0230 (door/keypad
  channels read the same field).
- MTA conceptual precedent: server-arbitrated entity locks (vehicle entry
  occupancy — `CVehicle` occupant slots, first-claim-wins + server relay);
  cite in the PR per RULE 2026-05-28.
