# Upgrades — living status tracker

**Update every session.** One row per upgrade / family: native behavior, the coop authority shape, and
the honest status. Seeded 2026-07-21 from the CXX SDK dump; see `README.md` for the source inventory.

STATUS legend: **VERIFIED** (hands-on / matching live log) · **AS-BUILT** (shipped coop sync, not
hands-on) · **DESIGN** (coop sync designed, not built) · **RE'd** (native behavior measured, no coop
design yet) · **OPEN** (not yet touched). Coop-sync-wise the WHOLE subsystem is **OPEN** today (it is
signals-TRACKER OPEN-3); the RE column tracks how much native truth we hold.

Evidence: **[V]** measured from the SDK this session · **[?]** binary/uasset only, names/fields not read.

---

## Families

| Family | Native mechanism | Native RE | Coop authority (proposed) | Coop status |
|---|---|---|---|---|
| **int_upgrade interface** | `getUpgradesList(TArray<FName>)` + `upgradeTake(FName)` + `intComs_stuffUpgraded(GM)` — implemented broadly | [V] signatures; `upgradeTake` mutation path [?] | — (the seam every family flows through) | OPEN |
| **Save storage** | `actor_save` holds the owned upgrade set (getUpgradesList/upgradeTake) | [V] present; `struct_upgrades` fields [?] | HOST-authoritative (the save is the host's) | OPEN |
| **Store / shop** | buy via drone `sendShop(Fstruct_storeOrder)`; catalog `list_store` datatable | [V] the buy seam; `struct_store`/`struct_storeOrder` fields [?]; row names [?] | INTENT → host validates cost/points → host applies + broadcasts | OPEN |
| **ATV physical modules** | 13 `prop_atvUpgrade_*_C : Aprop_physModule_C`; the ATV's `modules[]` (`TArray<enum_physicalModules>`) is the sole truth, `updUpgrades()` derives everything | **RE'd 2026-08-29** [V] — enum named (ids {8..19}u{33}), shop rows + prices read, install/remove/derive bytecode mapped, save slot located, dispatch measured INVISIBLE | ACT-AS-HOST intent (arbiter owns `modules[]`); mirror = write array + call `updUpgrades()` | **OPEN** (RE `docs/vehicles/ATV.md` §3-§5, §7) |
| **Object / base upgrades** | `uicomp_objectUpgradeSlot` / `ui_objectUpgrades`; `prop_transformerUpgrade`; `initialServerUpgradeSpawn` | [V] classes; effects [?] | HOST-authoritative (base/world objects) | OPEN |
| **Console / desk (SIGNAL) upgrades** | `uicomp_upgradeSlot_C` leveled slots; levels persist in `Fstruct_upgrades` (18 int32); parametrize download/ping/coord/comp/radar/detector sims | **RE'd** [V] — 18-level struct + 20 ui slots + effect fields mapped; slot->field wiring [?] (bytecode) | HOST-authoritative: mirror the whole `Fstruct_upgrades` host->client; buying = client intent -> host validates points -> broadcasts | **OPEN** (design in `SIGNAL_UPGRADES.md`) |

---

## ATV physical-module catalog (13) — **superseded by `docs/vehicles/ATV.md` §3.1-§3.2, §4.4**

Full table there: module id, display name, prop class, shop row, price, and **every effect
`updUpgrades()` applies**. Summary: ids **8-19 and 33**; 11 are buyable (150-1500 credits, subcategory
"Vehicle"); **`guns` and `fly` have no shop row**, and `hasGuns` is read nowhere in `ATV.json`.
The measured effects are: `bigLights` (light cone/intensity/colour + halves the light battery drain),
`bumper` (frontal damage multiplier via `getBumperMult`), `solar` (recharge), `belt` (mesh + a kerfur
branch), `container` (the `atv_inventoryContainer|<key>` container + collision), `floaties`
(`floater` + `metal_barrel` phys-material), `map` (`digitalMap` child actor), `radio`
(`prop_radio_atv` + pause/close on removal), `aircontrol` (mid-air steering), `fly` (flight branch),
`overchargedEngine` (**speed 1500->2000, turbo 2250->5000**, exhaust FX, **doubles battery drain**),
`alternator` (**zeroes the seat+engine battery draw**), `guns` (**no measured effect in the ATV**).

---

## CHANGELOG
- **2026-07-21** — folder + README + TRACKER created. Native RE surface mapped from the CXX SDK dump:
  the `int_upgrade` interface, save storage (`actor_save` upgradeTake/getUpgradesList), the store
  (`struct_storeOrder`, drone `sendShop`), 3 upgrade families (ATV 13 modules / object-base / console),
  `enum_physicalModules` (34 slots, unnamed in the dump). NO coop sync exists (OPEN-3). Names of the
  enums + `list_store` rows + the `struct_*` field layouts are the next excavation (binary uasset).
- **2026-08-29** — **the ATV family is RE COMPLETE.** `enum_physicalModules` decoded from the uasset
  `DisplayNameMap` (34 names; ATV = ids 8-19 + 33); `enum_atvUpgrades` measured **EMPTY** (dead asset);
  `list_store` read (473 rows -> 11 ATV upgrade rows + wheel + battery, prices captured; **no ATV
  vehicle row exists**); install / remove / derive bytecode disassembled; the save slot located
  (`getData` bytes[0]); `processKeys()` identified as the single re-derive seam; `playerUsedOn`
  measured `EX_LocalVirtualFunction` (**INVISIBLE**). Written up in `docs/vehicles/ATV.md`.
