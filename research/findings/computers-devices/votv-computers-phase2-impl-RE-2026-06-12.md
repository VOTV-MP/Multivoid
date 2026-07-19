# VOTV base-computers PHASE 2 — implementation-exact RE (sky signals / desk / dish aim / emails)

> **STATUS 2026-06-12: INCREMENT 1 IMPLEMENTED against this doc — v64**
> (`coop/console_state_sync` + `ue_wrap/space_renderer` + `ue_wrap/console_desk`).
> All three falsifications this doc made of the first-cut implementation were
> adopted: ui_coordinates dish aim (+updCursorLocations), coord_coordLog2Text via
> writeToCoordLog_2, WireSkySignal Alpha+Direction. Filter knobs ride the unclaimed
> edge set per §2.2's knob-state classification. Smoke-proven live (roller kill,
> snapshot apply, self-roll sweep, button edges). The §1.4 widget-visual push
> (dynmat dir/pingSpeed + RenderScale) and dish-apply updateCoordCoords are in.
> **INCREMENT 2 IMPLEMENTED: §3 emails — EmailAppend=56** (coop/email_sync +
> ue_wrap/email + ftext_utils Mint/Read; watermark + chunked rows + one-call
> addEmail apply; pfp by leaf name; deletes rebase-only).
> **INCREMENT 3 IMPLEMENTED (v65, 2026-06-12): email DELETES (EmailDelete=57,
> content-keyed; the watermark became a full shadow mirror), saved-signal
> deltas (SavedSignalAppend/Delete=58/59 on gamemode.savedSignals_0 — NOTE the
> savedSignals agent pass corrected this doc: rows are Fstruct_signalDataDynamic
> 0x70, not signal_data 0x1C8; image PNG deferred to a bulk lane), the refiner
> decode mirror (CompState/CompData=60/61 — §2.4 item 2's comp_start/comp_stop
> edge calls were RETIRED by the comp agent pass: the ticker has NO occupancy
> gate, so mirrors stay PASSIVE and only the native latch holder simulates;
> + the client world-up unlatch fixing the v56 setData->comp_start joiner
> double-sim), and U6 (VERDICT: YES-duplicates; fixed state-level — client
> TimeScale=0 per correction + the dailyDelivery latch). STILL NOT BUILT:
> the desk release-blob (signalPlayData re-derivation), email-domain task
> struct sync (saveSlot.taskNew), savedtime.Z calendar sync, comp_hecertext
> flavor mirror, the saved-signal image bulk lane, drive CONTENT sync.

Date: 2026-06-12. Companion to `votv-base-computers-RE-2026-06-11.md` (§4-5); this doc
is the byte/offset/signature ground truth for the phase-2 mirror, plus a cross-check of
the already-started uncommitted implementation (`ue_wrap/space_renderer.*`,
`ue_wrap/console_desk.*`, `coop/console_state_sync.*`, protocol v64 kinds 52-55).

Sources (all verified this pass, no guesses):
- Kismet bytecode: `research/bp_reflection/{spaceRenderer,analogDScreenTest,mainGamemode,
  ui_laptop,daynightCycle,coordRadarDish,panel_radar}.json` via `_fn.py`/`_cfg.py`/`_scan.py`/
  `_params.py`; NEW dumps made this pass: `ui_signal.json`, `ui_coordinates.json`,
  `uicomp_emailSlot.json` (kismet-analyzer `to-json`, read-only); full uber dump saved to
  `research/bp_reflection/_spacerenderer_uber_full.txt`.
- Live-layout truth: `Game_0.9.0n/.../CXXHeaderDump/*.hpp` (offsets cited below are from
  the reflection dump = runtime-authoritative).
- Note on sizes: the cooked FUNCTION-local property `ElementSize` for
  `struct_signalTableState` reads 0x3E0, the LIVE struct is **0x3B8** (the hpp). Build all
  param frames from the live UFunction (our ParamFrame already does); never trust
  cooked-asset ElementSize for raw copies.

---

## 1. SKY SIGNALS (spaceRenderer) — the host-roller mirror

### 1.1 Class + instance resolution

- Class `AspaceRenderer_C : AActor`, size 0x2F4 (spaceRenderer.hpp).
- **Resolution: `mainGamemode.master_spaceRenderer @ 0x0340`** (`AspaceRenderer_C*`,
  mainGamemode.hpp:25) — the game's own resolver; every desk reference goes
  `gamemode.master_spaceRenderer.…` (desk uber @9337/@9740/@12864/@45048/@78564/@79159).
  A `FindObjectsByClass` walk (what `space_renderer.cpp::Instance()` does) is equivalent —
  the actor is a singleton placed in the base.
- spaceRenderer resolves its own backrefs at BeginPlay (+1 s Delay): uber @365
  `atlas := gamemode.analogPanels.widget; coordinatesUI := gamemode.analogPanels.widget.ui_coordinates`,
  then JUMP @15 → **the FIRST `spawnSignal()` roll fires at BeginPlay+1 s**.

Key fields (spaceRenderer.hpp, all verified):

| field | off | type | note |
|---|---|---|---|
| `Atlas` | 0x0248 | `Uui_consolesAtlas_C*` | set @365 |
| `move_right/left/up/down` | 0x0260-0x0263 | bool×4 | LIVE — tick reads them (@1712+); desk coords-exit clears them (@78564) |
| `coords` | 0x0264 | FVector2D | **DEAD — zero references in ANY bytecode** (see §4 contradictions) |
| `coords_rot` | 0x026C | FVector2D | **DEAD — zero references** |
| `isMoving` | 0x0274 | bool | live (tick @2045) |
| `signals` | **0x0278** | `TArray<Fstruct_signal_spawn>` | THE row array |
| `signals_a` | **0x0298** | `TArray<Uui_signal_C*>` | the paired widget array — **same index pairs a row with its widget** (proof below) |
| `GameMode` | 0x02C0 | `AmainGamemode_C*` | |
| `coordinatesUI` | 0x02C8 | `Uui_coordinates_C*` | the coords-screen widget (the REAL aim state holder, §2.6) |
| `triangleLevel` | 0x02F0 | float | catch-probability exponent input (desk writes @12864) |

### 1.2 `Fstruct_signal_spawn` (0x2C) — every field

| field (GUID-mangled name) | off | size | type | POD/wire verdict |
|---|---|---|---|---|
| `coordinates_3_…` | 0x00 | 0xC | FVector | POD ✔ (also the cross-peer identity, with frequency) |
| `type_7_…` | 0x0C | 4 | int32 | POD ✔ (always 0 — addSignal @374) |
| `strength_12_…` | 0x10 | 4 | float | POD ✔ |
| `frequency_14_…` | 0x14 | 4 | float | POD ✔ |
| `frequencySpread_24_…` | 0x18 | 4 | float | POD ✔ |
| `polarity_16_…` | 0x1C | 4 | float | POD ✔ |
| `polaritySPread_26_…` | 0x20 | 4 | float | POD ✔ |
| `objectName_67_…` | 0x24 | 8 | **FName** | **NOT wire-safe raw** (per-process name index) — ship as string, rebuild via StringToFName on the receiver (what `RowToWire`/`OverwriteRow` already do) ✔ |

No FString / TArray / UObject* members. Raw row copy is safe EXCEPT the FName.

### 1.3 `addSignal(FVector InVec)` — exact behavior (fn body, 1792 B)

Signature (cooked + live agree): one input param `InVec` (FVector, frame @0x00, size 0xC,
frame total 0x10). Flags `FUNC_Public|HasDefaults|BlueprintCallable|BlueprintEvent`.

What the CALLER passes vs what is ROLLED:

- **Passed in:** `InVec` — used ONLY for X/Y (`MakeVector(X, Y, +0f)` @97 → row
  `coordinates`). spawnSignal passes a random point inside
  `coordinatesUI.getAreaSize()` (uber @4010-4227) — so **InVec is screen-space coords**;
  a mirror passing the wire coords places the row + widget natively.
- **Rolled inside addSignal:**
  - `objectName` ← `lib_C::getWeightedObject(self, out)` @5
  - `polaritySPread` ← RandomFloatInRange(0.01, 0.99) @148
  - `frequencySpread` ← RandomFloatInRange(0.01, 0.99) @186
  - `polarity` ← RandomFloatInRange(0, 360) @224
  - `frequency` ← RandomFloatInRange(0, 1000) @262
  - `strength` ← RandomFloatInRange(0.5, 2.0) @300
  - `type` := 0 (const) @374
  - widget `direction` ← **RandomBool()** @949
  - widget `maxLifetime` ← **RandomFloatInRange(120, 240)** @1043
  - (Construct-time) dynmat `rand` ← RandomFloat() (visual shimmer only)
- Special branch @649-827: if `lib_C::getMainGameInstance(...).gamemode == b2`, the row
  gets FIXED `frequency := 750`, `polarity := 180` (then continues at @828). Host-roller
  is unaffected (the wire copies whatever the host's branch produced).

### 1.4 THE ROW→WIDGET BUILD LIST (addSignal @828-1788 — the prompt's "@896-1719")

> Correction: the RE doc cited "spaceRenderer uber @896-1719"; these addresses are inside
> **addSignal itself**, not the ubergraph.

Exact sequence (replicate verbatim if hand-building; reflected `addSignal` does all of it):

1. @828 `KismetArrayLibrary::Array_Add(signals, sig)` — row appended.
2. @896 `widget := WidgetBlueprintLibrary::Create(self, ui_signal_C, _)`.
3. @969 `SetBoolPropertyByName(widget, 'direction', RandomBool())`.
4. @1010 `SetObjectPropertyByName(widget, 'spaceRenderer', self)`.
5. @1081 `SetFloatPropertyByName(widget, 'maxLifetime', RandomFloatInRange(120,240))`.
6. @1168 `KismetSystemLibrary::SetStructurePropertyByName(widget, 'coordinates', Conv_VectorToVector2D(sig.coordinates))`.
7. @1231 `gamemode.analogPanels.Widget.ui_coordinates.canvas_spaceSigns.AddChild(widget)`
   — **widget parent = `ui_coordinates.canvas_spaceSigns @0x02A0`**. (Construct fires here
   and consumes `maxLifetime`/`direction` — see §1.6.)
8. @1450 `SlotAsCanvasSlot(widget).SetPosition(Conv_VectorToVector2D(sig.coordinates))`.
9. @1520 `SlotAsCanvasSlot(widget).SetSize((0,0))`.
10. @1678 `widget.SetRenderScale(MakeVector2D(Lerp(0,1,sig.strength), same))`.
11. @1720 `Array_Add(signals_a, widget)` — **same-call append ⇒ row i ↔ widget i**.

There is NO refresh/rebuild function on spaceRenderer that re-derives widget visuals from
rows (16 functions total, none is an "upd"). After overwriting a row+widget on a mirror,
the self-correcting pieces are: dynmat `'lifetime'` (rewritten every reduceLifetime tick)
and slot position (we passed the wire coords as InVec). The pieces that DON'T self-correct
post-Construct and need explicit pushes for exact parity (cosmetic except `direction` —
see §1.8): dynmat `'dir'`, dynmat `'pingSpeed'`, RenderScale. Recipe after `OverwriteRow`:
reflected `widget.dynmat(@0x0288).SetScalarParameterValue('dir', direction?1:0)`,
`('pingSpeed', lifeTime/180)`, and `widget.SetRenderScale(Lerp(0,1,strength))`
(UMaterialInstanceDynamic + UWidget natives — all ProcessEvent-callable).

### 1.5 `deleteSignal(const Uui_signal_C*& ItemToFind)` — removal is BY WIDGET POINTER

Frame: 1 param `ItemToFind` (ObjectProperty ui_signal_C, @0x00, flags OUT|REF|CONST —
pass the pointer; nothing is written back). Body (305 B):
`ind := Array_Find(signals_a, ItemToFind)`; if ind >= 0 → `Array_Remove(signals, ind)` +
`Array_Remove(signals_a, ind)` + `ItemToFind.RemoveFromParent()` (despawns the widget from
canvas_spaceSigns). **Both arrays compact at the same index — index pairing is preserved
across deletions.** Not index-based at the API; index is derived internally.

### 1.6 Widget lifetime (`ui_signal_C`, size 0x2AC) + expiry

Fields: `Loc` FLinearColor @0x0270; **`Direction` bool @0x0280**; **`Alpha` float @0x0284**
(the 1→0 countdown fraction); `dynmat` @0x0288; **`LifeTime` float @0x0290**;
`spaceRenderer` @0x0298; **`MaxLifetime` float @0x02A0**; `Coordinates` FVector2D @0x02A4.

- `Construct` (uber @10): `lifetime := maxLifetime`; dynmat := Image_59.GetDynamicMaterial;
  dynmat `'loc'` := own canvas-slot position (xy); `'rand'` := RandomFloat();
  `'dir'` := BoolToFloat(direction); `'pingSpeed'` := lifetime/180.
- **The per-frame reducer is spaceRenderer's tick** (uber @1400-1707, gated
  GlobalTimeDilation <= 1 @3622): for each `signals_a[i]` →
  `reduceLifetime(DeltaSeconds)`.
- `reduceLifetime(deltaTime)` (292 B): `alpha -= deltaTime / lifetime`; if `alpha <= 0` →
  **`spaceRenderer.deleteSignal(self)`** (THE expiry removal — each peer's copy
  self-expires locally); else dynmat `'lifetime'` := alpha.
- So the 120-240 s lifetime lives on the WIDGET (`MaxLifetime` roll @1043; `LifeTime`
  copied from it at Construct; `Alpha` is the actual remaining fraction). For exact expiry
  + fade parity a mirror must receive **MaxLifetime AND Alpha** (write Alpha @0x0284 after
  AddChild; writing only LifeTime/MaxLifetime leaves the mirror's Alpha at the
  Construct-default ⇒ it lives a full extra lifetime until the next reconcile sweep
  removes it).

### 1.7 `spawnSignal` — the self-re-arm shape (CONFIRMED)

`spawnSignal()` (no params) = thunk → `ExecuteUbergraph_spaceRenderer(4251)` → @4251
JUMP @3819:

- @3819 `EX_BindDelegate` → **BIND `spawnSignal`** onto self (creates the dynamic
  delegate {object=this spaceRenderer, function='spawnSignal'}).
- @3880 **`UKismetSystemLibrary::K2_SetTimerDelegate(delegate, RandomFloatInRange(20,60),
  bLooping=false, InitialStartDelay=0, InitialStartDelayVariance=0)`** (EX_CallMath; the
  timer's owning object is the spaceRenderer via the delegate binding).
- @3937-4000: if GetGlobalTimeDilation > 1.0 → POP (re-armed but roll SKIPPED — time-skip).
- @4010-4227: roll `addSignal(MakeVector(rand(0..areaX), rand(0..areaY), 0))`.

**`spawnSignal` is the ONLY re-armer** (the only K2_SetTimer* in the class; `addSignal`
never re-arms). BeginPlay calls `spawnSignal()` once (@15, after the +1 s init Delay) —
the chain is then self-perpetuating. One timer kill therefore silences the roller for the
instance's lifetime; calling reflected `spawnSignal()` once re-arms the native loop (and
rolls one signal) — the disconnect-restore verb.

### 1.8 `gatherSignal` (the catch) — occupant-local, and `direction` is GAMEPLAY

`gatherSignal(bool skipCheck; OUT bool return; OUT int32 index; OUT Fstruct_signal_spawn
data; OUT bool dir; OUT ui_signal_C* element; OUT bool radiusCheck; OUT bool
caughtAtLeastOne)` — frame offsets @0x00/0x08/0x10/0x18/0x48/0x50/0x58/0x60, total 0x68.

Body: loops local `signals`; per row `prob = areaOfCirclesIntersection_percent ^
Lerp(2, 0.333, triangleLevel)`; **`gathered := RandomBoolWithWeight(prob)` @831 — rolls on
the INTERACTING peer's machine only** (called solely from the desk uber @79159, the
coords-panel ping of the occupant). On catch: optional `objectName` override from
`saveSlot.forceObjects[0]` (@5193), `save_main.stats.signals_found++`, returns
`dir := signals_a[i].direction` (@5038).

The desk consume path (uber @9124-10507) — runs on the catcher:
- gate @9189: **`gatherSignal_dir == widget.ui_coordinates.direction` OR
  `has_autoCoordRot`** — ⇒ **the widget's `Direction` field gates catch SUCCESS; it MUST
  ride the wire** (a mirror's self-rolled Direction would make the same signal
  catchable/uncatchable differently per peer). `WireSkySignal` currently lacks it — add
  `uint8 direction`.
- @9337 `gamemode.master_spaceRenderer.deleteSignal(element)` — the catch removal (the
  existing SkySignalCatch relay covers cross-peer propagation).
- @9404 `writeToCoordLog_2('Successful ping. …')`; @9494-10022 all `gamemode.dishs[i].
  startMovingTo(getCoords(normCoords())*100000)` (the BIG dishes — world actors, occupant-
  local; out of phase-2 scope, flag for the dish/world track); @10027 ping success sound;
  @10050-10244 clears `DL_signalDownloadData` + `DL_SignalDownloadDLData` (struct consts).

### 1.9 Timer kill (U5) — the engine API in THIS 4.27 build (Engine.hpp 13165-13189)

```
FTimerHandle K2_SetTimerDelegate(FK2_SetTimerDelegateDelegate Delegate, float Time,
                                 bool bLooping, float InitialStartDelay, float InitialStartDelayVariance);
FTimerHandle K2_SetTimer(UObject* Object, FString FunctionName, float Time, bool bLooping,
                         float InitialStartDelay, float InitialStartDelayVariance);
void K2_ClearTimerHandle(const UObject* WorldContextObject, FTimerHandle Handle);
void K2_ClearTimerDelegate(FK2_ClearTimerDelegateDelegate Delegate);
void K2_ClearTimer(UObject* Object, FString FunctionName);          // <- THE one
bool  K2_IsTimerActive(UObject* Object, FString FunctionName);      // verification helper
float K2_GetTimerRemainingTime(UObject* Object, FString FunctionName);
```

- **`K2_ClearTimer(Object, FString FunctionName)` EXISTS and is callable with only
  {object ptr + function-name string}** — UE4 source builds the FTimerDynamicDelegate from
  Object+FName internally, the exact inverse of the BP's K2_SetTimerDelegate({self,
  'spawnSignal'}). Call on the KismetSystemLibrary CDO with Object=the spaceRenderer,
  FunctionName=L"spawnSignal" (FString num/max = len+1 = 12).
- `K2_ClearTimerDelegate` needs a delegate param: a BP dynamic delegate in a param frame
  is FScriptDelegate = {FWeakObjectPtr (int32 idx + int32 serial), FName} = 0x10 bytes —
  buildable but pointless given K2_ClearTimer.
- Smoke verification recipe: `K2_IsTimerActive(sr, "spawnSignal")` true → KillClientSpawnTimer
  → false; `signals.Num` flat for 10 min with both peers idle (the U5 measurement).

---

## 2. DESK STATE (analogDScreenTest)

### 2.1 `gatherData` / `setData` — exact signatures

- `gatherData(OUT struct_signalTableState data)` — single OUT param @0x00 (cooked
  ElementSize 0x3E0; live struct 0x3B8 — use live). 24 stmts: pure MakeStruct copy of 21
  live fields, **no side effects**. Source-field mapping divergences worth knowing:
  `DL_resPercent ← DL_resDetecPercent@0x0970`, `backPlatesStates ← panelsState`,
  `backPlatesUnscrew ← panelsUnscrew`, `remapVal ← remapValue@0x1324`,
  `radarMods ← radar(0x0B58).upgrades` (the panel_radar actor's array). **No log text is
  packed** (contradiction vs the prior doc — §4).
- `setData(struct_signalTableState data)` — single input @0x00. Body (1305 B): copies all
  21 fields back, `DL_resDetecPercent = FClamp(DL_resPercent, 0, 0.999999)`, then the
  refresh chain in EXECUTION order:
  `updToggles → updateCoordCoords → updPolarity → updVolume → Widget."Create Signal List"
  → updComp(comp_data_0.size > 0) → [if comp_progress > 0: comp_start(comp_progress, OUT succ)]
  → formDownload(DL_SignalDownloadDLData.decoded, .polarity) → updPanels → updPhysMods
  → updRemap → radar.upgrades := radarMods; radar.updModules() → setFullyProcessedSignalObject()`.
- Callers (whole pak): `mainGamemode.loadObjects @6241` (setData from
  `saveSlot.analogPanelsData @0x08E8`) and `saveObjects @3573` (gatherData →
  `analogPanelsData`). Designed as the load/save marshal — i.e. the claim-release/join
  true-up verb, NOT a cadence verb.

### 2.2 `Fstruct_signalTableState` (0x3B8) field-by-field

| field | off | size | type | POD? | live-mirror verdict |
|---|---|---|---|---|---|
| `DL_signalDownloadData` | 0x000 | 0x70 | Fstruct_spaceObject | **NO** — FText displayName @+0x00, FName signalName @+0x18, UStaticMesh* ×2 @+0x20/+0x28, TSubclassOf @+0x30, rotators, 3 enum bytes | live mirror can SKIP: re-derivable on any peer via `formDownload` ← DataTable `list_objects[coord_signalData.objectName]` (formDownload @0-92 does exactly that). Ship only the objectName FName (string) |
| `DL_SignalDownloadDLData` | 0x070 | 0x70 | Fstruct_signalDataDynamic | **NO** — FString name @+0x00, FString id @+0x18, FDateTime @+0x30, FVector2D, FName object @+0x48, FName signal @+0x50, Fstruct_byteImage @+0x58 (TArray), 3 enum bytes, floats | must SERIALIZE name/id strings + scalars + FNames-as-strings if mirrored; `image` (byteImage) skippable live (heavy; rides save-transfer / bulk lane) |
| `DL_poFilterOffset` | 0x0E0 | 4 | float | ✔ | cheap set |
| `DL_FrFilterOffset` | 0x0E4 | 4 | float | ✔ | cheap set |
| `DL_poFilterSpeed` | 0x0E8 | 4 | float | ✔ | cheap set |
| `DL_FrFilterSpeed` | 0x0EC | 4 | float | ✔ | cheap set |
| `DL_activeFrFilter` | 0x0F0 | 1 | bool | ✔ | cheap set |
| `DL_activePoFilter` | 0x0F1 | 1 | bool | ✔ | cheap set |
| `DL_downloading` | 0x0F4 | 4 | float | ✔ | cheap set |
| `DL_PolarityDir` | 0x0F8 | 4 | int32 | ✔ | cheap set |
| `comp_maxLevel` | 0x0FC | 4 | int32 | ✔ | cheap set |
| `comp_progress` | 0x100 | 4 | float | ✔ | cheap set |
| `comp_data_0` | 0x108 | 0x70 | Fstruct_signalDataDynamic | **NO** (same as above) | same verdict |
| `signalPlayData` | 0x178 | 0x1C8 | Fstruct_signal_data | **NO** — FStrings name/unique_ID/specialResponse, **UTexture2D*×4 @+0x50-0x68, USoundBase*×4 @+0x70-0x88 (object PTRS — never wire)**, FText×4 @+0x90-0xD8, byteImage, nested spaceObject, 3 TArrays | live mirror SKIP (the play screen's loaded signal; a mirror only needs play_selectIndex + the savedSignals list which rides its own channel). On release-blob: re-derive from `gamemode.savedSignals_0[play_selectIndex]` on the receiver instead of serializing |
| `coord_signalData` | 0x340 | 0x2C | Fstruct_signal_spawn | ✔ except FName @+0x24 | ship (it's the §1.2 row shape — reuse WireSkySignal sans lifetimes) |
| `DL_resPercent` | 0x36C | 4 | float | ✔ | cheap set |
| `physMods` | 0x370 | 0x10 | TArray<enum byte> | array | ≤12 slots; ship as count+bytes if mirrored (drive-slot/module feature — currently out of scope) |
| `backPlatesStates` | 0x380 | 0x10 | TArray<bool> | array | same |
| `backPlatesUnscrew` | 0x390 | 0x10 | TArray<bool> | array | same |
| `remapVal` | 0x3A0 | 8 | FVector2D | ✔ | cheap set |
| `radarMods` | 0x3A8 | 0x10 | TArray<enum byte> | array | radar upgrade modules; same |

### 2.3 The CHEAP-SCALAR live set — §4.1 offsets ALL VERIFIED + additions

All prior-doc offsets confirmed against analogDScreenTest.hpp:
`play_selectIndex @0x06C4 int32` · `play_volume @0x06D0 int32` · `active_play @0x06E8 bool`
· `DL_nosiga @0x08F0 f` … `active_download @0x0A34 bool` (the DL block) ·
`active_coords @0x0A35 bool` · `coord_signalData @0x0A38 (0x2C)` ·
`coord_coordLogText @0x0A68 FString` (**DEAD — see §2.5**) · `coord_isPing @0x0A7C bool` ·
`coord_cooldown @0x0A80 f` · `comp_progress @0x0AA8 f` · `comp_hecertext @0x0B30 FString` ·
`active_comp @0x0B41 bool` · `canDL @0x1468 bool` · `controllingCoordinatePanel @0x1410` ·
`player_using @0x1358`.

Exact sub-block detail (live-visible scalars the current 21-field set uses or should know):

| name | off | type |
|---|---|---|
| DL_poFilterOffset | 0x09F4 | float |
| DL_FrFilterOffset | 0x09F8 | float |
| DL_poFilterSpeed | 0x09FC | float |
| DL_FrFilterSpeed | 0x0A00 | float |
| DL_activeFrFilter | 0x0A04 | bool |
| DL_activePoFilter | 0x0A05 | bool |
| DL_frData / DL_poData | 0x0A0C / 0x0A10 | float (the live needle inputs) |
| DL_downloading | 0x0A2C | float |
| DL_PolarityDir | 0x0A30 | int32 |
| DL_resDetecPercent | 0x0970 | float |
| comp_maxLevel | 0x0AA0 | int32 |
| comp_isDecodeActive | **0x0AAC** | bool (the decode latch — see §2.4) |
| comp_downloading / comp_mining / comp_miningInt | 0x0B20 / 0x0B24 / 0x0B28 | float/float/int32 |
| coord_ping_outer | 0x0A78 | float (ping ring anim) |
| coord_ping_inner / coord_ping_ping / coord_pingStage | 0x13AC / 0x13B0 / 0x13B4 | float/float/int32 |
| **coord_coordLog2Text** | **0x0BF8** | FString — THE live coord log (§2.5) |
| play_signalText | 0x0700 | FString (play screen text pane) |
| remapValue | 0x1324 | FVector2D |
| physMods | 0x12A0 | TArray<enum> |
| pingDishes | **0x13B8** | TArray<AcoordRadarDish_C*> (the 3 coordinate radars) |
| radar | 0x0B58 | Apanel_radar_C* |
| Widget | 0x0638 | Uui_consolesAtlas_C* (→ `ui_coordinates @0x0588` on the atlas) |
| deny | 0x0538 | UAudioComponent (sound `buttonshort14`) |

Obviously-missing live-visible scalars vs the shipped 21-field set: `comp_isDecodeActive`
(needed for the decode-edge logic, §2.4), `DL_frData/DL_poData` (needle positions — the
mirror's needles derive from its OWN tick; skip), `coord_pingStage/coord_ping_*` (ping
animation — occupant-local anim, skip), `comp_miningInt/comp_mining` (comp mining readout
— add if the comp pane must mirror mid-mine), `play_signalText`/`comp_hecertext` (FStrings,
text panes — add to the release snapshot if exactness wanted).

### 2.4 Refresh / repaint verbs — signatures + side-effect audit (U10)

All confirmed `FUNC_Public|BlueprintCallable|BlueprintEvent`; scanned for
SetTimer/PlaySound/SpawnActor/Delay/saveSlot-writes/Activate:

| verb | params | side effects |
|---|---|---|
| `updToggles` | none | CLEAN |
| `updPolarity` | none | CLEAN |
| `updateCoordCoords` | none | CLEAN — repaints `widget.txt1_azim_x/txt1_alti_y` from `normCoords()` (= viewCoordinate-derived; belongs in the dish-aim apply) |
| `updVolume` | none | CLEAN |
| `updComp(bool condition)` | 1 bool @0x00 | CLEAN (condition = "has comp data" = `comp_data_0.size > 0`) |
| `updPanels` / `updPhysMods` / `updRemap` / `updText` / `updScroll` | none | CLEAN |
| `updMaxLevelLights` / `updPolarityLights` / `updPlaybackLights` / `updCoordLights` / `setPanelLightColors` | none | CLEAN (reads saveSlot upgrades only) |
| `Widget."Create Signal List"` (ui_consolesAtlas) | none | CLEAN (rebuilds the saved-signal list pane) |
| `radar.updModules` (panel_radar) | none | CLEAN |
| `formDownload(float decoded, int32 polarity)` | @0x00/@0x08 | reads DataTable `list_objects[coord_signalData.objectName]` → `DL_signalDownloadData := row`; **writes `gamemode.objectRenderer.type` + calls `objectRenderer.init_objectRenderer(objectName)`** (the space-object 3D preview — shared singleton; mutating it on a mirror is the DESIRED parity); → `initDownloadSignal(coords2D, decoded, polarity)` (CLEAN) |
| `comp_start(float startPorgressFrom, OUT bool succ)` | @0x00/@0x08 | gates `level<3 && !comp_isDecodeActive && size>0 && level<upg_processLvl`; PASS → `comp_isDecodeActive:=true; comp_progress:=param;` widget text; `computerWorking_Cue.SetSound+Activate` (loop start). **FAIL → `deny.Activate(true)` — the deny buzz.** No saveSlot writes, no timers, no spawns |
| `comp_stop()` | none | if active → `comp_isDecodeActive:=false`, text reset, cue := `computerWorking_end` + Activate (wind-down). **If NOT active → deny buzz** — only call on a true 1→0 edge |

**U10 verdict refinement:** `setData` itself touches NO saveSlot, binds NO timers, spawns
nothing. The two behavioral hazards on a LIVE mirror:
1. `comp_start` deny-buzz: a repeated setData while `comp_isDecodeActive` is already true
   (a previous apply set it) hits the @57 gate → `deny.Activate` per apply. ⇒ setData is
   safe as the **claim-release/join one-shot** (the SP load shape: flag false → comp_start
   restarts the decode loop cleanly); before applying, write `comp_isDecodeActive=false
   @0x0AAC` to reproduce the fresh-actor precondition. NEVER call setData at cadence.
2. Decode END on the owner never propagates through scalars alone: the mirror's
   `computerWorking_Cue` keeps looping and `comp_isDecodeActive` stays true. Mirror the
   owner's `comp_isDecodeActive` edge: on 1→0 call reflected `comp_stop()` (plays the
   native wind-down); on 0→1 call `comp_start(wire comp_progress)`.

### 2.5 The coord log — TWO fields, one is dead, one fn is a stub

- `writeToCoordLog(FString B)` — **EMPTY STUB** (body = RETURN; 3 bytes).
- `writeToCoordLog_2(FString B)` (485 B) — THE log writer: appends `B + "\r\n"` to
  **`coord_coordLog2Text @0x0BF8`**, trims to the LAST 1000 chars (`Right(.,1000)`),
  repaints `widget.RichTextBlock_coordLog` + scrolls `scbox_coordLog2` to end.
- `coord_coordLogText @0x0A68` — **referenced NOWHERE in the desk bytecode** (legacy dead
  field; always empty).
- ⇒ **console_desk.cpp BUG (uncommitted impl, fixed same day; 2026-07-19
  `f74d05dc` retired the positional g_fields table entirely — the live-log
  offset is now the named `g_offCoordCoordLog2Text`):** `g_fields[20]` reads the dead
  `coord_coordLogText` (tail always empty) and `AppendCoordLog` calls the stub
  `writeToCoordLog` (silent no-op). Fix: read `coord_coordLog2Text`; append via
  `writeToCoordLog_2` — note the callee strips nothing but APPENDS `\r\n` per call, so the
  suffix-append protocol should send line-granular suffixes (or strip the trailing CRLF
  before re-append) to avoid double blank lines; the native 1000-char cap self-limits the
  field.

### 2.6 Dish aim — `spaceRenderer.coords` is DEAD; the truth is `ui_coordinates`

Bytecode-proven (scan over spaceRenderer + desk + dish + ui_coordinates dumps):
`spaceRenderer.coords @0x0264` and `coords_rot @0x026C` have ZERO references anywhere.
The real aim state lives on **`Uui_coordinates_C`** (size 0x458; resolve via
`gamemode.analogPanels(0x0EC0) → .Widget(0x0638) → .ui_coordinates(0x0588)`, or
spaceRenderer`.coordinatesUI @0x02C8`):

| field | off | type | meaning |
|---|---|---|---|
| `viewCoordinate` | **0x03B8** | FVector2D | the live cursor (screen-space; `normCoords()` maps it to az 0-360 / alt 0-90) |
| `Coordinate_0/1/2` | **0x03C0/0x03C8/0x03D0** | FVector2D ×3 | the 3 ping-tower aims (the triangle) |
| `selected` | **0x03D8** | int32 | which slot `getCursorLocation`/`setCoordinateLocation` address (0-3 switch) |
| `coordinate_middle` | 0x03DC | FVector2D | derived (updCursorLocations @1603) |
| `innerCircleRadius` | 0x03E4 | float | derived |
| `canPing` | 0x0420 | bool | |
| `exploreMode` | 0x0440 | bool | |
| `Direction` | 0x0441 | bool | the panel's direction toggle (the §1.8 catch gate's RHS) |
| `coordsInPlace` / `activeCoord_0/1/2` | 0x0450-0x0453 | bool×4 | tower state |
| `canvas_spaceSigns` | 0x02A0 | UCanvasPanel* | ui_signal parent |
| `coordRadarsSpeed` | 0x0454 | float | upgrade-derived |

Setter/getter/mover semantics (bytecode):

- `setCoordinateLocation(FVector2D Value)`: `Coordinate_<selected> := Value` (lvalue
  switch, 4 cases incl. viewCoordinate slot); `viewCoordinate := Value`;
  `updCursorLocations()`.
- `getCursorLocation() -> FVector2D`: returns the `selected` slot's value.
- `movePointToCursor(int32 selected, OUT bool onCooldown)`: gates broken-radar +
  `atlas.panel.coord_cooldown <= 0`; commits `Coordinate_<selected> := viewCoordinate`;
  `coord_cooldown := getMaxCooldown()/2`; logs + ping sound. (The "bring coord" buttons.)
- **`updCursorLocations()` (8225 B, CLEAN) is the one-true repaint AND the physical-dish
  mover**: recomputes middle/lines/perimeter, repositions the canvas verts, then loops
  **`atlas.panel.pingDishes` (desk @0x13B8): `getHorizontalAngle(canvas_vert_i position)`
  → `pingDishes[i].setRot(angle)` (@3230-4069; coordRadarDish.setRot =
  `rot_Z.K2_SetWorldRotation(yaw)`)**. Writing the 4 coordinate fields then calling this
  one verb mirrors screen AND the physical coordinate-radar dishes.
- The spaceRenderer tick keeps the loop alive on EVERY peer: uber @518-970
  `coordinatesUI.setCoordinateLocation(getCursorLocation() + movement)` runs per tick
  (movement decays to ~0 when move flags are false) → a remote peer's own tick re-runs
  updCursorLocations every frame. ⇒ raw-writing `viewCoordinate + Coordinate_0..2 +
  selected` on the remote widget is self-publishing within one frame — an explicit
  updCursorLocations call is optional belt-and-braces; `updateCoordCoords` (desk) repaints
  the az/alt TEXT and should be called on apply (nothing else calls it on a mirror).
- The move_* flags + `movementSpeed/coordinateDrift` (upgrade-lerped, uber @4256) drive a
  FRAME-RATE-LOCAL integrator (`Vector2DInterpTo` @679) — streaming flags reproduces
  motion only approximately and DIVERGES; stream the COORDINATES.

**⇒ DishAimStatePayload v64 (coordsX/Y + coordsRotX/Y + moveBits) mirrors dead fields +
the divergent integrator. Replace with `{viewCoordinate, Coordinate_0, Coordinate_1,
Coordinate_2, selected}` (36 B) read from/written to ui_coordinates; apply = 5 raw writes
(+ optional reflected `updCursorLocations()`, + desk `updateCoordCoords()`).** The
existing ~3 Hz claimed-only cadence + HolderOf gate are right.

### 2.7 Existing-impl cross-check (uncommitted, protocol v64)

- `ue_wrap/space_renderer.cpp`: addSignal-then-OverwriteRow shape matches bytecode
  (tail-append assumption correct: Array_Add appends; both arrays same call).
  `KillClientSpawnTimer` (K2_ClearTimer, FString num=12) and `RestoreRoller`
  (reflected spawnSignal) are exactly right. Gaps: no `Direction` (gameplay, §1.8), no
  `Alpha` (expiry/fade), no post-overwrite dynmat/RenderScale push (§1.4).
- `ue_wrap/console_desk.cpp`: 21-field live set offsets all correct; refresh verbs all
  CLEAN/param-less ✔ — but the list omits `updComp(bool)` + `updateCoordCoords` (comp pane
  + az/alt text stay stale on mirrors), and the coordLog pair is wired to the dead field +
  stub fn (§2.5).
- `coop/console_state_sync.cpp`: host-auth snapshot + catch relay + HolderOf gating are
  sound. The desk DiscreteDiffer edge set is sensible. `comp_isDecodeActive` edge → 
  comp_start/comp_stop is missing (§2.4 item 2).

---

## 3. EMAILS (gamemode.addEmail)

### 3.1 The append chain — exact signatures

- **`mainGamemode.addEmail(Fstruct_email item)`** — 1 param @0x00, size 0x50, flags
  OUT|REF|CONST (in-out by ref; nothing meaningful written back). Body (569 B):
  1. @0 `item.date := daynightCycle.timeZ` (**re-stamps the date** — receiver-side clock
     is host-synced via time_sync, dates converge).
  2. @58 `laptop.addEmail(item)` → **`ui_laptop.addEmail(Fstruct_email isNew)`** (433 B):
     `Array_Add(laptop.gamemode.saveSlot.emails, isNew)` — THE persistence append; then
     Create `uicomp_emailSlot_C`, `master := self`, `vb_emails.AddChild`,
     `Array_Add(slots_emails, slot)`, `slot.upd(isNew)`, `lib_C::recountChildren`.
  3. @103-225 `PlaySoundAtLocation(self, SoundWave'email', laptop.laptop.location + off,
     0.25, 1.0, att_small)` — the email ding at the physical laptop.
  4. @293+ if the email tab isn't open: highlight `button_tab_email` cyan.
  ⇒ Reflected `gamemode.addEmail(item)` on the receiving peer reproduces persistence +
  list row + sound + tab highlight in one call (the prior doc's recipe — CONFIRMED).
- `lib_C.addEmail(UTexture2D* "Item Pfp", enum_emailChars byte "Item Username1",
  FText "Item Topic", FText "Item Text", UObject* __WorldContext)` — static; builds the
  struct {new=true, date=(0,0,0)} → gamemode.addEmail. (The senders' funnel; param frame
  @0x00/0x08/0x10/0x28/0x40, total 0x48.)
- **Deletion exists** (NOT append-only): `ui_laptop.delEmail(int32 Index)` —
  `Array_Remove(slots_emails, Index)` + `Array_Remove(saveSlot.emails, Index)` (player
  clicks the row's del button). The watermark poll must tolerate count DECREASE (rebase
  watermark to Num) and, for full parity, mirror deletes by index (reflected `delEmail`)
  — arrays stay index-aligned because appends are host-relayed in one order.
- `ui_laptop.selectEmail(index)` sets `emails[index].new := false` (read marker — per-peer
  cosmetic divergence, ignore) and `Img_pfp.SetBrushFromTexture(emails[index].pfp)`; a
  null pfp renders an empty brush (no crash); index<0 path uses the default texture
  `obj<pfp>`.

### 3.2 `Fstruct_email` (0x50) layout

| field | off | size | type | wire |
|---|---|---|---|---|
| `new` | 0x00 | 1 | bool | ship (true) |
| `pfp` | 0x08 | 8 | **UTexture2D*** | ship the texture's full object PATH string; receiver `FindObject`; null-safe |
| `username` | 0x10 | 1 | TEnumAsByte<enum_emailChars> (values 0-9, display names resolved at runtime via `GetEnumeratorUserFriendlyName(obj<enum_emailChars>, byte)` — identical on every peer) | ship the byte |
| `date` | 0x14 | 0xC | FIntVector | ship-optional (gamemode.addEmail overwrites from the synced clock) |
| `topic` | 0x20 | 0x18 | FText | ship as string; rebuild via `UKismetTextLibrary::Conv_StringToText(FString)` (Engine.hpp:13354, class @13327) |
| `text` | 0x38 | 0x18 | FText | same |

FText build recipe (extends the pinned-empty ftext_utils precedent): reflected
`Conv_StringToText(wireString)` on the KismetTextLibrary CDO; memcpy the 0x18 OUT bytes
into the param-frame struct slot. The ParamFrame's raw-free leaks the +1 ref per mint
(the deliberate ftext_utils mechanism) — at email rates this is negligible; the receiving
Array_Add deep-copies with proper refs.

### 3.3 saveSlot backing + producers

- **`saveSlot.emails @0x0118`, `TArray<Fstruct_email>`, stride 0x50** (saveSlot.hpp:21;
  `emailsDef @0x02D0` is the new-game default set — ignore). Watermark-poll `Num` rising
  edge on the PRODUCING peer → serialize rows [watermark..Num) → host-relay (the
  OrderRequest shape); falling edge → rebase (+ optional EmailDelete{index}).
- Producers (all funnel through gamemode.addEmail, all `EX_LocalVirtualFunction` =
  PE-invisible — the poll is the only detector): daynightCycle task mails (uber
  @6671/@7094/@7900 — **every one passes the DEFAULT avatar `obj<pfp>` + username byte 0**),
  drone sell `lib_C::getResponse` (host-side, v48), SAT-console status mails (the console
  occupant), desk special-download mails (the desk occupant).
- **`list_emailCharacters` (DataTable, /Game/main/datatables/) is NOT read by the email
  pipeline at runtime** — pak-wide scans of ui_laptop/mainGamemode/lib/uicomp_emailSlot
  show zero references; the list row widget renders the username via
  `GetEnumeratorUserFriendlyName`, the reader pane uses the STORED pfp pointer.
  ⇒ The prior doc's "re-resolve the texture from list_emailCharacters" is unnecessary
  (and was never the game's mechanism): ship the pfp object path (usually
  `/Game/textures/pfp.pfp`-class; resolve-or-null on the receiver).

---

## 4. STRAIGHT ANSWERS

**U5 (timer kill)** — RESOLVED, verdict: **K2_ClearTimer(Object=spaceRenderer,
FunctionName="spawnSignal") exists in this build (Engine.hpp:13186) and kills the
K2_SetTimerDelegate-armed roller; spawnSignal (uber @3819 BIND + @3880 K2_SetTimerDelegate,
20-60 s one-shot) is the only re-armer, so one kill per instance suffices; reflected
spawnSignal() is the exact restore.** Evidence: §1.7/§1.9. The shipped
`KillClientSpawnTimer`/`RestoreRoller` implement exactly this; verify live with
`K2_IsTimerActive` + a 10-min `signals.Num` flat-line (the original U5 measurement).

**U10 (setData mid-session safety)** — RESOLVED, verdict: **setData writes no saveSlot,
binds no timers, spawns nothing — but it is a one-shot LOAD verb, not a cadence verb:
its comp_start step deny-buzzes whenever `comp_isDecodeActive` is already true, and
nothing in it ever STOPS a running decode.** Safe usage: claim-release/join true-up only,
preceded by `comp_isDecodeActive=false @0x0AAC` (the fresh-actor precondition the SP load
path has implicitly); live decode edges ride `comp_start(progress)` / `comp_stop()`
(§2.4). The current impl avoids setData entirely — fine, but it then must add the
decode-edge pair and (if download identity matters) `formDownload` on coord_signalData
changes.

### Contradictions vs the prior RE doc (and the uncommitted impl)

1. **"dish aim = `spaceRenderer.coords @0x0264` + move flags" is WRONG.** `coords` and
   `coords_rot` are referenced by NO bytecode anywhere (dead fields). The aim state =
   `ui_coordinates.viewCoordinate @0x03B8` + `Coordinate_0/1/2 @0x03C0-0x03D0` +
   `selected @0x03D8`; apply via raw writes (+ `updCursorLocations()` which also rotates
   the physical `pingDishes` via `setRot`). The v64 `DishAimStatePayload` and
   `SR::ReadDishAim/WriteDishAim` must be reworked (§2.6).
2. **"coord_coordLogText @0x0A68 — the coord log!" is WRONG.** That field is dead; the
   rendered log is `coord_coordLog2Text @0x0BF8` (self-capped 1000 chars), written only by
   `writeToCoordLog_2`; `writeToCoordLog` (no suffix) is an empty 3-byte stub. The
   uncommitted console_desk.cpp wires BOTH wrong ends (§2.5).
3. **"coordLog text rides `coord_coordLogText` in the [gatherData] blob" is WRONG** — the
   0x3B8 struct contains NO log/string field at all (§2.1/§2.2); the log needs its own
   wire piece (which DeskStatePayload already has — just pointed at the wrong field).
4. **"the RE doc cites spaceRenderer uber @896-1719"** — those addresses are inside
   `addSignal`, not the ubergraph (§1.4). Same content, wrong container.
5. **"pfp: re-resolve from `list_emailCharacters`" is unnecessary/not the game's
   mechanism** — the pipeline stores and renders the caller-supplied texture pointer;
   list_emailCharacters is unreferenced by the email path (§3.3). Ship the texture path.
6. **NEW gameplay-load-bearing find the prior doc missed:** the ui_signal widget's
   `Direction @0x0280` gates catch success (desk uber @9189
   `gatherSignal_dir == ui_coordinates.direction OR has_autoCoordRot`) — it must ride the
   sky-signal wire (WireSkySignal currently lacks it), together with `Alpha @0x0284` for
   expiry/fade parity (§1.6/§1.8).
7. Minor: emails are NOT append-only (`ui_laptop.delEmail(Index)` exists) — the watermark
   needs shrink handling (§3.1).
