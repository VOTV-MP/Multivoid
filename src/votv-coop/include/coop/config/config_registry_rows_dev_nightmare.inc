// coop/config/config_registry_rows_dev_nightmare.inc -- the [dev] rows of the nightmare lane: its drill, its staging and
// its red arms. Included by config_registry_rows_dev.inc after the disc rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(nightmare_drill, "nightmare_drill", "dev", "off", "off|1|sleeper|dreamer", "VOTVCOOP_RUN_NIGHTMARE",
    "Drill, the nightmare lane; set on BOTH peers, with the peers' folder VOTVCOOP_CAN_DIR and needs left alone (a "
    "needs refill writes sleep to 100, which is the game's own wake). 1 -- phase A: both peers go to bed, the client's "
    "bed is given a certain nightmare, and the client dreams while the host sleeps on and sees the client's copy hidden "
    "and its state dreaming; the client leaves the dream when the host has seen it. Phase B: the host dreams while the "
    "client is awake and sees the host's copy hidden. The host prints 'rig: VERDICT nightmare PASS|FAIL|INVALID' and "
    "'nightmare: DONE host'; each peer prints 'nightmare: ARM <phase> <role> PASS|FAIL' for its own check and "
    "'nightmare: DONE <role>'. Red arms: the client's nightmare_roll_off (no dream comes: FAIL) and the host's "
    "nightmare_hide_off (the dreamer's copy stays shown: FAIL). It MUTATES the world (dev writes: the nightmare rule on, "
    "the host's world at dilation 1, needs). sleeper and dreamer stage a person's test, each peer alone with no "
    "handshake: sleeper goes to bed and is held asleep for good, dreamer goes to bed with a certain nightmare once the "
    "host has ended the world's first event.")
CFG_STRING(nightmare_dream, "nightmare_dream", "dev", "", "VOTVCOOP_NIGHTMARE_DREAM", false,
    "Staging of the nightmare drill's dreamer: the name of a dream class (for example dream_burger_C) the dreamer starts "
    "itself, through the game's own wake and dream, 20 real seconds into its sleep, instead of waiting for the roll. "
    "Empty = the roll.")
CFG_FLAG(nightmare_roll_off, "nightmare_roll_off", "dev", false, "VOTVCOOP_NIGHTMARE_ROLL_OFF",
    "Developer red arm of the nightmare drill: never set. A client makes no nightmare roll of its own, as before the "
    "roll was the mod's, so a client never dreams.")
CFG_FLAG(nightmare_hide_off, "nightmare_hide_off", "dev", false, "VOTVCOOP_NIGHTMARE_HIDE_OFF",
    "Developer red arm of the nightmare drill: never set. The copy of a player in a nightmare is not hidden on this "
    "machine, so it stays standing where the dream's pose stream puts it.")
