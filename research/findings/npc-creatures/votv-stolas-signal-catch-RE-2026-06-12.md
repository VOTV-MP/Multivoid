# VOTV "STOLAS ASTRO PROCESSING UNIT" — signal-catch flow RE + coop sync design

Date: 2026-06-12. READ-ONLY RE pass (no src/ edits). Companion to
`votv-base-computers-RE-2026-06-11.md` (the device census) and
`votv-computers-phase2-impl-RE-2026-06-12.md` (the v64/v65 impl ground truth).
Answers the 4-symptom hands-on report (client triangulated + caught a signal;
host saw none of it) plus the 1 Hz idle DeskState broadcast storm.

## 0. Identity: which BP is the unit

The "STOLAS-ASTRO-PROCESSING-UNIT" is the in-game brand label on
**`AanalogDScreenTest_C`** — the 4-screen main desk (the v63/v64 "desk").
There is no separate astro/stolas BP. The parts the user described:

| user's description | engine object |
|---|---|
| left RT star map w/ draggable colored dots | `Uui_coordinates_C` widget (atlas child @0x0588) + `ui_signal_C` blips, composited over a `spaceSceneCapture` starfield RT (spaceRenderer uber @991-1399) |
| right green-text terminal | the coordLog pane — `RichTextBlock_coordLog` painted from `coord_coordLog2Text @0x0BF8` |
| front-panel Azimuth/Altitude/Cooldown 7-segs | atlas TextBlocks `txt1_azim_x` / `txt1_alti_y` / `txt1_cooldown` (RT crops on panel meshes, base RE §1.1) |
| letter board B..Y with lamps | `ui_atlasDishesStatus` widget (slots of `uicomp_dishStatusSlot`, one per BIG dish; letters from `lib_C::getSatelliteName(index)` static arrays) |
| the satellite dishes that auto-rotate | `Adish_C : Aactor_save_C` (objects/dish.uasset), array `mainGamemode.dishs @0x0330`, index-sorted by the level-authored `dish.Index @0x0358` (gamemode uber @55696 `Array_Set(dishs, item.index, item, true)`) |

Everything renders into the ONE desk `widget_RT @0x0650` via `widgetRender
@0x0618` (shared-RT architecture) — mirror the state and every screen +
front-panel display mirrors itself. RT repaint is viewer-gated
(`canRenderWidget := dist<2500 OR withinConsoles>0`, desk tick @16403-16698)
but state stays correct regardless.

## 1. C1 — where the triangulation state lives

### 1.1 The dots (player-draggable)

`Uui_coordinates_C` (size 0x458), SINGLETON child of the atlas
(`ui_consolesAtlas.ui_coordinates @0x0588`; also `spaceRenderer.coordinatesUI
@0x02C8`, assigned at spaceRenderer BeginPlay+1s, uber @428):

| field | off | meaning |
|---|---|---|
| `viewCoordinate` | 0x3B8 | the live cursor |
| `Coordinate_0/1/2` | 0x3C0/0x3C8/0x3D0 | THE 3 colored dots (ping-tower aims; the triangle) |
| `selected` | 0x3D8 | which slot the cursor drives |
| `coordinate_middle` | 0x3DC | derived (quick-scan writes it @79516) |
| `Direction` | 0x441 | the panel direction toggle — **gates catch success** (@9189) |

Writers:
- **The drag is tick integration, not events**: spaceRenderer tick
  (uber @518-970, runs on EVERY peer, every frame, gated only by
  GlobalTimeDilation<=1 @3622): `coordinatesUI.setCoordinateLocation(
  getCursorLocation() + movement)`; `movement` interp-decays from the
  `move_right/left/up/down` flags @0x260-0x263 (set by the desk's coords
  panel while `controllingCoordinatePanel`, cleared on exit @78564).
  `setCoordinateLocation` writes `Coordinate_<selected>` AND
  `viewCoordinate`, then `updCursorLocations()` (the 8 KB repaint that also
  rotates the 3 physical `pingDishes @0x13B8` roof radars via `setRot`).
- Commit buttons (desk uber): `screenbutton_bringCoord1/3/2/Cent` →
  @54272/@54368/@54445/@54522 `movePointToCursor(idx)` (commits
  `Coordinate_<idx> := viewCoordinate`, arms cooldown);
  `switchToCoord1/2/3` → @54049/@54136/@54204 `changeCursor(n)`.

**PE-visibility: NONE of it is PE-visible** — component-bound click events +
EX_LocalVirtualFunction widget calls + per-tick integration. Poll/stream is
the only detector; the existing 3 Hz claim-owner DishAimState stream is the
correct shape. Because the spaceRenderer tick re-runs
`setCoordinateLocation` (→ `updCursorLocations`) per frame on every peer, a
remote peer that receives raw field writes self-repaints within one frame —
no extra paint calls needed (the impl's explicit updCursorLocations +
updateCoordCoords calls are belt-and-braces).

### 1.2 The target signals

`spaceRenderer.signals @0x0278` rows (`Fstruct_signal_spawn`, coordinates in
the SAME screen space as the dots). Host-rolled + mirrored since v64
(SkySignalState=52). Nothing new needed here.

### 1.3 The catch verdict + Azimuth/Altitude/Cooldown

- Ping button → desk uber @79154: `master_spaceRenderer.gatherSignal(...)` —
  **the catch ROLL runs on the interacting peer only** (RandomBoolWithWeight
  inside gatherSignal @831). On success: **@79303 `coord_signalData :=
  gatherSignal.data`** (desk @0x0A38, struct 0x2C) then the consume chain
  (§2). Failed gates write error log lines but the row stays.
- Azimuth/Altitude: `normCoords()` = pure fn of `viewCoordinate`
  (az = lerp(0,360, x/areaX), alt = lerp(0,90, y/areaY)); painted by
  `updateCoordCoords` (txt1_azim_x/txt1_alti_y) which `process_coords` calls
  EVERY TICK on every peer (@82995). Self-mirroring once viewCoordinate
  mirrors.
- Cooldown: `coord_cooldown @0x0A80` — armed `:= coord_maxCooldown` @79733 on
  ping; decremented per tick on EVERY peer (@83284); painted per tick
  (`txt1_cooldown` @83191). Already in DeskStatePayload (`coordCooldown`) —
  one apply and it self-advances on mirrors.

## 2. C2 — the global "signal caught" state + the full chain

**The global truth is the desk's `coord_signalData @0x0A38`**:
`mainGamemode.getSignalSData` (3 stmts) returns `analogPanels.coord_signalData`
— this is what the downloader needles + download accrual read.

The full SP chain (all bytecode-verified):

```
ping (desk uber @79154 gatherSignal, occupant-local RNG)
  └ success: @79303 coord_signalData := data
     @9124 objectName gate -> @9189 Direction gate (ui_signal.Direction ==
       ui_coordinates.Direction OR has_autoCoordRot) -> @9323 radius gate
     @9337 spaceRenderer.deleteSignal(element)          (the row removal)
     @9404 writeToCoordLog_2('Successful ping. Initializing satellite rotation...')
     @9494-10022 FOR EACH gamemode.dishs[i]:
         startMovingTo( getCoords(normCoords()) * 100000 + offset )  <- ONE
         vector, same for all dishes (recomputed per iter, same inputs)
     @10027 playPingSound(newdesk_panelCoord_pingSuccess)
     @10050/@10244 DL_signalDownloadData / DL_SignalDownloadDLData := zeroed
       structs (download screen reset)
dish_C.startMovingTo(lookAt):  gate IFNOT(isMoving) (a moving dish IGNORES new
     targets); C_offset := 1-calibration; lookAt(0x378) := param + ActorLocation;
     startMove -> uber @6283: isMoving(0x384) := true ->
     @2994: maxSpeed := rand(maxSpeedRange); gamemode.activeDishes[Find(dishs,
     self)] := true (@3197); Delay(rand 1-12 s) -> rotate (axis_Y/axis_Z
     timeline, satellite cues) -> Delay(rand 2-4 s) ->
     @3815 activeDishes[i] := false; @3897 gamemode.checkFordDishes()
mainGamemode.checkFordDishes (uber @60831):
     IF NOT activeDishes.Contains(true):
       @60917 BROADCAST dishesStop (delegate)
       @60936 objectRenderer.cameraAxis rotation (reads the DEAD
         spaceRenderer.coords -> constant; equal on all peers)
       @61201 master_spaceRenderer.signalFound() -> runTrigger(trigger_OnFound)
desk.dishesStop (bound handler, thunk -> uber @34223):
       formDownload(0, -1)   <- builds DL_signalDownloadData from DataTable
         list_objects[coord_signalData.objectName] + initDownloadSignal
         == THE DOWNLOADER IS NOW ARMED
downloader:
  - "NO SIGNAL": download_playSignall @37: IsValid(DL_signalDownloadData.mesh)?
    NO + active_download -> @796 DL_nosiga := 1.0 (canv_nosign overlay,
    opacity nosiga^5, decays 2/s @70041-70226) + beep_detecEmpty.
  - detector pulse (uber @3541-4625, every DL_duration): if mesh valid &&
    resDetect < 1: DL_resDetecPercent += rand(0.001..lerp(0.004,0.02,upg)) *
    DL_detectorMultiplier (@4017); at >= 1: beep_detecFinish + canSaveSignal().
    If mesh INVALID: @4602 resDetect := 0.
  - canSaveSignal @313: canDL := (resDetect >= 1) AND (DLData.decoded >= size)
    — DERIVED, recomputed every pulse, edge-guarded.
  - download accrual (uber @66839-68635 per tick): DL_downloading :=
    f(poData^8 * frData^8 needles, * resDetect, * powerUsage ...) -> decoded
    accrues; needles DL_frData/DL_poData (0x0A0C/0x0A10) derive per tick from
    getSignalSData() (= coord_signalData) freq/pol vs the FILTER offsets
    (@64308-65064).
```

**The dishes are a TIME GATE, not a data path**: detection reads no dish
state; `dishesStop` (all dishes arrived) is what arms `formDownload`. The
letter board shows dish state (§4). Stats side effect: gatherSignal also does
`save_main.stats.signals_found++` on the catcher (out of scope — stats are
per-peer).

## 3. C3 — the star map screen rendering model

UMG, not actor-capture: the dots are canvas verts (`updCursorLocations`
repositions them) + `ui_signal_C` child widgets in
`ui_coordinates.canvas_spaceSigns @0x2A0`, inside the atlas drawn into
`widget_RT` by the desk's WidgetComponent. The starfield BEHIND them is a
SceneCapture (`spaceSceneCapture` → 512×256 RT → `mat_rt` dynmat, spaceRenderer
uber @1111-1399) of the local sky scene — peer-local, cosmetic, identical-ish.
**⇒ Mirroring {viewCoordinate, Coordinate_0..2, selected} + the signals set
mirrors the pixels.** The model was already right; the failure was the
instance resolve (§5.1).

## 4. C4 — right terminal + front panel drivers

### 4.1 The coordLog (right green terminal) — and why the host saw gaps

- The pane = `coord_coordLog2Text @0x0BF8`, written ONLY by
  `writeToCoordLog_2(B)` which **appends `B + "\r\n"`**, trims to the LAST
  1000 chars, repaints + scrolls (phase-2 doc §2.5).
- **A 0.2 s LOOPING timer `coordLog2`** (armed at desk BeginPlay, uber
  @12960-13036; runs on EVERY peer) appends ANIMATED lines:
  - while `coord_isPing` && pingStage != 3: `'AREA SCAN: [<c>bar</>] x%'` /
    `'APPROXIMATION: …'` / `'ANALYSIS: …'` selected by `coord_pingStage`
    (@43390-44488, write @44488);
  - while `coord_cooldown > 0`: `'CDOWN: [<c>bar</>] x%'` (@44626-45024) —
    5 lines/sec for the ENTIRE ~30 s cooldown;
  - while any `move_*` flag: `'CR:[<c>A:az</>||<c>B:alt</>]'` (@45348-45843).
- The 9 EVENT lines (one-shot, action-driven) are the only true events:
  `'Successful ping. Initializing satellite rotation...'` @9404,
  Err2 @79368 / Err3 @10689 / Err4 @10624 / Err5 @10508 / Err6 @78121,
  `'<r>Cooldown error</>'` @79659, `'<c>Initializing quick scan...</>'`
  @79760, `'Signal data deleted'` @34099.

**The "empty string gaps" root cause (3 compounding mechanisms):**
1. Our `AppendCoordLog` calls `writeToCoordLog_2`, which appends its own
   CRLF; the wire tail normally already ENDS with `"\r\n"` → every applied
   chunk produces `"...\r\n\r\n"` = one blank line per apply
   (console_state_sync.cpp `ApplyCoordLogTail` + console_desk.cpp:296-310).
2. During/after a ping the log grows ~5 lines/s (coordLog2 spam); the 96-char
   `DeskStatePayload.coordLog` window at 1 Hz slides past more text than it
   carries → the receiver-side suffix-prefix match finds no overlap (its
   local copy is also CRLF-polluted per (1)) → "append whole tail" → mid-line
   fragments + duplicates; fragments cut inside `<c>…</>` rich-text tags
   render as empty/garbled lines.
3. Once our scalar apply sets `coord_cooldown > 0` / `coord_isPing` on the
   receiver, the receiver's OWN coordLog2 timer generates its own CDOWN/AREA
   lines → two writers interleave in one pane.

The animated lines are SELF-GENERATING on mirrors from mirrored scalars; only
the 9 event lines belong on the wire (§6.4).

### 4.2 Front panel

- Az/Alt 7-segs: `txt1_azim_x`/`txt1_alti_y` ← `updateCoordCoords` ← per-tick
  `process_coords` @82995 (every peer). Cooldown 7-seg: `txt1_cooldown` ←
  per-tick @83063-83191. All derive from {viewCoordinate, coord_cooldown}.
- Letter board: desk timer `eventUpdateDishStats` (20 Hz, armed @14176;
  body @51638-53405) round-robins `ui_atlasDishesStatus.slots` (queue refill
  @51566): lamp `img_satStatus.SetBrushFromTexture(dish.isMoving ? redflare :
  greenflare)` @52431; bar dynmat `'level' := 16 * (dish.getDir()^2 *
  dish.calibration^2)` @53263 (`getDir` = |dot(Arrow1.fwd, Arrow.fwd)| —
  current vs target alignment); broken dishes skipped @52820. Letters =
  `lib_C::getSatelliteName(index)` static arrays via `dish.makeName`
  (gamemode names all dishes at boot @56096). **Fully derived from LOCAL
  `gamemode.dishs[i]` actor state — mirror the dish actors and the board
  mirrors itself.**

## 5. Root causes of the 4 reported symptoms

### 5.1 Symptom 1 — host never saw the dots move: ARCHETYPE-vs-INSTANCE resolve bug

`UiCoordsInstance()` (2026-07-19: now `ue_wrap::coords_panel::Instance()` in
`ue_wrap/desk/coords_panel.cpp` after the one-class-per-file split; the fixed
chain-resolve described below moved verbatim) resolved the widget via
`R::FindObjectsByClass(L"ui_coordinates_C")` first-live-hit. The cooked
`ui_consolesAtlas` asset contains a **NormalExport named `ui_coordinates`
(export #360, ui_consolesAtlas.json)** — the WIDGET-TREE TEMPLATE. At runtime
that template object: (a) is class `ui_coordinates_C`, (b) is NOT
`Default__`-prefixed (the CDO skip misses it), (c) is permanently live, and
(d) loads WITH THE CLASS → lower GUObjectArray index than the runtime widget
created later by the desk's WidgetComponent → **the walk returns the TEMPLATE
first and caches it forever** (g_uiCoordsInst + IsLiveByIndex never
invalidates it).

Consequences: the CLIENT's `ReadDishAim` reads frozen template values → after
at most one initial send the memcmp dedupe never fires → no stream; the
HOST's `WriteDishAim` writes the template → invisible. Both ends dead. (The
DeskState text stream still worked — it operates on the desk ACTOR, where the
class walk is template-free; hence "text mirrored, dots didn't".)

Repo-wide audit: `console_desk.cpp:184` is the ONLY `FindObjectsByClass`
over a widget class in src/ — the bug is isolated.

**Fix (the game's own resolution, spaceRenderer uber @428):** resolve by
field chain — desk instance → `Widget @0x0638` → `atlas.ui_coordinates
@0x0588` (or spaceRenderer actor → `coordinatesUI @0x02C8`). No class walk
for widgets, ever (a reusable lesson: UMG child-widget templates shadow live
instances in any GUObjectArray walk).

### 5.2 Symptoms 2+3 — downloader "no signal" + dishes never slewed: the consume chain is occupant-local

The whole §2 chain after `gatherSignal` runs only on the catcher:
`coord_signalData` (a struct — NOT in DeskStatePayload) never mirrors;
`gamemode.dishs[i].startMovingTo` never runs on other peers; therefore
`dishesStop` never fires there; therefore `formDownload` never arms their
downloader → `DL_signalDownloadData.mesh` invalid → `DL_nosiga := 1` →
"NO SIGNAL" + empty-beep, forever, on every non-catching peer. (On the
catcher itself, "no signal" is CORRECT SP behavior until its dishes arrive —
seconds to tens of seconds.) The existing SkySignalCatch=53 only deletes the
caught ROW host-side; the v64 docs explicitly deferred the dish/world part
(phase-2 §1.8 "out of phase-2 scope").

### 5.3 Symptom 4 — coordLog gaps: §4.1.

### 5.4 Bonus E — the 1 Hz idle broadcast storm: `DL_poFilterOffset` / `DL_FrFilterOffset`

Desk tick (uber blocks @66528-66735, also the twin @66058/@66389 pair):

```
@66548: IF DL_activePoFilter:  @66604 DL_poFilterOffset += DL_poFilterSpeed * dt
@66642: IF DL_activeFrFilter:  @66698 DL_FrFilterOffset += DL_FrFilterSpeed * dt
@64164: DL_poFilterOffset := DL_poFilterOffset mod 360;  DL_FrFilterOffset := clamp(0,1000)
```

The OFFSETS are knob-set + **tick-integrated while the filter toggle is
active** (that is what the "speed" knob means — the filter sweeps). The
active flags + speeds persist in `analogPanelsData` (setData @258/@286), so a
save left with a filter ON self-advances the offset every frame with zero
player input → `DiscreteDiffer` (console_state_sync.cpp:197-200, the v64-inc2
"knob edge" addition) sees a different float on EVERY 1 Hz poll → a desk edge
broadcast at exactly the poll rate, indefinitely, while idle. The v64
classification ("the 4 filter knob floats = pure knob state") is right for
the SPEEDS, wrong for the OFFSETS. Scalars mapping (console_desk.cpp
g_fields[0]/[1] → `DL_poFilterOffset @0x09F4` / `DL_FrFilterOffset @0x09F8`;
2026-07-19 `f74d05dc`: the positional table is gone — now the named
`g_offDlPoFilterOffset`/`g_offDlFrFilterOffset` in the same file).

**Fix:** drop the two OFFSET floats from `DiscreteDiffer` (keep speeds +
active toggles — genuinely input-only); offsets still ride the full payload
on every real edge + the claimed cadence + adopt snapshots, so receivers
re-baseline at each interaction. Residual divergence = each peer integrates
its own sweep between edges (bounded, cosmetic, converges on any edge).

## 6. D — sync design (minimum-viable, MTA-shaped)

Protocol today: `kProtocolVersion = 69`; last ReliableKind = PropStickState
= 64 → **next kind 65, next version 70**. Everything below is poll +
reflected-call (every seam in this subsystem is EX_LocalVirtualFunction /
component-bound / timer-driven — measured-zero for our PE detour; no hooks,
no death-watch — the only world actors involved (dish_C) are level-placed and
immortal).

### 6.1 Dot mirror — fix DishAimState=55 (no new wire)

- Resolve ui_coordinates via the field chain (§5.1). One-line ResolvePass
  change + drop the class walk (RULE 2: the walk goes).
- Add `uint8 direction` (ui_coordinates.Direction @0x441) into the existing
  `_pad` of DishAimStatePayload — it is aim state (the catch gate + panel
  lamp); the claim owner streams it, receivers raw-write.
- Keep: ~3 Hz claim-owner cadence, holder gating, the
  updCursorLocations/updateCoordCoords applies (already correct).
- MTA cite: the keysync owner-stream shape already adopted for DeskState.

### 6.2 Catch consume replay — extend SkySignalCatch=53 (the linchpin)

Replaces the current row-delete-only relay with the full event (one message
fixes symptoms 2 AND 3):

- **Payload v2**: `{ float x,y,z, frequency;  float slewX,slewY,slewZ;
  uint8 kind (0=catch, 1=cleared); uint8 _pad[3] }` (32 B). `slew` = the
  exact `startMovingTo` argument. Cheapest exact source on the catcher:
  `dishs[i].lookAt(0x378) − dishs[i].ActorLocation` of any just-started dish
  (startMovingTo stored param+location @116); fallback: reflected
  `normCoords()` + `getCoords()` (both pure).
- **Catcher detection** (the claim holder, 1 Hz poll — replaces the current
  vanish-only detector): `coord_signalData` CHANGED to identity X **AND**
  row X vanished from `signals`. The AND kills both false positives:
  expiry deletes the row but never touches the struct; failed pings
  (Err4/Err5) write the struct @79303 but keep the row. (The CURRENT
  vanish-only detector relays EXPIRY as a catch — harmless today because the
  host's RemoveSignalByIdentity no-ops, but lethal once the replay slews
  dishes: the v2 detector is required, not optional.)
- **Host**: validates the desk-claim holder (as today), applies the replay
  locally, rebroadcasts to everyone except the catcher. `kind=1` (cleared)
  needs no claim gate (the delete button is unclaimed — matches the
  unclaimed-edge trust model).
- **Receiver replay** (host + clients):
  1. look up the full row by identity in `g_skyMirror` (BEFORE removal) →
     raw-write `coord_signalData @0x0A38` (POD except objectName FName →
     StringToFName — the proven OverwriteRow path);
  2. `RemoveSignalByIdentity` (exists);
  3. append the `'Successful ping. Initializing satellite rotation...'` log
     line via writeToCoordLog_2 (or let §6.4 carry it — pick ONE source:
     §6.4 is the single channel, the replay does NOT write the line);
  4. reflected `playPingSound(newdesk_panelCoord_pingSuccess)` (presence);
  5. zero `DL_signalDownloadData` + `DL_SignalDownloadDLData` raw (the
     @10050/@10244 clears) — stale prior-signal data must not linger;
  6. FOR EACH `gamemode.dishs[i]` (array @0x0330): reflected
     `startMovingTo(slew)` — **the native chain then self-runs per peer**:
     per-dish random delays/speeds (cosmetic divergence), activeDishes
     bookkeeping, arrival → checkFordDishes → dishesStop BROADCAST →
     `formDownload(0,-1)` fires NATIVELY on every peer. The downloader,
     letter-board lamps (red while isMoving), bars, and `signalFound`
     trigger all arm themselves with zero extra wire.
  7. prime the desk edge detector (the house lastKnown pattern) so the
     replay's scalar fallout never echo-broadcasts.
- **Snapshot race**: generalize the existing catcher-side pending-removal
  filter — every receiver records the caught identity (TTL ~3 s) and filters
  it out of incoming SkySignalState snapshots (a stale in-flight snapshot
  must not resurrect the row).
- `kind=1 cleared` replay (the 'Signal data deleted' button, uber
  @33832-34222): detect `coord_signalData.objectName -> 'None'` edge on any
  peer → broadcast → receivers zero coord_signalData + the 2 DL structs +
  `DL_resDetecPercent := 0` + `initDownloadSignal((0,0),0,-1)` equivalents
  (raw writes suffice; the log line rides §6.4).
- MTA cite: one-shot world event relayed host-authoritatively + local
  simulation of deterministic consequences (the CClientExplosionManager /
  projectile shape) — same as the wind/firefly RESULT-mirror category.
- New ue_wrap surface: `ue_wrap/dish.{h,cpp}` (resolve gamemode.dishs
  @0x0330 via the existing gamemode wrapper; StartMovingTo(i, vec); read
  lookAt/isMoving; ~120 LOC, one feature per file) + console_desk additions
  (CoordSignalData read/write 0x2C, ZeroDownloadStructs, PlayPingSound).
- New coop module: `coop/signal_catch_sync.{h,cpp}` wired via
  coop/subsystems (console_state_sync.cpp is at 603 LOC; the replay logic
  would push it past the soft cap — extract instead of growing it).

### 6.3 Download progress — piggyback on DeskState (no new kind)

Add `float dlDecoded; float dlPolarity` to DeskStatePayload (these are
formDownload's exact parameters; version bump covers the reshape):

- Sender: the peer whose `DL_SignalDownloadDLData.decoded` ADVANCED locally
  since the last poll (the active operator — the downloader is button/knob
  operated, never claimed) → stream at the existing 1 Hz while advancing;
  last-writer-wins on the receivers (two peers tuning one physical machine
  is already physically silly; bounded jitter, documented).
- Receiver: if its local (decoded, polarity) differ materially AND
  coord_signalData is non-empty → reflected `formDownload(decoded,
  polarity)` (rebuilds DL_signalDownloadData + initDownloadSignal screen
  state from the DataTable — §2.2 of the phase-2 doc) + raw
  `DL_resDetecPercent` write.
- **Drop `canDL` from the wire-write AND from DiscreteDiffer** (RULE 2): it
  is DERIVED (`canSaveSignal` recomputes it from resDetect + decoded>=size
  every detector pulse — a raw-written canDL gets flipped back by the local
  recompute and echo-storms). Mirroring the INPUTS makes every peer converge
  canDL natively.
- Each peer's detector pulse self-accrues resDetect once ITS dishes arrive
  (the increments are RNG → small per-peer sawtooth, overwritten by the
  operator's 1 Hz stream; acceptable, converges at 1.0).

### 6.4 coordLog — event-line append replaces the raw-tail diff (RULE 1)

- **Delete** `coordLog[96]`/`coordLogLen` from DeskStatePayload,
  `ApplyCoordLogTail`, and the receiver suffix matcher (RULE 2 — the tail
  protocol goes fully).
- New `ReliableKind::DeskLogLine = 65`: `{ uint8 len; char line[120] }`,
  host-relayed, ASCII (the BP lines incl. `<r>/<c>` markup are ASCII).
- Producer (any peer — event lines originate where the action ran: catch /
  errors = the claim holder; 'Signal data deleted' = whoever pressed it):
  poll `coord_coordLog2Text` at the existing 1 Hz; diff OWNER-side (both
  strings come from one local monotone sequence → the suffix-prefix
  continuation is always exact there); split new content into complete
  CRLF lines; **filter the animated prefixes** (`CDOWN: [`, `AREA SCAN: [`,
  `APPROXIMATION: [`, `ANALYSIS: [`, `CR:[`) — those regenerate on every
  peer from mirrored scalars (cooldown/isPing ride DeskState; move-flag CR
  lines simply don't appear on mirrors — correct, nobody is dragging there);
  ship each event line.
- Receiver: `AppendCoordLog(line)` (writeToCoordLog_2 adds the CRLF + does
  the native repaint/scroll/1000-cap). No blanks, no dupes, no fragments.
- Echo-proofing: an applied line advances the receiver's own diff baseline
  past it (the email watermark prime precedent).

### 6.5 DiscreteDiffer trim (the storm fix)

Remove `dlPoFilterOffset`/`dlFrFilterOffset` (tick-integrated, §5.4) and
`canDL` (derived, §6.3) from the unclaimed edge set. Keep: speeds, active
toggles, polarity dir, volumes, indices, active_* screens, dlDownloading>0.

### 6.6 Late-joiner snapshot (QueueConnectBroadcastForSlot additions)

Order: existing sky snapshot → desk adopt (now incl. decoded/polarity) →
**IF `coord_signalData.objectName != 'None'`: one SkySignalCatch v2 to the
joiner** (identity from the desk struct; slew from any dish lookAt if
isMoving, else recomputed from normCoords) — the joiner's dishes slew fresh
(1-12 s, SP-faithful), dishesStop fires locally, formDownload arms its
downloader. The coordLog history is NOT replayed (the log is session-local
even in SP — `analogPanelsData` carries no log field, phase-2 §2.1; a joiner
starting with an empty terminal is SP-faithful).

### 6.7 Explicitly out (minimum-viable per COOP_SCOPE)

- Dish initial `randrot` divergence (uber @6648 rolls random boot rotations
  per peer) — cosmetic; converges at the first catch.
- `save_main.stats.signals_found` per-peer divergence (stats aren't scoped).
- The ping-anim scalars (`coord_pingStage`, `coord_ping_*`) — occupant-local
  animation; the mirrors' coordLog2 reproduces the bars from isPing alone.
- `spaceSceneCapture` starfield parity (peer-local sky render).
- `DL_frData/DL_poData` needles — per-tick derived from coord_signalData +
  filter offsets on every peer.

### 6.8 Validation list (pre-implementation)

- V1: after the resolve fix, solo-host: enter coords panel, drag; log the
  resolved widget ptr + verify viewCoordinate changes live (template would
  stay frozen).
- V2: two-peer: client catch → host dishes slew (lamps go red on host board),
  host downloader arms after its own dishesStop, detector accrues, no log
  blanks, no 1 Hz idle broadcasts with a filter left active.
- V3: catch-vs-expiry: let a signal expire on the client while it holds the
  desk claim → NO catch replay must fire (the AND detector).
- V4: formDownload-on-mirror: verify the minted objectName FName matches the
  list_objects row (the WireSkySignal StringToFName path already proves the
  mechanism).
- V5: joiner mid-detection: connect after a catch → dishes slew, downloader
  arms, decoded/polarity adopt applies.

## 8. v70 IMPLEMENTATION (2026-06-12, the section-6 design built + shipped)

Protocol 69 -> 70. New modules `coop/signal_catch_sync.{h,cpp}` +
`ue_wrap/dish.{h,cpp}`; `console_state_sync` reworked; `console_desk` grew the
catch surface; `reflection` grew `FindPropertyOffsetByPrefix` (GUID-mangled
UserDefinedStruct members by stable human prefix) + `PropertyInnerStruct`
(FStructProperty::Struct via a self-calibrating 0x70/0x78 slot probe validated
through GUObjectArray -- calibrated +0x78 on the live build).

CORRECTION to this doc's assumption: `DL_signalDownloadData`'s row type is
**`struct_spaceObject`**, NOT struct_signal_data -- proven by the live
calibration log (`property 'DL_signalDownloadData' -> struct
'struct_spaceObject'`); a global FindObject by the wrong name was the first
implementation's silent dead end (rowName/mesh=-1 in smoke #1). The property-
chain resolve is immune to the type's name by construction. Member offsets on
the live build: name FText @0x00 (0x18), `signalName_*` FName @0x18, `mesh_*`
@0x20 -- matching the @33832 reset literal's member order.

Two DESIGN REVISIONS vs section 6, both bytecode-grounded:

- **6.2's catcher detector REPLACED by the dish-edge detector.** The designed
  AND ("coord_signalData CHANGED to X" + "row X vanished") MISSES a real
  catch when a FAILED ping already wrote the struct to X (Err4/Err5 keep the
  row but write @79303): the later successful re-ping changes nothing in the
  struct, so the "changed" leg never fires. And after a failed ping, natural
  EXPIRY of that same row WOULD fire it (struct points at X, X vanished).
  The success signature that separates all cases is the DISH: only the catch
  chain's @9494 fan-out starts dishes (`startMovingTo` -> `startMove` sets
  `isMoving` immediately). v70 detector (1 Hz, claim holder): row X ==
  coord_signalData identity vanished from `signals` AND `MovingCount()` ROSE
  in the same poll window. Expiry: no dish edge. Failed ping: no vanish, no
  edge. Failed-then-caught same X: vanish + edge = fires.
- **6.3's download-progress stream REJECTED; decoded/polarity are ADOPT-ONLY.**
  The design's premise ("decoded advances on the active operator") is
  FALSIFIED by the accrual chain @66736-68635: the per-tick block gates only
  on `IsValid(DL_signalDownloadData.mesh)` -- NO occupancy condition -- so
  once the v70 replay arms every peer, decoded accrues on EVERY peer (inputs:
  mirrored needles/quality/powerUsage/upgrades + per-peer RNG noise that
  averages out). An advance-triggered stream would have made every peer a
  permanent 1 Hz DeskState sender -- the exact v69 RAM-balloon shape. v70:
  per-peer native accrual converges by itself; `dlDecoded`/`dlPolarity` ride
  DeskStatePayload but receivers consume them ONLY from the host's adopt=1
  connect snapshot, queued as a pending `formDownload(decoded, polarity)` +
  resDetect write applied once the joiner's own machine arms (mesh valid --
  before that the native @4602 pulse re-zeroes resDetect). Invariant making
  the pending safe with no TTL: coord_signalData=='None' implies host
  decoded==0 (the @34005 clear rebuilds DLData zeroed), so a stale pending is
  an idempotent no-op over a fresh arm.

Mechanics as designed elsewhere: SkySignalCatchPayload v2 = 80 B {full
WireSkySignal row filled from the CATCHER's coord_signalData + the exact slew
recovered as (dish.lookAt - ActorLocation) off any just-started dish + kind
0/1 + slewValid}; host validates the desk-claim holder, replays, rebroadcasts
to everyone but the catcher; receiver replay = struct write -> Remove-
SignalByIdentity -> ResetDownloadMachine (signalName None + mesh null + frData/
poData/resDetect zero + initDownloadSignal(0,0,-1) -- NOT a whole-struct raw
zero: FText members raw-zeroed are a dangling-shared-ref crash risk, and mesh
invalidity already gates everything) -> playPingSound(pingSuccess) ->
StartMovingAll(slew) (native chain arms formDownload per peer); slewValid=0
(joiner after dishes settled) arms formDownload(0,-1) directly. kind=1 cleared
replays the @33832 chain. Short-TTL recent-catch set filters stale in-flight
snapshots (console_state_sync hands assembled rows through
NoteIncomingSnapshot, which also runs the detector first -- the in-flight-
catch-vs-stale-snapshot race). coordLog tail protocol DELETED (RULE 2) for
DeskLogLine=65: producer-exact diff, complete CRLF lines, animated prefixes
(CDOWN:/AREA SCAN:/APPROXIMATION:/ANALYSIS:/CR:[) filtered, receiver
writeToCoordLog_2 append + own-baseline advance (+ flush-before-apply so the
advance can't swallow a pending local line). canDL left the wire AND the
Scalars surface (derived, 6.3). DishAimStatePayload carries Direction @0x441.
DeskState reshaped 152 -> 60 B.

Perf audit (2026-06-12): 0 CRITICAL FAIL. IMPROVE-1 (the 1 Hz log producer's
unconditional ~1 KB wstring build) FIXED with CoordLogTailEquals (in-place
FString compare; a length-only fast check is unsound at the 1000-char cap --
append+trim keeps the length pinned while content changes). IMPROVE-2 (the
suffix-overlap loop is O(n^2) right after a BP front-trim) ACCEPTED: bounded
<= ~1.2M wchar compares at 1 Hz only while the log is actively changing
(~0.25 ms/s worst) -- revisit with std::wstring-find if the desk log is ever
touched again. IMPROVE-3 (every peer detects the same cleared edge in the
same 1 Hz window -> up to N redundant kind=1 replays per button press)
ACCEPTED: the replay is idempotent (zeroing zeroed state), the window closes
when ApplyReplay primes g_prevSigName, and clears are a rare manual action.

Correctness audit (2026-06-12, focused retry after the first agent stalled):
detector truth table ALL-PASS across 10 scenarios (incl. the two the v70
detector exists for: failed-ping-then-catch fires, expiry never does);
marshaling field-for-field correct; no engine pointer cached across polls.
Four findings: (1) IMPORTANT, FIXED -- a mid-session level reload spawns a
fresh desk whose runtime-only coord_signalData is 'None'; the module-static
baseline surviving the reload would read armed->None as a delete-button press
and broadcast a spurious kind=1 (clears every peer's download). Fix:
CheckDeskInstance() re-primes all baselines without firing on any desk
instance change (the g_suppressedOn shape). (2) IMPORTANT, REJECTED -- the
auditor wanted the claim-holder gate on kind=1; that premise contradicts the
RE: the delete button is a PHYSICAL panel button (the v63 claim gates
ENTERING the screen, not pressing buttons), so a holder gate would drop
legitimate clears; kind=1 keeps the unclaimed-edge trust level (idempotent +
detector-priming = repeats are bounded no-ops). (3) MINOR, FIXED -- OnDeskState
now finite-validates all 8 payload floats before WriteScalars (the
WireRowFinite discipline). (4) MINOR, FIXED -- a pending download adopt older
than 60 s is voided at the next kind=0 (the join's own adopt->replay gap is
milliseconds; an old armed pending means its arm silently failed and would
true-up the WRONG signal). Plus one self-review fix the stalled first agent
never reached: stale g_prevRows across desk-claim release/re-claim could pair
an ancient row set with a fresh dish edge and false-fire past the recent-TTL
-- the row baseline is now invalidated whenever not actively maintained.

Section-6.8 validation status: V1 (dot mirror via the field-chain resolve)
shipped in v69 and pending hands-on; V2-V5 are the v70 smoke/hands-on list --
see memory/project_session13 NEXT SESSION ORDER for the round-5 checklist.

## 7. Tooling used (exact commands)

Existing dumps: `research/bp_reflection/{analogDScreenTest,spaceRenderer,
mainGamemode,ui_coordinates,ui_signal,ui_consolesAtlas,lib_fixed}.json` +
`_analogd_uber_full.txt`, `_spacerenderer_uber_full.txt`. New dumps made this
pass (kismet-analyzer e8982e9, UE4.27):

```
research/pak_re/tools/ka/kismet-analyzer-e8982e9-win-x64/kismet-analyzer.exe \
  to-json research/pak_re/extracted/VotV/Content/objects/dish.uasset \
  > research/bp_reflection/dish.json
… same for umg/windows/ui_atlasDishesStatus.uasset
```

Disassembly/scans (from repo root):

```
python research/bp_reflection/_fn.py analogDScreenTest {ReceiveTick,
  canSaveSignal,download_playSignall,updateCoordCoords,normCoords,coordLog2,
  dishesStop,eventUpdateDishStats,calculate_download}
python research/bp_reflection/_fn.py dish {startMovingTo,getDir,startMove,
  stop,getData,loadData,makeName}
python research/bp_reflection/_fn.py spaceRenderer {signalFound,getCoords}
python research/bp_reflection/_fn.py mainGamemode getSignalSData
python research/bp_reflection/_fn.py lib_fixed getSatelliteName
python research/bp_reflection/_scan.py analogDScreenTest <needles: canDL,
  "DL_PolarityDir :=", "active_* :=", "DL_activeXxFilter :=",
  "VictoryFloatPlusEquals(DL_poFilterOffset", "writeToCoordLog_2(",
  formDownload, "coord_signalData :=", dynmats_dishProgBars,
  ui_atlasDishesStatus, "ExecuteUbergraph_analogDScreenTest(",
  coord_cooldown, DL_nosiga>
python research/bp_reflection/_scan.py dish {dishesStop,activeDishes}
python research/bp_reflection/_scan.py mainGamemode {dishesStop,activeDishes,dishs}
python research/bp_reflection/_cfg.py dish ExecuteUbergraph_dish
python research/bp_reflection/_cfg.py mainGamemode ExecuteUbergraph_mainGamemode
```

Plus awk/sed reads over `_analogd_uber_full.txt` (blocks @3541-4625, @9124-
12960, @13120-16700, @33832-34637, @43326-45867, @51638-54590, @60262-70381,
@78855-79809, @82980-83260), CXXHeaderDump greps (`dish.hpp`,
`mainGamemode.hpp`, `ui_consolesAtlas.hpp`), a JSON walk over
`ui_consolesAtlas.json` (the `ui_coordinates` template export #360 proof),
and `grep -rn "FindObjectsByClass(L\"ui_" src/` (bug-isolation audit).
