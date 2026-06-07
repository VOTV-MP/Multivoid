# VOTV save-picker — ENUMERATE + LOAD-INTO-GAMEPLAY native call sequence (RE) — 2026-06-06

Load-bearing RE for the "Host Game -> pick save / New Game -> load -> host" feature.
User direction: **native VOTV functions, ImGui on top**. This documents the verified
native call sequence for (A) enumerating saves at the main menu, (B) resolving the
SaveGames directory, (C) loading a chosen save into gameplay, and (D) the residual
items that still need a live probe.

Authority = the CXX reflection dumps (`CXXHeaderDump/*.hpp`) + the live object dump
`UE4SS_ObjectDump_MAIN_MENU.txt` (captured AT the main menu — the correct dump for
menu-time enumeration) + IDA decompiles of the native plugin/engine helpers. Citations:
`hpp:line` for headers, `MM L#`-by-grep for the object dump, `IDA <addr>` for natives.
BP UFunction bodies are Kismet bytecode (not in these dumps, not decompilable in IDA);
where a claim depends on bytecode SEMANTICS it is flagged **[PROBE]**.

---

## TL;DR / RECOMMENDATION

- **Enumeration: drive VOTV's own `Uui_saveSlots_C::loadSlots()` on a live widget and
  harvest its DATA member arrays** (`saves[]`@0x0430, `valid_savesNames[]`@0x0440,
  `allSlotsNames[]`@0x0518). `loadSlots` is a pure DATA gather (no widget-tree
  creation — that's `gen()`/`updateList()`, which we do NOT call). This reproduces
  VOTV's exact filtering/sorting/subsave handling for free (RULE 2026-05-28).
  - **Fallback (compose primitives):** `GetProjectSavedDirectory` (KismetSystemLibrary)
    + `"SaveGames"` + `JoyFileIO_GetFiles(*.sav)` + per-name `LoadGameFromSlot` +
    `processSaveNameIntoSubsave` to classify. This is *literally what `loadSlots`
    does internally* (verified from its locals), so it's a faithful re-impl if driving
    the live widget proves fragile.
- **Directory: `GetProjectSavedDirectory()` + "SaveGames/"** is the native resolution
  (VOTV redirects ProjectSavedDir to `%LOCALAPPDATA%\VotV\Saved\` — matches the prior
  on-disk RE). Slot name = filename minus `.sav`. Subsave/`data`/`b_*` exclusion is
  native via `lib_C::processSaveNameIntoSubsave`.
- **Load: `engine::LoadStorySave(slot)` is SUFFICIENT for the data contract the
  gamemode consumes** (it sets `GameInstance.save_gameInst` + `loadObjects=1` +
  `GameMode`, then travels to `untitled_1`; the gamemode reads the save back via
  `lib_C::getSaveSlot` at BeginPlay and restores `objectsData`/`triggers`/`Day`/
  `localGameRules`/`Level`/`subArea` FROM THE SAVE OBJECT). The one field it does NOT
  set — `GameInstance.startDay` — is a **New-Game-only** parameter (irrelevant when
  loading an existing save, which carries its own `Day`@0x065C). **[PROBE]** the exact
  native travel verb (`open untitled_1` via console vs `lib_C::loadLevel`) and whether
  `Action(Type)` writes anything else on the GI before travel.

---

## A. ENUMERATION

### A.1 The native enumerator: `Uui_saveSlots_C::loadSlots()` — what it actually does

`loadSlots` (`ui_saveSlots.hpp:103`, UFunction `MM L144409`) is a self-contained DATA
gather. Decoded from its local-variable set in the object dump (`MM L144410-144489`),
the algorithm is:

1. `CallFunc_GetProjectSavedDirectory_ReturnValue` (`MM L144436`) — native saved dir.
   (Note: this `loadSlots` local uses `GetProjectSavedDirectory`; there is a second
   path local `CallFunc_ProjectSavedDir_ReturnValue` @0x1A0 `MM L144465` — VictoryBP's
   `VictoryPaths__SavedDir`, `VictoryBPLibrary.hpp:146`. Both resolve to the same root.)
2. `Concat` with `"SaveGames"` (the `Concat_StrStr` locals) + `CreateDirectory`
   (`CallFunc_CreateDirectory_ReturnValue` `MM L144439`) — ensures the folder exists.
3. **`JoyFileIO_GetFiles(Files, RootFolderFullPath, Ext)`** (VictoryBPLibrary,
   `VictoryBPLibrary.hpp:270`; locals `CallFunc_JoyFileIO_GetFiles_Files`@0x1C0 +
   `_ReturnValue`@0x1D0, `MM L144467-144469`) — lists the `.sav` files in SaveGames.
4. Per file (loop over `CallFunc_Array_Length_ReturnValue_1`): string-munge the
   filename into a slot name — `LeftChop` (strip the `.sav` extension), `Left`,
   `FindSubstring`/`HasSubstring` (`MM L144472-144476`), then **`LoadGameFromSlot`**
   (`CallFunc_LoadGameFromSlot_ReturnValue`@0x210 `MM L144477`) -> **`DynamicCast` to
   `saveSlot_C`** (`K2Node_DynamicCast_AsSave_Slot`@0x228 `MM L144479`). A failed cast
   or a name in `skipSlots`/already in the result (`Array_Contains` `MM L144482-144483`)
   is dropped.
5. Sort by date: `MakeDateTime` + **`MaxOfDateTimeArray`** (`CallFunc_MaxOfDateTimeArray_
   IndexOfMaxValue`@0x170 / `_MaxValue`@0x178, `MM L144459-144460`) over `datesToSort`,
   newest-first.
6. The cast `saveSlot_C*` objects + their names accumulate into `saves_buff`@0x48 /
   `names_buff`@0x38 (locals), which are written to the **DATA member arrays** (below).

Confirmed it does **NOT** build the visual rows: the `Create`(uicomp widget) +
`AddChild`(to ScrollBox) calls live in **`gen()`** (`MM L144309` `CallFunc_Create_
ReturnValue`, `MM L144319` `CallFunc_AddChild_ReturnValue`) and `updateList()`, which
we do **not** invoke. So `loadSlots` is safe to drive without a constructed/visible
widget tree (with one caveat — A.4).

### A.2 The DATA we harvest (member fields on `Uui_saveSlots_C`, `ui_saveSlots.hpp`)

After `loadSlots`, read these members directly (raw memory at the offset; each `TArray`
is `{void* Data; int32 Num; int32 Max}`, `reflection.h:25`):

| Field | Offset | Type | Use |
|---|---|---|---|
| `saves` | 0x0430 | `TArray<UsaveSlot_C*>` | the save objects (one per real top-level slot) — `ui_saveSlots.hpp:63` |
| `valid_savesNames` | 0x0440 | `TArray<FString>` | the slot NAMES parallel to `saves[]` — `ui_saveSlots.hpp:64` |
| `allSlotsNames` | 0x0518 | `TArray<FString>` | every slot name incl. empties — `ui_saveSlots.hpp:80` |
| `slotsNames` | 0x0410 | `TArray<FString>` | display names — `ui_saveSlots.hpp:59` |
| `saves_subslots` / `slotsSubsaves` | 0x04E8 / 0x04F8 | subsave arrays | the subsaves, kept SEPARATE — `ui_saveSlots.hpp:77-78` |

`saves[]` ⟷ `valid_savesNames[]` is the canonical "real top-level saves" pair to feed
the picker. Subsaves are partitioned into `saves_subslots`/`slotsSubsaves` — so they do
NOT pollute the main list (VOTV groups them under a parent via
`Uuicomp_saveSlotFolder_C`; for a first picker we can ignore subsaves entirely).

### A.3 Per-row metadata — read off each `UsaveSlot_C*` (`saveSlot.hpp`)

For each `saves[i]` (a `UsaveSlot_C*`), read directly (these are plain UPROPERTY data
on a `USaveGame`, safe memory reads — no UFunction needed):

| Display field | Member | Offset | Notes |
|---|---|---|---|
| Slot name | (from `valid_savesNames[i]`) | — | parallel array; or `uicomp_saveSlot_C.SlotName`@0x02C0 if going via Slots[] |
| Day | `Day` (float) | 0x065C | in-game day counter — `saveSlot.hpp:61` |
| Version | `Version` (FString) | 0x0418 | save-format/game version — `saveSlot.hpp:40` |
| Health | `health` / `maxHealth` (float) | 0x0428 / 0x08B4 | `saveSlot.hpp:41,93` |
| Points | `Points` (int32) | 0x0090 | money — `saveSlot.hpp:9` |
| Total time | `totalTime` (float) | 0x00E0 | seconds played — `saveSlot.hpp:15` |
| Last played | `lastDate` / `lastSavedDate` (FDateTime=int64 Ticks) | 0x0470 / 0x07F0 | `saveSlot.hpp:46,72` |
| Created | `creationDate` (FDateTime) | 0x0478 | `saveSlot.hpp:47` |
| Map | `mainMap` (FName) / `Level` (FName) | 0x07F8 / 0x0DA8 | `saveSlot.hpp:73,104` |
| Sub-area | `subArea` (FName) | 0x0828 | `saveSlot.hpp:77` |
| Thumbnail | `Preview` (TArray<uint8>) | 0x0460 | PNG/raw bytes — `saveSlot.hpp:45` (decode to an ImGui texture later) |
| Game mode | derive from name prefix via `getSavePrefix` | — | see C.3 / there is no enum field ON the save; mode lives in the name prefix |

`FDateTime` has no struct in our `reflection.h` — it is a single `int64 Ticks` (UE ticks,
100ns since 1/1/0001). Convert for display, or read `uicomp_saveSlot_C.Date`@0x0328
(also FDateTime) / the pre-formatted strings if going via the visual rows.

Alternative source = the per-row components `Uuicomp_saveSlot_C` (`uicomp_saveSlot.hpp`):
`SlotName`@0x02C0, `Days`@0x02D0 (int32), `ID`@0x02D4, `ver`@0x02D8, `save`@0x02E8
(the `UsaveSlot_C*`), `Size`@0x02F0 (int64, file bytes), `subsave`@0x02F8 (bool),
`FilePath`@0x0310, `Date`@0x0328. BUT these only exist after `gen()`/`updateList()`
builds `Slots[]`@0x0400 — so the **data arrays (A.2) are the right source for a
headless enumerate**; the components are the right source only if we let VOTV build its
own UI (we are not).

### A.4 Fragility / risks of driving the live `loadSlots`

1. **Needs a live `Uui_saveSlots_C` instance.** At the menu, `Uui_menu_C` owns it as
   `umg_saveSlots`@0x0488 (`ui_menu.hpp:75`). Resolve the menu via
   `FindObjectByClass(L"ui_menu_C")` (CONFIRMED live in the menu dump as
   `ui_menu_C_2147480910`, owner = the GameInstance), then read `+0x0488`. **WARNING
   (observed in the menu dump):** no live `ui_saveSlots_C_*` instance was present in the
   MAIN_MENU capture — i.e. `umg_saveSlots`@0x0488 is very likely **null until the user
   first opens the save list** (the menu lazily constructs that sub-widget). So a
   headless enumerate must first ensure the widget exists: read `+0x0488`; if null,
   either trigger the menu's open-save-list path, or NewObject/Construct a
   `ui_saveSlots_C` ourselves, or just use the fallback (A.5) which needs no widget.
   **[PROBE]** D.1 settles this.
2. **`loadSlots` calls `getMainGamemode`** (`MM L144434` `CallFunc_getMainGamemode_
   AsMain_Gamemode`). At the menu a `mainGamemode_C` IS live (it runs the menu world),
   so this resolves — but it is a dependency to be aware of (a null gamemode would
   change behaviour). **[PROBE]** confirm non-null at the menu.
3. **Synchronous + disk I/O.** `loadSlots` does N× `LoadGameFromSlot` (each parses a
   GVAS file; the test slot is ~19.6 MB). It is a blocking game-thread call. For a few
   saves this is fine (the game itself does it on menu open); for many large saves it
   could hitch. Acceptable for a picker opened on demand, but do it once and cache.
4. **It allocates `UsaveSlot_C` UObjects** (one per slot, via `LoadGameFromSlot`). These
   are transient saves; they are referenced by `saves[]`. If we read `saves[]` and stash
   pointers, treat them as live only while the widget holds them (re-running `loadSlots`
   replaces the array). For the picker, read what we need immediately into our own C++
   structs; do not cache the `UsaveSlot_C*` long-term.
5. **Marshaling note (calling `loadSlots`):** it takes no params -> `CallFunction(widget,
   loadSlotsFn, nullptr)`. The result is in the member arrays, not a return value.

### A.5 Fallback: compose the primitives ourselves (faithful re-impl)

If driving the live widget is fragile, reproduce `loadSlots` exactly:

1. `dir = ` UFunction `UKismetSystemLibrary::GetProjectSavedDirectory()` (static, CDO of
   `KismetSystemLibrary`; UFunction `MM L15880`; **same library our `ExecuteConsoleCommand`
   already uses** — `engine.cpp` `g_kslCdo`). Returns an `FString`. Append `"SaveGames/"`.
2. `JoyFileIO_GetFiles(files, dir, ".sav")` — UFunction on `VictoryBPLibrary`
   (`VictoryBPLibrary.hpp:270`, `bool JoyFileIO_GetFiles(TArray<FString>& files, FString
   RootFolderFullPath, FString Ext)`). Resolve its CDO via `FindClassDefaultObject(
   L"VictoryBPLibrary")`. NATIVE impl IDA-confirmed at `sub_140853F60` (lists files via
   the IFileManager `IterateDirectory` visitor; sets a success bool) — see IDA notes.
   - Marshaling: `Files` is an out `TArray<FString>` (16B struct, zero-init before call),
     `RootFolderFullPath` + `Ext` are in `FString` (16B each). Param-frame order per the
     header signature.
3. For each file name: strip `.sav` (`LeftChop` 4) -> candidate slot.
4. `processSaveNameIntoSubsave(slotName, WorldContext, isSubsave&, mainSaveName&)` —
   UFunction on `lib_C` (`lib.hpp:9`, `MM L230832`). **This is VOTV's native subsave
   classifier** — use it to drop subsaves from the top-level list (or group them).
5. `save = LoadGameFromSlot(slotName, 0)` -> `DynamicCast` to `saveSlot_C` (skip on
   fail). Read metadata per A.3.
6. Optionally sort by `lastDate` newest-first (mirror `MaxOfDateTimeArray`).

Pros of fallback: no dependency on widget Construct state / `getMainGamemode`. Cons: we
re-own the filter rules (must keep parity with VOTV across versions — the very thing
RULE 2026-05-28 says to avoid). **Recommendation: try A.1 first; keep A.5 as the
tested fallback.**

---

## B. SaveGames DIRECTORY RESOLUTION

- **Native path:** `UKismetSystemLibrary::GetProjectSavedDirectory()` (UFunction
  `MM L15880`) returns the project Saved dir as an `FString`; append `"SaveGames/"`.
  This is exactly what `loadSlots` does (A.1 step 1-2).
- **Under the hood (IDA `sub_141241E00` = `FPaths::ProjectSavedDir()`):** cached, honours
  the `-saveddirsuffix=` cmdline arg. For VOTV (packaged) this resolves to
  `%LOCALAPPDATA%\VotV\Saved\` (consistent with the prior on-disk RE:
  `research/findings/votv-save-path-RE-2026-05-30.md` §5 — root
  `%LOCALAPPDATA%\VotV\Saved\SaveGames\`). So `GetProjectSavedDirectory()+"SaveGames/"`
  == the real on-disk root; **no need to build it from the environment ourselves**.
- **VOTV-specific helpers (also callable on `lib_C` CDO):** `GetAssetFolder(skipCreation,
  WC, Path&)` (`lib.hpp:92`, `MM L229420`) and `generateAssetDirectory(WC, Path&)`
  (`lib.hpp:78`, `MM L229557`) build the per-save **asset** subfolder (`_bin`-style),
  not the SaveGames root — use only if we need the per-save asset dir, not for the
  picker list.
- **Slot-name ⟷ filename rule:** `<slot>.sav` (flat file in SaveGames). Slot name =
  filename minus the `.sav` extension (the `LeftChop` in `loadSlots`). VOTV also writes
  companions in the same folder; **exclude from the picker:**
  - `data.sav` — global meta/settings/gallery (not a playthrough). [PROBE] confirm it's
    filtered by name; `loadSlots`'s `DynamicCast` to `saveSlot_C` would fail for it (it
    deserializes as a different SaveGame class) so it likely self-excludes.
  - `b_*.sav` — backups (the prefix `b_` is the backup marker per the prior RE).
    [PROBE] confirm `loadSlots`'s `HasSubstring`/`skipSlots` drops the `b_` prefix.
  - subsaves — name-mangled top-level `.sav`; classify + exclude/group via
    `lib_C::processSaveNameIntoSubsave` (B-native, preferred) rather than guessing the
    mangling.
  - `_bin\` — a directory, not a `.sav`; `JoyFileIO_GetFiles(Ext=".sav")` won't list it.

---

## C. LOAD-INTO-GAMEPLAY — is `engine::LoadStorySave` faithful?

### C.1 What `LoadStorySave(slot)` does today (`engine.cpp:207-284`)
1. `UGameplayStatics::LoadGameFromSlot(slot, 0)` -> the `UsaveSlot_C*` (cached once).
2. `UmainGameInstance_C::setSaveSlotObject(save, slot)` — UFunction `MM L234680`
   (`mainGameInstance.hpp:44`). Writes `save_gameInst`@0x01A8 + `SlotName`@0x01B0.
3. `GameInstance.loadObjects = 1` (`@0x0229`, `mainGameInstance.hpp:18`).
4. `ApplyGameModeFromSlot` — derive `enum_gamemode` from the slot-name prefix via
   `ui_saveSlots_C::getSavePrefix(mode)` (a pure `K2Node_Select` mode->prefix map,
   UFunction `MM L144...`, no side effects) and write `GameInstance.GameMode`@0x01E1.
5. `ExecuteConsoleCommand("open untitled_1")` — travel to the (single) gameplay map.

### C.2 What the gamemode CONSUMES at load — proves the contract
- The gamemode pulls the save back via **`lib_C::getSaveSlot(WC, saveSlot&)`** (`lib.hpp:105`,
  `MM L229267`) whose body calls `getMainGamemode` and returns the GI/gamemode save — i.e.
  it reads `GameInstance.save_gameInst` that step 2 set. Confirmed the gamemode's
  ubergraph reads `CallFunc_getSaveSlot_saveSlot`@0x1AE8 (`MM L231925`) and
  `CallFunc_getMainGameInstance_AsGame_Inst`@0x2B70 (`MM L232311`).
- The restore itself is `mainGamemode_C::loadObjects` (UFunction `MM L277101`), gated by
  the `loadObjects` flag step 3 sets. It iterates the save's `objectsData`/`triggers`/
  primitives and respawns them (this is the well-trodden host-snapshot path).
- **Crucially, the world/run state is restored FROM THE SAVE OBJECT, not from the GI:**
  `Day`@0x065C, `localGameRules`@0x0DB0, `Level`@0x0DA8, `subArea`@0x0828, player
  transform@0x0210, inventory@0x02E0 all live ON `UsaveSlot_C` and are read by the
  gamemode/mainPlayer during BeginPlay restore. So once `save_gameInst` is set +
  `loadObjects=1`, those come along automatically. `LoadStorySave` does NOT need to
  touch them. (The 2026-06-03 GameMode-prefix fix already closed the one field that
  ISN'T in the save — the mode enum, which lives only in the name prefix.)

### C.3 The one field `LoadStorySave` does not set: `GameInstance.startDay`@0x01E4
- `startDay` (`mainGameInstance.hpp:12`) is a **NEW-GAME parameter** — "which day to
  start a fresh playthrough on". It is set by the New-Game flow (the save-slots widget
  has its own `startDay`@0x0428, `ui_saveSlots.hpp:61`, edited via `ETB_days`). When
  LOADING an existing save, the day comes from the save's own `Day`@0x065C, so
  `startDay` is irrelevant to a load. **No action needed for the load-existing path.**
  (When we later build the *New Game* branch of the picker, THAT path must set
  `startDay` — note it there.)

### C.4 Verdict
**`engine::LoadStorySave(slot)` faithfully covers the load-existing-save contract the
gamemode consumes** (save object + loadObjects + mode). It does not omit any field that
matters for loading an existing slot. The remaining uncertainty is purely the **travel
mechanism**, not the state setup:

- **[PROBE] travel verb.** `LoadStorySave` uses `open untitled_1` (console). VOTV's own
  menu launch likely uses `lib_C::loadLevel(Level, Option, clearSubArea, WC)` (`lib.hpp:44`,
  `MM L230178`) or `mainGamemode_C::transition(LevelName)` (`MM L275413`). Both reach
  `untitled_1`; `open` is proven working in our harness (it boots story gameplay). The
  only reason to switch to `loadLevel`/`transition` would be if `open` skips some BP
  setup the menu does via `Option` (e.g. a sub-area string). For loading a normal
  top-level story save this has not bitten us. Confirm by probing whether the menu's
  `button_launch` path ends in `loadLevel` vs `OpenLevel`/console.
- **[PROBE] `ui_menu_C::Action(Type, Slot)` side effects.** `Action` is a `SwitchInteger`
  on `Type` (`MM L256091-256094`) gated by `IsValid(Slot)`. Its per-branch bytecode
  (Launch existing / New Game / delete / etc.) is not visible in the dumps. Confirm it
  does not write additional GI fields before travel that `LoadStorySave` would miss.
  Best confirmed by diffing GI fields before/after a real menu launch (D.3).

---

## D. WHAT STILL NEEDS A LIVE PROBE

Each is phrased as concrete steps (find object -> call -> diff). Use UE4SS Lua at the
menu, or our reflection-drive harness.

**D.1 — Can `loadSlots` run headless at the menu?**
- Find `ui_menu_C` (`FindObjectByClass`), read `umg_saveSlots`@0x0488 -> the
  `ui_saveSlots_C*`. Read its `saves`@0x0430 `.Num` BEFORE. `CallFunction(widget,
  loadSlots, nullptr)`. Read `saves`@0x0430 / `valid_savesNames`@0x0440 `.Num` AFTER.
  Expect AFTER == the real save count, names readable. Confirms the widget is usable
  without opening the list UI, and that `getMainGamemode` inside it didn't no-op.
- Also log whether the widget needed `Construct()` first (call `Construct` if `saves`
  stays empty, then re-test).

**D.2 — Subsave / data / backup exclusion**
- After D.1, dump `valid_savesNames[]` and confirm `data`, any `b_*`, and known
  subsaves are ABSENT (or land in `saves_subslots`/`slotsSubsaves`@0x04E8/0x04F8).
- Independently call `lib_C::processSaveNameIntoSubsave("<a known subsave name>", WC,
  isSubsave&, mainSaveName&)` and confirm `isSubsave==true` + `mainSaveName` == the
  parent — so we can use it directly in the fallback path.

**D.3 — Load contract completeness (the decisive load probe)**
- Snapshot `GameInstance` fields `save_gameInst`@0x01A8, `SlotName`@0x01B0,
  `GameMode`@0x01E1, `startDay`@0x01E4, `loadObjects`@0x0229, `gameRules`@0x0318
  immediately BEFORE a real in-menu "Launch" of a known story save, and again right
  after the click (before travel completes). Diff. This reveals EXACTLY what VOTV's
  native `Action`/launch writes — the ground truth to compare `LoadStorySave` against.
  If the diff shows only `{save_gameInst, SlotName, GameMode, loadObjects}` changing,
  `LoadStorySave` is byte-for-byte faithful. If `startDay`/`gameRules`/anything else
  changes, add those writes.

**D.4 — Travel verb parity**
- Probe the menu's `button_launch` -> the final travel call. UE4SS: hook
  `lib_C::loadLevel` and `UGameplayStatics::OpenLevel` and `mainGamemode_C::transition`;
  click Launch on a story save; see which fires + with what `Level`/`Option`. If it's
  `loadLevel(untitled_1, "", ...)` with an empty Option, our `open untitled_1` is
  equivalent. If `Option` is non-empty, replicate it.

**D.5 — Thumbnail decode (cosmetic, deferrable)**
- `UsaveSlot_C.Preview`@0x0460 (`TArray<uint8>`). Probe the byte header (PNG magic
  `89 50 4E 47` vs raw BGRA) to know how to upload it as an ImGui texture. Not needed
  for a functional picker.

---

## Appendix — IDA native confirmations
- `JoyFileIO_GetFiles` native body: `sub_140853F60(resultArray, &boolOut)` — builds
  `<ProjectSavedDir>/SaveGames` (string `"SaveGames"`@`0x143a2d350`; saved-dir via
  `sub_141241E00`), then enumerates via the IFileManager visitor (vtable+256 =
  `IterateDirectory`); writes each name into the array and sets the success byte. A
  sibling `sub_140853CD0` is the recursive variant (vtable+328 / "." dir-walk) =
  `JoyFileIO_GetFilesInRootAndAllSubFolders`. (We do not need to re-derive these — VOTV
  calls them via reflection and so can we.)
- `FPaths::ProjectSavedDir()` = `sub_141241E00` (cached; `-saveddirsuffix=` aware).
- `GetProjectSavedDirectory` is a `UKismetSystemLibrary` static UFunction (`MM L15880`).
- Strings: `"SaveGames"`@`0x143a2d350`, `"GetProjectSavedDirectory"`@`0x1441beaf0`,
  `"JoyFileIO_GetFiles"`@`0x143a33af0`.

## Appendix — exact callable UFunctions (name | owning class | how to resolve CDO)
- `loadSlots` | `ui_saveSlots_C` | live instance via `ui_menu_C.umg_saveSlots`@0x0488 (NOT the CDO — needs the real widget)
- `getSavePrefix(enum_gamemode)->FString` | `ui_saveSlots_C` | CDO ok (pure map) — already used in `engine.cpp`
- `GetProjectSavedDirectory()->FString` | `KismetSystemLibrary` | CDO (`g_kslCdo` already cached in `engine.cpp`)
- `JoyFileIO_GetFiles(files&, root, ext)->bool` | `VictoryBPLibrary` | `FindClassDefaultObject(L"VictoryBPLibrary")`
- `processSaveNameIntoSubsave(name, WC, isSubsave&, mainSaveName&)` | `lib_C` | `FindClassDefaultObject(L"lib_C")`
- `getSaveSlot(WC, saveSlot&)` | `lib_C` | CDO (reads active save; menu-time returns the menu's save context)
- `GetAssetFolder(skipCreation, WC, Path&)` / `generateAssetDirectory(WC, Path&)` | `lib_C` | CDO (per-save asset dir, not the list)
- `loadLevel(Level, Option, clearSubArea, WC)` | `lib_C` | CDO — candidate native travel verb
- `LoadGameFromSlot(SlotName, UserIndex)->USaveGame*` | `GameplayStatics` | CDO (already cached as `g_loadGameFn`)
- `setSaveSlotObject(save, SlotName)` | `mainGameInstance_C` | the live GI instance (already used in `LoadStorySave`)
