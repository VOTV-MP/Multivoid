// coop/config/config_registry_rows_dev_ambient.inc -- the [dev] rows of the ambient creatures' and fixtures' drills: one red
// arm each. Included by config_registry_rows_dev.inc after its last row, as _dev_vulkan.inc is, so each row keeps its place
// in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(ambient_creature_red, "ambient_creature_red", "dev", "off", "off|no_rows|no_tree_state|no_deer_end|no_deer_floor", "VOTVCOOP_AMBIENT_CREATURE_RED",
    "Developer red arm of the ambient creature drill: never set; set on BOTH peers. no_rows: a client's gate leaves the "
    "deer, tree, hexahive and beehive spawner rows out, so the drill's refusal check must FAIL on the counters left at "
    "0. no_tree_state: the host sends no tree state, so the client's tree must FAIL on its size and landing. "
    "no_deer_end: the host never ends its second deer, so the client's mirror of it must FAIL to end. "
    "no_deer_floor (the client): a deer mirror's floor is never refreshed, so its lean must FAIL against the host's.")
CFG_ENUM(ambient_field_red, "ambient_field_red", "dev", "off", "off|no_rows|no_bind|no_census", "VOTVCOOP_AMBIENT_FIELD_RED",
    "Developer red arm of the ambient field drill: never set; set on BOTH peers. no_rows: a client's gate leaves the "
    "hexahive and beehive spawner rows out, so the refusal check must FAIL. no_bind (the client): a loaded branch is "
    "not bound by its key, so the join must FAIL on a copy beside it. no_census (the client): the census-end ends "
    "nothing, so a loaded branch the host lacks must FAIL to go.")
CFG_ENUM(sus_hole_red, "sus_hole_red", "dev", "off", "off|no_intent|out_of_ring_accepted", "VOTVCOOP_SUS_HOLE_RED",
    "Developer red arm of the sus hole drill: never set; set on BOTH peers. no_intent (the client): the hole its own "
    "spawner rolled is destroyed and no intent is sent, so the host must FAIL on a hole never made. "
    "out_of_ring_accepted (the host): the ring's inner bound is not judged, so the drill's near intent must FAIL on "
    "a refusal that never came.")
