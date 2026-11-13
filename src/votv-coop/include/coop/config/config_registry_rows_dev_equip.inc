// coop/config/config_registry_rows_dev_equip.inc -- the [dev] rows of the equipment drill (its arm and its red). Included
// by config_registry_rows_dev.inc after the disc rows. The entry forms are config_registry_rows.inc's.

CFG_STRING(equip_drill, "equip_drill", "dev", "", "VOTVCOOP_EQUIP_DRILL", false,
    "Drill (BOTH peers): every empty equipment slot takes an item. run -- once this peer's world is settled it offers "
    "wearables through the game's own AddEquipment while a slot stands empty; each peer says `rig: VERDICT "
    "equip-host|equip-client`, PASS when every slot took an item, FAIL when the game answered \"Not enough space\" with "
    "a slot empty, INVALID when the list ran out of acceptable wearables first.")
CFG_FLAG(equip_blank_zeroed, "equip_blank_zeroed", "dev", false, "VOTVCOOP_EQUIP_BLANK_ZEROED",
    "Developer: a joiner's blank equipment slots are written zeroed, not as the game's own empty (the equip drill's red).")
