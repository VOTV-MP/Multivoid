// coop/config/config_registry_rows_dev_hand_record.inc -- the [dev] rows of the held item's record drill and its red arms
// (coop/dev/hand_record_drill): the drill's mode, and the three knobs that take a piece of the lane away. Included by
// config_registry_rows_dev.inc after the prop-properties rows, so each row keeps its place in every expansion of the
// list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(hand_record_drill, "hand_record_drill", "dev", "off", "off|can|hand|joiner|forge|hostile|keyless|oversize|bounds|nonfinite|box|display",
    "VOTVCOOP_HAND_RECORD_DRILL",
    "Drill (BOTH peers; the host says the verdict `rig: VERDICT hand-record`): a held item's own record reaches the "
    "other players' mirrors of that hand. can -- the client opens a can and eats from it, drops it and eats from the "
    "dropped can: the host's copy carries each state at the client's mark; then it picks the can up and drops it again, and both "
    "copies must carry the state it was dropped with (INVALID when no new actor was born under the known key). hand -- the client holds a can and opens it "
    "and eats from it twice, then empties its hand: the host's mirror of that hand shows each state and the host clears "
    "its record. joiner -- the host holds an opened can before the client joins: the client's mirror of the host's hand "
    "shows it at the join's snapshot, and the client's copy of the record is cleared when the host empties its hand. "
    "forge -- the client sends a record whose class its hand does not hold: the host refuses it. hostile -- the client "
    "sends a record of its held can that names a class resolving to no class: the host refuses it and stores none (FAIL if it is stored). "
    "keyless, oversize, bounds, nonfinite -- the same arm for a record without its key, over the lane's 4 KB ceiling, with a string "
    "past the field bounds, with a NaN. box -- the client holds an order box, whose record names its items' classes: the host must store it. "
    "display -- the host holds a floppy disc, whose loadData turns physics on; the client's mirror of it, read one drill "
    "tick after the record is written into it, must neither simulate physics nor collide (the red hand_display_redo_off on the client).")
CFG_FLAG(hand_record_relay_off, "hand_record_relay_off", "dev", false, "VOTVCOOP_HAND_RECORD_RELAY_OFF",
    "Developer red arm (the host) of the joiner drill: the host stores each hand's record and sends no peer anything, "
    "the join snapshot included, so the joiner's mirror of the host's hand stays at the class defaults (FAIL at the "
    "client's mark).")
CFG_FLAG(hand_record_clear_off, "hand_record_clear_off", "dev", false, "VOTVCOOP_HAND_RECORD_CLEAR_OFF",
    "Developer red arm (the host) of the joiner drill: the host clears a hand's record but sends no empty record, so "
    "the client keeps the old item's record after the host's hand empties (FAIL at the client's second mark).")
CFG_FLAG(can_adopt_off, "can_adopt_off", "dev", false, "VOTVCOOP_CAN_ADOPT_OFF",
    "Developer red arm of the can drill: a new actor under a known can Key is compared with what this peer knew, as "
    "before the adoption, so the host's rebirth, born with defaults (measured), publishes them as its change. Read by the can arm's second birth "
    "(FAIL when the host publishes for that Key between the pickup and the rest); the relay and clear reds are read by the joiner arm.")
CFG_FLAG(hand_display_redo_off, "hand_display_redo_off", "dev", false, "VOTVCOOP_HAND_DISPLAY_REDO_OFF",
    "Developer red arm (the peer that holds the mirror) of the display arm: a record written into a hand mirror is not "
    "followed by the display setup, so the item's own loadData leaves its physics on (FAIL at the display read).")
