// coop/dev/dev_lanes.cpp -- see coop/dev/dev_lanes.h. The calls stand in the order the session's pump
// made them.

#include "coop/dev/dev_lanes.h"
#include "coop/dev/director/director.h"  // EndWalks: a drill's walks end with their session

#include "coop/dev/atv_probe.h"
#include "coop/dev/blackout_drill.h"  // [dev] a blackout on both peers, for the lanes' dev probe
#include "coop/dev/client_model_probe.h"  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
#include "coop/dev/container_join_drill.h"  // [dev] an edit no world-ready peer could take reaches the joiner
#include "coop/dev/container_opener_probe.h"  // [dev] the actors far containers open through, on real objects
#include "coop/dev/container_writer_probe.h"  // [dev] a container writer the container lane never saw
#include "coop/dev/container_selftest.h"  // [dev] container-lane e2e circle (organic addLoot)
#include "coop/dev/container_view_drill.h"  // [dev] a container view closes as its opener leaves reach
#include "coop/dev/container_route_census.h"  // [dev] each container verb counted by the route on its stack
#include "coop/dev/container_route_drill.h"  // [dev] the census drill: each route driven once on a client
#include "coop/dev/death_seam_census.h"  // [dev] every element end the death seam announces
#include "coop/dev/damage_probe.h"  // [dev] every player damage verb call, and by whom
#include "coop/dev/delivery_census_probe.h"  // COUNT the delivery-path actors
#include "coop/dev/desk_diag.h"  // [dev] desk/console divergence census
#include "coop/dev/door_drill.h"  // [dev] whether a remote player counts in a door's own sensor
#include "coop/dev/drive_selftest.h"  // [dev] rack-lane e2e circles
#include "coop/dev/rack_birth_drill.h"  // [dev] a rack born holding rows reaches the client, which sends no op
#include "coop/dev/burn_revive_drill.h"  // [dev] a client who dies burning is revived with no fire
#include "coop/dev/drone_call_drill.h"  // [dev] a client's console press flies the host's drone
#include "coop/dev/dronesack_drill.h"  // [dev] a crate dropped onto the drone sack, both peers told
#include "coop/dev/atv_health_drill.h"  // [dev] the ATV lane's table judged on the ATV the host spawns (trap: the world's)
#include "coop/dev/atv_eject_drill.h"  // [dev] a wheel knocked off an ATV is the host's to express, never a client's
#include "coop/dev/atv_spawn_drill.h"  // [dev] a runtime ATV reaches each client once
#include "coop/dev/throw_drill.h"  // [dev] a thrown container keeps its contents on the host
#include "coop/dev/fall_drill.h"  // [dev] a falling host birth lands on the client where it lands on the host
#include "coop/dev/remover_drill.h"  // [dev] the tool gun's remover pressed by a client
#include "coop/dev/prop_act_drill.h"  // [dev] a client's take or use of a shared prop asks the host first
#include "coop/dev/prop_syncer_drill.h"  // [dev] a client syncs a prop its own interaction moves: kick and forge, judged by the host and an observer
#include "coop/dev/sack_drill.h"  // [dev] a client's point sack is paid by the host, once
#include "coop/dev/server_upgrade_drill.h"  // [dev] a client's install and take-out at a server box
#include "coop/dev/server_drill.h"  // [dev] a server box's break is the host's; a client's repair runs there
#include "coop/dev/camera_drill.h"  // [dev] a picked-up camera: does it stick to its wall again, and by whose hand
#include "coop/dev/sleepingbag_drill.h"  // [dev] a sleeping bag unrolls and folds on both peers, one for one
#include "coop/dev/notebook_drill.h"  // [dev] a notebook's page is written at one peer's editor and read on the others
#include "coop/dev/camera_state_drill.h"  // [dev] a security camera's two bits are the host's: the camera drill's acceptance arms
#include "coop/dev/sat_console_drill.h"  // [dev] a client's SAT console commands run on the host
#include "coop/dev/calib_drill.h"  // [dev] a dish's precision is the host's; a player's verb reaches it
#include "coop/dev/desk_crossing_drill.h"  // [dev] the desk's detection needle crosses on the host alone
#include "coop/dev/desk_ping_drill.h"      // [dev] a client's ping is rolled on the host
#include "coop/dev/desk_verb_drill.h"  // [dev] a client presses the desk's save family through its own input
#include "coop/dev/hotbar_icon_probe.h"
#include "coop/dev/load_reroll_watch.h"
#include "coop/dev/radio_tower_probe.h"
#include "coop/dev/drive_drill.h"  // [dev] a drive's row is the host's; a client's eraser press and births reach it
#include "coop/dev/download_drill.h"  // [dev] the download machine's arm and reset are the host's
#include "coop/dev/laptop_drill.h"  // [dev] the laptop's slot is the host's; a file edit lands on its own disc
#include "coop/dev/laptop_power_drill.h"  // [dev] the laptop's power pressed on each peer, its replay silent
#include "coop/dev/laptop_tab_drill.h"  // [dev] everyone sees the tab the PC's user is on, judged on the viewing peer
#include "coop/dev/explosion_drill.h"  // [dev] the explosion lane's drill: dissolve, apply, coast, malformed spawns
#include "coop/dev/octa_drill.h"  // [dev] the Geom Octa lane's drill: the host's births and the Octa's measured liveness
#include "coop/dev/fire_ignite_drill.h"  // [dev] a client's light on a salt rocket is performed by the host and flown
#include "coop/dev/cord_drill.h"  // [dev] cords plugged on both peers, judged by the host's word
#include "coop/dev/late_join_drill.h"  // [dev] the bind wait's tallies per kind, on a joiner once its join is idle
#include "coop/dev/cord_pos_drill.h"  // [dev] a corded maker that binds on a joiner keeps the floor while hidden
#include "coop/dev/coffee_drill.h"  // [dev] the coffee devices pressed on a client and the maker's state, judged by the host's word
#include "coop/dev/grid_drill.h"  // [dev] the power grid: a client's presses and repair, the host's break and lockout
#include "coop/dev/tower_drill.h"  // [dev] the coordinate towers: the host's break, a client's repair, a joiner's copy
#include "coop/dev/drone_probe.h"
#include "coop/dev/end_play_probe.h"  // [dev] every end of play against the K2_DestroyActor seam
#include "coop/dev/puppet_reg_probe.h"  // [dev] a remote player's puppet is registered at its begin-play
#include "coop/dev/pi0_selftest.h"  // [dev] a cleared Player Element keeps its id
#include "coop/dev/pi1_selftest.h"  // [dev] the scope mark, the call site, the const-reference hook and the caller's locals
#include "coop/dev/pi2_selftest.h"  // [dev] the call locals, the puppet-count listener, the puppet-body query
#include "coop/dev/pi3_drill.h"  // [dev] a creature spawned at a still peer's puppet is not served as this machine's player
#include "coop/dev/event_drill.h"  // [dev] the event lanes: a scheduler fire, a dev fire, the join snapshot
#include "coop/dev/command_drill.h"  // [dev] a typed command is run by the host and answered privately
#include "coop/dev/mv_drill.h"  // [dev] the /mv commands end to end against a seeded permission store
#include "coop/dev/stats_probe.h"  // [dev] every player stat read, written and restored in one frame, and the game's status effects added, listed and removed
#include "coop/dev/stat_order_drill.h"  // [dev] the host orders and queries a client's stats and effects and checks every answer
#include "coop/dev/chat_drill.h"  // [dev] the chat checks, judged in memory
#include "coop/dev/drill_chat.h"  // [dev] the drills' queued chat lines, paced to the host's talk rule
#include "coop/net/net_clock.h"
#include "coop/dev/l10n_drill.h"  // [dev] the interface language, judged on each peer
#include "coop/dev/ban_drill.h"  // [dev] the host bans a joined client by the moderation verb
#include "coop/dev/bug_report_drill.h"  // [dev] a bug report is saved and its zip judged in memory
#include "coop/dev/grants_drill.h"  // [dev] this machine's local grants judged against an arm
#include "coop/dev/pause_quit_drill.h"  // [dev] a peer quits through the pause menu's own button
#include "coop/dev/settings_drill.h"  // [dev] the host sets its own rows and the modules show they followed
#include "coop/dev/mannequin_drill.h"  // [dev] the walking mannequin's spawn and baseline drill
#include "coop/dev/fireext_drill.h"  // [dev] a wall-mounted fire extinguisher taken off and carried, watched on both peers
#include "coop/dev/floppy_selftest.h"  // [dev] the disc-into-server media transfer, driven
#include "coop/dev/food_clock_probe.h"  // the food record's arrival catch-up and the two clocks behind it
#include "coop/dev/grime_drill.h"  // [dev] a clean on either peer reaching the other's copy
#include "coop/dev/hand_drop_selftest.h"  // [dev] a prop through a hand and back out, driven
#include "coop/dev/hookdrag_selftest.h"  // [dev] a prop dragged by a hook, driven
#include "coop/dev/inventory_pickup_drill.h"  // dev drill: one client pickup through putObjectInventory2
#include "coop/dev/kerfur_convert_drill.h"  // [dev] a client turns a kerfur off and on beside a disc of its own
#include "coop/dev/kerfur_serve_drill.h"  // [dev] the host runs a client's Omega commands for that client
#include "coop/dev/kerfus_drill.h"  // [dev] a client turns the plain kerfur on and off through the host
#include "coop/dev/kerfus_throw_drill.h"  // [dev] the host throws the Kerfus; the client watches its copy
#include "coop/dev/kerfur_menu_drill.h"  // [dev] a kerfur turned on through its menu event, nested in a watched body
#include "coop/dev/keypad_drill.h"  // [dev] whether a client's typing lands on the host's verdict on both copies
#include "coop/dev/keypad_probe.h"
#include "coop/dev/lid_drill.h"  // [dev] the portable PC's lid, opened on the host and closed on the client
#include "coop/dev/subject_drill.h"  // [dev] a remote insert leaves the local player's grab alone
#include "coop/dev/light_drill.h"  // [dev] whether a client's switch press moves the host's group, and never its own
#include "coop/dev/light_group_census.h"
#include "coop/dev/lightswitch_probe.h"
#include "coop/dev/live_store_readout.h"  // READ-ONLY live personal store observability
#include "coop/dev/lookat_aim_drill.h"  // hold a peer's aim on a resting prop, so the churn probe has a reading
#include "coop/dev/lookat_churn_probe.h"  // how often the interaction UI's look-at set is rebuilt under a held aim
#include "coop/dev/meadow_selftest.h"  // [dev] a row into the host's signal database and out, the client's copy following
#include "coop/dev/midnight_drill.h"  // [dev] the host's midnight on demand, awake or inside the host's own sleep
#include "coop/dev/sleep_drill.h"  // [dev] a host's native sleep and a client's rest at the native rate, measured
#include "coop/dev/badsun_drill.h"  // [dev] a peer's own Bad Sun copy against its own clock, measured
#include "coop/dev/event_relay_drill.h"  // [dev] the host's creature event in a client's count, read through the relay
#include "coop/dev/creature_hit_drill.h"  // [dev] a parked creature's hit delivered to a client, a client's hit on it judged by the host
#include "coop/dev/hostile_drill.h"  // [dev] the receive boundary: a client forges malformed packets, the host judges
#include "coop/dev/native_pile_inert_probe.h"
#include "coop/dev/native_spawn_probe.h"  // [dev] every allowlisted creature a Blueprint graph spawns here, and the client orb question
#include "coop/dev/order_probe.h"  // who writes the order queue, and what the drone delivers, per peer
#include "coop/dev/bind_wait_selftest.h"  // [dev] the bind wait's rules on stated inputs
#include "coop/dev/pile_late_load.h"   // [dev] the late-load seam: one save-loaded clump kept aside until its row defers
#include "coop/dev/pile_defer_drill.h"  // [dev] a joiner's real pile deferrals counted and their outcome checked, the host comparing poses
#include "coop/dev/order_selftest.h"  // exercise host-side order pricing end to end
#include "coop/dev/physmods_drill.h"  // [dev] a module plug and unplug are sent at their verbs, and a rejoin's load sends none
#include "coop/dev/pinecone_probe.h"
#include "coop/dev/host_settle_drill.h"  // [dev] a join in the host's start-up waits for it
#include "coop/dev/prop_birth_key_probe.h"  // the place/birth seam's key timing and drain exits
#include "coop/dev/pry_drill.h"  // [dev] a stuck pryable pried off its wall, watched on both peers
#include "coop/dev/recycled_slot_drill.h"  // [dev] a recycled object slot, staged on the local pawn
#include "coop/dev/rng_roll_census.h"  // the dev roll census
#include "coop/dev/server_breaker_census.h"  // [dev] the server breaker's inputs, every 30 s
#include "coop/dev/rollover_watch.h"  // [dev] the day rollover: its verbs, the pulses' consumers, the day numbers and the hash digest
#include "coop/dev/roster_token_selftest.h"  // [dev] successor-ban drill (moderation token vs a recycled slot)
#include "coop/dev/run_and_wait_selftest.h"  // [dev] the one game-thread wait's five endings, provoked
#include "coop/dev/spawn_match_probe.h"  // the fuzzy-match candidate set and adoption watch
#include "coop/dev/store_table_probe.h"  // which mechanism can read a list_store row
#include "coop/dev/swinger_drill.h"  // [dev] what a dragged hinge door shows on the other peers
#include "coop/dev/toggle_drill.h"  // [dev] whether a toggle device's state crosses both ways
#include "coop/dev/world_first_check.h"  // [dev] client_world_first's verdict
#include "coop/dev/vitals_keepalive.h"  // [dev] autonomous long-exposure keepalive (ini vitals_keepalive_sec)
#include "coop/dev/world_roll_drill.h"  // [dev] the day's world rolls: the host's crosses, a client's own is refused
#include "coop/dev/perf_probe.h"
#include "coop/net/session.h"

#include "ue_wrap/core/walk_timer.h"

namespace coop::dev::dev_lanes {

void Install(coop::net::Session& session) {
    coop::dev::rng_roll_census::Install(&session);  // [dev] driver/QuitGame interceptors (no-op unless rng_roll_census=1)
    coop::dev::server_breaker_census::Install(&session);  // [dev] the server breaker census (no-op unless server_breaker_census=1)
    coop::dev::desk_diag::Install(&session);  // [dev] desk divergence census: per-peer desk/comp/dish/coordLog snapshot (no-op unless desk_diag=1)
    coop::dev::rollover_watch::Install(&session);  // [dev] the day rollover instrument (no-op unless rollover_watch=1)
    coop::dev::midnight_drill::Install(&session);  // [dev] the midnight drill (no-op unless midnight_drill is set)
    coop::dev::sleep_drill::Install(&session);  // [dev] the sleep drill (no-op unless sleep_drill is set)
    coop::dev::badsun_drill::Install(&session);  // [dev] the Bad Sun drill (no-op unless badsun_drill is set)
    coop::dev::event_relay_drill::Install(&session);  // [dev] the creature event relay drill (no-op unless event_relay_drill is on)
    coop::dev::creature_hit_drill::Install(&session);  // [dev] the creature hit drill (no-op unless creature_hit_drill is set)
    coop::dev::hostile_drill::Install(&session);  // [dev] the receive-boundary drill (no-op unless hostile_drill is set)
    coop::dev::fire_ignite_drill::Install(&session);  // [dev] the salt rocket ignite drill (no-op unless fire_ignite_drill is set)
    coop::dev::cord_drill::Install(&session);  // [dev] the cord drill (no-op unless cord_drill is set)
    coop::dev::cord_pos_drill::Install(&session);  // [dev] the late-mirror drill (no-op unless cord_pos_drill is set)
    coop::dev::coffee_drill::Install(&session);  // [dev] the coffee drill (no-op unless coffee_drill is set)
    coop::dev::prop_act_drill::Install(&session);  // [dev] the act drill's phase says it is armed (no-op unless prop_act_drill is set)
    coop::dev::world_roll_drill::Install(&session);  // [dev] the world roll drill (no-op unless world_roll_drill is set)
    coop::dev::kerfur_menu_drill::Install(&session);  // [dev] the kerfur menu drill (no-op unless kerfur_menu_drill=1)
    coop::dev::kerfur_convert_drill::Install(&session);  // [dev] the kerfur conversion drill (no-op unless kerfur_convert_drill=1)
    coop::dev::kerfus_drill::Install(&session);  // [dev] the Kerfus drill (no-op unless kerfus_drill=1)
    coop::dev::kerfus_throw_drill::Install(&session);  // [dev] the Kerfus throw drill (kerfus_throw_drill=1)
    coop::dev::kerfur_serve_drill::Install(&session);  // [dev] the Omega serve drill (kerfur_serve_drill=1)
    coop::dev::container_selftest::Install(&session);  // [dev] the container-lane e2e circle (no-op unless container_selftest=1)
    coop::dev::drive_selftest::Install(&session);  // [dev] rack-lane e2e circles (no-op unless drive_selftest=1)
    coop::dev::rack_birth_drill::Install(&session);  // [dev] the rack birth drill (no-op unless rack_birth_drill=1)
    coop::dev::burn_revive_drill::Install(&session);  // [dev] the burn revive drill (no-op unless burn_revive_drill=1)
    coop::dev::hand_drop_selftest::Install(&session);  // [dev] hand pickup+drop episodes (no-op unless hand_drop_selftest=1)
    coop::dev::floppy_selftest::Install(&session);  // [dev] disc/server insert+eject episodes (no-op unless floppy_selftest=1)
    coop::dev::hookdrag_selftest::Install(&session);  // [dev] a hook-dragged prop, both peers logging its position (no-op unless hookdrag_selftest=1)
    coop::dev::roster_token_selftest::Install(&session);  // [dev] successor-ban drill: a token captured from the previous occupant must be refused (no-op unless roster_token_selftest=1)
    coop::dev::pi3_drill::Install(&session);  // [dev] the PI-3 drill's observer (no-op unless pi3_drill is set)
    coop::dev::native_spawn_probe::Install(&session);  // [dev] the native-spawn record (no-op unless native_spawn_probe is set; installed once)
}

void EndSession() {
    // A drill's director walks end with their session: the pawn they drive outlives it.
    coop::director::EndWalks();
    // The two prop-seam probes print their run totals before the state they describe is cleared.
    coop::dev::prop_birth_key_probe::EmitVerdict();
    coop::dev::spawn_match_probe::EmitVerdict();
    coop::dev::food_clock_probe::OnDisconnect();  // [dev] tallies and the cached class, which a level change can unload
    coop::dev::lookat_churn_probe::OnDisconnect();  // [dev] the aim episodes, after the run's last reading
    coop::dev::rollover_watch::OnDisconnect();  // [dev] the per-world arm and the session's watch totals
    coop::dev::midnight_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::sleep_drill::OnDisconnect();  // [dev] back to the first phase, the host's nightmare chance restored
    coop::dev::badsun_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::event_relay_drill::OnDisconnect();  // [dev] the client's steps back to the first; the host keeps its krampus
    coop::dev::creature_hit_drill::OnDisconnect();  // [dev] the client's steps back to the first; the host keeps its creature
    coop::dev::hostile_drill::OnDisconnect();  // [dev] back to WaitArm; a rejoin re-arms
    coop::dev::world_roll_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfur_menu_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfur_convert_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfus_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfus_throw_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfur_serve_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::door_drill::OnDisconnect();  // [dev] the door list and readings belong to one world
    coop::dev::toggle_drill::OnDisconnect();  // [dev] the device and its phase belong to one world
    coop::dev::swinger_drill::OnDisconnect();  // [dev] the lids, the walk and the counters belong to one world
    coop::dev::blackout_drill::OnDisconnect();  // [dev] the panel and the phase belong to one world
    coop::dev::light_drill::OnDisconnect();  // [dev] the switch, its group and the phase belong to one world
    coop::dev::keypad_drill::OnDisconnect();  // [dev] the keypad and the legs belong to one world
    coop::dev::container_view_drill::OnDisconnect();  // [dev] the ATV, the view and the walks belong to one world
    coop::dev::container_route_drill::OnDisconnect();  // [dev] the fixtures and the steps belong to one session
    coop::dev::container_route_census::OnDisconnect();  // [dev] the census reports and starts over
    coop::dev::physmods_drill::OnDisconnect();  // [dev] the client arms again, so a rejoin says its line
    coop::dev::throw_drill::OnDisconnect();  // [dev] the backpack and its copy belong to one session
    coop::dev::atv_spawn_drill::OnDisconnect();  // [dev] the spawn belongs to one session
    coop::dev::atv_health_drill::OnDisconnect();  // [dev] the phases and the walk belong to one session
    coop::dev::atv_eject_drill::OnDisconnect();  // [dev] the phases and the walk belong to one session
    coop::dev::drill_chat::Reset();  // [dev] the queued lines and the talk budget belong to one session
    coop::dev::fall_drill::OnDisconnect();  // [dev] the drop belongs to one session
    coop::dev::remover_drill::OnDisconnect();  // [dev] the props and the toolgun belong to one session
    coop::dev::prop_act_drill::OnDisconnect();  // [dev] the steps, the target and the walk belong to one session
    coop::dev::prop_syncer_drill::OnDisconnect();  // [dev] the phases, the target and the walk belong to one session
    coop::dev::sack_drill::OnDisconnect();  // [dev] the sack and the counts belong to one session
    coop::dev::drone_call_drill::OnDisconnect();  // [dev] the console, the legs and the host's watch belong to one world
    coop::dev::dronesack_drill::OnDisconnect();  // [dev] the sack, the crate and the records belong to one session
    coop::dev::container_writer_probe::OnDisconnect();  // [dev] the readings belong to one world
    coop::dev::server_upgrade_drill::OnDisconnect();  // [dev] the box and the legs belong to one world
    coop::dev::server_drill::OnDisconnect();  // [dev] the same for the server break drill
    coop::dev::camera_drill::OnDisconnect();  // [dev] a rejoin's client walks to the camera again
    coop::dev::sleepingbag_drill::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::notebook_drill::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::camera_state_drill::OnDisconnect();  // [dev] a rejoin's client reads the camera again
    coop::dev::sat_console_drill::OnDisconnect();  // [dev] the steps and the host's counts belong to one session
    coop::dev::calib_drill::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::drive_drill::OnDisconnect();  // [dev] the same for the drive drill
    coop::dev::late_join_drill::OnDisconnect();  // [dev] a rejoin's client prints its tallies again
    coop::dev::download_drill::OnDisconnect();  // [dev] the same for the download drill
    coop::dev::rack_birth_drill::OnDisconnect();  // [dev] a rejoin's peers run the drill again
    coop::dev::burn_revive_drill::OnDisconnect();  // [dev] a rejoin's client runs the drill again
    coop::dev::settings_drill::OnDisconnect();  // [dev] the steps start over
    coop::dev::mannequin_drill::OnDisconnect();  // [dev] the client's samples belong to one session
    coop::dev::laptop_drill::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::laptop_power_drill::OnDisconnect();  // [dev] a rejoin's peers press again
    coop::dev::laptop_tab_drill::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::explosion_drill::OnDisconnect();  // [dev] a rejoin's peers run their halves and arms again
    coop::dev::octa_drill::OnDisconnect();  // [dev] the arm's state back to its start, the guard put back
    coop::dev::fire_ignite_drill::OnDisconnect();  // [dev] a rejoin's peers run their steps again
    coop::dev::cord_drill::OnDisconnect();  // [dev] the client's steps start over; the host keeps its cast for a rejoin after its seven arms
    coop::dev::cord_pos_drill::OnDisconnect();  // [dev] both roles start over
    coop::dev::coffee_drill::OnDisconnect();  // [dev] the client's steps start over; the host keeps its cast for a rejoin after its five arms
    coop::dev::grid_drill::OnDisconnect();  // [dev] the panel and the legs belong to one world
    coop::dev::tower_drill::OnDisconnect();  // [dev] the tower and the legs belong to one world
    coop::dev::desk_verb_drill::OnDisconnect();  // [dev] the legs start over
    coop::dev::desk_crossing_drill::OnDisconnect();  // [dev] the legs start over, a held host step let go
    coop::dev::desk_ping_drill::OnDisconnect();  // [dev] the legs start over
    coop::dev::command_drill::OnDisconnect();  // [dev] the phases start over, the reply observer cleared
    coop::dev::mv_drill::OnDisconnect();  // [dev] the /mv script starts over, the reply observer cleared
    coop::dev::stats_probe::OnDisconnect();  // [dev] a rejoin's peers run the probe again
    coop::dev::stat_order_drill::OnDisconnect();  // [dev] the steps start over, an answer still in flight is ignored
    coop::dev::chat_drill::OnDisconnect();  // [dev] the drill starts over, a chat it opened is closed
    coop::dev::l10n_drill::OnDisconnect();  // [dev] the drill starts over, a menu it opened is closed
    coop::dev::ban_drill::OnSessionEnd();  // [dev] empty by design: its one state, the step, stands
    coop::dev::pause_quit_drill::OnDisconnect();  // [dev] a pending wait for the pause menu is dropped; the per-process latch stands
    coop::dev::order_selftest::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::bind_wait_selftest::OnDisconnect();  // [dev] a rejoin's client runs the checks again
    coop::dev::pile_late_load::OnDisconnect();  // [dev] a clump still aside goes back to its saved pose
    coop::dev::pile_defer_drill::OnDisconnect();  // [dev] a rejoin's client counts and reports again
    coop::dev::meadow_selftest::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::lid_drill::OnDisconnect();  // [dev] the PC and the legs belong to one world
    coop::dev::subject_drill::OnDisconnect();  // [dev] the drives and the legs belong to one world
    coop::dev::grime_drill::OnDisconnect();  // [dev] the decals and the phase belong to one world
    coop::dev::lookat_aim_drill::OnDisconnect();  // [dev] the held target, which the next world does not have
    coop::dev::fireext_drill::OnDisconnect();  // [dev] back to the first step, the watch emptied
    coop::dev::pry_drill::OnDisconnect();  // [dev] back to the first step, the watch emptied
    coop::dev::pi3_drill::OnDisconnect();  // [dev] a spawner mid-arm is abandoned, a still peer lets go
    coop::dev::native_spawn_probe::OnDisconnect();  // [dev] the per-source counts are said and the records cleared
    coop::dev::floppy_selftest::EmitVerdict();  // [dev] which disc episodes fired, and which never did
    coop::dev::hookdrag_selftest::EmitVerdict();  // [dev] how far the dragged prop moved here
    coop::dev::hand_drop_selftest::EmitVerdict();  // [dev] which hand episodes fired, and what each peer counted
}

void RearmSelftests() {
    coop::dev::container_selftest::OnDisconnect();  // [dev] re-arm the circle on reconnect
    coop::dev::container_join_drill::OnDisconnect();  // [dev] a later session's joiners start over
    coop::dev::floppy_selftest::OnDisconnect();  // [dev] re-arm the disc episodes on reconnect
    coop::dev::hookdrag_selftest::OnDisconnect();  // [dev] re-arm the drag on reconnect
    coop::dev::hand_drop_selftest::OnDisconnect();  // [dev] re-arm the hand episodes on reconnect
}

void TickLoadWindow(coop::net::Session& session) {
    coop::dev::hotbar_icon_probe::Tick();  // [dev] the quick-slot bar's icons, decided during the load
    coop::dev::load_reroll_watch::Tick(session);  // [dev] what it watches runs inside the world load
    coop::dev::radio_tower_probe::Tick(session);  // [dev] the radio tower's state and calls (one string read when off)
    coop::dev::desk_verb_drill::TickLoad(&session);  // [dev] the join's census, armed ahead of the load it counts
    coop::dev::explosion_drill::TickLoad(&session);  // [dev] the explosion drill's load half, inside the world load
}

void TickDrills(coop::net::Session& session) {
    coop::dev::drill_chat::Pump(coop::net::NowMs());  // [dev] the drills' queued chat lines, one per the host's talk pace (an empty-queue check when idle)
    coop::dev::rng_roll_census::Tick();  // [dev] the roll censuses (a single bool read when off)
    coop::dev::server_breaker_census::Tick();  // [dev] the server breaker census (a single bool read when off)
    coop::dev::desk_diag::Tick();  // [dev] desk divergence census (single bool read when off; self-throttled)
    coop::dev::rollover_watch::Tick();  // [dev] the day rollover (a single bool read when off)
    coop::dev::midnight_drill::Tick();  // [dev] the midnight drill's phases (a single enum read when off)
    coop::dev::sleep_drill::Tick();  // [dev] the sleep drill's phases (a single enum read when off)
    coop::dev::badsun_drill::Tick();  // [dev] the Bad Sun drill's phases (a single enum read when off)
    coop::dev::event_relay_drill::Tick();  // [dev] the creature event relay drill's steps (a single bool read when off)
    coop::dev::creature_hit_drill::Tick();  // [dev] the creature hit drill's steps (a single enum read when off)
    coop::dev::hostile_drill::Tick();  // [dev] the receive-boundary drill's arm/judge (a single enum read when off)
    coop::dev::world_roll_drill::Tick();  // [dev] the world roll drill's arms (a single enum read when off)
    coop::dev::kerfur_menu_drill::Tick();  // [dev] the kerfur menu drill's phases (a single bool read when off)
    coop::dev::kerfur_convert_drill::Tick();  // [dev] the kerfur conversion drill's phases (a single bool read when off)
    coop::dev::kerfus_drill::Tick();  // [dev] the Kerfus drill's phases (a single bool read when off)
    coop::dev::kerfus_throw_drill::Tick();  // [dev] the Kerfus throw drill's phases (a single bool read when off)
    coop::dev::kerfur_serve_drill::Tick();  // [dev] the Omega serve drill's phases (a single bool read when off)
    coop::dev::prop_birth_key_probe::Tick();  // [dev] periodic seam totals (a single bool read when off)
    coop::dev::host_settle_drill::Tick(session);  // [dev] the host settle drill's verdict (a single bool read when off)
    coop::dev::food_clock_probe::Tick();  // [dev] the food catch-up's clock reading (a single bool read when off)
    coop::dev::lookat_churn_probe::Tick();  // [dev] the interaction UI's rebuild rate under a held aim (a single bool read when off)
    coop::dev::lookat_aim_drill::Tick(&session);  // [dev] walk to a prop and hold the aim (a single bool read when off)
    coop::dev::door_drill::Tick(&session);  // [dev] the door drill's sensor readings and walk (a single bool read when off)
    coop::dev::event_drill::Tick(&session);  // [dev] the event drill's fires and count (a single bool read when off)
    coop::dev::settings_drill::Tick(&session);  // [dev] the settings drill's steps (a latched enum compare when off)
    coop::dev::mannequin_drill::Tick(&session);  // [dev] the mannequin drill's phases (a single enum read when off)
    coop::dev::toggle_drill::Tick(&session);  // [dev] the toggle drill's toggles (a single check when off)
    coop::dev::swinger_drill::Tick(&session);  // [dev] the swinger drill's watch and drag (a single check when off)
    coop::dev::world_first_check::Tick();  // [dev] client_world_first's verdict (a single bool read when off)
    coop::dev::blackout_drill::Tick(&session);  // [dev] the blackout drill's fire and reads (a single bool read when off)
    coop::dev::lid_drill::Tick(&session);  // [dev] the lid drill's legs (a single bool read when off)
    coop::dev::subject_drill::Tick(&session);  // [dev] the subject drill's legs (a single enum read when off)
    coop::dev::light_drill::Tick(&session);  // [dev] the light drill's presses (a single bool read when off)
    coop::dev::keypad_drill::Tick(&session);  // [dev] the keypad drill's legs (a single bool read when off)
    coop::dev::fireext_drill::Tick(&session);  // [dev] the fire extinguisher drill (a single bool read when off)
    coop::dev::pry_drill::Tick(&session);  // [dev] the pry drill (a single read when off)
    coop::dev::recycled_slot_drill::Tick();  // [dev] the recycled-slot drill (a single read when off)
    coop::dev::spawn_match_probe::Tick();  // [dev] periodic fuzzy-match totals (a single bool read when off)
    coop::dev::container_selftest::Tick();  // [dev] the container-lane e2e circle (a single bool read when off)
    coop::dev::container_join_drill::Tick(&session);  // [dev] the container join drill (a single bool read when off)
    coop::dev::container_opener_probe::Tick(&session);  // [dev] far containers' openers and the reach to them (latched read when off)
    coop::dev::container_writer_probe::Tick(&session);  // [dev] a container changed by a writer nothing watches (latched read when off)
    coop::dev::container_view_drill::Tick(&session);  // [dev] the view-close drill (a single bool read when off)
    coop::dev::container_route_census::Tick(&session);  // [dev] the route census (a latched string read when off)
    coop::dev::container_route_drill::Tick(&session);  // [dev] the census drill (a latched bool read when off)
    coop::dev::physmods_drill::Tick(&session);  // [dev] the module-plug drill (a single bool read when off)
    coop::dev::throw_drill::Tick(&session);  // [dev] the throw drill (latched string compares when off)
    coop::dev::atv_spawn_drill::Tick(&session);  // [dev] the ATV spawn drill (latched string compares when off)
    coop::dev::atv_health_drill::Tick(&session);  // [dev] the ATV health drill (a latched enum read when off)
    coop::dev::atv_eject_drill::Tick(&session);  // [dev] the ATV eject drill (a latched enum read when off)
    coop::dev::fall_drill::Tick(&session);  // [dev] the fall drill (a latched bool read when off)
    coop::dev::remover_drill::Tick(&session);  // [dev] the remover drill (a latched enum read when off)
    coop::dev::prop_act_drill::Tick(&session);  // [dev] the act drill (a latched phase read when off)
    coop::dev::prop_syncer_drill::Tick(&session);  // [dev] the prop syncer drill (a latched enum read when off)
    coop::dev::sack_drill::Tick(&session);  // [dev] the point sack drill (a latched bool read when off)
    coop::dev::drone_call_drill::Tick(&session);  // [dev] the drone console drill (a single bool read when off)
    coop::dev::dronesack_drill::Tick(&session);  // [dev] the drone sack drill (a single bool read when off)
    coop::dev::server_upgrade_drill::Tick(&session);  // [dev] the server upgrade drill (a single bool read when off)
    coop::dev::server_drill::Tick(&session);  // [dev] the server break drill (two short string compares when off)
    coop::dev::camera_drill::Tick(&session);  // [dev] the camera re-stick drill (a latched bool read when off)
    coop::dev::sleepingbag_drill::Tick(&session);  // [dev] the sleeping-bag drill (a latched string compare when off)
    coop::dev::notebook_drill::Tick(&session);  // [dev] the notebook drill (a latched string compare when off)
    coop::dev::camera_state_drill::Tick(&session);  // [dev] the camera state arms (a latched string compare when off)
    coop::dev::sat_console_drill::Tick(&session);  // [dev] the SAT console drill (two short string compares when off)
    coop::dev::calib_drill::Tick(&session);  // [dev] the precision drill (two short string compares when off)
    coop::dev::drive_drill::Tick(&session);  // [dev] the drive drill (two short string compares when off)
    coop::dev::late_join_drill::Tick(&session);  // [dev] the bind wait's tallies (a latched bool read when off)
    coop::dev::pile_late_load::Tick();  // [dev] the late-load seam (a cached bool read when off)
    coop::dev::pile_defer_drill::Tick(&session);  // [dev] the pile deferral drill (a latched enum read when off)
    coop::dev::download_drill::Tick(&session);  // [dev] the download drill (two short string compares when off)
    coop::dev::laptop_drill::Tick(&session);  // [dev] the laptop drill (a latched read when off)
    coop::dev::laptop_power_drill::Tick(&session);  // [dev] the laptop power drill (a bool read when off)
    coop::dev::laptop_tab_drill::Tick(&session);  // [dev] the PC tab drill (a latched string compare when off)
    coop::dev::explosion_drill::Tick(&session);  // [dev] the explosion lane's drill: dissolve's world half, the other arms' ticks (a latched string compare when off)
    coop::dev::octa_drill::Tick(&session);  // [dev] the Geom Octa drill (a latched string compare when off)
    coop::dev::fire_ignite_drill::Tick();  // [dev] the salt rocket ignite drill (a latched enum read when off)
    coop::dev::cord_drill::Tick();  // [dev] the cord drill (a latched read when off)
    coop::dev::cord_pos_drill::Tick();  // [dev] the late-mirror drill (a latched read when off)
    coop::dev::coffee_drill::Tick();  // [dev] the coffee drill (a latched read when off)
    coop::dev::grid_drill::Tick(&session);  // [dev] the power grid drill (a latched read when off)
    coop::dev::tower_drill::Tick(&session);  // [dev] the coordinate towers drill (a latched read when off)
    coop::dev::desk_verb_drill::Tick(&session);  // [dev] the desk's save family drill (a latched read when off)
    coop::dev::desk_crossing_drill::Tick(&session);  // [dev] the needle's crossing drill (a latched read when off)
    coop::dev::desk_ping_drill::Tick(&session);  // [dev] the ping verdict drill (a latched read when off)
    coop::dev::command_drill::Tick(&session);  // [dev] the commands drill (a latched read when off)
    coop::dev::mv_drill::Tick(&session);  // [dev] the /mv drill (a latched read when off)
    coop::dev::stats_probe::Tick(&session);  // [dev] the player stats and status effects probe (a latched read when off)
    coop::dev::stat_order_drill::Tick(&session);  // [dev] the stat order drill's steps (a latched read when off)
    coop::dev::chat_drill::Tick(&session);  // [dev] the chat drill (a latched read when off)
    coop::dev::l10n_drill::Tick(&session);  // [dev] the l10n drill (a latched read when off)
    coop::dev::ban_drill::Tick(&session);  // [dev] the host ban drill (a latched read when off)
    coop::dev::bug_report_drill::Tick(&session);  // [dev] the bug-report drill (a latched read when off)
    coop::dev::grants_drill::Tick(&session);  // [dev] the local grants drill (a latched read when off)
    coop::dev::pause_quit_drill::Tick(&session);  // [dev] the pause-quit drill (a latched read when off)
    coop::dev::drive_selftest::Tick();  // [dev] rack-lane e2e circles (single bool read when off; 5 s self-throttle)
    coop::dev::rack_birth_drill::Tick();  // [dev] the rack birth drill (a latched read when off)
    coop::dev::burn_revive_drill::Tick();  // [dev] the burn revive drill (a latched read when off)
    coop::dev::floppy_selftest::Tick();  // [dev] disc/server episodes (single bool read when off; 6 s census period)
    coop::dev::hookdrag_selftest::Tick();  // [dev] the hook drag and its 4 Hz position log (single bool read when off)
    coop::dev::hand_drop_selftest::Tick();  // [dev] hand pickup+drop episodes (single bool read when off)
    coop::dev::run_and_wait_selftest::Tick();  // [dev] the wait's endings, once (single latched read when off)
    coop::dev::end_play_probe::Tick();  // [dev] the end-of-play seam against the K2 seam (single latched read when off)
    coop::dev::puppet_reg_probe::Tick();  // [dev] each puppet judged at its begin-play and end of play (latched read when off)
    coop::dev::pi0_selftest::Tick(&session);  // [dev] a cleared Player Element keeps its id, once (latched read when off)
    coop::dev::pi1_selftest::Tick(&session);  // [dev] the substrate primitives' selftest, once (latched read when off)
    coop::dev::pi2_selftest::Tick(&session);  // [dev] the puppet-identity substrate's selftest, once (latched read when off)
    coop::dev::pi3_drill::Tick(&session);  // [dev] the puppet-identity verb drill (a latched read when off)
    coop::dev::native_spawn_probe::Tick(&session);  // [dev] the client orb drive (a latched read when off)
    coop::dev::death_seam_census::Tick();  // [dev] every element end the death seam announces (latched read when off)
    coop::dev::damage_probe::Tick();  // [dev] every player damage verb call: whose body, source, caller (latched read when off)
    coop::dev::grime_drill::Tick(&session);  // [dev] the grime drill's legs (a latched read when off)
    coop::dev::vitals_keepalive::Tick();  // [dev] long-exposure keepalive (single latched read when off)
}

void TickInventory(coop::net::Session& session) {
    namespace PP = coop::dev::perf_probe;
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:live_store_readout"}; coop::dev::live_store_readout::Tick(); }  // READ-ONLY observability for the live personal store (GObjStack[playerContainer.Index]) by content (no-op unless live_store_readout=1)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:inventory_pickup_drill"}; coop::dev::inventory_pickup_drill::Tick(&session); }  // dev drill: a client pockets one prop through the game's own verb (no-op unless its env switch is set)
}

void TickProbes(coop::net::Session& session, bool isConnected, bool isHost) {
    coop::dev::drone_probe::Install();  // dev-only delivery-drone RE probe (ini drone_probe=1; self-latches + retries until the BP class loads)
    coop::dev::drone_probe::Tick(isConnected, isHost);
    coop::dev::order_probe::Install(&session);  // ini order_probe=1; the order queue's writers and the drone's deliveries (a bool when off)
    coop::dev::store_table_probe::Tick();  // ini store_table_probe=1; ONE-SHOT: which mechanism can read a list_store row
    coop::dev::order_selftest::Tick(&session);  // ini order_selftest=1: a client's shop order, the host's daily order, each once a session
    coop::dev::bind_wait_selftest::Tick(&session);  // ini bind_wait_selftest=1: the bind wait's rules on synthetic eids, a client once its world is announced
    coop::dev::meadow_selftest::Tick(&session);  // ini meadow_selftest=1: the host's row into its signal database and out, once a session
    coop::dev::delivery_census::Tick(isHost);  // ini delivery_census=1; edges only // polls drone/order/radar; with drone_probe_drive=1 ALSO auto-fires one delivery (host) / order (client)
    coop::dev::native_pile_inert_probe::Install();  // GO/NO-GO gate for nativizing the trash mirror (ini native_pile_inert_probe=1)
    coop::dev::native_pile_inert_probe::Tick(isConnected, isHost);  // spawns 1 rooted runtime chipPile, logs [INERT-PROBE] IsLive/class 60s -> does a live-ubergraph native stay inert?
    coop::dev::client_model_probe::Install();  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
    coop::dev::client_model_probe::Tick(isConnected, isHost);  // spawns the comparison pair in front of the player -> one clean look settles the cook verdict
    coop::dev::atv_probe::Install();  // dev-only ATV rig baseline instrument (ini atv_probe=1)
    coop::dev::atv_probe::Tick(session, isHost);  // samples vehicleGetParts + vitals every 500 ms when on
    coop::dev::pinecone_probe::Install();  // dev-only pinecone-scare sync verification (ini pinecone_probe=1)
    coop::dev::pinecone_probe::Tick(isConnected, isHost);  // host force-spawns one pinecone ~5s after a client connects -> confirm it mirrors
    coop::dev::lightswitch_probe::Install();  // dev-only light-switch sync RE probe (ini lightswitch_probe=1)
    coop::dev::lightswitch_probe::Tick();  // one-shot synthetic flip: is SetActive blueprint-internal, and does the use verb flip both the switch and the lights
    coop::dev::light_group_census::Tick();  // dev-only READ-ONLY light-GROUP census (ini lightgroup_census=1); self-installs
    coop::dev::keypad_probe::Install();  // dev-only keypad digit-entry RE probe (ini keypad_probe=1)
    coop::dev::keypad_probe::Tick();  // synthetic inputNumber sequence -> does it append inPassword + flip isAcc
}

}  // namespace coop::dev::dev_lanes
