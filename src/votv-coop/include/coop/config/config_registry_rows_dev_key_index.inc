// coop/config/config_registry_rows_dev_key_index.inc -- the [dev] rows of the prop key index's probes (the Init seam
// census, the shadow-index parity pass and its red arm), moved in their order. Included by
// config_registry_rows_dev.inc where they stood, so each row keeps its place in every expansion of the list. The
// entry forms are config_registry_rows.inc's.

CFG_FLAG(init_seam_probe, "init_seam_probe", "dev", false, "VOTVCOOP_INIT_SEAM_PROBE",
    "Probe: a script-gate watch on every Init body of a keyed prop, counted by the body's owning "
    "class, its caller, whether it runs inside another Init of the same object and whether the key "
    "is still unset; logged every 30 s and at each world change. Holds the gate; read-only.")
CFG_FLAG(key_parity_probe, "key_parity_probe", "dev", false, "VOTVCOOP_KEY_PARITY_PROBE",
    "Probe: a shadow prop key index fed only by script-gate POST watches on init, setKey, getKey and "
    "loadData, and the key index the mod keeps, each compared every 2 s against a walk of the live keyed "
    "props; logs the props each lacks or holds under another key, with examples per world. Holds the "
    "gate; read-only but for the slot serials it assigns.")
CFG_FLAG(key_index_forget_one, "key_index_forget_one", "dev", false, "VOTVCOOP_KEY_INDEX_FORGET_ONE",
    "Red arm of the prop key index's parity pass: once a session, after the seed, one filed prop of the "
    "current world is dropped from the index. The pass must name it as a filing the feeds missed and file "
    "it again within a full pass (about 20 s).")
