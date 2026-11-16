// coop/config/config_registry_rows_dev_hunt.inc -- the [dev] rows of the hunters' lane: the switch that leaves the host out of their
// candidates and the drill that spawns each one. Included by config_registry_rows_dev.inc after the disc rows, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(hunt_skip_host, "hunt_skip_host", "dev", false, "VOTVCOOP_HUNT_SKIP_HOST",
    "Developer drill switch (HOST): never set in play. The host's hunters (coop/creatures/hunt_target) and the Grays' punch "
    "(coop/creatures/hunt_gray) leave the host out of the players they may go for, so that with every player at one spot a "
    "hunter must pick a client; the host stays the fallback when no client is in the creature's range. Without it a drill's "
    "two players stand at one spot and the host, the earlier slot, wins every tie.")
CFG_ENUM(hunt_drill, "hunt_drill", "dev", "off", "off|krampus|follower|gray|figura", "VOTVCOOP_HUNT_DRILL",
    "Drill (HOST; the client only joins), the hunters' target. The host first goes to open ground outside the base, where "
    "the client joins it (join_at_host). Once the client's puppet is live the host spawns the arm's "
    "creature 600 cm from itself on the line from the puppet (the Grays: a grayEventController_C, whose own despawn it "
    "calls three seconds later, as a gray's overlap or arrival does) and samples it once a second for 40 s (15 s for the "
    "Grays): the slot it hunts "
    "(coop/creatures/hunt_target), the slot served_player holds, both distances, the reads of the player answered. It "
    "prints one '[HUNT] VERDICT <arm> PASS|FAIL' line and 'hunt_drill: DONE <arm>'. With hunt_skip_host=1 the creature must "
    "be recorded as hunting slot 1 and its Blueprint's reads must be answered with the puppet; without it the first pick "
    "must be the nearer of the two by the distances measured at the spawn. The creatures that walk to their prey must also "
    "close on the puppet, and the Grays arm must deliver a hit to the client, whose own log shows it applied. figura (BOTH "
    "peers) is not a hunter: the host makes a Shadow Figura and the client says the verdict, PASS when it is shown a copy "
    "of it (coop/creatures/owner_entity_sync). It spawns creatures.")
