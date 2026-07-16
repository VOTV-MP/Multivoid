# Signal chain units 1-4 + drive + eraser — full RE (2026-07-16)

Byte-level RE of the whole signal-desk chain, produced from the user's 2026-07-16 gameplay
walkthrough (units 1..6 conspect in the session scratchpad). Sources:
`research/bp_reflection/{analogDScreenTest,driveSlot,prop_drive,signalDriveEraser,lib,mainGamemode,ui_consolesAtlas,ui_coordinates,spaceRenderer,ui_laptop}.json`
+ `_analogd_uber_full.txt` (uber byte offsets `@N`) + `CXXHeaderDump/*.hpp` (offsets). Statement
cites: `fn[i]` = statement index inside the named function, `@N` = uber byte offset. Everything
below is [MEASURED] static bytecode unless tagged otherwise. Companion docs:
`votv-dish-rotation-RE-2026-07-16.md` (the 24 big dishes),
`votv-tape-caddy-daily-task-RE-2026-07-16.md` (wallunit_tapes + daily task),
`votv-desk-sim-v111-coop-bugs-audit-2026-07-16.md` (OUR-code bug root causes).

**One actor.** All four desk units are panes of ONE `AanalogDScreenTest_C`: unit 1 = the
`coord_*` cursor/triangulation pane, unit 2 = the `DL_*` STOLAS download pane, unit 3 = the
`play_*` ASO deck (`obj_driveSlot_play @0x0648`), unit 4 = the `comp_*` AUTO
PROCESSING AND ENCRYPTION pane (`obj_driveSlot_comp @0x0A88`). The caddy is a separate
`Awallunit_tapes_C` (level-baked pointer `tapeObject @0x1360`); the eraser is
`AsignalDriveEraser_C`.

---

## 0. THE KEY ROUTER (corrects the 07-15 RE §C)

The desk keyboard verbs (SHIFT/1/2/3/ENTER/arrows) do NOT enter through the panel actor's input
events (`playerHandAnyKey`/`intComs_anyKey` uber entries are bare POP). The router is
**`ui_consolesAtlas.OnKeyDown / OnKeyUp`** — the shared UMG widget (`atlas.panel -> the desk`,
`atlas.ui_coordinates @0x0588`). Keys resolve by KEYBIND NAME via `lib_C.getKeybindFromName`:
`coord_quickScan` (SHIFT), `coord_ping` (ENTER), `coord_bringTarget_1/2/3` (1/2/3),
`coord_switchTarget`, `coord_right/left/up/down`. Gates at the top of OnKeyDown [2-8]:
`controllingCoordinatePanel`, `bIsFocusable`, **`coord_isPing` (a running ping swallows ALL
keys)**, `active_coords` (power). Every accepted keydown plays `audio_coordKeyPress`.
Mouse-click screen buttons are a SECOND path to the same verbs (`screenbutton_switchToCoord1/2/3`
-> `changeCursor(0/1/2)`, `screenbutton_bringCoord1/2/3` -> `movePointToCursor(0/1/2)`) which
SKIPS the OnKeyDown gates. Enter/exit control: `button_coords` -> `controllingCoordinatePanel :=
true` @29313 + `PC.bEnableClickEvents`; `intComs_unfocused` -> @78784 clears both + all
`master_spaceRenderer.move_*`.

**Dispatch: OnKeyDown/OnKeyUp are PE-VISIBLE widget input (fires on the occupant's machine
only).** Everything downstream is EX_Local*/inline.

## 1. UNIT 1 — cursor / triangulation pane

### Verbs
- **SHIFT = quick scan** (`useSearch` thunk -> @83010): if `coord_cooldown <= 0` -> @79696
  `playPingSound(newdesk_beepLong1)` + **`spawnDirs()`** + `coord_cooldown := coord_maxCooldown`
  + log `<c>Initializing quick scan...`. Else @79636 `newdesk_beep4` + `<r>Cooldown error`.
  **SHIFT does NOT touch the ping FSM** (the 07-15 doc's "SHIFT sets coord_pingStage=3" was
  wrong — corrected there). `spawnDirs` loops `master_spaceRenderer.signals_a` and creates one
  **`ui_coordArrow_C` UMG widget per signal** (NOT an actor): `lookat`=signal.coordinates,
  `renderer`, `alpha`; added to `ui_coordinates.CanvasPanel_245`, 512x512 centered. The arrows
  self-orient in their own widget tick.
- **1/2/3 = place dot** (`ui_coordinates.movePointToCursor(0/1/2)` [0-17]): gate 1
  `isCoordinateWorking(selected)` — reads the 3 physical tower dishes
  `atlas.panel.pingDishes[i].isBroken` (`AcoordRadarDish_C`), broken -> log error +
  `audio_coordFail`; gate 2 **`coord_cooldown <= 0`** else `Cooldown error` + `newdesk_beep4`.
  Action: `coordinate_{0|1|2} := viewCoordinate`, **`coord_cooldown := getMaxCooldown()/2`**,
  log `Initiating automatic radar positioning...`, `newdesk_beepLong1`. Dot visuals
  (`canvas_vert_0/1/2`) interp toward `coordinate_0/1/2` at `coordRadarsSpeed`
  (`updMainCursor` [61-81]); `coordsInPlace` = all three within 0.1; while moving each tower dish
  live-rotates (`pingDishes[i].setRot(getHorizontalAngle(...))`, `updCursorLocations` [82-105]).
  The desk's red/green/blue bulbs (`eff_coordbulb_*`) are TOWER-STATUS indicators (blink timers
  `timer_cr1/2/3` on `stateChanged`), not dot markers.
- **ENTER = triangulate/ping** (atlas OnKeyDown [30-60]): preconditions — towers working;
  **no `gamemode.activeDishes` true** (else `Error code [1] Satellites are active`);
  `ui_coordinates.canPing`. On pass: **`coord_isPing := true`** (the external set-TRUE the 07-15
  doc couldn't find — it lives in the atlas widget), `Pinging...`, move_* cleared,
  `newdesk_panelCoord_pingStart` + `audio_coord_pingLoop` on, ring floats zeroed,
  `coord_pingStage := 0`.
- **Cursor motion**: OnKeyDown/Up write `master_spaceRenderer.move_right/left/up/down`;
  spaceRenderer's tick integrates `movement` and writes back via
  `coordinatesUI.setCoordinateLocation(getCursorLocation()+movement)` — writes
  `coordinate_{selected}` or `viewCoordinate`. Movement loop sound `corrds_loop`.

### The cooldown — ONE shared field
**`coord_cooldown @0x0A80`** serves BOTH verbs: SHIFT charges to `coord_maxCooldown @0x0C08`,
1/2/3 charge to `getMaxCooldown()/2`. `getMaxCooldown() = Lerp(30, 5, (upg_coordCooldown/16)^2)`
seconds. Decay: per-tick `VictoryFloatMinusEquals(coord_cooldown, dt)` + FMax 0 (@83284, inline
native — PE-invisible). Display `txt1_cooldown` = round(cooldown*10). The ENTER ping has NO
cooldown field — its lockout is `coord_isPing` itself.

### The small log screen ("chat")
Buffer **`coord_coordLog2Text @0x0BF8`**; writer **`writeToCoordLog_2(B)`**: `Right(log + B +
CRLF, 1000)` -> `RichTextBlock_coordLog` + autoscroll. (`coord_coordLogText @0x0A68` +
`writeToCoordLog` are DEAD — bare RETURN, field written nowhere.) Inline EX_Let, invoked as
EX_Local* — PE/Func-invisible end to end. Periodic pump: event `coordLog2` on a 0.2 s timer ->
while pinging writes `AREA SCAN/APPROXIMATION/ANALYSIS: [bar] N%` (switch on `coord_pingStage`);
while `coord_cooldown > 0` writes `CDOWN: [bar] N%`; while any `move_*` writes the `CR:[A:y||B:x]`
cursor readout. Per-verb appends (census): SHIFT ok/cooldown-error; scan results `Error [2] no
signal in local area` @79368 / `Error [6] inner circle too small` @78121; 1/2/3 ok/cooldown/tower
errors; `CURSOR MODE: [n]`; MOVE_R/L/U/D edges; `Pinging...`; catch results (`Successful ping.
Initializing satellite rotation...` @9404, `Error [3] Invalid object`, `Error [4] Triangulation
rotation error`, `Error [5]` unreachable — below); `Signal data deleted` @34099; per-signal
percent lines from `gatherSignal`.

### Triangulation math + the catch
`ui_coordinates.updCursorLocations` [6-80]: `coordinate_middle @0x03DC` = incenter;
`line_0/1/2`, `perim` = semi-perimeter, **`innerCircleRadius @0x03E4` = inradius (Heron)**;
**`canPing @0x0420` = all sides <= 740 AND inradius > 1 AND coordsInPlace**; `direction @0x0441` =
triangle winding sign. Ping FSM (panel tick, gated `coord_isPing @0x0A7C`): stages 0/1/2 integrate
`coord_ping_outer/inner/ping` at rate `dt/5 * Lerp(1,4,(upg_coordPingSpeed/16)^2) /
(inradius/100 + 1)` — bigger triangle = slower; stage transitions play
`newdesk_panelCoord_stageChange`; stage 2->3 stops the loop -> **`gatherSignal`**.

`spaceRenderer.gatherSignal` [0-160]: per signal, overlap `percent` of the signal circle
(radius = strength*50) vs the ping's inner circle; **`prob = percent ^ Lerp(2.0, 0.333,
(upg_triangleProb/16)^2)`; `gathered = RandomBoolWithWeight(prob)` — THE CATCH IS RNG** (per-peer;
host-auth already covers the catch result via v70 `coord_signalData`, but the ROLL is local to the
pinging peer). Post-catch panel chain @9124-@10444: `objectName != None` else Error[3]; **winding
must match** `gatherSignal_dir == ui_coordinates.direction` (or `has_autoCoordRot`) else Error[4];
then `master_spaceRenderer.deleteSignal(element)`, log success, **all `gamemode.dishs[i]
.startMovingTo(...)`** (see the dish RE), `newdesk_panelCoord_pingSuccess`, DL_* structs reset.
Cleanup: every terminal path -> Delay(1.0 s) latent @5069 -> `coord_isPing := false`, rings zeroed,
`coord_pingStage := 0`. Dead/unreachable: `gatherSignal`'s `skipCheck` param never read;
`radiusCheck` hardwired -> `Error [5] Signal stronger than the sensor` unreachable in 0.9.0n;
`spawn_trifo`/tardis spawns belong to the SAVE-button special-name switch, not triangulation.

### Unit-1 sound census
`newdesk_beepLong1` (SHIFT ok + dot ok), `newdesk_beep4` (cooldown errors),
`newdesk_panelCoord_pingStart/pingSuccess/pingFailed/stageChange/pingChangeCursor`,
`buttonquick1` (dot press), `buttonlong4` (switchTarget), `buttonlong5` (quickScan),
`buttonmetallic2` (ping key), `buttonshort14` (satellites-active error); loops:
`audio_coord_pingLoop @0x0390`, `corrds_loop` (cursor movement); one-shot channels
`audio_coord_pingSound @0x0388` (playPingSound = SetSound+Play), `audio_coordKeyPress`,
`audio_coordFail`.

## 2. UNIT 2 — STOLAS download pane: sounds, weather, save/delete/lid, phone

(The download rate formula + knob integrators are in `votv-desk-download-machine-RE-2026-07-15.md`
— not restated.)

### Sound map (all per-tick modulation inside `calculate_download`)
| Component (offset) | Asset | Driver |
|---|---|---|
| `newdesk_beepLoop_polarityMove` @0x0370 (template vol 0) | same-name cue | vol = `|DL_poFilterSpeed| > 0 ? 0.2 : 0` @61326 (HARD step!=0 gate); pitch = `Lerp(0.5, 1.5, DL_poData)` @61490 |
| `newdesk_radioLoop_frequency` @0x0360 | radio static | `SetActive(DL_activeFrFilter && active_download)` — the toggle LOOP the user hears |
| `newdesk_radioLoop_frequencyTune` @0x0368 (vol 0) | tune whine | `snd_filtFreq @0x13F8 = FInterpTo(., clamp(|DL_FrFilterSpeed|,0,10)/10, dt, 1000)`; vol = `Lerp(0,1,DL_frData) * snd_filtFreq * 0.5` @63034; pitch = `Lerp(0.2, 2.0, DL_frData^8)` @62899 |
| `beep_detecProcess` @0x0560 | `newdesk_beep3` (vol .04 pitch .5) | each repeat-play cycle while needle < 1 @4641 |
| `beep_detecFinish` @0x0550 | `buttonlong6` (vol 1.5) | ONCE when `DL_resDetecPercent >= 1` @4479 (+ `canSaveSignal`) |
| `beep_detecEmpty` @0x0558 | `blip1` | idle blip when no signal armed && `DL_soundActiveBlip` (CDO true, never written) |
| toggles | `button_computer_turnon/turnoff` 2D | on press |
| `computerHum_downl/coords/play` | hum cues | `sound_buzz @0xFE1` settings mute via `soundUpdated()` |

`updToggles` [0-21] owns the loop SetActives (runs on toggle press @25439, power change @32902).
**User's rules all CONFIRMED**: sounds only while step != 0 (the two speed gates), toggle
ON = loop start / OFF = stop, detector-100 stinger = `beep_detecFinish`.
**Download-100 has NO dedicated stinger** [MEASURED absence] — `decoded >= size` only recomputes
`canDL @0x1468` (SAVE-button glow `eff_downlSave`); the audible "done" is detector-side only.
Repeat-play cadence: `DL_alpha += dt` vs `DL_duration = Lerp(1.0, 0.333, (upg_scannerFr...)^3)`
(CDO 0.4 s).

### Weather
`@63215-63465` at the head of every `calculate_download`: **`DL_rain @0x0A08 :=
gamemode.daynightCycle.rain`** (`AdaynightCycle_C.rain @0x02E0`) — UNLESS `physMods` contains
module byte 6 (weather shield) then 0. Consumption `@65765-66426`, every tick, even with no
signal: **both knob offsets random-walk**:
`DL_{po,Fr}FilterOffset += dt * RandomFloatInRange(-1,1) * RandomFloat()^3 * Lerp(0.01, 10.0,
DL_rain)`. So weather = multiplicative jitter AMPLITUDE on the filter offsets (0.01 baseline
wobble -> 10.0 in a storm), NOT a rate-formula term; degradation follows via frData/poData
recompute. Independent from the rate formula's `DL_noiseScale` noise. Both are per-peer RNG,
dispatch-invisible.

### SAVE SIGNAL (unit 2) / autoSave
Button `button_downl_saveSig1 @0x0590` -> @25832. Gate **`DL_resDetecPercent >= 1`** (NOT
decoded>=size — a partial download is saveable once the detector is done). Special-name switch on
`DL_SignalDownloadDLData.signal` (`virus`/`shitDuende`/`process`/`tardis`/`trifo`/`ker` specials)
else: screenshot `TextureRenderTarget2DToBytes(DL_current)` -> `image`; name = `"<ObjectType> [N]"`
(counter `saveSlot.getSigObj`); **`lib::setSignalID`** (see §5-ID); **`gamemode.saveSignal(data,
...)`** (EX_LocalVirtualFunction — PE-invisible) -> `savedSignals_0.Add` + widget list rebuild;
if physMod byte 5: also `gamemode.laptop.addSignal` (auto-forward to the Meadow DB); then clears
`DL_signalDownloadData`, `gamemode.deleteActiveSignal()`, `addGloss`, `button_computer_click`.
**No dedicated "saved" jingle.** `autoSave` (event @83504) = literal JUMP into the same block;
in-asset caller: the `looker_behind` special (forces resDetec=1, decoded=size, saves).

### Lid + DELETE SIGNAL
Lid = the `Cap` mesh @0x0460 is itself the button ("Open lid"): `capOpened @0x0FE0 = !capOpened`;
**`button_downl_delSig.SetCollisionEnabled(capOpened)`** — the reveal is purely collision; lid
animates `MoveComponentTo` 0.25 s; **no lid sound** [MEASURED absence]. DELETE
(`button_downl_delSig @0x0588`, label `[[[Delete signal]]]`) -> if `active_download` ->
**`gamemode.deleteActiveSignal()`**: fans out `analogPanels.intComs_signalDeleted()` +
`objectRenderer.deleteSignalActor()` + kills sigCam — i.e. aborts the CURRENT space signal (does
NOT touch saved arrays; saved-list removal is `gamemode.deleteSignal(idx)` = unit-3 export only).
The desk's `intComs_signalDeleted` handler then resets DL_* locals + logs `Signal data deleted`.

### Red phone
Label literally "Doesn't work" — **no click handler exists**. Native FSM: BeginPlay arms
`redphoneEventTimer` looping at `RandomFloatInRange(3600, 4500)` s; on fire
`RandomBoolWithWeight(0.025)` -> `startRinging()`: `lib::setEvent(true)`, 0.5 s blink timer
(handler `timerRedphoneLight` is a NO-OP — dead code), `tryRinging()`: if player camera within
350 uu -> stop + `setEvent(false)`; else `audio_redphone.Play()` (vol 2.0) -> Delay 3.5 s -> loop.
Ringing = a rare per-peer RNG world event; approach cancels silently.

## 3. UNIT 3 — ASO play deck (`play_*`, `obj_driveSlot_play`)

### The saved-signal list
Store: **`AmainGamemode_C::savedSignals_0 @0x0968`, `TArray<Fstruct_signalDataDynamic>`** (stride
0x70; row layout in `ue_wrap/signal_dynamic.h`: name@0x00, level@0x10, id@0x18, size@0x28,
decoded@0x2C, date@0x30, isCopy@0x38, polarity@0x3C, loc@0x40, object@0x48, signal(template
key)@0x50, image bytes@0x58, freq/qual/objType@0x68-6A, downloadedAtQuality@0x6C). Appender:
`gamemode.saveSignal` (gate `decoded>=size && size>0`; stamps date; uniques ->
`saveSlot.specials`; **NO CAP**; rebuilds the list UI `Create Signal List`). Delete:
`gamemode.deleteSignal(i)` = `Array_Remove` + rebuild. Legacy `savedSignals @0x0388`
(`Fstruct_signal_data`) is a leftover still read by the widget's "more below" arrow only.
Selection: `play_selectIndex @0x06C4`; scroll = E on `button_play_scroll` binds
`event_up/event_down` to `mainPlayer.input_scrollUp/Down` delegates (+1/-1, no upper clamp —
`playSignal` guards with IsValidIndex). The row display "-1" = the row's `level` for a raw
never-processed signal [INFERRED — uicomp_signalSlot not dumped].

### Play / Stop / volume
`button_play_right`: `signalSound.IsPlaying() ? stopSound() : playSignal()`. `playSignal`
(@76909): gates `active_play` + valid index + `downloaded >= size` (deny beep otherwise);
`lib_C.dynamicToSignal(row)` -> template from the `list_signals` DataTable by `row.signal`, then
SWITCH on `decodedLevel` 0..3 -> `play_signalImage/play_SignalSound/play_signalText` :=
raw/low/noisy/high variants (level-0 = the raw noise the user hears); `play_nrt :=
lib_C.getNRT(sound)` (UConstantQNRT — the spectrum analyzer); binds `playback` ->
`signalSound.OnAudioPlaybackPercent` + `fin` -> `OnAudioFinished`; `SetSound` + `Activate`.
`playback` -> `setPlayPlayback(alpha)`: spectrum bars from
`GetNormalizedChannelConstantQAtTime` + `RandomFloatInRange(0,1)^8 * 0.05` jitter (per-peer RNG,
cosmetic); typewriter text = `LeftChop(text, Lerp(len,0,alpha))`. `fin` -> `stopSound()` =
`Deactivate` + unbind + `endCanvas`. Volume: `button_play_vol` binds scroll ->
`play_volume @0x06D0` [0..50] -> `signalSound.SetVolumeMultiplier(clamp(vol/10, 0.1, 5.0))` +
"VOL: N0%" overlay (3 s autohide). NOTE `signalSound @0x570` is a world-space audio component —
the playback is audible at the desk, so a non-occupant peer standing nearby natively hears
NOTHING unless mirrored.

### IMPORT/EXPORT (the drive slot) — MOVE semantics
`button_play_left` -> gates click/`active_play`/`IsValid(obj_driveSlot_play.drive)`; branch on
**`drive.data_0.size > 0`**:
- IMPORT (drive -> list): `gamemode.saveSignal(drive.data_0, ..., selfQuality=true)`;
  `drive.data_0 := zero-struct`; `drive.upd()`.
- EXPORT (list -> drive): `drive.data_0 := savedSignals_0[play_selectIndex]`;
  **`gamemode.deleteSignal(play_selectIndex)`** — the row LEAVES the list (true move);
  `drive.upd()`. Both end `stopSound()`.
The deck-side `driveIn_play/driveOut_play` handlers are tutorial-hint-only; `driveDetached` and
`intComs_signalSaved` on the desk are no-ops.

### SARV MIN/MAX
Backing field `remapValue` FVector2D @0x1324 (note the crossed internal naming: the MIN button
binds `scrollRemapMax*` handlers and vice versa — measured as-is). Scroll steps +-0.1, clamp
[-10,10]; `updRemap()` pushes X/Y as `remap_min`/`remap_max` scalar params into
`dynmat_vis_spect/vis_graph/signalImage` — SARV is a pure DISPLAY value-range (contrast) remap of
the visualizer/graph/image panes; `visibilitySarvText` = the 3 s "min/max" overlay. No signal
data touched.

### SAVE SIGNAL (unit 3) -> the Meadow DATABASE
`button_play_saveSig @0x0240` -> row = `savedSignals_0[play_selectIndex]`, gate
`decoded >= size && size != 0`, -> **`gamemode.laptop.addSignal(row)`** — a COPY (the row stays
in the deck list). `ui_laptop.addSignal`: **`saveSlot.savedSignals_0 @0x0680`.Add** (the
persistent Meadow DATABASE store; sibling `savedSignals_comp_0 @0x0690` = the comp-processed DB)
+ creates the DATABASE-tab row widget. So there are THREE stores: gamemode.savedSignals_0 (deck
list) / saveSlot.savedSignals_0 (Meadow DB) / saveSlot.savedSignals_comp_0 (processed DB).

### Signal ID generation (identity fact)
**`lib_C.setSignalID`** — called ONLY from the unit-2 save chain (and unit-4 level-up): if `id`
empty -> **`GenerateRandomBytes(16)` -> `BytesToBase64Url`** (the `_BRIJ0ceq...` string — pure
per-peer PRNG, NOT a FGuid); if non-empty (copy) -> `Pearson(bytes,16)` -> Base64Url
(deterministic from the parent). Unit 3 never mints ids. Nothing in the game looks rows up by id.

## 4. UNIT 4 — AUTO PROCESSING AND ENCRYPTION (`comp_*`, `obj_driveSlot_comp`)

Confirms the standing claim: the coop "refiner pane" (CompState=60/CompData=61) IS this unit.
Screen mapping: DATA=`text_comp_data`, File size=`text_comp_size`, Progress=`text_comp_progress`,
Process=`text_comp_process`, Efficiency=`text_comp_eff` (= `comp_downloading*100*size` B\s),
Consumption=`text_comp_cons` (= `FMax(powerRatio*100 - RandomFloat()^10, 0)` — per-peer RNG,
cosmetic), Target level=`text_comp_maxlvl`, right console=`text_compConsole`.

### Per-tick sim (`calculate_comp`, gated `active_comp` + `comp_isDecodeActive`)
```
rate = comp_speed * dt
     * (1 - clamp01((RandomFloat()-0.5) * comp_noiseScale))   [PER-PEER RNG]
     / FMax(comp_noiseScale, 1) / comp_data_0.size
     * BoolToFloat(usesp_calc) * powerUsage
     * (comp_ignoreServers ? 1 : serverEfficiency_calc^6)
     * Lerp(1, 5, (upg_processSpeed/16)^2)                    [UPGRADE]
     * comp_processMultiplier * comp_diffMult                 (difficulty 1.25..0.75)
     * freqMult(comp_data_0.frequency)                        (enum 0..8 -> 1.0/0.5/0.75/0.9/1.0/1.1/1.25/1.5/1.0)
comp_downloading := rate; comp_progress += rate               (VictoryFloatPlusEquals)
```
Completion (`comp_progress >= 100`): latch off, **`comp_data_0.level += 1`**,
**`lib.setSignalID(comp_data_0)` — the payload gets a NEW ID at every level-up** (drive ID is NOT
stable across processing), `isCopy := false`, `addGloss(signal, level)`; if level==3:
`stats.signals_processed++` + per-signal world triggers (`'evil'` -> deferred-spawn `theEvil_C`;
`'lifecrystal'` -> rozship trigger; `'deer'` -> deadDeer trigger). Beep: `prog` (more levels to
go) or `Done` (capped). **Auto-continue to the next level ONLY if `physMods` contains byte 3**;
without the module every completion stops after one level (`computerWorking_end` wind-down).
Non-completion tail: the right-console "hecer" mining char-stream (`comp_mining` accrual + RNG
easter eggs `AMOGUS` p=0.005 — per-peer RNG, cosmetic).

### Verbs
- **Import/Export** `button_comp_upload` (needs drive-in-slot + `active_comp`):
  `comp_uploadData(drive, succ)` — refuses while `comp_isDecodeActive` (deny audio); a SWAP-mover:
  drive full -> `comp_data_0 := drive.data_0`, drive zeroed; drive empty -> reverse. Payload
  MOVES between fields; nothing spawned/destroyed; `drive.upd()` repaints the LED.
- **Start** `button_comp_start` -> `comp_start(0)`: gates IN ORDER: `level < 3` (hard cap), not
  active, `size > 0`, **`comp_data_0.level < saveSlot.upgrades.upg_processLvl` — THE UPGRADE
  GATE** (early game upg_processLvl=0 -> nothing processable). Pass -> latch on, progress := 0,
  per-level Process text, `computerWorking_Cue` loop.
- **Stop** `button_comp_stop` -> `comp_stop()`: latch off + wind-down sound.
- **Change max computing level** `button_comp_maxLevel`: `comp_maxLevel = (comp_maxLevel+1) % 3`;
  "Target level" text shows a number ONLY if `physMods` contains byte 3 — else the NaN literal
  (the user's screenshot); level lamps `eff_lvl1..3` need `active_comp && upg_processLvl != 0 &&
  physMods.Contains(3)`.
- `pingComp`: a BeginPlay one-shot 3 s timer printing `Pinging... Status: [FINE]` — cosmetic
  boot line, measures nothing.
- Physical modules: `plugInModule` (mod1..mod12 overlaps @57159) — module prop's byte ->
  `physMods[slot]`, **module prop DESTROYED**; hot-plug while any unit powered and
  !coldswapEnabled -> `explotano` (explosion). physMod byte 3 = the level-cap/auto-continue
  module; byte 5 = laptop auto-forward; byte 6 = weather shield; byte 21 = tape compression.

## 5. The drive chain — `AdriveSlot_C`, `Aprop_drive_C`, `AsignalDriveEraser_C`

### driveSlot FSM (identity-critical)
**The drive prop is NEVER destroyed on insertion**: `putDriveIn(actor)` -> cast; `drive := it`;
collision off; `driveIn.Broadcast(drive)` (PE-VISIBLE); `setPropProps(frozen=TRUE)`;
`K2_SetActorLocationAndRotation(drivePort)` (teleport, NO attach); `dropGrabObject()`;
`OnDestroyed += dest`; `drive.upd()`; **`drive.slot := self`** (@0x378). Identity survives;
nothing is copied. Insert triggers: port BeginOverlap (throw-in; refused while
`isRecentlyDetached` or occupied) or E-with-drive-in-hand (`playerUsedOn` ->
`simulateDrop` -> `putDriveIn(lastDroppedItem)`). Eject (`drivePulledOut`): unbind dest,
`drive.slot := None`, `drive := None`, `driveOut.Broadcast()`, **`isRecentlyDetached := true`**
(cleared only by port EndOverlap — the drive must physically leave the volume; anti-bounce
latch), collision on. Eject triggers live on prop_drive: `playerTryToGrab` (slot ->
drivePulledOut), `playerTryToHold/Collect` -> `driveAction` (lift +8Z, pull out, re-enable
physics, hold or pocket), and `dest` (destroy-while-inserted cleans the slot). Dev leftover: the
slot's ReceiveTick PrintStrings the drive's display name on screen every tick while occupied.

### prop_drive payload
**`Data_0 @0x0550`** (Fstruct_signalDataDynamic 0x70 — layout in §3). Writers: construction
(from `dataPaste` template), deck import/export, comp_uploadData swap, **eraser wipe**,
`loadData` (payload persists as `signals[0]` of the prop's save row). LED `upd()`: particle
template by `level` (`eff_driverLight_l0..l3`); color = size>0 ? (isCopy ? YELLOW : GREEN) : RED.
Hover = `lib.makeDriveData` (Name/ID/Level/Size, "empty" if size<=0, "*copy" suffix). Drives
register in `gamemode.allDrives @0x07A0`.

### signalDriveEraser (unit 6)
Own `driveSlot` child bound to the same slot machinery. Erase verb (`actionOptionIndex` action 4):
requires drive present; `processing=true`, wiper anim rate 2.0, `audio_begin`, **Delay 3.0 s** ->
if drive still valid && `data_0.size != 0` -> **`drive.data_0 := empty-struct`** (the ENTIRE 0x70
payload — name/id/level/size/image all cleared), `drive.upd()` (LED -> red), `audio_done`; else
`audio_deny`; wiper back to 0.1, `processing=false`. Also: driveIn handler awards the
`driveKick` achievement if the drive arrived kicked.

## 6. Dispatch-visibility summary (the whole chain)

PE-VISIBLE seams only: `ui_consolesAtlas.OnKeyDown/OnKeyUp` (occupant machine only);
`actionOptionIndex`/`playerUsedOn`/`player_use` (interaction entries); ComponentBoundEvent
button/overlap thunks; multicast delegates (`driveIn/driveOut`, `OnAudioPlaybackPercent`,
`OnAudioFinished`, `input_scrollUp/Down`, tower `stateChanged`, `OnDestroyed`); timer/latent
resumes (0.2 s `coordLog2` pump, `redphoneEventTimer`, `pingComp`, Delay re-entries).
EVERYTHING ELSE — every state mutation, every gamemode/lib call (`saveSignal`, `deleteSignal`,
`deleteActiveSignal`, `laptop.addSignal`, `setSignalID`, `comp_*`, `putDriveIn`,
`drivePulledOut`), every audio SetActive/Play/SetVolume/SetPitch — is EX_Local*/inline
EX_Let/VictoryFloat±/native EX_FinalFunction: **PE-invisible AND Func-patch-invisible**. The
doctrine holds chain-wide: MIRROR STATE + re-apply, never verb-hook
(`[[lesson-votv-world-system-sync-mirror-state-not-verb]]`).

## 7. RNG census (new sites, for COOP_RNG_AUTHORITY)

| Site | Roll | Class |
|---|---|---|
| `gatherSignal` catch | `RandomBoolWithWeight(percent^k)` | MECHANIC — decides the catch on the pinging peer |
| weather offset jitter | `RandomFloatInRange(-1,1)*RandomFloat()^3*Lerp(0.01,10,rain)` per tick x2 | MECHANIC input (feeds frData/poData) — covered on clients by the v111 host-stream overwrite |
| `setSignalID` mint | `GenerateRandomBytes(16)` | IDENTITY — per-peer id mint at save/level-up |
| comp rate noise | `(RandomFloat()-0.5)*comp_noiseScale` | MECHANIC — contained: only the simulating peer runs it (CompState streams outputs) |
| red phone | 60-75 min timer + 2.5% roll | EVENT timing, per-peer |
| detector needle / DL noise | (already in the 07-15 doc / T2-5b) | host-streamed v111 |
| spectrum jitter, hecer chars, consumption% | cosmetic | leave local |
