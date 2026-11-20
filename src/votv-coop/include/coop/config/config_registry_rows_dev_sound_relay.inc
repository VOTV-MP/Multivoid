// coop/config/config_registry_rows_dev_sound_relay.inc -- the [dev] rows of the sound relay's drill: its red arms. Included
// by config_registry_rows_dev.inc after the world-sound rows, so each row keeps its place in every expansion of the list.
// The entry forms are config_registry_rows.inc's.

CFG_FLAG(sound_send_off, "sound_send_off", "dev", false, "VOTVCOOP_SOUND_SEND_OFF",
    "Developer red arm of the sound drill: never set. The relay's hook plays a catalogued sound here as the game does and "
    "sends nothing for it, so no other peer hears it.")
CFG_FLAG(sound_judge_off, "sound_judge_off", "dev", false, "VOTVCOOP_SOUND_JUDGE_OFF",
    "Developer red arm of the sound drill: never set. The host does not judge a client's sound (its caller, its budget, "
    "its place): it plays what it is asked and sends it on.")
CFG_FLAG(sound_index_check_off, "sound_index_check_off", "dev", false, "VOTVCOOP_SOUND_INDEX_CHECK_OFF",
    "Developer red arm of the sound drill: never set. A peer that receives a sound whose index is past its table takes "
    "the index modulo the table's size and plays that sound, instead of refusing it.")
CFG_ENUM(sound_drill, "sound_drill", "dev", "off", "off|hit|knock|far|hostworld|index", "VOTVCOOP_SOUND_DRILL",
    "Drill, the sound relay; set on ALL THREE peers (one host, two clients, so the rig's --clients 2) with "
    "VOTVCOOP_CAN_DIR on all. hit -- client 1 takes a crowbar and swings at the floor: the host and client 2 must play its "
    "sound. knock -- the host fires the vent knocker and runs its knock: both clients must play it. far, hostworld, index "
    "-- client 1 sends the host a sound that is out of reach of its body, a church bell, a sound past the table: the host "
    "must refuse it for that reason and no peer may play it. The host says 'rig: VERDICT sound-<arm> PASS'; any peer may "
    "say FAIL or INVALID. Red arms: sound_send_off (hit, knock), sound_judge_off (far, hostworld), sound_index_check_off "
    "(index).")
