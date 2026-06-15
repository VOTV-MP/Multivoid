# VOTV Sandbox "Q" prop-spawn menu — RE for host→client sync mirroring

Date: 2026-06-11
Engine: stock non-nativized UE4.27 (every BP is Kismet bytecode; not in IDA).
Tooling: `tools/bp_reflect.py` → `research/bp_reflection/<name>.json`;
disasm via `research/bp_reflection/_disasm.py <name> <fn>`; CXX header dump at
`Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/*.hpp`.

## TL;DR / RECOMMENDATION

**Verdict (b): extend the M2/host_spawn_watcher seam to catch Q-menu spawns, but
broadcast them KEYED (read `Aprop_C.Key @ +0x02E0`) via the existing keyed
`PropSpawn` pipeline — NOT keyless/eid.** The Key is minted **synchronously at
spawn** but only **during `FinishSpawningActor`** (the UCS runs `init()` →
`getKey()` → `lib::assignKey` → `generateRandomKey`). So:

- The host watcher must observe at **`UGameplayStatics::FinishSpawningActor` POST**
  (where the Key is already set), **not** at `BeginDeferredActorSpawnFromClass`
  POST (where M2 currently observes — the Key is still `None` there).
- Both seams are `EX_CallMath` on the `GameplayStatics` CDO → **ProcessEvent-
  OBSERVABLE** (the same family M2 already hooks). No new substrate needed.
- The spawned actor's OWN `init()` (the function `prop_lifecycle` hooks as the
  `Init` UFunction) is dispatched **`EX_LocalVirtualFunction` from the prop's
  `UserConstructionScript`** → bypasses our ProcessEvent detour →
  **`prop_lifecycle` does NOT catch Q-menu spawns today** (the M2 gap, confirmed).

This is option (b) from the prompt. It is **not** (a): the existing keyed
Init-POST path does NOT already mirror these (the init is BP-internal). It is
**not** the keyless M2 path either: Q-menu props are real keyed/saved `Aprop_C`,
so they must ride the keyed identity, exactly like a hand-dropped prop.

The category split (Q4/Q6 below) does **not** change the verdict: all three spawn
branches produce **`Aprop_C`-lineage keyed props**, so a single rule — "at
FinishSpawningActor POST, if the finished actor `IsDescendantOfProp`, read its
Key + Name, broadcast keyed PropSpawn" — covers the whole catalog.

---

## 1. What class is the Q menu, and what is the spawn function?

There are **two** spawn entry points; both converge on the same observable seam.

### Primary (the Q-key menu): `ui_spawnmenu_C` → `mainGamemode_C::spawnPropThroughGamemode`

- The Q key is `AmainPlayer_C::InpActEvt_spawnmenu_K2Node_InputActionEvent_*`
  (mainPlayer.hpp:612-613) — the "spawnmenu" input action.
- It drives `Uui_spawnmenu_C` (`ui_spawnmenu.hpp:4`, UMG widget), held by
  `ApropProcessor_C::spawnmenu @ +0x0298` (propProcessor.hpp:16). The gamemode
  owns the processor at `AmainGamemode_C::propRenderer @ +0x0518`
  (mainGamemode.hpp:82, `ApropProcessor_C* propRenderer`).
- The catalog lives in the processor: `propsNames`/`Names`/`propData`
  (propProcessor.hpp `propsNames @ +0x0258`, `propData @ +0x03A0`), sourced from
  the **`list_props`** + **`list_food`** DataTables.
- Clicking a slot calls **`Uui_spawnmenu_C::spawn(FName name)`**
  (ui_spawnmenu.json `spawn`, 18 exprs). It traces from the camera, builds the
  spawn transform, and at expr **[13]** calls:
  `gamemode->CALLVIRT spawnPropThroughGamemode(n, a, 1, ...)`.
- **`AmainGamemode_C::spawnPropThroughGamemode(FName prop, FTransform, int amount, AActor*& actor)`**
  (mainGamemode.json, 59 exprs) is **THE Q-menu spawn function.**

### Secondary (the physical toolgun weapon): `prop_toolgun_C` → `tool_spawn_C`

- `Aprop_toolgun_C : Aprop_C` (prop_toolgun.hpp:4) holds a `ui_spawnmenu_C` at
  `+0x0440` and an `AtoolObject_C* activeToolReference @ +0x0430`.
- The "spawn" tool is `Atool_spawn_C : AtoolObject_C : Aactor_save_C`
  (tool_spawn.hpp:4). Its spawn logic is in
  `Atool_spawn_C::ExecuteUbergraph_tool_spawn` (tool_spawn.json, 79 exprs),
  reached via `Init(toolgun)` → `ExecuteUbergraph_tool_spawn(3506)`.

Both functions are byte-for-byte the **same three-branch spawn pattern** (below)
and hit the **same GameplayStatics seam**. Mirroring at FinishSpawningActor POST
covers both with one observer.

---

## 2. HOW does it spawn — and 3. is the dispatch observable?

**Decisive: every spawn call is `BeginDeferredActorSpawnFromClass` +
`FinishSpawningActor` on the `GameplayStatics` CDO via `EX_CallMath`
(cross-object, ProcessEvent-OBSERVABLE — the exact M2 seam).**

`mainGamemode::spawnPropThroughGamemode` bytecode (the Q-menu path), with the
dispatch type of each call verified from the raw JSON `$type` + import owner:

```
[2]  Prop_To_Object_ReturnValue = Default__lib_C->CALLVIRT "Prop To Object"(prop, ...)   # EX_LocalVirtualFunction (resolves class from list_props/list_food)
[3]  IsValidClass(Prop_To_Object_object)
[4]  JUMPIFNOT 743                                          # if row has explicit class -> DYNAMIC branch
--- DYNAMIC-CLASS branch (list_props row that names a BP class) ---
[9]  BeginDeferredActorSpawnFromClass(self, Prop_To_Object_object, InputPin, b2, _)   # EX_CallMath, owner=GameplayStatics  (OBSERVABLE)
[10] FinishSpawningActor(BeginDeferred_RV_2, InputPin)                                # EX_CallMath, owner=GameplayStatics  (OBSERVABLE)
[11] Array_Add(Temp_wildcard, FinishSpawningActor_RV_2)
[17] (AsProp)->CALLVIRT asProp(asProp_return)
[18] asProp_return->name = prop                                                       # writes Aprop_C.Name @ +0x0258
[19] asProp_return->CALLVIRT init()                                                   # EX_LocalVirtualFunction (BP-internal!) — see Q5
--- FOOD branch (list_food entry) ---
[29] BeginDeferredActorSpawnFromClass(self, obj(prop_food_C), InputPin, b2, _)        # EX_CallMath, GameplayStatics
[30] Default__KismetSystemLibrary->SetStructurePropertyByName(BeginDeferred_RV_1, n'foodData', ...)
[31] SetNamePropertyByName(BeginDeferred_RV_1, n'name', prop)                         # sets Name BEFORE finish
[32] FinishSpawningActor(BeginDeferred_RV_1, InputPin)                                # EX_CallMath, GameplayStatics
--- GENERIC branch (the common case: base prop_C, mesh data-driven by Name) ---
[43] BeginDeferredActorSpawnFromClass(self, obj(prop_C), InputPin, b2, _)             # EX_CallMath, GameplayStatics
[44] SetNamePropertyByName(BeginDeferred_RV, n'name', prop)                           # sets Name BEFORE finish
[45] FinishSpawningActor(BeginDeferred_RV, InputPin)                                  # EX_CallMath, GameplayStatics
```

Verified import owners (StackNode → Imports):
`BeginDeferredActorSpawnFromClass` (StackNode -42) Outer=`GameplayStatics`;
`FinishSpawningActor` (StackNode -44) Outer=`GameplayStatics`.
Verified `$type` for [9][10][29][32][43][45] = `EX_CallMath` (all six).

`tool_spawn::ExecuteUbergraph_tool_spawn` is identical: BeginDeferred at exprs
[5] (`prop_food_C`), [22] (`prop_C`), [47] (dynamic `Prop_To_Object_object`);
FinishSpawningActor at [10], [26], [50]; all `EX_CallMath` on GameplayStatics
(StackNode -42/-44). The dynamic-branch `asProp().init()` is at [59]
(`EX_LocalVirtualFunction`, `VirtualFunctionName="init"`).

**So M2's seam directly applies.** The only change vs M2's current hook is which
GameplayStatics function to observe (FinishSpawningActor instead of
BeginDeferred), so the Key read happens after `init()` has minted it.

---

## 4. Are the spawned props KEYED, and WHEN is the Key set?

**YES — keyed `Aprop_C`, Key minted synchronously AT SPAWN, inside
`FinishSpawningActor` (NOT available at BeginDeferred POST).**

The key chain (all in `Aprop_C` / `lib_C`, bytecode-cited):

1. `FinishSpawningActor` runs the actor's `UserConstructionScript`.
   `Aprop_C::UserConstructionScript` (prop.json, 12 exprs):
   ```
   [4] key = n'None'          # (only if resetKey)
   [8] CALLVIRT init()        # EX_LocalVirtualFunction  -> calls Aprop_C::init
   ```
2. `Aprop_C::init` (prop.json `init`, 77 exprs — note BP name is lowercase
   `init`; C++ header prop.hpp:92 capitalizes it `Init()`):
   ```
   [5]  name = propPicker                                  # (if propPicker != None)
   [21] GetDataTableRowFromName(obj(list_props), name, ...)   # resolves mesh/mass from catalog
   [50] CALLVIRT getKey(getKey_key)                        # <-- KEY ACQUISITION
   [51] key = getKey_key                                   # writes Aprop_C.Key @ +0x02E0
   [62] StaticMesh->SetStaticMesh(propData.<mesh>)         # data-driven mesh from list_props
   ```
3. `Aprop_C::getKey` (prop.json, 5 exprs):
   ```
   [1] Default__lib_C->CALLVIRT assignKey(key, self, self, assignKey_keyOut)
   ```
4. `lib_C::assignKey` (lib.json, 32 exprs):
   ```
   [6] EqualEqual_NameName(keyIn, n'None')
   [8] generateRandomKey()      # if key was None -> MINT a fresh random key  ("the NewGuid branch")
   [9] keyIn = generateRandomKey_RV
   [17-27] register (key, object) into gamemode->keyObj_key / keyObj_obj      # global key registry
   [29] keyOut = keyIn
   ```

Conclusion: a fresh Q-menu prop arrives at `init` with `Key == None`, so
`assignKey` **mints a random key and stores it into `Aprop_C.Key @ +0x02E0`
during `FinishSpawningActor`**. Therefore:

- At **`BeginDeferredActorSpawnFromClass` POST** (where M2 hooks today): Key is
  **None** — the actor exists but UCS/init have not run yet. Reading the Key here
  yields `None`.
- At **`FinishSpawningActor` POST**: Key is **set** — read `Aprop_C.Key @ +0x02E0`
  (FName) directly, or `ue_wrap::prop::GetKeyString(actor)` /
  `GetInteractableKeyString(actor)` (already in `prop.h`).

These props are **persistent/saved** (the standard `Aprop_C` save lifecycle —
the key is registered in `gamemode->keyObj_*` and the prop participates in
getData/loadData), so the **keyed identity is the correct cross-peer identity**,
identical to a hand-dropped or container-extracted prop. They are NOT the
keyless/transient class (contrast the pinecone/clump path).

Relevant offsets (already in `sdk_profile.h`):
- `Aprop_Name = 0x0258` (FName; the list_props row → which mesh)
- `Aprop_Key  = 0x02E0` (FName; cross-peer identity)
- `propPicker = 0x0240` (FDataTableRowHandle; pre-init source for `name`)

---

## 5. Are these ALREADY mirrored by the existing prop_lifecycle Init-POST observer?

**NO. They are currently MISSED — this is precisely the M2 gap.**

- `prop_lifecycle` registers POST observers on the `Init` UFunction across the
  `prop_C` lineage (prop_lifecycle.cpp:686-724, scanning for `P::name::PropInitFn
  = "Init"`). Observers fire **only when the UFunction is dispatched through
  `UObject::ProcessEvent`** — the observer table is a ProcessEvent detour
  (`ue_wrap/game_thread.h:6-7,113-128`: "the detour fires every matching observer
  for the dispatched UFunction").
- But the prop's `init` is invoked from its `UserConstructionScript[8]` as
  **`EX_LocalVirtualFunction`** (verified raw `$type`:
  `EX_LocalVirtualFunction, VirtualFunctionName="init", Parameters=[]`). An
  `EX_LocalVirtualFunction` dispatches via `UObject::ProcessInternal` off the
  vtable — it **does NOT route through `ProcessEvent`** — so the
  `prop_lifecycle` Init-POST observer **never fires** for a UCS-driven spawn.
- The dynamic branch also calls `asProp().init()` explicitly
  (`spawnPropThroughGamemode[19]` / `tool_spawn[59]`), again
  `EX_LocalVirtualFunction` → still bypasses the detour.

So the architectural fact from the prompt holds exactly: the spawned actor's
`Init` is BP-internal (EX_LocalVirtualFunction → ProcessInternal), bypassing the
ProcessEvent detour — **UNOBSERVABLE** — while the GameplayStatics
Begin/FinishSpawning calls are cross-object EX_CallMath — **OBSERVABLE**. The
keyed Init-POST pipeline cannot see Q-menu spawns; the host must catch them at
the GameplayStatics seam instead.

(The container-drop path is the analogue that DOES work today only because it
hooks `propInventory_C::takeObj` via ProcessEvent — see
`GrabObserver_PropInventory_TakeObj_POST` — not because the prop's own init is
observable.)

---

## 6. What can be spawned (catalog vs arbitrary)?

**A bounded DataTable catalog — `list_props` (+ `list_food`), keyed by FName —
NOT arbitrary classes.** `lib_C::"Prop To Object"(prop)` (lib.json, 49 exprs):

```
[0]  GetDataTableRowFromName(obj(list_props), prop, OutRow)
[2]  IsValidClass(OutRow.<class>)          # row may carry an explicit TSubclassOf<AActor>
[6]  object = OutRow.<class>               # -> DYNAMIC branch class (e.g. prop_toolgun_C)
[8]  propData = OutRow                      # the Fstruct_prop (mesh/mass/...)
...
[34] GetDataTableRowFromName(obj(list_food), prop, OutRow_1)   # else: food
[37] foodData = OutRow_1; isFood = true
...
[42] (fallthrough) propData = OutRow; isFood=false            # else: generic prop_C, mesh by name
```

Routing of the three spawn branches:
- **Generic `prop_C`** (the overwhelming majority): catalog rows with no dedicated
  BP class. The mesh/mass/collision are **data-driven** inside
  `init` via `GetDataTableRowFromName(list_props, name)` → `SetStaticMesh`. The
  per-prop identity is the **`Name` FName** (which row) + the minted **Key**.
- **`prop_food_C`**: `list_food` entries (food items), carrying `foodData`.
- **Dynamic class**: rows that name an explicit BP class (e.g.
  `prop_toolgun_C`). These are **still `Aprop_C` descendants**
  (`Aprop_toolgun_C : Aprop_C`, prop_toolgun.hpp:4) → still keyed via the same
  inherited `init`→`getKey`→`assignKey`.

The spawn menu exposes 10 category tabs (`enum_spawnmenuTabs`, enum_MAX=10). The
exact row count isn't extractable here (`list_props`/`list_food` are inside the
packed `.pak`, not on disk), but it doesn't affect the design: **every catalog
entry, regardless of branch, becomes an `Aprop_C`-lineage keyed actor finished by
`FinishSpawningActor`.** A class allowlist is therefore unnecessary — the right
gate is structural: **"finished actor `IsDescendantOfProp` (Aprop_C) with a
non-None Key."**

---

## Recommended implementation (concrete)

Implement as **option (b)** — extend `coop/host_spawn_watcher` (or a sibling
observer) to hook **`UGameplayStatics::FinishSpawningActor` POST** and broadcast
**keyed** PropSpawn. Key points:

1. **Seam:** register a POST observer on `GameplayStatics.FinishSpawningActor`
   (`P::name::FinishSpawningActorFn` already exists in sdk_profile.h:755;
   `BeginDeferredSpawnFn`/`FinishSpawningActorFn` both already resolved by
   `remote_prop_spawn.cpp` for the receiver). Read the finished actor from the
   `ReturnValue` out-param (use `R::FindParamOffset(fn, L"ReturnValue")`, same as
   M2 does for BeginDeferred).
   - Why FinishSpawningActor, not BeginDeferred: the Key is minted by `init`
     *during* FinishSpawningActor (Q4). At BeginDeferred POST the Key is `None`.

2. **Gate (host-only):** `if role==Host && actor IsLive &&
   ue_wrap::prop::IsDescendantOfProp(actor)` then read
   `keyStr = GetInteractableKeyString(actor)`; if `keyStr` is empty/"None", skip
   (defensive — shouldn't happen for a finished Q-menu prop). Dedupe against
   `PT::GetPropElementIdForActor(actor)` and `PT::HasProcessedInit(actor)` exactly
   like M2, so a prop that somehow *is* seen by both paths isn't double-broadcast.

3. **Broadcast:** use the **KEYED** `PropSpawn` payload — same construction as
   `prop_lifecycle.cpp`'s Init-POST broadcaster (the canonical keyed path):
   set `p.key` = keyStr, `p.propName` = `GetPropNameString(actor)`
   (the `Name @ +0x0258` row), real transform + scale, `kSimulatePhysics`,
   the Aprop_C parity bools (`IsHeavy/IsFrozen/IsStatic/IsSleeping/
   ReadRemoveWOrespawn`), and `PT::MarkPropElement(actor, keyStr, cls)` +
   `p.elementId`. Then `RecordClaimIfTracking(actor)`.
   - The receiver path is already correct: `remote_prop::OnSpawn` does
     `BeginDeferred → setKey(wireKey) BEFORE FinishSpawningActor` so the mirror's
     own `init` sees a non-None Key and skips the NewGuid branch
     (sdk_profile.h:1204-1215, remote_prop_spawn.cpp:10-11). So a keyed PropSpawn
     reproduces the prop with the **same Key on both peers** → stable identity,
     destroy/pose all route by Key as usual.

4. **No death-watch needed** (unlike M2's keyless pinecones): a Q-menu prop is a
   normal keyed prop. Its destruction goes through `K2_DestroyActor` (the player
   deletes it / undo), which the **existing** `GrabObserver_Actor_K2DestroyActor_PRE`
   already broadcasts by Key. (The undo path: `spawnPropThroughGamemode`'s caller
   `ui_spawnmenu::spawn[15]` registers `gamemode->addUndo`; undo eventually
   `K2_DestroyActor`s the actor → observable.)

5. **RULE-2 note:** if this observer fully subsumes M2's purpose, keep M2 for the
   keyless ambient set (pinecone/stick/crystal) — those are a genuinely different
   (keyless/transient + lifespan-expiry) case and stay. The new keyed handler is
   additive, on a different GameplayStatics function (FinishSpawningActor vs
   BeginDeferred), so they don't conflict. Do NOT route Q-menu props through the
   keyless eid path — they're keyed.

### Does the answer differ by prop category?
No material difference for the sync design:
- Generic `prop_C`, `prop_food_C`, and dynamic-class rows are **all Aprop_C
  keyed** and all finished by `FinishSpawningActor`. One structural gate
  (`IsDescendantOfProp` + non-None Key at FinishSpawningActor POST) covers all
  three.
- The only per-category nuance is *visual identity*: generic/food props need the
  `Name` FName (`+0x0258`) on the wire so the mirror's `init` resolves the right
  mesh/food data (the keyed PropSpawn already carries `propName` for this);
  dynamic-class rows carry their identity in the spawned **class** itself
  (`className` on the wire), which the keyed PropSpawn also carries. Both are
  already part of the keyed payload, so no special-casing is required.

---

## File / citation index
- `Game_0.9.0n/.../CXXHeaderDump/ui_spawnmenu.hpp` (Uui_spawnmenu_C; `spawn`, `Slots`, `activeTool`)
- `.../CXXHeaderDump/mainGamemode.hpp:82` (`propRenderer` ApropProcessor_C @ +0x0518)
- `.../CXXHeaderDump/mainPlayer.hpp:612-613` (`InpActEvt_spawnmenu_*` — the Q key)
- `.../CXXHeaderDump/propProcessor.hpp:16` (`spawnmenu` @ +0x0298; `propsNames`/`propData` catalog)
- `.../CXXHeaderDump/prop_toolgun.hpp:4,15` (Aprop_toolgun_C : Aprop_C; activeToolReference)
- `.../CXXHeaderDump/tool_spawn.hpp` / `toolObject.hpp` (Atool_spawn_C : AtoolObject_C; Init/Commit)
- `.../CXXHeaderDump/prop.hpp:10,12,20` (propPicker@0x0240, Name@0x0258, Key@0x02E0)
- `research/bp_reflection/mainGamemode.json` → `spawnPropThroughGamemode` (exprs [2][9][10][29][31][32][43][44][45]; BeginDeferred/Finish = EX_CallMath @ GameplayStatics)
- `research/bp_reflection/tool_spawn.json` → `ExecuteUbergraph_tool_spawn` (exprs [5][10][22][26][47][50][57-59])
- `research/bp_reflection/prop.json` → `UserConstructionScript[8]` (EX_LocalVirtualFunction init), `init` (exprs [5][21][50][51][62]), `getKey[1]`
- `research/bp_reflection/lib.json` → `assignKey` (exprs [6][8][9][17-27]), `Prop To Object` (exprs [0][2][6][8][34][37])
- `src/votv-coop/src/coop/host_spawn_watcher.cpp` (M2 — hooks BeginDeferred POST only; keyless ambient set)
- `src/votv-coop/src/coop/prop_lifecycle.cpp:686-724` (Init-UFunction POST scan — ProcessEvent-only, misses UCS init)
- `src/votv-coop/include/ue_wrap/game_thread.h:6-7,113-128` (observer = ProcessEvent detour)
- `src/votv-coop/include/ue_wrap/sdk_profile.h:344-350` (Aprop offsets), `:754-755` (Begin/Finish fn names), `:1204-1215` (receiver setKey-before-finish)
- `src/votv-coop/src/coop/remote_prop_spawn.cpp:10-11,66-67` (receiver BeginDeferred+setKey-before-Finish pipeline)
