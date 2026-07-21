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
| **ATV physical modules** | 13 `prop_atvUpgrade_*_C : Aprop_physModule_C`, attached to the ATV; `enum_physicalModules` (34 slots) | [V] class list; enum names [?] (`NewEnumerator*`) | shared-world SET (host-auth) + ATV's existing sync carries presence | OPEN |
| **Object / base upgrades** | `uicomp_objectUpgradeSlot` / `ui_objectUpgrades`; `prop_transformerUpgrade`; `initialServerUpgradeSpawn` | [V] classes; effects [?] | HOST-authoritative (base/world objects) | OPEN |
| **Console / desk (SIGNAL) upgrades** | `uicomp_upgradeSlot_C` leveled slots; levels persist in `Fstruct_upgrades` (18 int32); parametrize download/ping/coord/comp/radar/detector sims | **RE'd** [V] — 18-level struct + 20 ui slots + effect fields mapped; slot->field wiring [?] (bytecode) | HOST-authoritative: mirror the whole `Fstruct_upgrades` host->client; buying = client intent -> host validates points -> broadcasts | **OPEN** (design in `SIGNAL_UPGRADES.md`) |

---

## ATV physical-module catalog (13, measured [V] from `prop_atvUpgrade_*.hpp`)

| Upgrade | Class | Effect (native) | Notes |
|---|---|---|---|
| aircontrol | `prop_atvUpgrade_aircontrol_C` | air control while airborne | [?] effect unread |
| alternator | `prop_atvUpgrade_alternator_C` | power/charge | [?] |
| belt | `prop_atvUpgrade_belt_C` | drivetrain | [?] |
| bigLights | `prop_atvUpgrade_bigLights_C` | lighting | [?] |
| bumper | `prop_atvUpgrade_bumper_C` | collision/protection | [?] |
| container | `prop_atvUpgrade_container_C` | cargo storage | [?] (an inventory container -> coop container-contents question, cf. take-4 R11) |
| floaties | `prop_atvUpgrade_floaties_C` | water buoyancy | [?] |
| fly | `prop_atvUpgrade_fly_C` | flight | [?] |
| guns | `prop_atvUpgrade_guns_C` | weapons | [?] (spawns projectiles -> host-auth) |
| map | `prop_atvUpgrade_map_C` | map/nav | [?] |
| overchargedEngine | `prop_atvUpgrade_overchargedEngine_C` | engine power | [?] |
| radio | `prop_atvUpgrade_radio_C` | audio | [?] |
| solar | `prop_atvUpgrade_solar_C` | solar charging | [?] |

---

## CHANGELOG
- **2026-07-21** — folder + README + TRACKER created. Native RE surface mapped from the CXX SDK dump:
  the `int_upgrade` interface, save storage (`actor_save` upgradeTake/getUpgradesList), the store
  (`struct_storeOrder`, drone `sendShop`), 3 upgrade families (ATV 13 modules / object-base / console),
  `enum_physicalModules` (34 slots, unnamed in the dump). NO coop sync exists (OPEN-3). Names of the
  enums + `list_store` rows + the `struct_*` field layouts are the next excavation (binary uasset).
