// coop/config/config_registry_rows_dev_world_sound.inc -- the [dev] rows of the world-sound lane: its drill and its red
// arms. Included by config_registry_rows_dev.inc after the woodchipper rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(world_sound_earth_off, "world_sound_earth_off", "dev", false, "VOTVCOOP_WORLD_SOUND_EARTH_OFF",
    "Developer red arm of the world sound drill: never set. The host's hook on the sky's two sounds sends no cue, so "
    "a client plays neither.")
CFG_FLAG(world_sound_faucet_off, "world_sound_faucet_off", "dev", false, "VOTVCOOP_WORLD_SOUND_FAUCET_OFF",
    "Developer red arm of the world sound drill: never set. A peer that takes a faucet's change here plays no click "
    "for it.")
CFG_FLAG(alien_join_off, "alien_join_off", "dev", false, "VOTVCOOP_ALIEN_JOIN_OFF",
    "Developer red arm of the world sound drill: never set. The host does not tell a joiner at its ready edge that the "
    "alien sounds' noisers are out, so a rejoined client has none around it.")
CFG_ENUM(world_sound_drill, "world_sound_drill", "dev", "off", "off|1|join", "VOTVCOOP_WORLD_SOUND_DRILL",
    "Drill, the world-sound lane; set on BOTH peers, and VOTVCOOP_CAN_DIR on both. 1 -- a faucet each peer spawns under "
    "one key is pressed by the client and by the host: the OTHER peer must play its click; the host runs the sky's tp: "
    "the client must receive and play both of its sounds; the host fires the alien sounds: each peer must have three "
    "noisers around it and hear their noises. The host prints 'rig: VERDICT world-sound PASS|FAIL|INVALID' and "
    "'world-sound: DONE on host'. join -- with the rig's --rejoin: the host fires the alien sounds, the client leaves, "
    "and the rejoined client (second life: VOTVCOOP_WORLD_SOUND_DRILL_REJOIN=1) must have three noisers within its "
    "join: 'rig: VERDICT world-sound-join PASS|FAIL|INVALID' (INVALID when the host's noisers are gone at the ready "
    "edge) and 'world-sound: JOIN DONE on host'. Red arms: world_sound_earth_off on the host and "
    "world_sound_faucet_off on the receiving peer (1), alien_join_off on the host (join). It MUTATES the world. Run it "
    "on a fresh host world.")
