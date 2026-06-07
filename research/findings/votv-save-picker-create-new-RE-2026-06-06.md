# VOTV save-picker CREATE-NEW ("New Game") RE — native create-save sequence — 2026-06-06

**Target**: Alpha 0.9.0-n. **Task**: the Host-Game save-picker CREATE-NEW half — create a
brand-new, NAMED, mode-correct, persisted VOTV save at a chosen start-day and enter gameplay,
using VOTV's OWN native functions, callable from our reflection layer at the MAIN MENU.

**Method/authority**: CXXHeaderDump `*.hpp` (struct layout authority) + the UE4SS object dump
`UE4SS_ObjectDump_{MAIN_MENU,GAMEPLAY_SAVE}.txt` (object/property/function LOCAL-VARIABLE layout
authority — cited as `L#`) + our own runtime call logs (the empirical `getSavePrefix` map) +
IDA (native engine fn confirmation) + the live source (`engine.cpp`/`engine.h`, our existing
reuse surface). **The object dump is a STRUCTURAL reflection dump — it lists every object,
property offset, and per-UFunction local variable, but NOT Kismet bytecode and NOT default
VALUES.** BP UFunction *semantics* are therefore reconstructed from each function's
local-variable set (which names every CallFunc node the graph invokes) + reflected names; the
two genuinely-bytecode-internal orderings are flagged TIER-3 (live probe) at the end. Verified,
not guessed.

---

## 0 — TL;DR (the recommended native CREATE-NEW sequence)

VOTV's real "new game" is **path (a): the saveSlots-widget create path**, and its load-bearing
primitive is one BP function — **`UsaveSlot_C::regenerate(FString InputName)`**. Its local set
(GAMEPLAY dump L296385–296395) proves it is exactly:

```
regenerate(InputName):
   LoadGameFromSlot(InputName,0)            // probe existing (for the overwrite/confirm branch)
   CreateSaveGameObject(saveSlot_C)         // -> a fresh BLANK UsaveSlot_C  (L296395, pc=saveSlot_C)
   Cast -> saveSlot_C (AsSave_Slot)         // L296391
   SaveGameToSlot(<new>, savName, 0)        // L296394 -> WRITES <savName>.sav TO DISK NOW
   SpawnSoundAttached(...)                  // L296393 UI confirm SFX (skippable for us)
```

So a new VOTV save = **`CreateSaveGameObject(saveSlot_C)` → `SaveGameToSlot(obj, "<prefix><name>", 0)`**.
The blank object's empty `objectsData/triggers/...` arrays ARE the deterministic New-Game baseline
(content is generated at load, not stored at create). **The mode is encoded SOLELY in the slot-name
prefix** (no `GameMode`/`startDay` field is stamped into the saveSlot at create — confirmed: neither
`setGamemode`, `reset_days`, nor `setSaveSlotObject` appears as a `CallFunc_*` local of either
`regenerate` or the create branch of `ExecuteUbergraph_ui_saveSlots`). `startDay` rides
`mainGameInstance.startDay`@0x01E4 (set from `ETB_days` at launch) and is read by the gamemode at
BeginPlay.

The widget's create ubergraph wraps that primitive with validation + the second companion write
(`ExecuteUbergraph_ui_saveSlots` locals, MAIN_MENU dump):
```
getSavePrefix(mode)         (L144198)  -> "s_" for story (empirically verified, §2)
isStringLegal(name) -> legal (L144245) -> reserved-name + illegal-char guard (§3)
CreateSaveGameObject        (L144246, pc=saveSlot_C)
SaveGameToSlot              (L144256)  -> main slot
SaveGameToSlot              (L144267)  -> a second/companion write (subsave or data.sav refresh)
```

**Recommendation (§7): build a new `engine::CreateNamedSave(name, mode, startDay)` that mirrors
this primitive, then chain into the EXISTING `LoadStorySave("<prefix><name>")` to enter gameplay.**
Do NOT try to drive the widget's BP buttons. Reuse, don't fork: this is a ~30-line superset of
`StartFreshGame`.

---

## 1 — Which path does VOTV actually use? (a) vs (b)

**Path (a) — the saveSlots-widget create path — IS the create path.** Evidence:
- `ExecuteUbergraph_ui_saveSlots` carries `CallFunc_getSavePrefix` (L144198), `CallFunc_isStringLegal_legal`
  (L144245), `CallFunc_CreateSaveGameObject_ReturnValue` (L144246, prop-class = `saveSlot_C`), and TWO
  `CallFunc_SaveGameToSlot_ReturnValue` (L144256, L144267). That is a complete create-and-persist.
- The create BUTTONS are pure dispatchers into that ubergraph: `BndEvt__button_create_…_8`,
  `BndEvt__umg_saveSlots_button_create_1_…_11`, `BndEvt__umg_saveSlots_button_duplicate_…_12`,
  `BndEvt__button_apply_…_9` are all listed as functions (MAIN_MENU L144287–144299) but `button_create`'s
  BndEvt has **no local variables** → it just routes an entry-point int into `ExecuteUbergraph_ui_saveSlots`.
- The seed primitive is `UsaveSlot_C::regenerate` (§0). `saveSlot_C::Duplicate` is the name-collision
  variant (§3).

**Path (b) — the menu NewGame path — does NOT create or name a save.** `Uui_menu_C::BndEvt__button_NewGame_…_0`
→ `openStorymode()`/`openSandbox()`/`openInfinite()`/`open_halloween()`/`openSolar()`/`openAmbience()`/
`openTutor()` (ui_menu.hpp:115–128) → opens the `Uui_gamemode_C` submenu and **shows the saveSlots
widget** (`umg_saveSlots`@0x0488). Picking a mode in `Uui_gamemode_C` (button_g_storyMode@0x0298 etc.,
ui_gamemode.hpp) sets which mode the saveSlots widget will create UNDER, but the actual create still
happens via path (a) inside `umg_saveSlots`. So **(b) is just the front-door that selects the mode and
surfaces the (a) widget; (a) is where the save is born.** They converge on (a).

`gen()`, `regenSave()`, `regenObjects()` are **NOT creators** — they are list/preview builders:
- `gen()` (MAIN_MENU L144301–144360): builds the on-screen slot list (`AddChild`, `Create` the slot
  *widget*, `getSandboxMaps`, `sortEmptyFolders`, iterates `saves`/`folders`). It calls `getSavePrefix`
  only to GROUP/label slots.
- `regenSave()` (L144631–144680) + `regenObjects()` (L144700+): count `propEquipment/propInventory/
  propContainers/totalObjects/propNames`, cast actors `AsProp`, build `FormatArgumentData` for the
  preview text + read `gameVersion`/`Now()`. These populate the "this save has N objects" PREVIEW panel.
  Neither calls `CreateSaveGameObject` or `SaveGameToSlot`. Ignore them for create.

---

## 2 — Name / prefix / mode encoding (EMPIRICALLY VERIFIED)

`Uui_saveSlots_C::getSavePrefix(TEnumAsByte<enum_gamemode::Type> Index) -> FString` (ui_saveSlots.hpp:92)
is a pure `K2Node_Select` (8-way switch on the enum → one literal FString; dump L253843
`K2Node_Select_Default` + the 8 `Temp_string_Variable_0..7` case literals). The literals are bytecode
operands (not in the dump), but **we already call it across all 8 ordinals at runtime and log each
result** (`engine.cpp:168-182 ApplyGameModeFromSlot`). Captured identically in host + client + solo logs
(`votv-coop-{host,client}.log:10648-10655`, `votv-coop.log:10652-10659`):

| `enum_gamemode` ordinal | mode (from `Uui_gamemode_C` button) | `getSavePrefix(N)` |
|---|---|---|
| 0 | **Story** (`button_g_storyMode`) | **`s_`** |
| 1 | Infinite (`button_g_infinite`) | `i_` |
| 2 | (unused) | `-` |
| 3 | (unused) | `-` |
| 4 | Sandbox (`button_g_sandbox`) | `b_` |
| 5 | Halloween (`button_g_halloween`) | `SPOOKY_` |
| 6 | Ambience (`button_g_ambience`) | `a_` |
| 7 | Solar (`button_g_solar`) | `l_` |

**CRITICAL: the enum ordinal order is NOT the submenu button order** (story=0, infinite=1, sandbox=4 —
sandbox is NOT 1). Ordinals 2 and 3 return `-` (a sentinel "no real prefix"). For our coop target:
**story save name = `"s_" + <userName>`** (e.g. `s_Alex`). Sandbox = `"b_" + name`, infinite = `"i_" + name`.

**Mode encoding has NO other carrier**: the saveSlot has a `GameMode`@0x00D8 (an `AmainGamemode_C*`
pointer, NOT the enum) and the GameInstance has `GameMode`@0x01E1 (the enum). The create path writes
NEITHER at create time; the mode is recovered FROM the name prefix on load — which is exactly what our
`ApplyGameModeFromSlot` does (matches longest prefix → writes `mainGameInstance.GameMode`@0x01E1). So a
correctly-prefixed name is necessary AND sufficient to make the new save load in the right mode.

### Name-legality (`isStringLegal`)
`isStringLegal(FString InputPin, bool& legal)` (ui_saveSlots.hpp:91; locals MAIN_MENU L253845-253862):
builds two literal arrays — `illegalNames` (reserved slot names, e.g. the meta `data`, backup `b_*`
names) and `IllegalCharacters` (filesystem-unsafe chars) — then `Contains`-checks the input against
both (the two `K2Node_MakeArray_Array` + `CallFunc_Contains_ReturnValue` + a loop). Pure validation,
no side effects, callable standalone on the widget CDO. The exact literal contents are bytecode (TIER-3
if we want to replicate them exactly; for our own generated names we control legality, so a light C++
guard — non-empty, no `\ / : * ? " < > |`, not starting with `b_`/`data` — suffices and is RULE-1-clean
since it's our input, not user-typed yet).

---

## 3 — Collision handling

Two layers, both native:

1. **`isStringLegal`** rejects reserved/illegal names up front (§2) — this is a *legality* gate, not a
   *uniqueness* gate.
2. **Uniqueness / auto-increment = `UsaveSlot_C::Duplicate(FString Name)`** (saveSlot.hpp:122; locals
   GAMEPLAY L296397-296426). Its local set proves the algorithm: loop with `inc`, build
   `saveName_plusone` via `Concat`, test `DoesSaveGameExist(<candidate>,0)` (TWO checks: L296409,
   L296413), parse any trailing numeric suffix (`GetSubstring`→`IsNumeric`→`Conv_StringToInt`→`Add`),
   and when a free name is found `CreateSaveGameObject` + `SaveGameToSlot` it. i.e. `foo` taken → `foo2`,
   `foo2` taken → `foo3`, … This is VOTV's "name collision" resolver and the engine call that backs it
   is **`UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex)`** (param frame in §5).

The widget ALSO keeps `valid_savesNames`@0x0440, `slotsNames`@0x0410, `allSlotsNames`@0x0518
(ui_saveSlots.hpp) which the UI uses to grey-out / warn, but the authoritative on-disk uniqueness test
is `DoesSaveGameExist`. **For our create flow: call `DoesSaveGameExist("<prefix><name>",0)` first; if
taken, either reject (let the user pick another) or mirror `Duplicate`'s `_2/_3` increment.** Recommend
reject-with-message for the picker UX (simpler, RULE-1-clean), with the increment as an option.

---

## 4 — Is the new .sav persisted at creation? — YES

**`regenerate` calls `SaveGameToSlot` inline** (GAMEPLAY L296394), and the create ubergraph calls it
twice (L144256/L144267). So the .sav is written to disk **at creation**, not deferred to first autosave.
This is exactly what the picker needs: the new save appears in the slot list next time AND the host has
a real on-disk save to be authoritative over immediately. Confirmed mechanics from
`votv-save-path-RE-2026-05-30.md`: `SaveGameToSlot` → `FFileHelper::SaveArrayToFile` writes
`%LOCALAPPDATA%\VotV\Saved\SaveGames\<slot>.sav` (GVAS/UE4.27), SYNC on the game thread.

**Is calling SaveGameToSlot at create safe for us?** Yes, with one coop caveat:
- A freshly-`CreateSaveGameObject`'d blank saveSlot has EMPTY `objectsData/GObjStack/triggers/...` →
  there are no coop mirror actors to leak into it (the leak surface in the save-path RE only exists once
  the world is populated). So a create-time write is clean.
- **Caveat (client role only):** our client installs a `SaveGameToSlot` *write-block* detour
  (`coop/save_block.cpp`, the ephemeral-client design). If CreateNamedSave is ever run on a client with
  that block active, the create write would be blocked. **Resolution: CreateNamedSave is a HOST/menu
  action** (you create a save to HOST it); it runs at the menu BEFORE any client save-block is relevant.
  If we ever need it client-side, gate the block to skip our own create write (the block already keys on
  role). Not a concern for the Host-Game picker.
- The second ubergraph `SaveGameToSlot` (L144267) is a companion write — most likely the `data.sav`
  meta/`lastPlayed` refresh or an empty subsave. We do NOT need to replicate it for a functional create
  (the main slot write is what makes the save exist + loadable). TIER-3 if we want byte-identical parity.

The second `SaveGameToSlot` writes `data.sav` indirectly via `save_main` is plausible but unconfirmed;
not load-bearing for create.

---

## 5 — Native param frames (authoritative for our marshaling)

From the object dump (`/Script/Engine.GameplayStatics:*`), these match our existing `ParamFrame`
usage exactly:

```
CreateSaveGameObject(SaveGameClass:Class @0x00, ReturnValue:Object @0x08)     // L55164-55165
SaveGameToSlot(SaveGameObject:Object @0x00, SlotName:FString @0x08,
               UserIndex:int @0x18, ReturnValue:bool @0x1C)                    // L55493-55496
DoesSaveGameExist(SlotName:FString @0x00, UserIndex:int @0x10, ReturnValue:bool @0x14)  // L55190-55192
LoadGameFromSlot(SlotName:FString @0x00, UserIndex:int @0x10, ReturnValue:Object @0x18) // L55372-55374
```
All are static `UFunction`s on the `GameplayStatics` CDO (`Default__GameplayStatics`), dispatched via
ProcessEvent — i.e. driven through our `R::CallFunction`/`ParamFrame` the same way `LoadStorySave`
already drives `LoadGameFromSlot`. (IDA: these statics are not symbol-named in the IDB — they are reached
via the reflected exec-thunk, so no raw address is needed; the param frames above are the authority.)

`mainGameInstance_C::setSaveSlotObject(save_gameInst:Object, SlotName:FString)` (mainGameInstance.hpp:44)
and the fields we write: `loadObjects`@0x0229 (bool), `GameMode`@0x01E1 (enum byte), `startDay`@0x01E4
(int). `startDay` ALSO exists on `mainGamemode_C`@0x0698 (GAMEPLAY L230987) — the gamemode copies it from
the GameInstance at BeginPlay.

---

## 6 — The data a blank New-Game save carries (and what we DON'T need to seed)

`UsaveSlot_C` (saveSlot.hpp, 0xE41). A `CreateSaveGameObject(saveSlot_C)` blank object holds the CDO
defaults. **The object dump does NOT serialize default VALUES** (it is structural), so the exact CDO
defaults of `Version`@0x0418, `mainMap`@0x07F8, `Level`@0x0DA8, `Day`@0x065C, `localGameRules`@0x0DB0
are a TIER-3 read — BUT we have already PROVEN empirically (host-world-snapshot RE 2026-06-04, and
`StartFreshGame` 3-run validation) that a blank `saveSlot_C` + `loadObjects=1` + `open untitled_1`
**reaches working story gameplay in ~2 s with the level defaults** (mainPlayer spawns at КПП, 2313 props
generate, memory-stable ~4 GB). So **the blank defaults are sufficient to create+enter a New Game; we do
NOT need to hand-seed map/version/rules/day into the saveSlot for the world to come up.** Content is
generated by the gamemode at load from the (empty) save + the GameInstance mode/day, not restored from
the save's arrays.

What this means for "seed startDay/rules/map" in the task:
- **startDay**: NOT a saveSlot field write. It is `mainGameInstance.startDay`@0x01E4 (set from `ETB_days`
  in the widget; the gamemode reads it at BeginPlay). For us: write `gi.startDay = <day>` alongside
  `setSaveSlotObject` (same place `LoadStorySave` sets `loadObjects`). Day 1 = the story default
  (`ETB_days` default; the widget clamps to `maxDay`@0x042C). **Confirm the day-1 vs day-0 base via the
  TIER-3 probe** — VOTV story "day 1" may be startDay=0 or 1 (the field is an int the gamemode adds to).
- **gameRules**: the widget exposes `ui_gameRulesList_newSlot`@0x03E8 + writes `localGameRules`@0x0DB0 on
  the save and/or `mainGameInstance.gameRules`@0x0318. A blank save's default `localGameRules` =
  story-default rules (the CDO default of `Fstruct_gameRules`, 0x29 bytes, struct_gameRules.hpp). For a
  vanilla story create we want the DEFAULTS → do nothing (leave blank). Only if the picker exposes
  difficulty toggles do we write `localGameRules`/`gameRules` (a flat 0x29-byte POD blob — trivial raw
  write, no marshaling). **DEFER rules customization** beyond difficulty unless the picker needs it.
- **map (`mainMap`/`Level`)**: NOT needed for story (untitled_1 is fixed; story has one map). Only
  SANDBOX uses `cbox_sboxLevel`@0x0318 + `selectedMap`@0x0460 + `sboxMaps_map`@0x0528 + `makeSboxMaps()`/
  `findSBMap_*` to pick a sandbox level, written into the save's `mainMap`/`Level`. **Story create
  ignores all sandbox-map machinery.** (Sandbox create, if we add it later, would: `makeSboxMaps()` →
  pick a display name → `findSBMap_string(display)->levelName` → write `selectedMap`/`mainMap`.)

Per-object record shape (`Fstruct_save` 0xF8) and trigger record (`Fstruct_triggerSave` 0x118) are
irrelevant to CREATE (they're empty in a new save).

---

## 7 — Reuse-vs-build recommendation (RULE-1 / RULE-2026-05-28)

**Build `engine::CreateNamedStorySave(name, startDay)` as a thin superset of the existing
`StartFreshGame`, then enter gameplay via the existing `LoadStorySave`.** This mirrors VOTV's own create
primitive (path a) faithfully and reuses our proven boot machinery.

Our current `StartFreshGame(storyMode)` (engine.cpp:295-371) already does CreateSaveGameObject(saveSlot_C)
→ setSaveSlotObject(blank, "coop_client_fresh") → loadObjects=1 → ApplyGameModeFromSlot → open untitled_1.
It is **90% of CreateNew already** — it just (a) uses a hardcoded throwaway name, (b) never calls
`SaveGameToSlot` (so nothing persists), (c) doesn't seed startDay. The create flow = add those three.

### Recommended exact sequence (game thread, at the MAIN MENU)
Given user inputs `name` (e.g. "Alex"), `mode` (story=0), `day` (e.g. 1):
```
1. slot = getSavePrefix(mode) + name              // "s_Alex"  (call our cached getSavePrefix, §2)
   (light C++ legality guard on `name`, §2; optionally call isStringLegal on the widget CDO if loaded)
2. if DoesSaveGameExist(slot, 0):                  // §3 collision
        reject (UI: "name taken")  [or mirror Duplicate's _2/_3 increment]
3. save = CreateSaveGameObject(saveSlot_C)         // blank New-Game baseline (§5 frame)
4. SaveGameToSlot(save, slot, 0)                   // PERSIST NOW (§4) -> <slot>.sav on disk
   // ^ this is the one line StartFreshGame lacks; makes the save real + pickable + host-authoritative
5. ENTER GAMEPLAY — reuse LoadStorySave(slot):
      it does LoadGameFromSlot(slot,0) -> setSaveSlotObject(save,slot) -> loadObjects=1
              -> ApplyGameModeFromSlot(slot)  [prefix "s_" -> GameMode=0 story]
              -> open untitled_1
   ALSO set gi.startDay = day  (write @0x01E4 in the same setSaveSlotObject block; §6)
```
Step 5 reuses `LoadStorySave` verbatim — after step 4 the slot exists on disk, so `LoadGameFromSlot`
returns the (blank, freshly-written) save and the existing boot poll drives it to gameplay exactly like
loading any save. **One code path for "load existing" and "enter the just-created" — no fork** (RULE 2).

### Why not drive the widget's BP buttons?
Driving `button_create`/the ubergraph would mean synthesizing UMG clicks + populating `ETB_slotName`/
`ETB_days`/`cbox` — fragile (raw-input cursor defeats synthetic clicks, per the menu-shot findings) and
indirect. The widget's create path bottoms out at exactly `CreateSaveGameObject`+`SaveGameToSlot` (§1),
so calling those directly IS "driving VOTV's native functions" — the user's stated direction — without
the UMG fragility. The ImGui picker sits on top and calls CreateNamedStorySave; that is precisely
"native functions and imgui on top of them."

### Modularity
This is a distinct subsystem (save creation). `engine.cpp` is the existing home of LoadStorySave/
StartFreshGame and is well under the 800-LOC soft cap region for these (the save-boot trio lives
together). Adding `CreateNamedStorySave` next to them keeps the create/load/fresh family in one place —
acceptable. If `engine.cpp` is near the cap at implementation time, extract the save-boot trio
(`LoadStorySave`/`StartFreshGame`/`CreateNamedStorySave` + their `g_storyGs*`/`ApplyGameModeFromSlot`
statics) into `ue_wrap/engine_saveboot.cpp` as one feature file (RULE 2026-05-25). Check `wc -l`
at implementation.

---

## 8 — What still needs a LIVE PROBE (UE4SS or empirical reflection-drive)

All are confirm-not-block — the §7 sequence is sound on current evidence; these tighten parity/values.

**P1 — `startDay` base value for "Day 1" story.** *Object:* `mainGameInstance` (live). *Action:* before
the existing menu's NEW GAME → Story → create, read `gi.startDay`@0x01E4 right after a vanilla create,
OR drive our CreateNamedStorySave with day=1 and day=0 and observe the in-game day. *Verify:* which value
yields the story "Day 1" the user expects (VOTV may treat startDay as a 0-based offset the gamemode +1's).
Low risk — default-leave (don't write startDay) reproduces the widget's default if we also leave `ETB_days`
semantics; safest is to NOT write startDay for a vanilla create and confirm the default day matches NEW GAME.

**P2 — saveSlot CDO defaults (mainMap/Level/Version/Day/localGameRules).** *Object:* `Default__saveSlot_C`
(GAMEPLAY L296493) or a freshly `CreateSaveGameObject`'d instance. *Action:* dump those field bytes via a
UE4SS Lua probe (or our reflection log). *Verify:* a blank save's `Version` is set/empty (does VOTV stamp
the version at create or at first real save? — `regenerate` does NOT show a version write, but `save`
does carry `CallFunc_gameVer_version` GAMEPLAY L228277, suggesting version is stamped on the FULL save,
not the create-blank). Confirms whether we should stamp `Version`@0x0418 at create for the slot list to
show a version string. Cosmetic; not boot-blocking.

**P3 — the second create-ubergraph `SaveGameToSlot` (L144267).** *Action:* UE4SS hook
`UGameplayStatics::SaveGameToSlot` during a real NEW GAME create and log the two slot-name args. *Verify:*
whether the companion write is `data.sav`/a subsave/`lastPlayed` — i.e. whether skipping it loses the
slot from the picker list or just a cosmetic "last played" entry. If it's needed for the slot to LIST, add
it; else skip (RULE 2 — don't replicate cosmetic writes).

**P4 — does `regenerate` seed `this` or only the blank?** *Resolved by the locals to NOT seed `this`*:
`regenerate` creates a NEW blank and writes it, with no reset_*/setGamemode locals. But CONFIRM there is no
hidden member-write (e.g. it copies `this.localGameRules` into the new object via a struct-set node that
wouldn't show as a distinct local). *Action:* call our CreateNamedStorySave (which seeds nothing) and
confirm the resulting save loads as a clean vanilla story Day-1 with default rules. If the world comes up
wrong (rules off / wrong day), the widget DOES pre-seed `tempSave` before `regenerate` and we must
replicate that pre-seed. Most-likely-fine (StartFreshGame already proved a blank save boots clean).

**P5 — `isStringLegal` literal sets.** *Action:* UE4SS dump the `illegalNames`/`IllegalCharacters`
arrays (or call `isStringLegal` across candidate names and observe `legal`). *Verify:* our C++ legality
guard matches VOTV's reserved set. Low risk — we generate the name; only matters if we ever feed raw
user text straight in. If we surface a free-text name box, call the REAL `isStringLegal` on the widget CDO
rather than reimplementing (RULE 1).

---

## 9 — Cross-refs
- `src/votv-coop/src/ue_wrap/engine.cpp` (LoadStorySave :207, StartFreshGame :295, ApplyGameModeFromSlot
  :150 — the reuse surface + the empirical getSavePrefix map logging).
- `src/votv-coop/include/ue_wrap/engine.h` (the API to extend with CreateNamedStorySave).
- `src/votv-coop/src/harness/harness.cpp:287 BootStorySaveBlocking` (`fresh_boot` ini gate — the boot poll
  that calls StartFreshGame/LoadStorySave; CreateNamedStorySave plugs in here or via the Host button).
- `research/findings/votv-save-path-RE-2026-05-30.md` (SaveGameToSlot → on-disk layout, non-atomic write,
  the client save-block).
- `research/findings/votv-host-world-snapshot-RE-and-design-2026-06-04.md` (StartFreshGame validation;
  blank-save-boots-clean proof).
- `CXXHeaderDump/{ui_saveSlots,saveSlot,ui_menu,ui_gamemode,mainGameInstance,save_main,struct_gameRules,
  uicomp_saveSlot,enum_gamemode_enums}.hpp`.
- Object dump anchors: MAIN_MENU L144198-144360 (create ubergraph + gen), L253831-253862
  (getSavePrefix/isStringLegal), L55164-55496 (native save fns); GAMEPLAY L296385-296426
  (regenerate/Duplicate), L228270-228354 (saveSlot save/setGamemode/reset_days).
- Runtime log proof of the prefix map: `Game_0.9.0n/.../votv-coop-{host,client}.log:10648-10655`.
