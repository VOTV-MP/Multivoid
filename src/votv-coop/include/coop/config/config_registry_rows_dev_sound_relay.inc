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
