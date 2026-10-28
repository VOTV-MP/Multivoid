// coop/config/config_registry_rows_dev_coffee.inc -- the [dev] rows of the coffee lane: its drill and its red arm.
// Included by config_registry_rows_dev.inc after the cord rows, so each row keeps its place in every expansion of
// the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(coffee_drill, "coffee_drill", "dev", false, "VOTVCOOP_COFFEE_DRILL",
    "Drill, the coffee lane; set on BOTH peers. The host spawns a coffee maker by a powered wall socket and plugs "
    "it in; the client's player walks to the vending machine and the maker by the director and presses them, "
    "while the host drops a cup and a bag onto the maker, and the client lifts its own copy of a cup over its "
    "copy of the maker and carries a cup to it. Five arms, each judged by both peers' copies; lines tagged "
    "[COFFEE]. The client prints 'arms 1-5 PASS' and, rejoined, 'client DONE -- PASS'. Run it on a fresh host "
    "world.")
CFG_ENUM(coffee_red, "coffee_red", "dev", "off", "off|no_intent|no_publish|no_cancel", "VOTVCOOP_COFFEE_RED",
    "Developer red arm of the coffee drill: never set. no_intent (the client): its press of a coffee device runs "
    "its own body, so arm 1 must FAIL on the host's points left unchanged. no_publish (the host): a brew does "
    "not publish the maker's record, so arm 3 must FAIL on the client's flags left at 1. no_cancel (the client): "
    "its own run of the maker's hit handler is not cancelled, so arm 4 must FAIL on the client's maker taking "
    "the cup.")
