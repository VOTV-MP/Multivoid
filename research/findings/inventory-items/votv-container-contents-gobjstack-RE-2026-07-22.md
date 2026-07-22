# RE: the delivery container + `propInventory_C` contents storage (`saveSlot.GObjStack`)

**Date:** 2026-07-22 · **Context:** take-4 bug **R11** (drone-delivery container full on host, `0.0` on
client). Durable RE (`*-RE-*`), not a design. Method: `kismet-analyzer to-json` over the cooked
`.uasset`/`.uexp` under `research/pak_re/extracted/` + the take-4 host/client logs
(`Game_0.9.0n_{HOST,CLIENT_1}/.../multivoid.log`, 2026-07-21 14:07).

Confidence tags: `[V]` = measured this session (citation inline) · `[RD]` = RE-derived ·
`[?]` = unverified.

---

## 0. Scope correction (settled before this dig)

`prop_dronesack_C` **does not exist at runtime** on either peer `[V]` — `grep -ci sack` = 1 per log and
that single hit is the `hasSack@0x0501` offset-resolve line, not an entity. Its `ignoreSave` returns
`EX_True` `[V]` (cooked `objects/prop_dronesack.uasset`, export `ignoreSave` =
`EX_LetBool(EX_LocalOutVariable "ignoreSave", EX_True) -> EX_Return`), i.e. it is save-excluded by
design. **This RE therefore covers `prop_inventoryContainer_drone_C` + `propInventory_C` only.**

---

## 1. The entity — `prop_inventoryContainer_drone_C` `[V]`

Client take-4 log:

```
13:16:26 remote_prop::OnSpawn: cls='prop_inventoryContainer_drone_C' key='drone_InventoryContainer'
         name='coffee_m' loc=(76373.0, 218086.0, 33125.0) physFlags=0x08
13:16:26 remote_prop::OnSpawn: key 'drone_InventoryContainer' resolves to live actor 000001336AF81750
         -- already aligned (d=0.00cm), skipping teleport
13:16:26 sync::CreateOrAdoptPropMirror: eid=6941 bound to actor=000001336AF81750
         key='drone_InventoryContainer' cls='prop_inventoryContainer_drone_C' ownerSlot=0
```

- **Persistent, not delivery-born.** Only 4 container log lines exist on the client, all at join; the
  OnSpawn *adopts* an actor the client's own world-load had already spawned ("resolves to live actor
  … already aligned d=0.00cm"). No container/sack spawn between 13:17 and 14:07 on either peer `[V]`.
- **Stable named key** `drone_InventoryContainer` — a fixed name, not a GUID; identity is already
  solved for the ACTOR.
- Siblings in the same family: `prop_inventoryContainer_player`, `prop_inventoryContainer_atv` `[V]`
  (`research/pak_re/extracted/VotV/Content/objects/`).

Delivery timing, same log `[V]`: `13:35:08 drone: FX mirror -- dust ON` (take-off) →
`13:35:33 drone: FX mirror -- arrival cue + signal light ON (canTakeOff rising edge)` (arrival).
**The contents are authored ~19 minutes AFTER the container's birth-sync reached the client.**

---

## 2. Where the contents actually live — NOT on the container `[V]`

`propInventory_C` class variables (cooked `objects/propInventory.uasset`, class export
`LoadedProperties`) — **there is no contents array among them**:

| Property | Type | Note |
|---|---|---|
| `index` | Int | **the slot into `saveSlot.GObjStack`** (see §3) |
| `maxVol` / `currVol` | Float | `currVol` is the displayed volume — **the 686 vs 0.0 the user photographed** |
| `obj` | Object | single object ref, not the contents list |
| `mass` / `ownMass` | Float | |
| `owner` | Object | the owning prop |
| `lootTableEntry` | Name | |
| `randomLoot` | Array | loot-table entries, not live contents |
| `infinite`, `player` | Bool | |
| `volumeMult`, `customVolume` | Float | |
| `filter` | Array | accepted-class filter |
| `maxLoot`, `NewVar_0` | Int | |
| `gam` | Object | gamemode cache |

Both `addObject` and `takeObj` bytecode reference the chain `gamemode` → `saveSlot` → **`GObjStack`**
(10 field-path hits each) plus the struct field `obj_11_89CC26B1…` (5 each) `[V]`.

**=> A container's contents are a slice of one GLOBAL save-side array, addressed by an integer
`index`. The container actor holds only the index and the cached volume.**

---

## 3. The storage format `[V]`

```
saveSlot_C::GObjStack : TArray<struct_mObject>        // FArrayProperty, Inner FStructProperty
                                                      // Struct = import -335 = struct_mObject
                                                      // ElementSize 16
struct_mObject { obj : TArray<struct_save> }          // exactly ONE field (16 B = one TArray)
struct_save {
    class      : ClassProperty      // what to respawn
    transform  : StructProperty
    key        : NameProperty       // the prop's save Key
    bools[] floats[] ints[] strings[] signals[] classes[]
    vectors[] rotators[] transforms[] bytes[] names[]  // per-class payload arrays
}
```

Sources: `main/saveSlot.uasset` property table (`GObjStack` FArrayProperty decl, `Struct: -335`,
import 335 = `struct_mObject`); `main/structs/struct_mObject.uasset` (single `obj` ArrayProperty of
StructProperty `struct_save`); `main/structs/struct_save.uasset` (`LoadedProperties`) `[V]`.

**Two consequences worth stating plainly:**
1. Each inventory entry is a **full generic save-record**, not an item id — including `signals[]`,
   which is exactly the payload R14/15/16 fights over for `prop_drive_C`. Container contents and
   loose-prop content are the SAME serialization currency.
2. `TArray<struct_save>` stride: `ElementSize 16` for `struct_mObject` is the outer TArray only;
   any raw read of `struct_save` elements must re-derive the stride — see
   `[[feedback-tarray-stride-aligned-not-raw-size]]` (16-aligned, not raw size).

---

## 4. The verb surface `[V]`

`propInventory_C` bytecode-bearing functions (name / bytecode size):

| Verb | Size | Role |
|---|---|---|
| `init` | 679 | calls `addLoot` |
| **`addObject`** | 3860 | **the write** — appends into `GObjStack[index].obj`; calls `getData` |
| **`takeObj`** | 1569 | **the read+remove** — `Array_IsValidIndex` → `BeginDeferredActorSpawnFromClass` → `loadData` → `FinishSpawningActor` |
| `addLoot` | 2655 | rolls the loot table, calls `addObject` x4 |
| `moveIndex` | 2434 | re-slots entries |
| `removeVol` | 39 | volume bookkeeping |
| `getObj` | 365 | read one entry |
| `checkObjectsVolume` | 520 | calls `takeObj` |
| `isObjectInInventory` / `isClassInInventory` | 798 / 1265 | queries |
| `recalculateNames` | 778 | display refresh |

`takeObj` confirms the contents are **data, not hidden live actors**: it spawns the extracted actor
deferred and restores it via `loadData` — matching `prop_container_extract.cpp`'s existing comment
that the extracted prop spawns *inside* `takeObj` before `loadData` restores its Key.

---

## 5. Dispatch visibility — the seam `[V]` (this is the load-bearing result)

Call-target resolution over the cooked bytecode (`StackNode`/`VirtualFunctionName` resolved through
the import/export tables):

| Caller | Opcode | Target |
|---|---|---|
| `drone::compileOrder` | **`EX_LocalVirtualFunction`** x2 | `addObject` |
| `propInventory::addLoot` | **`EX_LocalVirtualFunction`** x4 | `addObject` |
| `propInventory::init` | `EX_LocalVirtualFunction` | `addLoot` |
| `propInventory::addObject` | `EX_LocalVirtualFunction` | `getData` |
| `propInventory::checkObjectsVolume` | `EX_LocalVirtualFunction` | `takeObj` |

**Every content-mutating call measured here is `EX_Local*` — BP-internal dispatch that neither the
ProcessEvent detour nor the `UFunction::Func` patch can see** (`docs/COOP_DISPATCH_VISIBILITY.md`;
the documented `init()`-is-BP-internal trap).

- The substrate for exactly this already ships: `ue_wrap/core/vm_dispatch` — the **GNatives[0x45]
  swap for `EX_LocalVirtualFunction`**, name-keyed per-verb callbacks, already consumed by
  `drive_sync`, `meadow_db_sync` and `kerfur_form_assembler` `[V]`.
- **Caveat, honestly flagged `[?]`:** `prop_container_extract.cpp` registers PRE/POST *ProcessEvent*
  observers on `propInventory_C::takeObj` and is described as working, which implies `takeObj` has at
  least one PE-visible caller (a player-use path) that this census did not cover — I only enumerated
  callers inside `drone.uasset` and `propInventory.uasset`. The take-4 logs show `takeObj POST` = 0
  hits on both peers `[V]`, i.e. no extraction occurred in that session, so they cannot confirm or
  refute it. **A caller census across `mainPlayer` / the interaction path is owed before any design
  leans on `takeObj` being PE-visible.**

---

## 6. What actually diverges host vs client

| Layer | State | Synced today? |
|---|---|---|
| The container ACTOR | `prop_inventoryContainer_drone_C`, key `drone_InventoryContainer` | **Yes** — prop element, eid 6941, `ownerSlot=0` `[V]` |
| The drone's motion/FX | pose + FX | **Yes** — `drone_sync` host-auth stream `[V]` |
| The drone's BRAIN | `ReceiveTick` | **Parked on client by design** — `drone_sync.cpp:1-6` "the client suppresses the drone's own ReceiveTick (so it is purely a mirror)" `[V]` |
| Container LID | `swinger` | **Yes** — `g_containerAdapter` / `ReliableKind::ContainerState`, 56 keyed lids `[V]` |
| **`saveSlot.GObjStack`** | the contents themselves | **NO LANE, either direction** `[V]` |
| `propInventory.currVol` | the displayed volume | derived from the above; diverges with it |

**Root, stated as an invariant rather than a site list:** every container in the game reads its
contents from **one global per-peer array `saveSlot.GObjStack`**, which the client receives once in
the join save snapshot and never again. The host mutates it (`addObject` at delivery, `addLoot`,
`takeObj`) behind `EX_Local*` dispatch that no shipped observer watches. R11 is one visible symptom of
that single missing lane — not a drone bug.

Authority is **already correct** (host authors, client parked); this is purely a missing-lane defect.

---

## 7. Open questions the lane design must answer (NOT answered here)

1. **Is `propInventory.index` stable cross-peer? PARTIALLY ANSWERED — and my first answer was too
   strong, corrected here.** `propInventory_C::init` statements 11-15, read in full `[V]`:
   ```
   11  LetBool  tmp = GreaterEqual_IntInt(self.index, <IntConst>)   // index >= 0 ?
   12  JumpIfNot tmp                                                 // if already valid -> SKIP append
   14  Let ret  = Array_Add(gamemode.saveSlot.GObjStack, Temp_struct_Variable)
   15  Let self.index = ret
   ```
   CDO `Default__propInventory_C.index = -1` `[V]`. So: a container whose `index` is still `-1`
   **appends** a fresh empty `struct_mObject` and takes the append position; a container that already
   has `index >= 0` **REUSES** it. My earlier flat claim ("append-order counter, not stable") was
   derived from the `Array_Add` alone and is **retracted as stated** — there is a reuse branch.
   **RESOLVED `[V]`: `index` DOES persist, via the owning prop's save record.**
   `prop_container::getData` references `index` 2x and `ints` 8x; `prop_container::loadData`
   references `index` 3x and `ints` 2x. So the container's `GObjStack` slot number is written into
   `struct_save.ints[]` and restored on load — which is exactly what the `index >= 0` guard in `init`
   exists to honour (restored container: reuse; fresh container: append).
   **But this is MOOT for the lane** (see §10.0): under element-key addressing the index never crosses
   the wire, so its stability is unobservable to the design. It matters only for §10.3 (nesting).
2. Is the correct wire unit a `GObjStack`-delta (global, index-addressed) or a per-container
   contents-state keyed on the already-stable element key/eid? Q1 makes index-addressing unusable as
   an identity, but the *global-array* framing may still be the right transport shape `[?]`.
3. `takeObj`'s PE-visibility (see §5 caveat) — decides whether extraction needs a `vm_dispatch` verb
   too, or already has an observable seam `[?]`.
4. How the client's `GObjStack` is seeded today (save_transfer snapshot) and what the mid-activity
   join answer is (principle 8) `[?]`.
5. `struct_save` wire codec: `signals[]` already has `signal_wire`; the other 11 arrays do not `[?]`.

---

## 8. Blast radius — `GObjStack` is NOT the drone's array `[V]`

Cooked assets referencing `GObjStack` (20):

```
main/lib · main/mainGameInstance · main/mainGamemode · main/mainPlayer · main/saveSlot
objects/propInventory · objects/prop_container · objects/prop_backpack · objects/prop_backpack1
objects/prop_garbageBin · objects/prop_garbageBin2 · objects/prop_mailbox · objects/prop_digcam
objects/prop_funGun · objects/droneSellLocation · objects/subAreaTransition · objects/theEvil
umg/interfaces/ui_playerInventory · umg/interfaces/ui_saveSlots · umg/interfaces/ui_UI
```

**`mainPlayer` and `ui_playerInventory` are in that list** — the player's own inventory is backed by
the same global array as world containers. **Authority is therefore ASYMMETRIC and a single
host-authored write over the array would stomp each client's personal inventory:** world containers
are host-owned; a player's personal inventory is owned by that player. Any lane must carry that split
at the ROOT, not as a filter. Compare `[[lesson-held-collected-prop-is-per-player-inventory-not-a-world-actor]]`
(learned in R14/15/16) — this is the storage mechanism behind that lesson.

## 9. Two seam findings that change the shipped picture

**(a) No PE-visible caller of `takeObj` was found anywhere `[V]`.** Full caller census across every
cooked asset that mentions `takeObj` (`main/lib`, `objects/propInventory`, `objects/prop_container`,
`objects/prop_inventoryContainer_player`):

| Caller | Opcode | Target |
|---|---|---|
| `lib::findInventoryObject` | `EX_LocalVirtualFunction` | `takeObj` |
| `prop_container::extract` | `EX_LocalVirtualFunction` | `takeObj` |
| `prop_container::getObject` | `EX_LocalVirtualFunction` | `takeObj` |
| `propInventory::checkObjectsVolume` | `EX_LocalVirtualFunction` | `takeObj` |
| `prop_container::putObjectIn_overlap` | `EX_LocalVirtualFunction` | `addObject` |
| `prop_container::ExecuteUbergraph_prop_container` | `EX_LocalVirtualFunction` | `addObject` |

**Every call measured IN THESE FOUR ASSETS is `EX_Local*`** — but the "possibly dead seam" reading is
**RETRACTED (2026-07-22, reconciled against `docs/COOP_DISPATCH_VISIBILITY.md:88`)**. That map row
already records, `[RD]`, that this is "the reason **takeObj-POST missed R-drops while the UI route
fired**" — i.e. a **PE-visible caller does exist** (the UI/interaction route), and my census simply did
not cover the asset that holds it (I enumerated only `drone`, `propInventory`, `prop_container`,
`prop_inventoryContainer_player`).

So the true picture is a **PARTIAL** seam, which was already known: `prop_container_extract.cpp`'s
ProcessEvent observers fire on the UI extraction route and miss the `EX_Local*` routes (`lib::
findInventoryObject`, `prop_container::{extract,getObject}`, `checkObjectsVolume`). take-4's
`takeObj POST` = 0 hits is explained by no extraction having occurred that session, not by a dead seam.
**Do not "retire a dead seam" on this section** — it is not dead
(`[[feedback-verify-before-retiring-a-fix]]`). The open question is narrower: which extraction routes
the existing seam misses, and whether the `takeObj` half needs a `vm_dispatch` verb for coverage. That
belongs to the deferred `takeObj` work, not to R11.

**(b) `GObjStack` appears NOWHERE in our source `[V]`** (`grep -rn GObjStack src/votv-coop` = 0 hits).
The client's copy arrives only inside the opaque `save_transfer` blob at join. So the mid-join answer
(principle 8) may already exist as blob custody — what is missing is a **delta anchor from the
snapshot**, not a new snapshot lane. Compare `[[lesson-opaque-blob-custody-donor-dictates-the-remainder]]`
and `[[lesson-anchor-the-accumulator-dont-stream-it]]`.

## 10. The codec — measured shape (this dissolves the "full vs narrow" question)

**10.0 Addressing.** The wire unit is **contents-state keyed on the stable element key**
(`drone_InventoryContainer` / eid 6941 — §1), never on `index`. Sender ships `(element key,
contents)`; each receiver resolves its OWN container actor → its OWN `propInventory` → its OWN local
`index` → writes its own slot. **`index` therefore never crosses the wire**, and its cross-peer
stability is unobservable to the lane (which is why §7 Q1 is moot for R11). The global,
index-addressed `GObjStack`-delta shape is REJECTED: `moveIndex` re-slots indices, and it would drag
in the player-inventory half (§8) that R11 does not reproduce.

**10.1 The payload shape is UNIFORM `[V]`.** Every one of the 11 non-signal payload arrays is a
single-field jagged array of primitives:

| `struct_save` field | element struct | that struct's single field |
|---|---|---|
| `bools` | `struct_mBool` | `bools : Array` |
| `floats` | `struct_mFloat` | `floats : Array` |
| `ints` | `struct_mInt` | `ints : Array` |
| `strings` | `struct_mString` | `strings : Array` |
| `classes` | `struct_mClass` | `ints : Array` |
| `vectors` | `struct_mVector` | `vectors : Array` |
| `rotators` | `struct_mRotator` | `vectors : Array` |
| `transforms` | `struct_mTransform` | `vectors : Array` |
| `bytes` | `struct_mByte` | `vectors : Array` |
| `names` | `struct_mName` | `vectors : Array` |
| `signals` | `struct_signalDataDynamic` | **the one complex member** |

(The inner field NAMES are inconsistent — `struct_mClass.ints`, `struct_mByte.vectors` — the BP author
reused labels; the SHAPE is uniform: one array field each, i.e. `TArray<TArray<primitive>>`.)

**=> There is no "full record vs narrow subset" choice and no 11 codecs to write.** A single generic
serializer over `{class, transform, key} + 11 jagged primitive arrays` is class-agnostic and complete;
`signals[]` is the only member needing a real codec and it **already has a proven author** (`signal_wire`,
reused per `[[lesson-reuse-proven-author-not-raw-reimpl]]`). The rule-of-three concern dissolves because
there is no per-class code.

**10.2 What is actually in the take-4 delivery: NOT MEASURABLE from surviving artifacts `[?]`.** The
host save `s_1234.sav` is dated 2026-07-19 17:46, i.e. it predates take-4 (2026-07-21) and never
captured the delivery. What IS measured statically is the AUTHORING path: `addObject` calls the item's
own `getData`, so each entry's payload is **whatever that item class chooses to serialize** — open-ended
and per-class. This is a second, independent reason the codec must be generic rather than hand-picked
per observed item.

**10.3 NESTING — the real constraint, and it bites `[V]`.** `struct_save` does **not** contain itself:
none of its 14 fields is a `struct_save` array (§3, element structs above). A container **inside** a
container is therefore represented by INDIRECTION — its record's `ints[]` carries its own `GObjStack`
index (§7 Q1), and its contents live in a **separate `GObjStack` slot**.

**Consequence: shipping a nested container's `struct_save` ships a POINTER into the SENDER's
`GObjStack`, which is meaningless on the receiver.** A flat codec would deliver a backpack/case whose
contents resolve to the receiver's unrelated slot — or to nothing. The take-4 payload reportedly
included a "case", so this is plausibly already in scope rather than hypothetical.

**Is nesting actually reachable on THIS path?** Static evidence says yes, by design:
- `drone.uasset` imports `Default__prop_container_C` / `prop_container_C` (and `prop_toolbox_C`) —
  the drone references container classes `[V]`.
- `compileOrder` has `manyObj : Bool` + `manyObjects : Array` and **two distinct `addObject` call
  sites** (`CallFunc_addObject_return` and `..._1`) `[V]` — the signature of a branch "few items →
  addObject each into the sack" vs "many items → pack into a box, addObject the box".
- `prop_container_orderbox_C` exists in `list_props` `[V]`.
- **NOT nailed `[RD]`:** that the many-objects branch spawns `prop_container_C` *into the delivery
  container*. The inference is strong but the specific spawn was not traced to that class.

**This flips the burden.** Nesting is not an exotic edge on this path — it is how a large order is
apparently packed. So:
- **(b) hard boundary = the risky assumption**, and per §10.3's own logic it does not FIX the nested
  case, it converts it into the identical user-visible bug one level deeper ("case is empty on the
  client"). Acceptable only as an explicitly-labelled scoped defer, never as "fixed".
- **(a) transitive is the correct default**, and it needs explicit BOUNDS, because `GObjStack` is
  index-addressed and **nothing structurally guarantees acyclicity** — a record's `ints[]` index can
  name any slot, including an ancestor. No engine-side depth bound was found `[V]`. The traversal
  therefore requires an explicit **visited-set + depth cap + total-payload cap, enforced on the
  RECEIVER** (a sender-side cap is not a cap —
  `[[lesson-send-side-caps-are-not-caps]]`), so a cyclic or corrupt index cannot hang or unbound the
  apply. Max observed depth is unmeasured; the bound is ours to declare, not to discover.

A live delivery replay on a fresh save would settle the `[RD]` above and is worth doing before build.

## Related

- `research/findings/computers-devices/votv-take4-hands-on-bugs-2026-07-21.md` (R11 row — the
  candidate roots (a)/(b) there are both superseded by this dig)
- `research/findings/computers-devices/votv-drive-disc-content-birth-DESIGN-2026-07-21.md` (R14/15/16;
  same `struct_save` currency, the BIRTH half — R11 is the STEADY-STATE half)
- `docs/COOP_DISPATCH_VISIBILITY.md` · `docs/COOP_VM_DISPATCH_PLAN.md` (the 0x45 substrate)
- `src/votv-coop/src/coop/props/prop_container_extract.cpp` (the existing `takeObj` bracket)
