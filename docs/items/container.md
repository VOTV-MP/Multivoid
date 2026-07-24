# container (prop_container family + propInventory brain + prop_openContainer) — the storage-container system   (STATUS: DESIGN)

RE pass 2026-07-11 (static bytecode, offsets computed with the validated `research/pak_re/size_ubergraph.py`
sizer; call censuses resolve StackNode import indices per `[[lesson-bp-json-grep-resolve-imports]]`).
Sources: `research/bp_reflection/{propInventory,prop_container,prop_inventoryContainer_player,
uicomp_playerInvContainerSlot,uicomp_playerInvSlot,ui_playerInventory,prop}.json`, CXXHeaderDump,
and the shipped mod code (cited file:line). Sync DESIGNED (§3, /qf-converged 2026-07-15) — DESIGN,
not built, no code; ONE open probe-gate (the slot `OnClicked` PE-hookability). Full trail:
`[[project-drone-sack-contents-qf-design-2026-07-15]]`.

Naming trap up front: the wire `ReliableKind::ContainerState` (interactable_sync.cpp:98-107) syncs
**`prop_swinger_C` LIDS** (wardrobe doors etc.), NOT this system. `prop_container_C` has **no lid
state at all** — "open" = open the UI (§1.4).

## 1. Native behavior (ground truth)

### 1.1 Class graph

```
UpropInventory_C : UActorComponent          [propInventory.hpp, size 0x128]  — the storage BRAIN
Aprop_container_C : Aprop_C                 [prop_container.hpp, size 0x42A] — the closed container actor
  +- ~40 asset variants, ALL empty subclasses [hpp census: crate, oldCrate(_S), oldBarrel(_vert/_notap),
  |    barrel, cardboardBox family (2x2x2..8x8x8), minifridge, desk, drawers, ffDrawers, fileCabs,
  |    wardrobe, bedsidetable/wbedtable, suitcase, wboot, pbasket, itemBox, orderbox, sbox, gembox,
  |    giftbox, brickstack, fishman, garbagebin2, bin2, oldbox(_kerfurJoints), singularity, ...]
  +- Aprop_atvUpgrade_container_C            [prop_atvUpgrade_container.hpp]
  +- Aprop_inventoryContainer_player_C       [prop_inventoryContainer_player.hpp] — the PLAYER BACKPACK
       (an invisible container actor; mainGamemode.playerContainer @0x780 [mainGamemode.hpp:129])
Aprop_openContainer_C : Aprop_C             [prop_openContainer.hpp, size 0x3A9] — SEPARATE open-top model
  +- fridgeshelf1/2/3, scrapbox, wastebasket, plate_S/L, bowl_S/M/L, easterbasket, ffWdL(S) [hpp census]
  +- Aprop_garbageContainer_C                [prop_garbageContainer.hpp]
```

- `propInventory` is a **component** on the container actor (`Aprop_container_C.propInventory @0x398`);
  it **owns the verbs**: `takeObj`, `addObject`, `addLoot`, `moveIndex`, `getObj`, `removeVol`,
  `checkObjectsVolume`, `isClass/isObjectInInventory`, `recalculateNames`, `Init` [propInventory.hpp:24-34].
- The actor owns the interaction layer: `extract`, `getObject`, `openContainer`, `putObjectIn_overlap`,
  `playerUsedOn`, `playerHandUse_RMB`, `broken`/`broken_fire`, `dingus`, `getData`/`loadData`
  [prop_container.hpp:29-55].
- `prop_openContainer_C` is **NOT a prop_container subclass** and has NO propInventory: its contents are
  **live actors** in `TArray<AActor*> itemsInside @0x378`, glued/held physically (`checkPickup`,
  `glueInside`, `doStuff`, `propAwoken`, `playerHoldPre`, `isHolding`) [prop_openContainer.hpp; no
  bytecode dump exists — function-name + RE-finding level only, see §4].

Key component fields (UpropInventory_C): `Index @0xB0` (**default -1** [propInventory.json CDO]),
`maxVol @0xB4` (default 1000), `currVol`, `Owner @0xD0`, `lootTableEntry @0xDC`, `randomLoot @0xE8`
(TArray<Fstruct_lootWeighted> = {object FName, amount, weight} [struct_lootWeighted.hpp]), `infinite`,
`Player @0xF9`, `volumeMult/customVolume`, `Filter @0x108` (class allowlist), `maxLoot @0x11C`
(default 10), `gam @0x120`. Actor fields: `lootEntry @0x3A0` (DataTableRowHandle), `spawnLoot @0x3B0`,
`massData/volumeData/nameData @0x3B8-0x3D8` (parallel accounting arrays), `Locked @0x3E8`,
`fridge @0x3F8`, `overlapDelay @0x3FC`, `velLinear/velAngular @0x410/0x41C`, `accurateInertia @0x428`.

### 1.2 The CONTENTS model — serialized structs in the save object, not actors

**Stored items are DATA, not live actors.** The backing store is on the live save object:
`UsaveSlot_C.GObjStack : TArray<Fstruct_mObject> @0x198` [saveSlot.hpp:23], where
`Fstruct_mObject = { TArray<Fstruct_save> obj }` [struct_mObject.hpp]. Each container's
`propInventory.Index` is a slot into GObjStack; the inner array is the ordered contents list; each
element is a full `Fstruct_save` (class + transform + key + the typed value-group arrays)
[struct_save.hpp] — the same record shape as player inventory items (see ue_wrap/inventory.h header
comment, "an inventory item is DATA, not an actor").

- **Registration**: `propInventory.init(Target, Owner)` — computes maxVol from `Owner.Volume`
  (× volumeMult², or customVolume) [init@~0-400]; then **only if Index < 0**:
  `Index = Array_Add(Owner.gamemode.saveSlot.GObjStack, empty)` and `addLoot()` [init@~501-676].
  Slots are append-only — an index is forever (removing a slot would shift every other container's
  Index), so orphan slots from destroyed containers stay as dead weight [inference from the index
  scheme; no Array_Remove(GObjStack) exists anywhere in the dumps].
- **Save**: `prop_container.getData` stores ONLY `propInventory.Index` (ints[0][0]) + `nameData`
  (names slot 2) into the prop's own Fstruct_save [getData bytecode, via lib.setSaveDataBySlots];
  `loadData` restores them [loadData bytecode: ints[0][0] → propInventory.index, names[2] →
  nameData]. The contents themselves persist for free because GObjStack lives ON the USaveGame
  object. A save-loaded container has Index >= 0, so init skips register+addLoot (**no loot re-roll
  on load**) [init GreaterEqual gate].
- **Joining client**: the v56 save transfer ships the host's whole `.sav` blob
  (save_transfer.cpp:60-110, whole-file capture) → the client save-loads the host world → its
  GObjStack copy == host's at capture, and keyed container props restore matching Index values via
  loadData. **Late-join contents are correct at join, then diverge** (no wire channel mutates
  GObjStack after that — §1.7).

### 1.3 Verb census — every mutator, with dispatch visibility

Offsets are statement byte-offsets in the named function/ubergraph (`uber@N` =
`ExecuteUbergraph_prop_container@N`). Per `[[lesson-ex-callmath-invisible-to-processevent]]` /
`docs/COOP_DISPATCH_VISIBILITY.md`, a static `$type` cannot PROVE PE-visibility either way — rows
below cite the dispatch map's live-verified entries where they exist.

**TAKE-OUT — `propInventory.takeObj(Index, removeVol)`** [takeObj@0-1568]:
- `removeVol=TRUE` → spawn INSIDE takeObj: deferred-spawn `stored.class` at owner-or-player location
  (switch on a bool — the `Player` flag selects `GetPlayerPawn(0)`, an SP assumption)
  [takeObj@224,572], `FinishSpawningActor`@~729, cast `int_save_C` → `loadData(storedStruct)`@846
  (restores the item's saved Key AFTER the spawn), `updateVolumesAndMass`@996; then fall into the
  removal block.
- `removeVol=FALSE` → removal only, no spawn [popflow@169 → @1087].
- Removal block @1087-1545: `Array_Remove` from GObjStack[Index].obj + owner.volumeData +
  owner.nameData; returns the struct + (spawn path) the actor.
- Callers: `checkObjectsVolume` passes **TRUE** [checkObjectsVolume@212 — the over-volume "burp"
  ejects the last item as a live actor]; `prop_container.extract`@5, `prop_container.getObject`@5 and
  `prop_inventoryContainer_player.extract`@5 all pass **FALSE** and run their own spawn+loadData
  outside takeObj.
- Dispatch: takeObj is a SCRIPT UFunction; called via `EX_Local*` it is invisible to BOTH
  ProcessEvent and a Func patch — the shipped catch is the `FinishSpawningActor` Func seam inside it
  (host_spawn_watcher, v106) plus the takeObj PRE/POST observer pair for PE-dispatched routes
  [docs/COOP_DISPATCH_VISIBILITY.md rows 87-88, live-tagged; prop_container_extract.cpp:53-159].

**TAKE-OUT wrappers on the actor:**
- `getObject(Index, customLoc, Loc)` [getObject@5-1015]: takeObj(false) → deferred spawn at
  player-or-custom loc [@219,506 SelectVector] → loadData@742 → `removeVol` → `dingus`. THE UI verb.
- `extract(Index)` [extract@5-995]: takeObj(false) → spawn at player pawn →
  `mainPlayer.putObjectInventory2(actor)`@544 → on accept `K2_DestroyActor`@631 (item became data in
  the player inventory); on decline the live actor stays at the player; → removeVol → dingus.
  Caller not found in the available dumps [?].
- `prop_inventoryContainer_player.extract` override [its extract@5-]: same shape minus
  putObjectInventory2 — spawns the item into the world at the pawn.

**PUT-IN — two entry points, one sink (`propInventory.addObject`):**
- `playerUsedOn(player, hit, ..., holdObject, ...)` [uber@4324-4527]: `Locked` gate @4324 →
  `player.input_run` gate @4339 → `addObject(holdObject)`@4371 → on success
  **`K2_DestroyActor(holdObject)`@4453** + Audio@4489; on error `lib.addHint(err)`@4527. (Use the
  container while holding an item.)
- `putObjectIn_overlap(Actor, Comp, Audio)` [BndEvt overlap → uber@3740 → Delay(0) → uber@169;
  body census]: gates IsSimulatingPhysics + not-self + `ignoreSave`==false + `canPickup` →
  `addObject` → **`K2_DestroyActor`** + `dingus` + Audio. (Throw a physics prop into the catch
  volume.)
- `addObject(Actor, insertIndex)` [addObject@0-3857]: `Filter` reject → "no class"; `lib.safeAsProp`;
  `propData.canCollect && !heavy` gate; class special-cases — **`prop_container_C` and
  `prop_mailbox_C` are REJECTED (no container nesting)**, `printedObject_C` and plain `prop_C`
  accepted [addObject@~1181-2474]; `int_save.getData(actor)` serializes the item; **`owner.fridge`
  is appended to the item's saved bools[0]** [addObject@~2540] (the fridge flag rides INSIDE the
  stored item's record — food-preservation plumbing); volume gate (`Get Volume` + currVol vs maxVol
  unless `infinite`); `Array_Insert/Add` into GObjStack[Index].obj + nameData@2941-3720;
  `updateVolumesAndMass`.
- Dispatch: addObject itself is not hooked by the mod and its call sites are `EX_Context` +
  `EX_LocalVirtualFunction` [bytecode $type] — presumed hook-invisible on the BP-internal routes,
  UNPROVEN live either way [?]. The put-in **K2_DestroyActor IS caught** by the v106 destroy-seam
  Func patch (prop_destroy_seam.cpp:40-130) regardless of dispatch route.

**OPEN — `openContainer`** [uber entry 4802 → @3920-4104]: if `Locked` → `lib.addHint` +
`audio_locked.Play` @3934-4025; else `gamemode.openPropInv(self)`@4067 [mainGamemode.hpp:443] which
brings up the shared inventory widget. **There is no lid, no open/close state, no animation** —
sound_open/sound_close exist as assets but no state bool. Reached from: interaction menu
(`actionOptionIndex` [uber@4210-4303]: base `prop_C.actionOptionIndex` then byte-compare → openContainer)
and `playerHandUse_RMB` [uber@4309] (RMB while carrying the container in hand).

**LOCK**: `Locked @0x3E8` is READ at open @3920 and put-in @4324; **no writer exists anywhere in
prop_container bytecode** — it is a class/instance default (locked variants). No unlock verb found:
`prop_C.crowbarOpen` is a no-op (frame-write → `ExecuteUbergraph_prop@458` = bare popflow) and
prop_container does NOT override `crowbarOpen` [prop_container.functions.txt]. How a locked
container ever unlocks (pryingCrowbar_C writing the field directly? a key item? never?) is **[?] —
pryingCrowbar_C has no bytecode dump**. (The `crowbarOpen` RNG residue named in
`docs/COOP_RNG_AUTHORITY.md` T1-4 is **actorChipPile/garbageClump's** crowbarOpen, not containers'.)

**LOOT ROLL — `propInventory.addLoot`** [addLoot@0-2652]: `GetDataTableRowFromName(lootTableEntry)`
→ append the row's weighted list to `randomLoot`; if len > maxLoot → `Array_Shuffle`; then per
entry: **`RandomBoolWithWeight(weight)`** → deferred-spawn a TEMP actor of the loot class
(`SetNamePropertyByName 'name'` for parseNameToObject rows; food + player-interface special cases)
→ `addObject(temp)` (serialize into GObjStack) → `K2_DestroyActor(temp)` [addLoot@~1682-2503].
Trigger chain: `ReceiveBeginPlay` [uber@3300] → register in `gamemode.allContainers`@3450 + random
tick interval 0.4-0.5 s @3392 → `propInventory.lootTableEntry = spawnLoot`@3690 → Delay(0) →
`spawned()` [uber@70/@85] → `propInventory.init` → **register + addLoot ONLY if Index < 0** (fresh,
never-saved container). **Unseeded local RNG, rolled once per container at first registration, on
whichever peer runs that BeginPlay.**

**BREAK/SPILL — `broken` [uber entry 3795], `broken_fire` [uber@4304 → goto 3795]**: overlap
collision off @3795 → loop over GObjStack[Index].obj [uber@250-3295]: deferred-spawn EVERY stored
item at `RandomPointInBoundingBox(bounds/1.5)` + `RandomRotator`, `loadData` each, inherit the
container's cached velocities + a random angular kick [@50-70 region], `int_objects.ignite(fuel/1.1)`
on each if the container burns [@76-77]. **No `Array_Clear` of the slot afterwards** [ubergraph
census: only Array_RemoveItem(allContainers) exists] — the GObjStack slot keeps stale structs after
a spill; harmless orphan data in SP.

**Housekeeping**: `dingus` [uber@3861] = `RetriggerableDelay(overlapDelay)` → re-enable the swallow
overlap (`SetCollisionEnabled(QueryOnly)` @211) — called after every extraction/insertion so a
just-ejected item is not instantly re-swallowed. `ReceiveTick` [uber@4716↔4625] caches
velLinear/velAngular (only when `accurateInertia` enabled the tick @4190). `ReceiveDestroyed`
[uber@4105-4189] → base + `Array_RemoveItem(gamemode.allContainers, self)` [census; the @4115
statement renders flattened]. `updateVolumesAndMass` recomputes volumeData + currVol from the
stored records via `lib.Get Volume`. `moveIndex(ID, Add)` reorders the slot arrays (hotbar-style
up/down) [moveIndex bytecode, pure array shuffle].

### 1.4 The UI flow — one widget, the same verbs

`mainGamemode.openPropInv(prop)` sets the container panel of the SHARED inventory widget
`Uui_playerInventory_C` (gamemode.propInventory @0x680 [mainGamemode.hpp:110]); the container side
renders `uicomp_playerInvContainerSlot` rows, the player side `uicomp_playerInvSlot` rows. **Every
UI move routes through the same takeObj/addObject verbs — there is no separate drag-drop path**,
and every move materializes a REAL transient actor and then destroys it:

- Container → backpack [uicomp_playerInvContainerSlot uber@82-320]: `as_prop.getObject(id, false)` →
  `gamemode.playerContainer.propInventory.addObject(actor)` → on success `K2_DestroyActor`; on
  failure the actor is left live (falls at the container) + `addHint`.
- Backpack → container [ui_playerInventory uber@1611-1805]: `playerContainer.getObject(ind, false)`
  → `as_prop.propInventory.addObject` → destroy on success.
- Backpack → world (drop/place) [ui uber@3085-3193]: `playerContainer.getObject(ind, traceHit)` →
  `mainPlayer.moveOutProp(actor)`.
- Hold/equip paths use `putObjectInventory2` / equipment arrays [ui uber@4381, @6642].
- Reorder within a panel: `moveIndex` [uicomp_playerInvSlot uber census].

The player backpack itself is `prop_inventoryContainer_player_C` — a real (invisible) container
actor whose propInventory rides the same GObjStack machinery. Its `loadData` / `ignoreSave` /
`skipPreDelete` overrides are no-op bodies and `getData` returns a **default-constructed empty
`struct_save`** [all four exports are 3 statements; `getData`'s single statement writes the empty
struct], i.e. it never persists as a world prop.

**[V] MEASURED 2026-07-24 — the GObjStack-vs-`inventoryData` relation is COPY-ON-SAVE, one way.**
This block previously asked "copy-on-save? two views of one thing?" and marked it `[?] unmeasured`.
Answered: `mainGamemode::saveObjects` statements [7]-[8] do
`Array_Get(saveSlot.GObjStack, playerContainer.propInventory.index)` and assign the result into
`saveSlot.inventoryData` — a wholesale overwrite at save time, and the **only live writer** of that
field. Going the other way, **nothing in the cooked game reads `inventoryData` back**: a census over
all 3050 packages finds it named in three (writers `saveObjects` + the dead `putObjectInventory`,
clears `saveSlot::reset_*`, and one reader — `ui_saveSlots::regenSave`, the save-slot menu's repair
routine). The player's slot is `GObjStack[0]`, baked by the class's component template
(`index=0, player=True`), which is why the `loadData` override can be a no-op — the slot is born,
not restored.

Consequence for our lane: `player_inventory_sync` polls the **projection**, so its `equipment`/`hold`
thirds address fields the game genuinely reads while its `inventory` third addresses one it never
reads back. (The earlier "live-verified" phrasing for the blob path was over-stated: the evidence is
two samples with zero variation between them, i.e. the lane *carries* those arrays — not that it
tracks a change.) Full RE:
`research/findings/inventory-items/votv-player-inventory-two-layer-RE-2026-07-24.md`.

### 1.5 prop_openContainer — the OTHER contents model (live actors)

Open-top containers (fridge shelves, plates, bowls, scrapbox, wastebasket, garbage container) keep
their contents as **live actors glued inside**: `itemsInside : TArray<AActor*> @0x378`, maintained
by `ReceiveTick` → `checkPickup` / `doStuff` / `glueInside` walking the array every tick
[prop_openContainer.hpp; research/findings/piles-trash/votv-garbage-trash-interaction-RE-2026-05-27.md §(a)].
`playerHoldPre`/`isHolding` handle carrying the whole assembly. No propInventory, no GObjStack slot,
no loot roll, no UI. **No bytecode dump exists for this class** — the mutator set ("likely
putObjectIn_overlap-equivalent + propAwoken/doStuff") was left probe-TBD by the 2026-05-27 finding
and remains **[?]**.

### 1.6 What ALREADY crosses the wire today (MEASURED-IN-CODE)

- **Extraction spawn crosses.** The extracted item's actor spawn is broadcast: the
  `propInventory_C::takeObj` PRE/POST observer pair (prop_container_extract.cpp:53-159 — POST reads
  the `Object` out-param, ensures a key, `SendPropSpawn`, with the `g_takeObjInFlight` bracket
  deferring the nested Init POST) plus the v106 `FinishSpawningActor` Func seam
  (host_spawn_watcher.cpp — catches the EX_Local-dispatched routes the observers can't; dispatch map
  rows 87-88 [V hands-on 2026-07-07]). Installed at StartCoopSession via subsystems.cpp:106.
- **Put-in world-actor destroy crosses.** The `K2_DestroyActor` of the swallowed item is caught by
  the v106 destroy-seam Func patch and broadcast as `PropDestroy` by key/eid
  (prop_destroy_seam.cpp:40-130; subsystems via prop_lifecycle Install).
- **Late-join contents cross once**, inside the v56 whole-save blob (§1.2).
- **The lid-named wire lane is someone else's**: `ReliableKind::ContainerState` = prop_swinger lids
  (interactable_sync.cpp:98-107, `docs/COOP_SYNC_MAP.md:114`).
- **prop_openContainer**: the contained actors individually ride the normal prop lanes
  (pose/destroy); the `itemsInside` MEMBERSHIP does not cross. The stale-pointer AV this causes on
  clients is PRE-cancelled for `Aprop_garbageContainer_C` only (garbage_sync.cpp:73-104, class-gated,
  running()+Client-gated); storage openContainers tick natively on both peers.
- `docs/COOP_ENTITY_EXPRESSION_MAP.md:52` already records the takeObj-POST catch [V]; nothing stale
  found in the two maps for this family, but **`docs/COOP_RNG_AUTHORITY.md` has NO row for the
  container loot roll (addLoot)** — a tracker gap (it is spawn-adjacent T1/T2 territory: unseeded
  `RandomBoolWithWeight` + `Array_Shuffle` per peer).

### 1.7 What DIVERGES today (the gap, stated per peer action)

No module reads or writes GObjStack — the CONTENTS LIST never crosses:

- **Take-out** (either peer): actor spawn crosses [V], but only the acting peer's GObjStack loses
  the struct. The other peer's copy of the container still LISTS the item → opening it there shows a
  ghost entry → extracting it manufactures a **duplicate**. [MEASURED-IN-CODE: absence of any
  GObjStack writer in src/votv-coop; no live probe of the dupe yet.]
- **Put-in** (either peer): the world actor's destroy crosses [MEASURED-IN-CODE], but only the
  acting peer's GObjStack gains the struct. On the other peer the item actor is destroyed AND its
  container gained nothing → the item is **lost** from that peer's world view.
- **Loot roll**: a save-loaded world container never re-rolls (join-safe), but a container spawned
  mid-session (delivery orderbox, Q-menu, mirror spawn) runs BeginPlay → spawned → init with
  Index=-1 **on each peer independently** → each peer appends its OWN GObjStack slot and rolls its
  OWN loot [bytecode chain §1.3; not live-measured]. Client-side mirror spawns of containers
  self-roll unless suppressed.
- **UI transfer failure paths** leave a live actor on ONE peer only (the getObject spawn crossed;
  the failed-addObject "actor stays" branch is local) — edge divergence [bytecode; unmeasured].
- **Locked** is a static default → consistent via save; no runtime divergence until an unlock verb
  exists [?].
- **prop_openContainer.itemsInside**: per-peer arrays over per-peer-visible actors; the garbage
  subclass's tick is cancelled client-side (AV fix), storage shelves/plates diverge silently.

## 2. Sync-axis table

Owner column: TBD where it is a design call. Pattern rows from `docs/items/README.md` §"The
pattern" that obviously apply: **row 2** (commit intent → host: eid-addressed ACTION on an existing
host-owned prop — put-in/take-out are exactly this), **row 7** (loot/RNG payouts host-rolled,
client trigger, prop lane delivers), **row 10** (held-vs-world cut: a container is used IN WORLD via
lookAt/use → HOST-authoritative side of the cut), and row 11's root shape (a client copy
self-simulating world-prop state = divergence class).

| axis | owner (who simulates) | peers need | carried by (lane/wire) |
|---|---|---|---|
| contents list (GObjStack slot per container) | TBD — pattern rows 2+10 point HOST | identical ordered list (UI shows it; extract indexes into it) | **NOT CARRIED today** (only the join-time save blob) |
| take-out (extract/getObject/UI move) | TBD — today: acting peer runs takeObj locally | the spawned actor (carried [V]) + the list decrement (NOT carried) | actor: PropSpawn via takeObj-POST + FinishSpawning seam [V]; list: nothing |
| put-in (playerUsedOn / overlap swallow / UI move) | TBD — today: acting peer runs addObject locally | the world-actor destroy (carried) + the list insert (NOT carried) | destroy: v106 K2_DestroyActor seam PropDestroy [MEASURED-IN-CODE]; list: nothing |
| loot roll (addLoot at first init) | TBD — pattern row 7 says HOST | identical rolled contents for mid-session-spawned containers | NOT CARRIED; per-peer unseeded RNG today; no COOP_RNG_AUTHORITY row |
| overflow burp (checkObjectsVolume → takeObj(true)) | follows contents-list owner | the ejected actor + decrement | actor: takeObj-POST spawn path; list: nothing |
| lid state | N/A — prop_container has NO lid (open = UI); swinger lids are a different class | — | swinger lids already ride ReliableKind::ContainerState [V, interactable_sync.cpp] |
| open/locked deny (openContainer) | per-player local UI (openPropInv) | nothing (UI is per-player); Locked is a static default | local only — fine as-is unless an unlock verb is found [?] |
| late-join contents | HOST (save author) | the full GObjStack at join | v56 whole-save blob [V at join; diverges after] |
| container actor itself (pose/grab/destroy) | prop lane (already owned there) | — | existing keyed-prop lanes [V, COOP_SYNC_MAP:86] |
| prop_openContainer itemsInside membership | TBD (member actors already ride prop lanes) | consistent membership OR proven-safe divergence | NOT CARRIED; garbage subclass tick cancelled client-side (garbage_sync.cpp) |

## 3. Coop design (/qf-converged 2026-07-15 — DESIGN, not built, no code)

> SUPERSEDES the 2026-07-11 "1 Hz count-diff post-hoc report" design that ACCEPTED the
> concurrent-take dupe ("Races accepted in Inc-1"). RULE 1 rejects an accepted-dupe design; a
> 15-round `/qf` (drone sack as the motivating instance) converged on the dupe-PREVENTING shape
> below. Do NOT re-propose the count-diff/accept-the-race approach. Full trail:
> `[[project-drone-sack-contents-qf-design-2026-07-15]]`.

**INVARIANT: a client cannot authoritatively mutate a host-owned container's contents. The HOST
is the sole author of the canonical contents; the client mirror is a read-only host-ordered copy.**
The design splits into TWO mutations with two mechanisms (fusing them was the trap the `/qf` caught):

- **POPULATION** (drone deposit via `compileOrder`→`addObject`; `addLoot` birth roll; any host-side
  add/remove) — HOST-authored, FIELD-mirrorable. Fix = the `alarm_sync` shape
  (`[[lesson-votv-world-system-sync-mirror-state-not-verb]]`): host-authoritative array mirror of
  `GObjStack[Index].obj` + BIRTH-GATE the client mirror container (pre-allocate a local GObjStack
  slot + set member `Index>=0` BEFORE `FinishSpawningActor` so `init` skips its self-register +
  `addLoot` — §1.2 GreaterEqual gate; no self-rolled garbage to hide, gate-off-at-birth not
  overwrite-after). **ATOMIC-OVERWRITE INVARIANT (measured, reader census below):** apply the whole
  clear+repopulate in ONE game-thread task (one fn call), never split across frames — that is
  sufficient because ALL readers are GT UFunctions (GT run-to-completion → a reader runs entirely
  before/after the apply, never mid-populate). **NO build-then-swap / generation counter needed.**
  After the overwrite, call `recalculateNames` + `updateVolumesAndMass` + fire `inventoryUpdated`
  so the parallel `nameData`/volume and any open UI refresh (the notify-free re-applier).
- **TAKE** (discrete player click → `getObject`→`takeObj`) — MEASURED-NEGATIVE for field
  neutralization (propInventory field census: no disable flag `takeObj` checks; `takeObj@0-1568`
  has no entry gate). A take is a one-shot player action, not a continuous generator, so
  `alarm_sync` cannot reach it. Dupe-prevention = **host-authoritative take, GATE-BEFORE** (never
  correct-after: an optimistically-spawned item lands in the backpack and `player_inventory_sync`,
  a SEPARATE lane, can propagate it before a deny arrives = cross-lane leak). The transfer verbs are
  `EX_Local*` → pre-hoc unhookable (§1.3), BUT the take is AUTHORED one layer up at the UMG **Button
  `OnClicked` delegate** (`BndEvt__Button_103_...OnButtonClickedEvent` → ubergraph → `CallFunc_getObject`,
  `uicomp_playerInvContainerSlot`) — a seam that fires BEFORE the verb. On a CLIENT: intercept the
  click → suppress the local `getObject` → send a request → host adjudicates against its LIVE
  GObjStack (single game thread serializes concurrent requests; the 2nd finds the record gone → DENY)
  → the item materializes FROM the host. Loser: nothing materializes (no spawn, no yank, no flicker).
- **Addressing**: the client names the record via the host-published canonical list (a host-minted
  id or the full `Fstruct_save`, which `takeObj`'s `Output` param also returns); the host validates
  against its live list (stale → deny). Index is PER-PEER append-only (§1.2/§4) → never the wire
  address. `Fstruct_save` serializer = the live-verified `ue_wrap/inventory.h` record machinery.
- **Exclusions**: `prop_inventoryContainer_player_C` (backpack) class-excluded (player_inventory_sync
  owns it). `prop_openContainer` out (live-actor model, Inc-3). Locked stays static pending
  pryingCrowbar RE. Put-in = LOSS not dupe (separate: same host-request mechanism, no contention
  arbitration; overlap/use paths have no UI seam → post-hoc). `moveIndex` reorder deferred
  (content-addressing makes it non-duping).

**THE ONE OPEN GATE (needs a read-only game-probe):** is the slot Button `OnClicked` delegate
PE-dispatched AND cancelable on the client (can our observer skip the original so `getObject` never
runs)? **PASS** → intercept-at-click-and-reroute (clean, cheap, no substrate). **FAIL** → GNatives
`EX_Local*` verb-interception (`docs/COOP_VM_DISPATCH_PLAN.md`) is the fallback (kerfur-class effort).
Every `/qf` fork pushed AWAY from GNatives; this probe is the last thing that could force it. Probe =
an ini-gated read-only `container_take_probe` (client PRE-observer on the OnClicked delegate; logs
fires? + does skipping suppress the `getObject` spawn). UNWRITTEN.

**Reader census (PERFORMED 2026-07-15, `bp_reflect` disasm):** readers of `GObjStack[Index].obj` =
`recalculateNames` (full-array `Array_Get` loop, re-resolves per element, no cached pointer),
`getObj(index)` (`Array_IsValidIndex` bounds-check → single-element read), `updateVolumesAndMass`
(recompute volume/mass), UI `uicomp_playerInvContainerSlot` (`init`/`changeInfo`/`setAmount` render
COPIED display data, populate on `inventoryUpdated`). ALL are game-thread UFunctions; the client is
ephemeral (no off-GT save reader). → single-GT-task overwrite is atomic w.r.t. every reader (this is
what killed the build-then-swap/generation-counter idea — no reader holds the array across the write).

**Build order (when the probe greenlights):** (1) population mirror + birth-gate + single-task
overwrite (fully specced); (2) the take gate (mechanism per the probe result); (3) put-in (symmetric
request); (4) burp/spill fall out of host-authority. `coop/props/container_contents_sync.{h,cpp}`,
backpack-excluded, + a `COOP_RNG_AUTHORITY.md` addLoot row + a `COOP_SYNC_MAP.md` row on build.

## 4. Caveats / known quirks

- **`GetPlayerPawn(0)` / `getMainPlayer` SP assumptions** in takeObj's spawn-location switch
  [takeObj@224], extract, getObject — an extraction spawns at "the" player; with a puppet in the
  world the pawn-index-0 answer differs per peer.
- **`player.input_run` gates the use-key put-in** [uber@4339] — put-in-by-use only fires while the
  run input is held; any intent hook must capture the same gate result, not re-evaluate it.
- **The fridge flag is injected INTO the stored item's record** [addObject@~2540] — container state
  contaminates item state; a contents mirror must carry the stored STRUCTS, not just class names.
- **GObjStack indexes are append-only forever** (§1.2) — any "rebuild contents" sync design must
  never compact or renumber slots; stored Index values in prop saves depend on them.
- **No container nesting** — addObject rejects prop_container_C (and mailboxes) [addObject census].
- **broken/broken_fire spill uses local RNG** for scatter (RandomPointInBoundingBox/RandomRotator)
  [uber@250-3295] — a break replay will scatter differently per peer; the spawned items themselves
  cross via the normal spawn seams.
- **The overlap swallow is latency-sensitive**: a mirrored physics prop drifting into a container's
  catch volume on ONE peer only will be swallowed there and destroyed on both (destroy crosses) —
  a probable "items vanish near containers" symptom source. [inference from §1.3; unmeasured]
- **prop_openContainer's per-tick itemsInside walk AVs on stale pointers** — the known client crash,
  fixed for garbageContainer only by class-gated PRE-cancel (garbage_sync.cpp; the storage variants
  still walk live).
- Static-dump $type cannot prove PE-visibility (`[[lesson-ex-callmath-invisible-to-processevent]]`);
  the takeObj rows above lean on the dispatch map's LIVE-tagged entries, the addObject row is
  honest-[?].

## 5. Verification — what was proven, how, what remains

Proven (static bytecode, offsets sizer-computed; live tags cited from shipped-code docs):
- The GObjStack contents model end-to-end (init/addObject/takeObj/getData/loadData/addLoot/spill).
- The full verb census + entry points (all ubergraph stubs mapped to entry offsets).
- The UI routes all reduce to getObject/addObject (slot + ui ubergraphs).
- Mod coverage/gaps as cited in §1.6/§1.7 (file:line).
- **Reader census (2026-07-15, `bp_reflect` disasm):** every reader of `GObjStack[Index].obj`
  (`recalculateNames`/`getObj`/`updateVolumesAndMass`/UI-copy) is a game-thread UFunction and none
  caches the array across the write → a single-GT-task overwrite is atomic w.r.t. them (§3).

Open questions — the 2026-07-15 `/qf` design pass (§3) ANSWERED 1-4 (host-owned gate-before-at-
OnClicked; full `Fstruct_save` on the wire; loot via birth-gate + host mirror; put-in seam = the
OnClicked click-intercept). RESIDUAL: the ONE probe-gate in §3 (is OnClicked cancelable?), plus 5-6
below still open:
1. **Contents-list authority**: host-owned GObjStack with intent messages (pattern rows 2+10), or
   per-action delta mirror? Must answer the UI-index race (two peers extracting index 0
   simultaneously) and the append-only-slot constraint (§4).
2. **What the wire carries for a stored item**: full Fstruct_save record (needed — fridge flag,
   uses counters, keys live inside it; ue_wrap/inventory.h already has the POD + serializer) vs
   class-only (loses state).
3. **Mid-session container spawn loot**: suppress client-side init-roll on mirrors (host's rolled
   contents arrive how — a contents snapshot per new container?) and whether addLoot needs a
   COOP_RNG_AUTHORITY row/tier of its own (currently missing).
4. **The put-in intent seam**: no hookable addObject dispatch is proven — candidate seams are the
   playerUsedOn/overlap CALLERS (PE-visibility unmeasured [?]) vs polling the GObjStack slot for
   deltas (the votv-world-system lesson: mirror STATE, not the EX_Local verb).
5. **prop_openContainer**: measure the itemsInside mutator seams (the 2026-05-27 probe was never
   run) and decide mirror-vs-doctrine-local; the storage variants still tick their stale-pointer
   walk on clients.
6. **Locked/unlock**: RE pryingCrowbar_C (no dump) to find whether Locked is ever cleared at
   runtime; until then the axis is static.
