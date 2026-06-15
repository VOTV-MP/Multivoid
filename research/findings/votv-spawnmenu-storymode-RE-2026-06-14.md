# VOTV prop-spawn menu (Q) in story mode -- RE + dev-toggle implementation (2026-06-14)

Goal: a DEV toggle that lets the sandbox-mode "Q" prop-spawn menu work in STORY
mode (for testing). OFF by default, client-locked via `coop::dev_gate`, gated so
it never affects normal play. No asset edits (RULE 3); root-cause approach (RULE 1).

---

## 1. RE ground truth

### 1a. enum_gamemode mapping -- SANDBOX = 4, STORY = 0

`enum_gamemode::Type` names are stripped to `NewEnumerator0..7` in the SDK dump
(`Game_0.9.0n/.../CXXHeaderDump/enum_gamemode_enums.hpp`). Recovered the real
names from the UserDefinedEnum's `DisplayNameMap` (explicit key->value pairs, so
order-independent) in `research/pak_re/extracted/VotV/Content/main/enums/enum_gamemode.uexp`:

| value | NewEnumeratorN | name      |
|-------|----------------|-----------|
| 0     | 0              | **story** |
| 1     | 1              | infinite  |
| 2     | 2              | tutorial  |
| 3     | 3              | custom    |
| **4** | 4              | **sandbox** |
| 5     | 5              | halloween |
| 6     | 6              | ambient   |
| 7     | 7              | solar     |

Corroborated by live bytecode (`research/bp_reflection/mainGamemode.json`,
`gameInstance.gamemode` = `mainGameInstance.GameMode @0x01E1`):
- `ExecuteUbergraph_mainGamemode @34435: EqualEqual_ByteByte(gameInstance.gamemode, b4)`
  -> `@34488: hasWeapon := isFlying OR (==b4)` (sandbox grants weapons/creative).
- `canBackrooms @749: NotEqual_ByteByte(gameInstance.gamemode, b4)` (no backrooms in sandbox).
- `saveObjects @4237: ==b0` / `@106376: ==b1 OR ==b0` (story/infinite narrative paths).

(`enum_difficulty` Easy++/Easy/Normal/Hard/Hard++ is a SEPARATE enum -- not gamemode.)

### 1b. THE KEY FINDING: the Q spawn-menu OPEN path is NOT gamemode-gated

The Q key fires `mainPlayer` UFunctions (`research/bp_reflection/mainPlayer.json`):
- `InpActEvt_spawnmenu_K2Node_InputActionEvent_2(FKey Key)` -- PRESSED edge,
  forwards to `ExecuteUbergraph_mainPlayer` entry **@12077** (OPEN).
- `InpActEvt_spawnmenu_K2Node_InputActionEvent_3(FKey Key)` -- RELEASED edge,
  forwards to entry **@11555** (CLOSE).

The OPEN block (@12077..@12834), exact trace:
```
@12077: Temp_struct_Variable_1 := K2Node_InputActionEvent_Key_2
@12104: IsValid(activeInterface)               <- guard 1
@12133: IFNOT(activeInterface valid) JUMP @12148
@12147: POP->ret                               (already in another UI -> do nothing)
@12148: lib.isBuoyant(...)  -> IFNOT POP        <- guard 2 (don't open while swimming)
@12205..@12386: SetVisibility(spawnmenu, Visible)
@12494: SetInputMode_GameAndUIEx(PC, spawnmenu, ...)
@12650: spawnmenu.Button_search.SetVisibility(...)
@12754: spawnmenu.opened()
```
**There is NO `EqualEqual_ByteByte(gamemode, ...)` anywhere in this block.** The
only guards are `IsValid(activeInterface)` and `lib.isBuoyant`.

Everything downstream is likewise ungated:
- `ui_spawnmenu_C` (widget): the entire asset (`opened()`, `Construct()`, `Spawn`,
  the ubergraph) has ZERO references to `GameMode`/`enum_gamemode`/`sandbox`.
- `mainGamemode.spawnPropThroughGamemode`: spawns unconditionally (no gamemode gate).
- The widget itself is created in EVERY gamemode at `propProcessor` startup
  (`ExecuteUbergraph_propProcessor @16400: WidgetBlueprintLibrary.Create(ui_spawnmenu_C)`
  + `@16498: AddToViewport`); `propRenderer`/`propProcessor_C` has no gamemode check.
- The shipping exe has NO `"spawnmenu"`/`"isSandbox"`/`"canSpawn"`/`"enum_gamemode"`
  strings (IDA: `VotV-Win64-Shipping.exe.i64`); the `InpActEvt_spawnmenu_*` events
  are BP-VM-dispatched (ProcessEvent), so there is no native C++ handler interposing
  a gate. The 4 `"Sandbox"` strings are UE cooker internals (`FSandboxPlatformFile`).

**Conclusion:** the restriction of the Q spawn menu to sandbox is NOT an enum check
on the open/spawn path -- by the code, invoking the open UFunction opens the menu in
ANY gamemode (subject only to: not already in a UI, and not swimming). The remaining
candidate for "why Q doesn't open it in story for a normal player" is the engine-level
InputAction KEY MAPPING for the `"spawnmenu"` action (the Q->action binding lives in
`DefaultInput.ini`, baked into the cooked config -- not extractable from the assets we
have here). Static RE could NOT confirm/deny whether that mapping fires in story; this
is the one open question. The implementation below is robust to BOTH cases (see 3).

---

## 2. Approach chosen (and rejected alternatives)

**Chosen:** when the dev toggle is ON, watch the Q key ourselves (foreground-gated,
off-game-thread) and invoke the game's OWN spawnmenu-open UFunction
(`InpActEvt_spawnmenu_K2Node_InputActionEvent_2`) on the local `mainPlayer_C` via
ProcessEvent. This reproduces a real Q press verbatim -- the same SetVisibility /
SetInputMode_GameAndUIEx / `opened()` path, INCLUDING the `activeInterface`/`isBuoyant`
guards -- so the menu behaves identically to sandbox and can never double-spawn the
(already-existing) widget. An F1-menu "Open spawn menu now" button calls the same
path directly (no key race).

Why this is the root-cause, least-invasive fix:
- It does not fight or remove any gate (there is none on the open path), and does not
  edit `DefaultInput.ini` / any asset (RULE 3).
- It is robust to the one unknown (does the native Q mapping fire in story?): if the
  native mapping does NOT fire in story, our watcher supplies the open; if it DOES,
  the open is idempotent over an already-open menu.

**Rejected:**
- *Flip `mainGameInstance.GameMode` to sandbox (=4).* A crutch (RULE 1): `gamemode==4`
  drives weapons/creative-flight (`mainGamemode @34488`), backrooms (`canBackrooms`),
  and save/narrative branches (`saveObjects`) -- flipping it perturbs many unrelated
  systems. Rejected.
- *Manually CreateWidget(ui_spawnmenu_C)+AddToViewport ourselves.* Duplicates engine
  logic and risks a second widget (one already exists from `propProcessor` startup),
  and would re-implement the input-mode/cursor handling the BP open path already does.
  Rejected in favor of reusing the game's own open UFunction.

---

## 3. Files created / modified

Created:
- `src/votv-coop/include/ue_wrap/spawn_menu.h` + `src/votv-coop/src/ue_wrap/spawn_menu.cpp`
  -- engine substrate (principle 7). `Open()`: resolve+cache the
  `InpActEvt_spawnmenu_K2Node_InputActionEvent_2` UFunction on `mainPlayer_C`, build a
  zeroed `ParamFrame` (the FKey `Key` param is copied into the persistent frame but the
  open ubergraph never reads it for gating, so an empty/default FKey is correct), and
  `ue_wrap::Call(localPlayer, frame)`. Local player from `coop::players::Registry::Get().Local()`.
- `src/votv-coop/include/coop/dev/spawn_menu_unlock.h` + `src/votv-coop/src/coop/dev/spawn_menu_unlock.cpp`
  -- dev/gameplay layer. `SetEnabled(bool)` / `IsEnabled()` / `OpenNow()` / `Init()`.
  A lazily-started Q-key watcher thread (60 Hz, mirrors `freecam`'s InputDriverThread)
  that, while enabled, posts `ue_wrap::spawn_menu::Open()` to the game thread on Q's
  rising edge. Foreground-gated (`ini_config::IsOurWindowForeground()`) so a same-box
  host+client pair doesn't double-fire. `dev_gate::Allowed()` re-checked at SetEnabled,
  OpenNow, and EVERY watcher poll (auto-disables the instant this peer is a client).

Modified:
- `src/votv-coop/src/ui/dev_menu.cpp` -- include + `RenderSpawnMenuUnlock()` (a checkbox
  "Prop spawn menu in story mode (Q)" + an "Open spawn menu now" button) + tree entry
  under `Game > Entities` (`dev`-flagged, so it shows only under `[dev] devkeys` and
  hides for clients via the central `DevMode()` gate).
- `src/votv-coop/src/harness/harness.cpp` -- include + `coop::dev::spawn_menu_unlock::Init()`
  near `freecam::Init()` (boot force-on only via `[dev] spawn_menu_unlock=1`).
- `src/votv-coop/CMakeLists.txt` -- added `src/ue_wrap/spawn_menu.cpp` (ue_wrap block)
  and `src/coop/dev/spawn_menu_unlock.cpp` (coop/dev block), surgical inserts.

INI flag: `[dev] spawn_menu_unlock=1` force-enables at boot (default absent = OFF). The
master switch `[dev] enabled=0` forces it off; the F1 menu shows it under `[dev] devkeys`.

LOC (all well under the 800 soft cap): spawn_menu.h 36, spawn_menu.cpp 73,
spawn_menu_unlock.h 39, spawn_menu_unlock.cpp 112, dev_menu.cpp 304.

---

## 4. Build status

`cmake --build build/votv-coop --config RelWithDebInfo --target votv-coop`: **clean**.
Both new objects compiled (`spawn_menu.obj`, `spawn_menu_unlock.obj`); `votv-coop.dll`
linked. Only pre-existing opus/CRT link warnings (LNK4098/LNK4217), unrelated.

---

## 5. Needs user hands-on (cannot launch the game here)

1. **Does it open in story?** Start a STORY save, F1 > Game > Entities > check
   "Prop spawn menu in story mode (Q)", press Q (or click "Open spawn menu now").
   Expect the prop-spawn menu to appear and spawn props normally.
2. **The one static-RE unknown:** if Q already opened the spawn menu in story WITHOUT
   the toggle (native mapping present), the toggle is redundant for Q -- but the
   "Open spawn menu now" button + the explicit enable are still useful, and no harm
   (the open is idempotent). If Q did NOT open it natively, the toggle is what makes
   it work. Either way the feature is correct; please report which case is observed so
   the doc can record the input-mapping truth.
3. **Client lock:** as a joined client, the toggle/button must be hidden (dev menu
   gates clients out) and refuse if somehow triggered (logged "REFUSED ... client").
