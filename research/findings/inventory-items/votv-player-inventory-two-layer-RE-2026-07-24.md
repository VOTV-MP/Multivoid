# The player inventory is TWO layers: a live `propInventory`/`GObjStack` store and a save-side `inventoryData` projection — RE (2026-07-24)

**Status: MEASURED.** Every claim in sections 2-4 is read off the cooked Blueprint bytecode of
VOTV 0.9.0n, with call targets resolved through each package's `Imports[]`/`Exports[]` tables (see
§2 "How this was measured" — the resolution method is load-bearing and is the reason this document's
table can be trusted where an earlier name-grep could not). Section 5 states what is NOT measured,
in the same detail.

**This document is about the two-layer picture, not about a bug.** It was produced while chasing a
concurrent container-take duplication, but the duplication is the occasion, not the subject. What is
recorded here is the item-storage topology, because that topology is what any future inventory,
custody, persistence or arbitration work has to build on.

---

## 1. The finding in one paragraph

VOTV keeps the player's items in **`UpropInventory_C` → `saveSlot.GObjStack[propInventory.Index]`**
— the live store, mutated during play by the container verbs `addObject` / `takeObj`. A **separate**
field, `saveSlot.inventoryData`, is a **save-side projection**: `mainGamemode::saveObjects` reads
`propInventory` / `GObjStack` and writes `inventoryData`. Across the entire player-inventory UI and
container surface — four cooked assets — `inventoryData` is referenced **zero** times. The live item
flow never touches it.

---

## 2. How this was measured (read this before trusting the numbers)

Assets were extracted from the cooked game (`research/pak_re/extracted/VotV/Content/...`) and dumped
with `kismet-analyzer to-json` (UE4.27) into `research/pak_re/inv_ui_dump/` — generated RE output,
gitignored, regenerate rather than commit.

Two different techniques were used, and the difference matters:

- **Call targets were RESOLVED**, not grepped: every bytecode node whose `$type` is a call
  (`EX_FinalFunction`, `EX_VirtualFunction`, `EX_LocalVirtualFunction`, `EX_LocalFinalFunction`,
  `EX_CallMath`) was walked recursively, and its `StackNode` index resolved against the package's
  `Imports[]` (negative index) / `Exports[]` (non-negative index) tables, falling back to
  `VirtualFunctionName` for name-dispatched calls. **This is what produced §3's call chain.**
- **Field references were COUNTED by name** over the exports' bytecode JSON. That is the weaker
  technique, and on its own it is exactly the method that failed in §5.3.

**Why §3's zeros are nevertheless usable:** they are not the finding. The finding is the POSITIVE
call chain in §3.1, which names the path the item actually takes. The `inventoryData` zeros are
corroboration sitting beside a positive result, not a conclusion drawn from an absence. Per
`[[lesson-negative-grep-verify-against-known-positive]]`, a negative count is evidence only when the
same pattern demonstrably matches a known-positive — here the known-positive is
`mainGamemode::saveObjects`, where the identical name-count technique returns `inventoryData` **3**
(§4). The pattern can match; it simply finds nothing in these four assets.

---

## 3. The live path — measured

### 3.1 A container slot press

`uicomp_playerInvContainerSlot`'s exported `pressButton` is the usual thin stub; the body is in
`ExecuteUbergraph_uicomp_playerInvContainerSlot`. Its resolved calls, in order of appearance:

```
PrintString, getObject, addObject, K2_DestroyActor, gen_player, addHint,
Conv_NameToText, setHovertext, setHoverContainerSlot, pressButton,
IsVisible, IsHovered, setHoverContainerSlot
```

So one slot press = **`getObject` (read the record out of the source container) → `addObject` (into
the player's `propInventory`) → `K2_DestroyActor` (the transient actor)**.

It calls **`putObjectInventory` zero times** and references `inventoryData` zero times.

`IsHovered` + `setHoverContainerSlot` appearing in the same body is the bytecode form of the
already-recorded hover gate (`[[lesson-umg-click-handler-gated-on-hover-state]]`) — a reflected
`pressButton` on a slot the UI does not consider hovered no-ops.

### 3.2 The player's own container

`Aprop_inventoryContainer_player_C` derives from `Aprop_container_C`, so it carries a
`propInventory` component exactly like a world container. Its `extract` resolves to:

```
takeObj, removeVol, BeginDeferredActorSpawnFromClass, FinishSpawningActor,
GetPlayerPawn, GetTransform, K2_GetRootComponent, MakeTransform, BreakTransform,
loadData, dingus
```

That is the **inventory → world** direction: take the record, spawn a world actor from it. Its
`loadData` / `getData` / `ignoreSave` / `skipPreDelete` exports carry **empty bytecode**.

Recorded because it corrects an earlier framing: a prior note retracted "the extracted item lands in
the player's inventory" in favour of "`takeObj` SPAWNS A WORLD ACTOR". Both are true of *different*
verbs — the spawn belongs to the **player** container's `extract` (dropping out of your inventory),
while a **world** container's slot press adds into the player's `propInventory` (§3.1). The two
directions were being described as one.

### 3.3 Field references across all four assets

Name-counts over export bytecode (see §2 for why these are corroboration, not the finding):

| asset | `inventoryData` | `propInventory` | `GObjStack` | `obj_11` | `putObjectInventory` |
|---|---|---|---|---|---|
| `prop_container` | **0** | 22 | 6 | 0 | **0** |
| `prop_inventoryContainer_player` | **0** | 3 | 0 | 0 | **0** |
| `uicomp_playerInvContainerSlot` | **0** | 2 | 0 | 0 | **0** |
| `ui_playerInventory` | **0** | 20 | 4 | 0 | **0** |

---

## 4. The save-side projection — measured

In `mainGamemode` (dump `docs/piles/re-artifacts/mainGamemode.json`, same resolution method):

| function | `inventoryData` | `GObjStack` | `propInventory` |
|---|---|---|---|
| `saveObjects` | **3** | 2 | 2 |
| `putObjectInventory` | **6** | 0 | 0 |
| `loadObjects` | 0 | 0 | 0 — **but see §5.3** |

`saveObjects` reading `propInventory` / `GObjStack` while writing `inventoryData` is the **only
measured live → save writer**, and it is what makes `inventoryData` a projection rather than a
parallel store.

`putObjectInventory` writes `inventoryData` (×6) with zero `GObjStack` — a second writer whose
caller is unknown (§5.1).

---

## 5. What is NOT measured

### 5.1 Who calls `putObjectInventory`
Unattributed. It appears in **none** of the four assets in §3.3, so it is not part of the
container-take path — but no caller was found in any dump currently on disk. Whether it is the
hand→inventory path, a UI path in an undumped package, or something else is open. Note it is not
needed to explain the one recorded case of `inventoryData` holding an item: that was the coop
first-join starter kit, written by our own `BuildFirstJoinStarterKit` → `ApplyToSaveObject`
(measured: the persisted blob decodes to `prop_equipment_compass_C` +
`prop_equipment_flashlight_C`, both `StarterIndex` classes).

### 5.2 Whether `saveObjects` runs on a save-blocked client
`coop/save/save_block.cpp` detours `SaveGameToSlot` and blocks client world-saves. The detour is at
the **disk-write** seam. Whether the Blueprint-side `saveObjects` still executes on a client and
refreshes `inventoryData` **in memory** upstream of that block is unmeasured, and it cannot be
settled from a single-peer run — a client only exists inside a session.

### 5.3 `loadObjects` reads through a dispatcher — and the earlier zero was a FALSE NEGATIVE
`loadObjects` shows 0 name-references to `inventoryData` / `GObjStack` / `propInventory`. That zero
was briefly taken as "the load path does not read the save-form." **It is a false negative.**
Resolving its calls shows:

```
loadData ×3 (EX_LocalVirtualFunction), gatherDataFromKey ×2, getKey ×2,
BeginDeferredActorSpawnFromClass ×2, FinishSpawningActor ×2, addHint ×8, ...
```

`loadObjects` dispatches to **`loadData`** on the loaded actors — the same indirect-dispatch shape
already recorded for `loadTriggers`. So which layer the load path populates, and from what, is
**undetermined**, with `loadData` as the concrete next target.

**This is a fresh instance of `[[lesson-negative-grep-verify-against-known-positive]]`, committed in
this very investigation**, and it is recorded rather than quietly fixed because the reasoning it
produced survived two rounds before being caught: a name-level answer was accepted for a
dispatch-level question. The general rule was already in the ledger; what this adds is that the trap
fires just as readily when the negative result is the *inconvenient* one, and that the tell is
structural — asking "what does X read?" and answering with a grep over X's own body, in an engine
whose Blueprints dispatch through interfaces and virtuals.

### 5.4 Which layer a rejoining client's items come back from
Not investigated here.

---

## 6. The one line about our lane

`coop/items/player_inventory_sync` polls `saveSlot.inventoryData` / `equipment` / `hold` (via
`ue_wrap::inventory::ReadAll`) — i.e. **the save-side projection**, not the live store. Whether any
defect follows from that depends entirely on §5.2, which is unmeasured.

*(Deliberately not stated as a persistence gap. Several stronger phrasings were drafted and withdrawn
during this investigation; the measured facts support the sentence above and no more.)*

---

## 7. Provenance / reproduce

- Dumps: `kismet-analyzer to-json` over
  `research/pak_re/extracted/VotV/Content/{objects/prop_container, objects/prop_inventoryContainer_player,
  umg/components/uicomp_playerInvContainerSlot, umg/interfaces/ui_playerInventory}.uasset`
  → `research/pak_re/inv_ui_dump/` (gitignored, regenerable).
- `mainGamemode` figures: `docs/piles/re-artifacts/mainGamemode.json` (also gitignored/generated).
- SDK cross-check: `CXXHeaderDump/propInventory.hpp` (`Index`, `Player`, `Owner`, `takeObj`,
  `addObject`, `recalculateNames`), `saveSlot.hpp` (`GObjStack` and `inventoryData` are distinct
  fields), `prop_inventoryContainer_player.hpp` (derives from `Aprop_container_C`),
  `ui_playerInventory.hpp` (`as_player`).
- Related: `votv-container-contents-gobjstack-RE-2026-07-22.md` (the world-container half),
  `votv-inventory-impl-plan-2026-06-14.md` (the 2026-06-14 plan, whose "KEY APPLY UNKNOWNS" block
  poses the source-vs-mirror question this document partially answers — its own §"SOLID" line calling
  `inventoryData` "the MIRROR" is a DESIGN-phase working model, not an as-built measurement).
