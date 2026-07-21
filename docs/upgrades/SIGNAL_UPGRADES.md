# Signal-workstation upgrades — RE + coop authority (2026-07-21)

**Durable RE.** The signal/console upgrades ONLY (the coop-relevant subset the user scoped). ATV physical
modules + base/object upgrades are catalogued in `TRACKER.md`, not here. All facts measured from the CXX
SDK dump (`Game_0.9.0n_HOST/.../CXXHeaderDump/`) 2026-07-21; **[V]** = measured, **[?]** = inferred / needs
bytecode.

## TL;DR (the coop conclusion)
The signal-upgrade LEVELS are **one persistent struct of 18 `int32` levels** — `Fstruct_upgrades`
(`struct_upgrades.hpp`, size 0x48), saved in the game. They PARAMETRIZE the console's download / ping /
coordinate / comp / radar / detector simulations. Those sims are already **host-authored** in coop
(DeskSimPose, per `docs/signals/TRACKER.md`), so the upgrade levels are the **missing host-authoritative
INPUT**: if the host and a client hold different levels, download speed, cooldown, ping, triangulation,
detector quality, comp processing, spectrogram length all DIVERGE. **The fix is small and clean: mirror
`Fstruct_upgrades` host->client (on change + at join).** One struct, host-authoritative, never
client-applied (buying is an intent the host validates — it spends research points, a shared resource).

---

## 1. The authoritative storage — `Fstruct_upgrades` [V]
`struct_upgrades.hpp`, size 0x48, 18 `int32` levels (the trailing `_NN_HEX` are BP property GUID/order
suffixes, not part of the name):

| # | Field | Drives (console effect field, `analogDScreenTest.hpp`) | Domain |
|---|---|---|---|
| 0 | `upg_downloadSpd` | `DL_downloadSpeed` / `DL_downloadMultiplier` [?] | download rate |
| 1 | `upg_downloadFiltSize` | the FF/PF filter window size [?] | download tuning |
| 2 | `upg_serverStability` | server/base stability [?] | base |
| 3 | `upg_processLvl` | `comp_maxLevel` [?] | comp/decode |
| 4 | `upg_processSpeed` | `comp_speed` / `comp_processMultiplier` [?] | comp/decode |
| 5 | `upg_coordDrift` | coord sensor drift [?] | coordinate |
| 6 | `upg_coordPingSpeed` | `coord_ping_*` speed [?] | ping |
| 7 | `upg_coordMovementSpeed` | cursor/coordinate move speed [?] | coordinate |
| 8 | `upg_coordRadarSpeed` | `radar_spdLevel` / `radar_radar_spd` [?] | radar |
| 9 | `upg_coordCooldown` | `coord_cooldown` / `coord_maxCooldown` [?] | cooldown |
| 10 | `upg_scanner` | sensor/scan [?] | sensor |
| 11 | `upg_scannerFr` | sensor frequency [?] | sensor |
| 12 | `upg_detecQual` | `upgradeDetectorQuality` / `resMultQuality` [?] | detector |
| 13 | `upg_radarHist` | radar history length [?] | radar |
| 14 | `upg_radar_speed` | `radar_spdLevel` [?] | radar |
| 15 | `upg_transofrmer` (sic) | power transformer [?] | power |
| 16 | `upg_compTime` | comp processing time [?] | comp/decode |
| 17 | `upg_triangleProb` | triangulation success probability [?] | coordinate |

The precise field-by-field wiring (which upgrade -> which console float, and the multiplier curve) needs a
bytecode read of `analogDScreenTest.upgraded_pcUpgrades()` / `mainGamemode.upgraded()` — the DOMAIN column
is measured-from-name, the effect column is inferred.

## 2. The shop UI — `ui_laptop.hpp` upgrade slots [V]
The laptop shop exposes one `Uuicomp_upgradeSlot_C*` field per purchasable upgrade. Signal-relevant slots
(20; `upgrade_atvsolar` is ATV, listed for completeness):
`upgrade_autopolarity, upgrade_autoprocess, upgrade_breakerSpeed, upgrade_computerLvl, upgrade_computerSPeed,
upgrade_cooldown, upgrade_coordinateSpeed, upgrade_deteQ, upgrade_downloadSPeed, upgrade_downloadSPeed_1,
upgrade_pingSpeed, upgrade_pingStrength, upgrade_radarHist, upgrade_radarSpd, upgrade_sensor,
upgrade_sensor_fr, upgrade_sensorDrift, upgrade_sensorSpeed, upgrade_spect` (+ `upgrade_atvsolar`).

Note the UI-slot set (20) and the `Fstruct_upgrades` set (18) are NOT 1:1: `autopolarity` / `autoprocess`
read like AUTO toggles (a maxLvl=1 on/off) rather than numeric levels, and some slots map to one struct
field. The slot->field correspondence is [?] until the bytecode is read; the AUTHORITY story does not
depend on it (both live in the save, both host-owned).

### `uicomp_upgradeSlot_C` — a slot IS leveled [V]
`uicomp_upgradeSlot.hpp`: `int32 Index`, `FText Name/Description`, `int32 price`, `int32 maxLvl`,
`int32 levelAccumulation`, `bool Module`. Buttons `button_upgUp` / `button_upgDown` -> buy/refund a level;
`getPrice()`/`updPrice()` scale price by level. So each upgrade is bought in LEVELS up to `maxLvl`.

## 3. The interface + apply path [V]
`int_upgrade` (`int_upgrade.hpp`): `getUpgradesList(TArray<FName>)`, `upgradeTake(FName)`,
`intComs_stuffUpgraded(AmainGamemode_C*)`. On the console (`analogDScreenTest.hpp`): `upgradeTake(FName)`
(`:526`), `radarUpgrade(GameMode)` (`:544`), `upgraded_pcUpgrades()` (`:603`), and the level float
`upgradeDetectorQuality` (`:335`). On the gamemode (`mainGamemode.hpp`): `upgraded()` (`:455`),
`intComs_stuffUpgraded` (`:478`). The save (`actor_save.hpp`) holds the list (`getUpgradesList`/
`upgradeTake` `:106`) -> **persistent, host-owned.**

Flow (measured shape): buy in the laptop shop UI -> `upgradeTake(FName)` mutates `Fstruct_upgrades` on the
save + spends research points -> `intComs_stuffUpgraded` / `upgraded()` / `upgraded_pcUpgrades()` re-derive
the console's effect floats from the new levels.

## 4. Coop authority design (OPEN-3 -> a concrete plan)
- **`Fstruct_upgrades` is HOST-authoritative and MIRRORED.** The host owns the save; broadcast the whole
  18-int struct on any change + in the join snapshot. Clients apply it and re-run `upgraded_pcUpgrades()`
  (or write the derived console floats) so their local sims + shop UI show the host's levels.
- **Buying is an INTENT.** A client's `button_upgUp` press must NOT self-apply (it spends research points,
  a shared host-owned resource). Client -> host "buy upgrade X" intent -> host validates points/maxLvl ->
  host applies + broadcasts the new `Fstruct_upgrades`. Mirrors the store/order path (drone `sendShop`).
- **Mid-join (principle 8):** a peer joining mid-purchase gets the current `Fstruct_upgrades` in the join
  snapshot; an in-flight buy intent is host-serialized like any other.
- **Why it matters (ties to take-4):** R2/R3/R13/R17 and the whole download/ping/coordinate sim are
  host-authored (DeskSimPose). The upgrade levels are the PARAMETERS of those sims. Without mirroring
  `Fstruct_upgrades`, a client that bought (or didn't) a `downloadSpd`/`cooldown`/`pingSpeed` level would
  compute a different sim than the host even after the sim itself is synced. This is the input side of the
  same host-authority story.

## 5. NEXT (bytecode-level, when building the sync)
- [?] Read `analogDScreenTest.upgraded_pcUpgrades()` + `mainGamemode.upgraded()` bytecode: exact
  upgrade-level -> console-float wiring + the multiplier curves (fills the effect column above).
- [?] Confirm `Fstruct_upgrades` is where the SAVE serializes levels (vs a parallel TMap on the gamemode);
  `mainGamemode` has `TMap<FName,float>` for other stores (fishingItems/xmasStore) — check upgrades aren't
  ALSO in such a map.
- [?] Resolve the 20-slot (ui) vs 18-field (struct) correspondence + which slots are auto-toggles.
- Then design the wire lane (one reliable `UpgradeState` struct mirror + a buy-intent kind) per the
  syncer model.
