// coop/config/config_registry_rows_dev_basedmove.inc -- the [dev] rows of the based-movement drill: its arm and its
// collision knob. Included by config_registry_rows_dev.inc after the flood rows. The entry forms are
// config_registry_rows.inc's.

CFG_FLAG(basedmove_drill, "basedmove_drill", "dev", false, "VOTVCOOP_BASEDMOVE_DRILL",
    "Drill (BOTH peers, default off; mp.py smoke --host-fresh on a fresh game): what a client riding the base's cargo lift "
    "does to the host's lift. The client is walked onto the platform by the director, the host presses the lift (the game's "
    "own call) and, every frame until the lift arrives or the Blueprint reverses it, logs the platform and its `attach` point, "
    "the gap between them (the Blueprint reverses the lift at 10 cm), `dir`, `ignoreCheck`, the client's puppet and its last "
    "pose, and the puppet capsule's overlap; the client logs its own height, its copy of the platform and the movement base its "
    "engine reads. It MEASURES: the host's line is 'rig: VERDICT basedmove-<ride|ride_nocoll> MEASURED -- flips, largest "
    "gap, frames, the client's base readings', and the rig's --done-marker is that line; a precondition missing says INVALID.")
CFG_FLAG(basedmove_puppet_nocoll, "basedmove_puppet_nocoll", "dev", false, "VOTVCOOP_BASEDMOVE_PUPPET_NOCOLL",
    "Drill arm ride_nocoll (the host; with basedmove_drill): the client's puppet has its collision switched off on the host "
    "before the lift is pressed (the call a seated body takes), so the run says whether the puppet's capsule is what holds the "
    "platform back. Leave off for the ride arm.")
