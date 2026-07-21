# Upgrades subsystem — the home for all upgrade RE + coop design

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
- [?] Extract the real names of `enum_physicalModules` (34) + `enum_atvUpgrades` + the `list_store` rows
  (uasset string tool / IDA) — the CXX dump left them `NewEnumerator*`.
- [?] Read `struct_upgrades` / `struct_store` / `struct_storeOrder` field layouts.
- [?] RE each family's APPLY path (`upgradeTake` → what it mutates) — decides host-auth vs per-player.
- [?] Design the coop upgrade-sync (OPEN-3) per the syncer model, with the mid-join answer.
