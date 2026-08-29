# Upgrades subsystem — the home for all upgrade RE + coop design

*[↑ docs index](../README.md)*

**Created 2026-07-21.** This folder is the canonical home for the VOTV **upgrade system**: how upgrades
are stored, bought, applied, and displayed natively — and (the open work) how they sync in coop. It
mirrors the `docs/signals/`, `docs/events/`, `docs/items/` pattern: this `README.md` is the hub + the
sync-shape rules, `TRACKER.md` is the per-upgrade status table, and per-upgrade / per-family RE docs land
here as they are excavated.

Upgrades are **NOT a signals concept** — they are their own domain that touches many systems (ATV,
base/console, power). The signal-desk TRACKER already flagged upgrade-sync as **OPEN-3, "its own
workstream"**; this folder is that workstream's home. No coop upgrade sync is built yet.

Evidence tags: **[V]** measured from the CXX SDK dump / pak this session (file cited) · **[?]** not yet
excavated (binary uasset only, or design not done).

---

## 1. What the upgrade system IS (measured 2026-07-21)

Native VOTV has ONE upgrade INTERFACE and several upgrade FAMILIES that implement it, all funnelled
through the SAVE and the STORE.

### The interface — `int_upgrade` [V]
`CXXHeaderDump/int_upgrade.hpp`. Implemented BROADLY (dozens of classes: `actor_save`, `ATV`,
`actorChipPile`, `drone`, `panel_radar`, ...). Methods:
- `getUpgradesList(TArray<FName>& Items)` — enumerate an object's upgrades (FName keys).
- `upgradeTake(FName Item)` — apply/consume one upgrade (seen on `actor_save.hpp:106`).
- `intComs_stuffUpgraded(AmainGamemode_C* GameMode)` — the "an upgrade happened" notify
  (`actor_save.hpp:47`).

### Storage — the SAVE holds upgrades [V]
`actor_save.hpp` implements `getUpgradesList` + `upgradeTake` + `intComs_stuffUpgraded`. So the owned/
purchased upgrade set is **persistent save state** — which makes it host-authoritative territory for
coop (the save is the host's). Data shape: `struct_upgrades.hpp` [?] (fields not yet read).

### The store / shop — bought via the drone [V]
`struct_store.hpp` (`Fstruct_store`), `struct_storeOrder.hpp` (`Fstruct_storeOrder`). The drone is the
buy path: `drone.hpp:198 sendShop(Fstruct_storeOrder order)`, `drone.hpp:45 order` field,
`daynightCycle.hpp:119 "Make Default Order"`. `research/pak_re/.../datatables/list_store.uasset` is the
store catalog [?] (row names binary, not yet extracted).

### The families (implementers of the upgrade slots)
- **ATV physical-module upgrades** [V] — `Aprop_atvUpgrade_C : Aprop_physModule_C`
  (`prop_atvUpgrade.hpp`, size 0x364, base is empty; behavior in the subclasses + `Aprop_physModule_C`).
  **13 named subclasses**: `aircontrol, alternator, belt, bigLights, bumper, container, floaties, fly,
  guns, map, overchargedEngine, radio, solar`. These are physical props ATTACHED to the ATV.
  `enum_physicalModules` [V/?] = 34 slots (`enum_physicalModules_enums.hpp`) but the CXX dump exports
  them UNNAMED (`NewEnumerator0..33`) — the real display names live in the uasset enum (needs a uasset
  string tool / IDA to name).
- **Object / base upgrades** [V] — `uicomp_objectUpgradeSlot.hpp`, `ui_objectUpgrades.hpp`,
  `prop_transformerUpgrade.hpp` (a power-transformer upgrade), `initialServerUpgradeSpawn` (pak:
  `objects/misc/initialServerUpgradeSpawn.uasset`). These upgrade base/world objects.
- **Console / desk upgrades** [V] — `uicomp_upgradeSlot.hpp`; the log-observed `upgrade_autopolarity`
  rides `uicomp_upgradeSlot_C`. These modify workstation/base systems (autopolarity, etc.) — the ones
  most likely to change SHARED world behavior in coop.

### The UI [V]
`uicomp_upgradeSlot.hpp`, `uicomp_objectUpgradeSlot.hpp`, `ui_objectUpgrades.hpp`, `ui_laptop.hpp` (the
laptop shows upgrades). `panel_radar.hpp:30 upgrades` is a `TArray<TEnumAsByte<enum_physicalModules>>`.

---

## 2. Source inventory (where to RE each piece)
- **CXX SDK dump** (our standalone SDK): `Game_0.9.0n_HOST/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`
  — `int_upgrade.hpp`, `struct_upgrades.hpp`, `struct_store.hpp`, `struct_storeOrder.hpp`,
  `prop_atvUpgrade*.hpp` (13), `prop_transformerUpgrade.hpp`, `uicomp_upgradeSlot.hpp`,
  `uicomp_objectUpgradeSlot.hpp`, `ui_objectUpgrades.hpp`, `enum_physicalModules_enums.hpp`.
- **Extracted pak** (the datatable/enum ground truth, binary — needs a uasset reader/IDA for names):
  `research/pak_re/extracted/VotV/Content/main/` — `datatables/list_store`, `enums/enum_atvUpgrades`,
  `interfaces/int_upgrade`, `structs/struct_store`, `structs/struct_storeOrder`,
  `objects/misc/initialServerUpgradeSpawn`, `objects/prop_atvUpgrade`.
- **Bytecode** (BP graphs) via the reflection dumper when the effect logic of a specific upgrade is needed.

---

## 3. Why upgrades matter for coop (the OPEN design question)
Upgrades split by AUTHORITY along the same line as everything else (principle 6 / the syncer model):
- **Save-persistent + shared-world upgrades** (console/base/autopolarity, transformer, server upgrades)
  are HOST-authoritative — they live in the save the host owns and change SHARED world behavior. A client
  must not self-apply; the host applies + broadcasts, like every other host-authored world state.
- **Per-player / per-vehicle upgrades** (ATV physical modules) are trickier: the ATV is a shared world
  entity (one vehicle), so its attached modules are shared-world too, but the *effect* (fly, guns) is
  felt by whoever drives. Likely host-authoritative on the module SET (what's attached), with the ATV's
  existing sync carrying the physical presence.
- **The store/order** (buying) is an INTENT → host validates the purchase (research points / cost) →
  host applies + broadcasts the resulting upgrade. Never client-authoritative (it spends a shared
  resource).

None of this is built. It is **OPEN-3** in `docs/signals/TRACKER.md` and a scope item in
`docs/COOP_SCOPE.md`. When designed, follow the syncer model (`docs/COOP_SYNCER_MODEL.md`) + the
mid-activity-join rule (principle 8: a peer joining mid-upgrade / mid-purchase has a defined answer).

---

## 4. Reading order
1. This README (the map).
2. `TRACKER.md` (per-upgrade / per-family status).
3. **`SIGNAL_UPGRADES.md`** (RE'd 2026-07-21) — the signal/console upgrades (the coop-relevant subset):
   the 18-level `Fstruct_upgrades` storage, the 20 laptop-shop slots, the effect fields, and the
   host-authoritative mirror design. Read this before any workstation upgrade-sync work.
4. Per-family RE docs for ATV / base (added as excavated).
5. Cross-refs: `docs/signals/TRACKER.md` OPEN-3, `docs/COOP_SCOPE.md`, `docs/COOP_RNG_AUTHORITY.md`
   (if an upgrade gates RNG), `docs/COOP_SYNCER_MODEL.md` (the authority model), `docs/COOP_SYNC_MAP.md`.

## 5. NEXT (the RE work this folder will hold)

**2026-08-29 — the ATV family is DONE. Its RE lives in `docs/vehicles/ATV.md` §3-§5** (one doc, because
the ATV modules are inseparable from the vehicle that derives them). Three of the four items below were
closed by it:

- [V] **`enum_physicalModules` named** — 34 values, decoded from the uasset `DisplayNameMap`
  (`ATV.md` §3.1). The ATV set is ids **{8..19} ∪ {33}** = 13 modules. **`enum_atvUpgrades` is an EMPTY
  asset** (only `_MAX`) — a dead enum; do not build against it.
- [V] **`list_store` rows named** — 473 rows read; the ATV block is 11 buyable upgrades + `atvwheel` +
  `atvcarbattery`, all subcategory "Vehicle", prices in `ATV.md` §3.2. **`guns` and `fly` have no shop
  row**, and **no row sells the ATV itself**.
- [V] **The ATV family's APPLY path is RE'd** (`ATV.md` §4): `modules[]` is the sole truth, every
  `has*` bool and every visual/physical effect is derived by the parameterless `updUpgrades()`, install
  is `playerUsedOn` + a held `prop_atvUpgrade_C` (consumed), removal is `takeOffUpgrade` (re-spawns the
  prop into the player's hands). Storage is the ATV's own `getData` **bytes[0]** slot — **disjoint from
  the save's `Fstruct_upgrades`**, which is the signal/console family.
- [V] **Dispatch verdict for the ATV family: the trigger is INVISIBLE** (`playerUsedOn` is
  `EX_LocalVirtualFunction`), so it is an act-as-host INTENT lane, not a hook lane (`ATV.md` §7).

Still open:
- [?] Read `struct_upgrades` / `struct_store` / `struct_storeOrder` field layouts (the **signal**
  family's storage; `SIGNAL_UPGRADES.md` has the 18-int shape but not the store structs).
- [?] RE the **base / console** families' APPLY paths (`upgradeTake` → what it mutates).
- [?] Design the coop upgrade-sync for the non-ATV families (OPEN-3) per the syncer model, with the
  mid-join answer. (The ATV family's design is `research/findings/vehicles/votv-ATV-full-sync-DESIGN-2026-08-29.md`.)
