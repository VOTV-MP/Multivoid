# container (prop_container family + propInventory brain + prop_openContainer) — the storage-container system   (STATUS: DESIGN)

RE pass 2026-07-11 (static bytecode, offsets computed with the validated `research/pak_re/size_ubergraph.py`
sizer; call censuses resolve StackNode import indices per `[[lesson-bp-json-grep-resolve-imports]]`).
Sources: `research/bp_reflection/{propInventory,prop_container,prop_inventoryContainer_player,
uicomp_playerInvContainerSlot,uicomp_playerInvSlot,ui_playerInventory,prop}.json`, CXXHeaderDump,
and the shipped mod code (cited file:line). Sync NOT designed — §3 is TBD.

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
actor whose propInventory rides the same GObjStack machinery; its `getData`/`loadData`/`ignoreSave`/
`skipPreDelete` overrides are EMPTY bodies [prop_inventoryContainer_player.json — all four have no
statements], i.e. it never persists as a world prop. How GObjStack[playerContainer.Index] relates to
the separately-saved `saveSlot.inventoryData @0x2E0` (which our player_inventory_sync reads/writes)
— copy-on-save? two views of one thing? — is **[?] unmeasured** (mainGamemode-side; the mod's
inventory blob path works against inventoryData/equipment/hold and is live-verified, see
ue_wrap/inventory.h).

### 1.5 prop_openContainer — the OTHER contents model (live actors)

Open-top containers (fridge shelves, plates, bowls, scrapbox, wastebasket, garbage container) keep
their contents as **live actors glued inside**: `itemsInside : TArray<AActor*> @0x378`, maintained
by `ReceiveTick` → `checkPickup` / `doStuff` / `glueInside` walking the array every tick
[prop_openContainer.hpp; research/findings/votv-garbage-trash-interaction-RE-2026-05-27.md §(a)].
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

## 3. Coop design (primary, 2026-07-11 — DESIGN, not built)

**One mechanism: a host-canonical CONTENTS-LIST state mirror, key/eid-addressed, driven by a
1 Hz per-container count-diff.** Chosen over verb intents because the mutating verbs are
EX-internal/unproven-hookable (§1.3 addObject [?]; `[[lesson-votv-world-system-sync-mirror-state-not-verb]]`),
and over any Index-based addressing because GObjStack Index values are PER-PEER (append-only,
minted locally — a mid-session container gets different Index values on each peer; §1.2/§4).

- **Detection**: both peers walk `gamemode.allContainers` (the game's own registry, §1.3
  ReceiveBeginPlay) at 1 Hz reading ONLY `GObjStack[propInventory.Index].obj.Num` per container
  (raw memory via reflection offsets, no UFunction dispatch — serverbox-poll cost class). A count
  change on a container = a contents mutation on THIS peer (take-out, put-in, addLoot, burp).
  Count-only deliberately misses `moveIndex` reorders (rare; Inc-2 adds an order hash).
- **Authority flow (the email-delete/serverbox shape)**: on a local count change, a CLIENT sends
  the container's FULL record list to the host as a post-hoc action report (it cannot be a
  request — the native verb already ran); the HOST applies it to its own slot and rebroadcasts
  the canonical list; a HOST local change broadcasts directly. Receivers overwrite their slot
  wholesale (idempotent; no index races on the wire — last host word wins) and refresh the
  actor's parallel accounting (`updateVolumesAndMass`/`recalculateNames` — dispatchability [?],
  else recompute in C++; build-time item).
- **Wire payload**: the full `Fstruct_save` records (open question 2 answered: class-only loses
  the fridge flag/uses/keys that live INSIDE the record, §4). Serializer = the live-verified
  `ue_wrap/inventory.h` record machinery (player inventory items are the same record shape, §1.2).
  Addressed by container prop key/eid, chunked via blob_chunks if a list exceeds one frame.
- **Loot divergence closes for free**: a mid-session client container (mirror or local) rolls its
  own loot into its own slot, then the host's canonical broadcast (triggered by the HOST copy's
  own 0→k count change) overwrites it — no init/addLoot hook needed at all (mirror-step-3 by
  state, not source-kill). Add the missing `docs/COOP_RNG_AUTHORITY.md` row for addLoot, closed
  by this lane.
- **Exclusions**: `prop_inventoryContainer_player_C` (the backpack) is class-excluded — it is
  per-player state owned by player_inventory_sync; mirroring it here would fight that lane.
  `prop_openContainer` stays out (different model, live actors already ride prop lanes; its
  itemsInside probe = Inc-3). Locked stays static (§1.3) pending pryingCrowbar RE.
- **Races accepted in Inc-1** (documented, native-parity): two peers extracting the same record
  inside one convergence window can double-spawn the item (native SP has no arbitration either);
  the GHOST/LOSS divergences — the user-visible bugs — converge ≤1 s. Host adjudication
  hardening (reject impossible reports) = Inc-2.
- **Late-join**: already correct at join via the v56 save blob (§1.2); this lane keeps it
  correct afterward. New mid-session containers: the actor crosses via the prop lanes, contents
  via the first count-diff broadcast.

**Increments**: Inc-1 = `coop/props/container_contents_sync.{h,cpp}` (count-diff + key-addressed
full-list mirror + backpack exclusion + RNG tracker row + SYNC_MAP row). Inc-2 = order hash /
UI-open refresh nudge / host adjudication / burp+spill edge audit. Inc-3 = prop_openContainer
probe + doctrine call.

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

Open questions a design pass must answer (no designs here):
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
