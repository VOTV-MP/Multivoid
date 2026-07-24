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

### 3.2a The parent's `extract` is a THIRD direction (world container → player inventory)

`Aprop_container_C::extract` — the base of §3.2's override — is **not** the same function, and the
difference is exactly one step:

```
prop_container_C::extract(index):                  prop_inventoryContainer_player_C::extract(index):
  propInventory.takeObj(index, ...)                  propInventory.takeObj(index, ...)
  spawn actor at the player's transform              spawn actor at the player's transform
  gamemode.mainPlayer.putObjectInventory2(actor)     --                     (step absent)
  actor->K2_DestroyActor()                           --                     (step absent)
  int_save(actor)->loadData(takeObj.Output)          int_save(actor)->loadData(takeObj.Output)
  propInventory.removeVol(...)                       propInventory.removeVol(...)
```

So the spawned world actor is a **carrier**, not the outcome: a world container hands it straight to
`putObjectInventory2` and destroys it, while the player's own container leaves it standing in the
world. Same verb name, opposite directions — which is why §3.2's "inventory → world" reading is
correct *for the override* and would be wrong if applied to the base class.

### 3.2b The TRANSFER mints a new identity — measured statically (2026-07-24)

The container -> personal transfer does NOT move a record. It **re-creates** it, and the ordering is
what makes that visible. `Aprop_container_C::extract`'s 33 statements, decoded (statement indices,
not byte offsets):

```
[02] popflow if !takeObj(index)                       <- Output = the SOURCE record (key intact)
[08] BeginDeferredActorSpawnFromClass(class, xform)   <- takes ONLY the CLASS off takeObj_Output
[09..12] transform recompute
[13] FinishSpawningActor
[14] gamemode.mainPlayer.putObjectInventory2(actor)   <- CAPTURE happens here
[16] actor->K2_DestroyActor()
[20] int_save(actor)->loadData(takeObj_Output)        <- restore, AFTER capture AND destroy
```

**The deferred-spawn window [08]-[13] contains no property application at all** — every statement in
it is `GetPlayerPawn` / `GetTransform` / `BreakTransform` / `MakeTransform` plus the two spawn calls.
The only thing drawn from the source record before spawning is its class.

And the capture is immediate: `UpropInventory_C::addObject` (reached through
`putObjectInventory2`) resolves to `safeAsProp -> ... -> getData -> Array_Add`. So it serialises the
actor **at call time**, and the actor at that moment is freshly spawned.

**Therefore: mint at spawn, captured at add, restored too late.** Two consequences:

1. **[MEASURED] The record that lands in `GObjStack[0]` carries a NEW key** — the carrier actor's,
   not the source record's. Confirmed at runtime: across two runs of the same save the four
   save-loaded personal items had byte-identical keys while the one item that passed through
   `extract` had a different key each time, and that key matched the `grab_hook[destroy-seam]` line
   for the carrier actor in the same second.
2. **[FALSIFIED as stated -- measured 2026-07-24] "the saved per-item DATA does not survive."**
   Predicted from the ordering: a taken record should land with a default/empty payload. Measured
   with the control in the same log line (four save-loaded records that never went through a take, vs
   the one that did):

   ```
   prop_crowbar_C |...{b5,f1,nm2}      <- save-loaded
   prop_drive_C   |...{b5,f1,sig1,nm2} <- save-loaded
   prop_food_C    |...{b6,f4,i1,nm2}   <- save-loaded
   prop_food_mre_C|...{b5,f3,nm2}      <- THE TAKEN ONE: not empty
   ```

   The taken record is **not empty**, so the strong form of the inference is dead.

   **And the instrument does not settle the weak form either -- it measures the wrong thing.** Those
   shapes differ BY CLASS (crowbar b5,f1 / food b6,f4,i1 / drive +sig1), i.e. the group slots are a
   class fingerprint that exists regardless of the VALUES in them. A spawn-default
   `prop_food_mre_C` would emit the same `b5,f3,nm2`. So "are the SAVED VALUES preserved across a
   take?" is still **OPEN**, and this readout cannot answer it: it confirms a shape rather than
   counting the quantity in question ([[feedback-probe-must-count-not-confirm]]).

   What would actually answer it: read the SOURCE container's slot as well and compare the record's
   values before vs after the take -- ideally on an item whose saved value is known to differ from
   its default (the `sig1` on `prop_drive_C` is the obvious candidate: take a drive carrying a
   signal and see whether `sig1` survives, since a fresh drive should carry none).

   Recorded in full because the sequence is the lesson: an ordering argument produced a confident
   prediction, the prediction was measured rather than assumed, and it was wrong -- but only the
   FIRST instrument built for it was capable of showing that, and it was not the one originally
   proposed (a `takeObj` log line, which would have separated nothing).

**What this means for identity across a transfer** (recorded here because a design will need it):
the SOURCE record and its key exist in the container's `GObjStack` slot *before* any spawn happens,
so two peers contending over a take are contending over an entity that already has a stable name.
The freshly-minted key on the destination side is downstream of that contention, not part of it.

### 3.3 Field references across all four assets

Name-counts over export bytecode (see §2 for why these are corroboration, not the finding):

| asset | `inventoryData` | `propInventory` | `GObjStack` | `obj_11` | `putObjectInventory` |
|---|---|---|---|---|---|
| `prop_container` | **0** | 22 | 6 | 0 | **0** |
| `prop_inventoryContainer_player` | **0** | 3 | 0 | 0 | **0** |
| `uicomp_playerInvContainerSlot` | **0** | 2 | 0 | 0 | **0** |
| `ui_playerInventory` | **0** | 20 | 4 | 0 | **0** |

**Read the last column with §4.1.** Those zeros are exact-name counts and are literally correct, but
the live call in `prop_container` (×1) and `ui_playerInventory` (×2) is to the near-twin
**`putObjectInventory2`** — a different function on a different class. The column truthfully says
"`putObjectInventory` is not called here" while the *behaviour* it names is present. An exact-name
count is only as good as the assumption that the name is unique.

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
parallel store. The copy is its statements [7]-[8]:

```
Array_Get(saveSlot.GObjStack, playerContainer.propInventory.index, item)
saveSlot.inventoryData = item.obj_11...        <- wholesale overwrite from the player container's slot
```

`putObjectInventory` writes `inventoryData` (×6) with zero `GObjStack` — a second writer whose
caller was unknown when this document was written. **It has since been measured to have NO caller
at all: see §4.1.**

### 4.1 The complete `inventoryData` census — and the near-twin that hid it (measured 2026-07-24)

**Method.** The name `inventoryData` was grepped over the *raw* `.uasset` headers of all 3050 cooked
packages (31 MB — the name table is in the header, so a package that references the field by name
must contain the string). That returned **three** packages. Each was then censused per-export over
bytecode, and every hit rendered.

| package | function | writes / reads | role |
|---|---|---|---|
| `mainGamemode` | `saveObjects` ×3 | **WRITE** | the projection copy from `GObjStack[playerContainer.propInventory.index]` |
| `mainGamemode` | `putObjectInventory` ×6 | WRITE | legacy pickup — **zero callers anywhere** |
| `saveSlot` | `reset_days`, `reset_levels`, `reset_player_stats` ×1 each | CLEAR | reset paths (`EX_InstanceVariable` parameter) |
| `ui_saveSlots` | `regenSave` ×2 | READ | the save-slot menu's save-**repair** routine (also regenerates `objectsData`, every `GObjStack[i].obj_11`, and `equipment`) |

**So no gameplay path READS `inventoryData`, and no load path restores it into the live store.**

**Positive control for that negative** (per `[[lesson-negative-grep-verify-against-known-positive]]`
— the same census must be able to FIND readers, or its zero measures its own blindness). Run over
`mainGamemode` + `mainPlayer` for the sibling fields:

```
mainGamemode:  ExecuteUbergraph equipment=3 hold=7 | AddEquipment=6 | RemoveEquipment=16 | saveHoldObj hold=2
mainPlayer:    ExecuteUbergraph equipment=12 hold=8 | addEquip=4 | updateHold=2 | checkEquip=4
               updateEquipment=13 | processArmor=3
```

~10 gameplay touchers for `equipment`/`hold` versus **two writers and no reader** for
`inventoryData`. The census sees readers when they exist.

**Why `putObjectInventory` looked live for so long — the near-twin.** A raw-header grep for
`putObjectInventory` returns 24 packages, which reads like a widely-called verb. Every one of those
24 is a substring hit on **`putObjectInventory2`**, a *different* function on a *different* class
(§4.2). Filtering for the bare name leaves exactly one package: `mainGamemode`, where it is defined.
Note this also means §3.3's `putObjectInventory` column of zeros was **literally correct and
substantively misleading** — the container path does put items into the player's inventory, just
through the `2` variant, so an exact-name count truthfully reported 0 while the behaviour was
present.

**The reverse side — this trap has already been sprung twice in this repo, and the record is now
corrected (checked 2026-07-24).** The forward direction is fine: our own source and the item docs
have referred to `putObjectInventory2` correctly for months (`inventory_pickup_sync.h` counts 26
call sites; `container.md`, `hook.md`, `prop_lifecycle.cpp`, `prop_sound.cpp`). But two documents
had absorbed the near-twin as the same verb:

- `docs/COOP_DISPATCH_VISIBILITY.md` glossed `putObjectInventory` as "=R-pickup" inside the
  caller-class-gate refutation. **Corrected in place.** The refutation itself is unaffected — it
  rests on a measured caller CLASS (`mainGamemode_C` 2270/2270), not on which function; and the
  static enumeration is right, because `putObjectInventory` genuinely does contain a
  `K2_DestroyActor` (at [944]). Only the gloss was wrong.
- `votv-inventory-drop-spawn-RE-2026-05-24.md` listed it as "Helper for moving a world actor INTO
  the player inventory" with no note that nothing calls it. **Row struck and annotated.**

That second row is almost certainly what made this session's first pass treat the function as live.
Recording the pair here, at the field, is the point: the discriminator is not the function's body
(which does exactly what the row says) but its **caller count**, and a body-only reading cannot see
the difference between an implementation and a live path.

### 4.2 The LIVE pickup verb is `mainPlayer::putObjectInventory2` — and it writes `GObjStack`

Resolved from `mainPlayer.uasset` (dumped this session):

```
putObjectInventory2(InputPin, noNotif):
  DoesImplementInterface -> cast int_player -> canBeCollected()          gate
  lib.safeAsProp(InputPin) -> if prop.frozen: awakeUnfreeze()
  ind = gamemode.propInventory.getInd()
  gamemode.playerContainer.propInventory.addObject(InputPin, ind, ret, err)    <- THE WRITE
  if ret:  PlaySound2D(inventory_Cue); InputPin->K2_DestroyActor();
           gamemode.playerInterface.updateSlotInv()
  else:    addHint(...)                                                  inventory-full
```

It references `inventoryData` **zero** times (per-export census of `mainPlayer`: `propInventory` ×4,
`inventoryData` ×0 — and the whole `mainPlayer` package's name table does not contain the string
`inventoryData` at all, while it does contain `GObjStack`, which is that negative's positive control).

`addObject` is the **same verb** as the container-slot press in §3.1. So the world-pickup path and
the container-take path converge on one store: `saveSlot.GObjStack` via the player container's
`propInventory`.

---

## 5. The open questions — and the two that CLOSED the same day

Written as "what is NOT measured" when this document was created on 2026-07-24; §5.2 closed later the
same day, so the heading would otherwise misfile a measured result. Section numbers are kept as-is
because the scope brief cites them — status is per sub-section, not inherited from this heading.

### 5.1 Who calls `putObjectInventory` — CLOSED 2026-07-24: **nobody**

**Measured: it has no caller in any cooked package.** A raw-header grep across all 3050 `.uasset`
files returns exactly one package containing the bare name — `mainGamemode`, where it is defined.
The 24 packages that appear to reference it are all substring hits on the near-twin
`putObjectInventory2` (§4.1, §4.2), which is a different function on `mainPlayer` and writes
`GObjStack`, not `inventoryData`.

So `putObjectInventory` is **legacy/dead code**: the pre-refactor pickup implementation, still
compiled in, still the reason `inventoryData` looks like it has a live pickup writer. Removing it
from consideration leaves `saveObjects` as the **only** live writer of `inventoryData` — which
strengthens rather than weakens §4's projection framing.

It is also not needed to explain the one recorded case of `inventoryData` holding an item: that was
the coop first-join starter kit, written by our own `BuildFirstJoinStarterKit` → `ApplyToSaveObject`
(measured: the persisted blob decodes to `prop_equipment_compass_C` +
`prop_equipment_flashlight_C`, both `StarterIndex` classes). Note `BuildFirstJoinStarterKit`
(`player_inventory_sync.cpp:461`) fills `out.inventory` **and** `out.equipment` / `out.hold`, and
those two latter fields do have live gameplay readers (§4.1 positive control) — so the kit's arrival
is not evidence that the `inventory` third of the lane does anything.

### 5.2 Why `saveObjects` does not run on a client — MEASURED (bytecode)

The gate is in **`saveSlot_C::save`**, and both the projection refresh and the disk write sit behind it:

```
op03  EX_JumpIfNot   BooleanExpression = EX_Context -> gamemode.disableSave   (no EX_Not wrapper)
                     CodeOffset 132  -> jumps PAST op04 when the value is FALSE
op04  EX_PopExecutionFlow                    <- reached when disableSave is TRUE = early RETURN
op12  EX_Context  calls 'saveObjects'        <- the projection refresh, downstream of the gate
op19  EX_LetBool  calls 'saveToSlot'         <- the disk write, downstream of the SAME gate
```

Our `coop/save/save_block.cpp` sets `disableSave = true` on a client (`FindBoolProperty`, real
byte+mask). So on a client `saveSlot_C::save` returns at op04 and **neither** `saveObjects` **nor**
`saveToSlot` runs. That is why the client's `inventoryData` never refreshes during a session (§6.1).

**Two facts worth carrying forward:**

1. **This was known and INTENDED — do not read it as an accident.** An earlier version of this section
   claimed the projection effect was "an undocumented second consequence." **That was false**, produced
   by grepping `save_block.cpp` and asserting about "that file" while the contract lives in
   `save_block.h`. The header's Part 3 states it exactly, and predates this investigation by three
   weeks: *"(user mandate 2026-07-04, 'выключить полностью save цикл нативный'): the native save CYCLE
   off, not just the write. `saveSlot_C::save` checks `gamemode.disableSave` at its HEAD [V bytecode]
   **BEFORE the world gather (`saveObjects`/`saveTriggers`)** and the `saveToSlot` write funnel."*
   So the gate is a deliberate user decision, the header names `saveObjects` explicitly, and it
   independently corroborates the bytecode read above (same finding, arrived at twice, three weeks
   apart). What is genuinely open is not whether this was known, but whether per-player inventory
   needs something the disabled cycle used to provide — a question for the scope brief, not a defect
   report against `save_block`.
2. **The `SaveGameToSlot` detour is a backstop that has never fired** — zero `save_block: BLOCKED`
   lines across every log on disk. It therefore has **no known-positive**, and its silence proves
   nothing (`[[lesson-negative-grep-verify-against-known-positive]]`).

> **Why this section was rewritten twice on 2026-07-24:** both earlier versions located the mechanism
> at the `SaveGameToSlot` seam, because they restated *our own log line's wording* ("client native save
> cycle OFF") as a fact about the game instead of reading the game's bytecode. Same error mode, twice,
> in the same session — our log describes our INTENT; only the BP says what is gated.

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

### 5.4 Which layer a rejoining client's items come back from — MEASURED 2026-07-24 (the load side)

Resolving the load chain rather than name-grepping it (the §5.3 correction, applied):

**A world container restores only its INDEX.** `prop_container_C::loadData(data)`:

```
prop_C.loadData(data)                                     <- explicit parent call
propInventory.index = data.ints[0].ints[0]
nameData            = data.names[2].vectors
```

It restores **no contents** — only the index into the global array. The contents ride in
`saveSlot.GObjStack`, which is deserialized wholesale with the save. That is the load-side
counterpart of `[[lesson-container-contents-live-in-one-global-gobjstack]]`.

**The player's own container is excluded from the per-actor save path entirely.**
`Aprop_inventoryContainer_player_C` overrides both halves with stubs:

- `loadData` — **empty, and it does NOT call the parent** (3 statements: `None = None / return / END`;
  contrast `prop_container_C::loadData`, whose first statement is an explicit `prop_C.loadData`). So
  the player container does not restore `propInventory.index` the way a world container does.
- `getData` — returns a **default-constructed empty `struct_save`**, so it contributes nothing to the
  save's per-object data.

**MEASURED — the slot is a COMPONENT-TEMPLATE DEFAULT, which is why the stubs can be empty.**
`Aprop_inventoryContainer_player_C`'s Simple Construction Script carries a component template
`propInventory_GEN_VARIABLE` with three serialized overrides:

| property | type | value |
|---|---|---|
| `index` | Int | **0** |
| `player` | Bool | **True** |
| `customVolume` | Float | 50000.0 |

The **base** `Aprop_container_C`'s own `propInventory_GEN_VARIABLE` has **zero** serialized overrides
— so a world container inherits `player = false` and gets its index from the save (§5.4's
`prop_container_C::loadData`), while the player's container is born at `GObjStack[0]`, flagged
personal, with a 50000 volume cap.

That closes the loop cleanly:

- the player container's slot is **baked at construction**, not restored — so `loadData` has nothing
  to do, and its empty override is correct rather than suspicious;
- `player = true` is the discriminator that separates the personal store from world containers in the
  one shared `GObjStack` — the same flag our BOUNDARY 1 reads (`Player != 0`, fail-closed), and the
  same one behind the 2026-07-22 finding;
- `mainPlayer::putObjectInventory2` calling `gamemode.propInventory.getInd()` before `addObject` is
  a lookup within that container, not a search for it.

**Still inferred:** that the runtime index REMAINS 0 for the player container. `propInventory` does
expose `moveIndex(ID, Add)`, so a runtime change is expressible; nothing measured says it happens.

> **How this was nearly recorded as unanswerable.** The first pass searched for `"Player"` — the
> spelling in the SDK header (`propInventory.hpp: bool Player; // 0x00F9`) — got zero hits across
> every dump including this asset's, and concluded the setter was invisible to bytecode and would
> need native RE. The serialized Blueprint property is spelled **`player`**, lowercase. A
> case-sensitive query against the wrong one of two spellings of the same field, and the null read as
> "not our toolchain". This happened in the same session that wrote
> `[[lesson-negative-grep-verify-against-known-positive]]`, and it is logged there as a further
> instance.

**Consequence for the question this section was opened to answer:** a rejoining player's carried
items can only come back through `GObjStack` (with the save that carries it), never through
`inventoryData` — because per §4.1 nothing reads `inventoryData` back into anything.

---

## 6. The one line about our lane

`coop/items/player_inventory_sync` polls `saveSlot.inventoryData` / `equipment` / `hold` (via
`ue_wrap::inventory::ReadAll`) — i.e. **the save-side projection**, not the live store. Whether any
defect follows from that depends entirely on §5.2, which is unmeasured.

*(Deliberately not stated as a persistence gap. Several stronger phrasings were drafted and withdrawn
during this investigation; the measured facts support the sentence above and no more.)*

### 6.1 AUTHOR-REPORTED, **NOT MEASURED** (2026-07-24)

Kept as its own sub-item so that reported and measured do not merge into one paragraph. The
paragraph above is unchanged and still states only what §3-§4 measured.

The project author states, verbatim:

> личные инвентари и персист в `<guid>.json` толком не работают и строились наспех

**Status: author report. Not a measurement, and not treated as one.** It is recorded because it
removes the possibility §6 was hedging against ("perhaps nothing is wrong"), not because it
establishes a mechanism. No paraphrase of this sentence should be promoted into a measured claim —
during the investigation that produced this document, restating someone else's formulation as the
primary's own thesis happened twice and had to be withdrawn both times.

What the measured chain would predict, IF the report's cause is the topology in this document
(§3 live store, §4 `saveObjects` as the only measured live→save writer, `save_block.cpp` blocking
client world-saves at `SaveGameToSlot`): a client's `inventoryData` stays at its load-time contents
for the whole session, so the lane streams the join-time state and nothing acquired during play is
persisted. That prediction is **coherent but not confirmed** — the open link is §5.2, and note it
governs only the SIZE of the effect, not whether one exists.

#### Update 2026-07-24 (same day) — the MECHANISM is measured; the report is still a report

Three separate statements, kept separate on purpose:

1. **MEASURED — the mechanism.** A two-peer run called `mainGamemode::saveObjects` directly on each
   peer right after each had taken an item. Both projections CHANGED (host `inv 4→5`, client
   `inv 0→6`), each peer serving as its own known-positive. So `saveObjects` does propagate live →
   projection, and the client's projection was sitting **six records behind its live store**. The
   downstream half of the lane is **not broken**: the moment the projection changed, the client
   streamed and the host persisted, both correctly and immediately. The lane is **starved of input**,
   not defective.
2. **STILL AUTHOR-REPORTED — the report.** §6.1's sentence has *not* been verified against an
   observation. What the author actually saw in play (items missing after a rejoin? an empty
   inventory? something else) was never established, so the measured mechanism may be the cause of
   that report, part of it, or a different fault coexisting with it. Do not merge 1 and 2.
3. **INSTRUMENT CAVEAT — the probe perturbs what it measures.** The `saveObjects` call is NOT
   read-only: refreshing `inventoryData` makes `player_inventory_sync`'s ~1 Hz poll see a hash change,
   stream, and the host persist — one call rewrote `coop_players/<guid>.json` from 2204 to 4848 bytes
   with a state no organic run produces (restored from `.bak`). The earlier "no `SaveGameToSlot`, so
   no disk write" claim reasoned about one disk path and asserted about all of them. The probe is now
   PASSIVE by default; the forcing call is behind `VOTVCOOP_PROJWATCH_FORCE=1` with this warning at
   the call site.

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
