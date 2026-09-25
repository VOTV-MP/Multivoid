// coop/dev/dev_lanes.cpp -- see coop/dev/dev_lanes.h. The calls stand in the order the session's pump
// made them.

#include "coop/dev/dev_lanes.h"

#include "coop/dev/atv_probe.h"
#include "coop/dev/blackout_drill.h"  // [dev] a blackout on both peers, for the lanes' dev probe
#include "coop/dev/client_model_probe.h"  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
#include "coop/dev/container_opener_probe.h"  // [dev] the actors far containers open through, on real objects
#include "coop/dev/container_selftest.h"  // [dev] container-lane e2e circle (organic addLoot)
#include "coop/dev/container_view_drill.h"  // [dev] a container view closes as its opener leaves reach
#include "coop/dev/death_seam_census.h"  // [dev] every element end the death seam announces
#include "coop/dev/delivery_census_probe.h"  // COUNT the delivery-path actors
#include "coop/dev/desk_diag.h"  // [dev] desk/console divergence census
#include "coop/dev/door_drill.h"  // [dev] whether a remote player counts in a door's own sensor
#include "coop/dev/drive_selftest.h"  // [dev] rack-lane e2e circles
#include "coop/dev/drone_call_drill.h"  // [dev] a client's console press flies the host's drone
#include "coop/dev/drone_probe.h"
#include "coop/dev/end_play_probe.h"  // [dev] every end of play against the K2_DestroyActor seam
#include "coop/dev/event_drill.h"  // [dev] the event lanes: a scheduler fire, a dev fire, the join snapshot
#include "coop/dev/fireext_drill.h"  // [dev] a wall-mounted fire extinguisher taken off and carried, watched on both peers
#include "coop/dev/floppy_selftest.h"  // [dev] the disc-into-server media transfer, driven
#include "coop/dev/food_clock_probe.h"  // the food record's arrival catch-up and the two clocks behind it
#include "coop/dev/grime_drill.h"  // [dev] a clean on either peer reaching the other's copy
#include "coop/dev/hand_drop_selftest.h"  // [dev] a prop through a hand and back out, driven
#include "coop/dev/hookdrag_selftest.h"  // [dev] a prop dragged by a hook, driven
#include "coop/dev/inventory_pickup_drill.h"  // dev drill: one client pickup through putObjectInventory2
#include "coop/dev/kerfur_convert_drill.h"  // [dev] a client turns a kerfur off and on beside a disc of its own
#include "coop/dev/kerfus_drill.h"  // [dev] a client turns the plain kerfur on and off through the host
#include "coop/dev/kerfur_menu_drill.h"  // [dev] a kerfur turned on through its menu event, nested in a watched body
#include "coop/dev/keypad_drill.h"  // [dev] whether a client's typing lands on the host's verdict on both copies
#include "coop/dev/keypad_probe.h"
#include "coop/dev/light_drill.h"  // [dev] whether a client's switch press moves the host's group, and never its own
#include "coop/dev/light_group_census.h"
#include "coop/dev/lightswitch_probe.h"
#include "coop/dev/live_store_readout.h"  // READ-ONLY live personal store observability
#include "coop/dev/lookat_aim_drill.h"  // hold a peer's aim on a resting prop, so the churn probe has a reading
#include "coop/dev/lookat_churn_probe.h"  // how often the interaction UI's look-at set is rebuilt under a held aim
#include "coop/dev/midnight_drill.h"  // [dev] the host's midnight on demand, awake or inside the shared sleep
#include "coop/dev/native_pile_inert_probe.h"
#include "coop/dev/order_probe.h"  // who writes the order queue, and what the drone delivers, per peer
#include "coop/dev/order_selftest.h"  // exercise host-side order pricing end to end
#include "coop/dev/physmods_drill.h"  // [dev] a module plug and unplug are sent at their verbs, and a rejoin's load sends none
#include "coop/dev/pinecone_probe.h"
#include "coop/dev/prop_birth_key_probe.h"  // the place/birth seam's key timing and drain exits
#include "coop/dev/pry_drill.h"  // [dev] a stuck pryable pried off its wall, watched on both peers
#include "coop/dev/recycled_slot_drill.h"  // [dev] a recycled object slot, staged on the local pawn
#include "coop/dev/rng_roll_census.h"  // the dev roll census
#include "coop/dev/rollover_watch.h"  // [dev] the day rollover: its verbs, the pulses' consumers, the day numbers and the hash digest
#include "coop/dev/roster_token_selftest.h"  // [dev] successor-ban drill (moderation token vs a recycled slot)
#include "coop/dev/run_and_wait_selftest.h"  // [dev] the one game-thread wait's five endings, provoked
#include "coop/dev/sleep_probe.h"
#include "coop/dev/spawn_match_probe.h"  // the fuzzy-match candidate set and adoption watch
#include "coop/dev/store_table_probe.h"  // which mechanism can read a list_store row
#include "coop/dev/toggle_drill.h"  // [dev] whether a toggle device's state crosses both ways
#include "coop/dev/vitals_keepalive.h"  // [dev] autonomous long-exposure keepalive (ini vitals_keepalive_sec)
#include "coop/dev/perf_probe.h"
#include "coop/net/session.h"

#include "ue_wrap/core/walk_timer.h"

namespace coop::dev::dev_lanes {

void Install(coop::net::Session& session) {
    coop::dev::rng_roll_census::Install(&session);  // [dev] driver/QuitGame interceptors (no-op unless rng_roll_census=1)
    coop::dev::desk_diag::Install(&session);  // [dev] desk divergence census: per-peer desk/comp/dish/coordLog snapshot (no-op unless desk_diag=1)
    coop::dev::rollover_watch::Install(&session);  // [dev] the day rollover instrument (no-op unless rollover_watch=1)
    coop::dev::midnight_drill::Install(&session);  // [dev] the midnight drill (no-op unless midnight_drill is set)
    coop::dev::kerfur_menu_drill::Install(&session);  // [dev] the kerfur menu drill (no-op unless kerfur_menu_drill=1)
    coop::dev::kerfur_convert_drill::Install(&session);  // [dev] the kerfur conversion drill (no-op unless kerfur_convert_drill=1)
    coop::dev::kerfus_drill::Install(&session);  // [dev] the Kerfus drill (no-op unless kerfus_drill=1)
    coop::dev::container_selftest::Install(&session);  // [dev] the container-lane e2e circle (no-op unless container_selftest=1)
    coop::dev::drive_selftest::Install(&session);  // [dev] rack-lane e2e circles (no-op unless drive_selftest=1)
    coop::dev::hand_drop_selftest::Install(&session);  // [dev] hand pickup+drop episodes (no-op unless hand_drop_selftest=1)
    coop::dev::floppy_selftest::Install(&session);  // [dev] disc/server insert+eject episodes (no-op unless floppy_selftest=1)
    coop::dev::hookdrag_selftest::Install(&session);  // [dev] a hook-dragged prop, both peers logging its position (no-op unless hookdrag_selftest=1)
    coop::dev::roster_token_selftest::Install(&session);  // [dev] successor-ban drill: a token captured from the previous occupant must be refused (no-op unless roster_token_selftest=1)
}

void EndSession() {
    // The two prop-seam probes print their run totals before the state they describe is cleared.
    coop::dev::prop_birth_key_probe::EmitVerdict();
    coop::dev::spawn_match_probe::EmitVerdict();
    coop::dev::food_clock_probe::OnDisconnect();  // [dev] tallies and the cached class, which a level change can unload
    coop::dev::lookat_churn_probe::OnDisconnect();  // [dev] the aim episodes, after the run's last reading
    coop::dev::rollover_watch::OnDisconnect();  // [dev] the per-world arm and the session's watch totals
    coop::dev::midnight_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfur_menu_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfur_convert_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::kerfus_drill::OnDisconnect();  // [dev] back to the first phase
    coop::dev::door_drill::OnDisconnect();  // [dev] the door list and readings belong to one world
    coop::dev::toggle_drill::OnDisconnect();  // [dev] the device and its phase belong to one world
    coop::dev::blackout_drill::OnDisconnect();  // [dev] the panel and the phase belong to one world
    coop::dev::light_drill::OnDisconnect();  // [dev] the switch, its group and the phase belong to one world
    coop::dev::keypad_drill::OnDisconnect();  // [dev] the keypad and the legs belong to one world
    coop::dev::container_view_drill::OnDisconnect();  // [dev] the ATV, the view and the walks belong to one world
    coop::dev::physmods_drill::OnDisconnect();  // [dev] the client arms again, so a rejoin says its line
    coop::dev::drone_call_drill::OnDisconnect();  // [dev] the console, the legs and the host's watch belong to one world
    coop::dev::order_selftest::OnDisconnect();  // [dev] a rejoin's peers run their legs again
    coop::dev::grime_drill::OnDisconnect();  // [dev] the decals and the phase belong to one world
    coop::dev::lookat_aim_drill::OnDisconnect();  // [dev] the held target, which the next world does not have
    coop::dev::fireext_drill::OnDisconnect();  // [dev] back to the first step, the watch emptied
    coop::dev::pry_drill::OnDisconnect();  // [dev] back to the first step, the watch emptied
    coop::dev::floppy_selftest::EmitVerdict();  // [dev] which disc episodes fired, and which never did
    coop::dev::hookdrag_selftest::EmitVerdict();  // [dev] how far the dragged prop moved here
    coop::dev::hand_drop_selftest::EmitVerdict();  // [dev] which hand episodes fired, and what each peer counted
}

void RearmSelftests() {
    coop::dev::container_selftest::OnDisconnect();  // [dev] re-arm the circle on reconnect
    coop::dev::floppy_selftest::OnDisconnect();  // [dev] re-arm the disc episodes on reconnect
    coop::dev::hookdrag_selftest::OnDisconnect();  // [dev] re-arm the drag on reconnect
    coop::dev::hand_drop_selftest::OnDisconnect();  // [dev] re-arm the hand episodes on reconnect
}

void TickDrills(coop::net::Session& session) {
    coop::dev::rng_roll_census::Tick();  // [dev] the roll censuses (a single bool read when off)
    coop::dev::desk_diag::Tick();  // [dev] desk divergence census (single bool read when off; self-throttled)
    coop::dev::rollover_watch::Tick();  // [dev] the day rollover (a single bool read when off)
    coop::dev::midnight_drill::Tick();  // [dev] the midnight drill's phases (a single enum read when off)
    coop::dev::kerfur_menu_drill::Tick();  // [dev] the kerfur menu drill's phases (a single bool read when off)
    coop::dev::kerfur_convert_drill::Tick();  // [dev] the kerfur conversion drill's phases (a single bool read when off)
    coop::dev::kerfus_drill::Tick();  // [dev] the Kerfus drill's phases (a single bool read when off)
    coop::dev::prop_birth_key_probe::Tick();  // [dev] periodic seam totals (a single bool read when off)
    coop::dev::food_clock_probe::Tick();  // [dev] the food catch-up's clock reading (a single bool read when off)
    coop::dev::lookat_churn_probe::Tick();  // [dev] the interaction UI's rebuild rate under a held aim (a single bool read when off)
    coop::dev::lookat_aim_drill::Tick(&session);  // [dev] walk to a prop and hold the aim (a single bool read when off)
    coop::dev::door_drill::Tick(&session);  // [dev] the door drill's sensor readings and walk (a single bool read when off)
    coop::dev::event_drill::Tick(&session);  // [dev] the event drill's fires and count (a single bool read when off)
    coop::dev::toggle_drill::Tick(&session);  // [dev] the toggle drill's toggles (a single check when off)
    coop::dev::blackout_drill::Tick(&session);  // [dev] the blackout drill's fire and reads (a single bool read when off)
    coop::dev::light_drill::Tick(&session);  // [dev] the light drill's presses (a single bool read when off)
    coop::dev::keypad_drill::Tick(&session);  // [dev] the keypad drill's legs (a single bool read when off)
    coop::dev::fireext_drill::Tick(&session);  // [dev] the fire extinguisher drill (a single bool read when off)
    coop::dev::pry_drill::Tick(&session);  // [dev] the pry drill (a single read when off)
    coop::dev::recycled_slot_drill::Tick();  // [dev] the recycled-slot drill (a single read when off)
    coop::dev::spawn_match_probe::Tick();  // [dev] periodic fuzzy-match totals (a single bool read when off)
    coop::dev::container_selftest::Tick();  // [dev] the container-lane e2e circle (a single bool read when off)
    coop::dev::container_opener_probe::Tick(&session);  // [dev] far containers' openers and the reach to them (latched read when off)
    coop::dev::container_view_drill::Tick(&session);  // [dev] the view-close drill (a single bool read when off)
    coop::dev::physmods_drill::Tick(&session);  // [dev] the module-plug drill (a single bool read when off)
    coop::dev::drone_call_drill::Tick(&session);  // [dev] the drone console drill (a single bool read when off)
    coop::dev::drive_selftest::Tick();  // [dev] rack-lane e2e circles (single bool read when off; 5 s self-throttle)
    coop::dev::floppy_selftest::Tick();  // [dev] disc/server episodes (single bool read when off; 6 s census period)
    coop::dev::hookdrag_selftest::Tick();  // [dev] the hook drag and its 4 Hz position log (single bool read when off)
    coop::dev::hand_drop_selftest::Tick();  // [dev] hand pickup+drop episodes (single bool read when off)
    coop::dev::run_and_wait_selftest::Tick();  // [dev] the wait's endings, once (single latched read when off)
    coop::dev::end_play_probe::Tick();  // [dev] the end-of-play seam against the K2 seam (single latched read when off)
    coop::dev::death_seam_census::Tick();  // [dev] every element end the death seam announces (latched read when off)
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
    coop::dev::delivery_census::Tick(isHost);  // ini delivery_census=1; edges only // polls drone/order/radar; with drone_probe_drive=1 ALSO auto-fires one delivery (host) / order (client)
    coop::dev::native_pile_inert_probe::Install();  // GO/NO-GO gate for nativizing the trash mirror (ini native_pile_inert_probe=1)
    coop::dev::native_pile_inert_probe::Tick(isConnected, isHost);  // spawns 1 rooted runtime chipPile, logs [INERT-PROBE] IsLive/class 60s -> does a live-ubergraph native stay inert?
    coop::dev::client_model_probe::Install();  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
    coop::dev::client_model_probe::Tick(isConnected, isHost);  // spawns the comparison pair in front of the player -> one clean look settles the cook verdict
    coop::dev::atv_probe::Install();  // dev-only ATV rig baseline instrument (ini atv_probe=1)
    coop::dev::atv_probe::Tick(session, isHost);  // samples vehicleGetParts + vitals every 500 ms when on
    coop::dev::pinecone_probe::Install();  // dev-only pinecone-scare sync verification (ini pinecone_probe=1)
    coop::dev::pinecone_probe::Tick(isConnected, isHost);  // host force-spawns one pinecone ~5s after a client connects -> confirm it mirrors
    coop::dev::sleep_probe::Install();  // dev-only sleep-gate exerciser (ini sleep_probe=1)
    coop::dev::sleep_probe::Tick(isConnected, isHost);  // client sleeps T+15, host T+25 (ACCELERATE), host wakes T+40 (END)
    coop::dev::lightswitch_probe::Install();  // dev-only light-switch sync RE probe (ini lightswitch_probe=1)
    coop::dev::lightswitch_probe::Tick();  // one-shot synthetic flip: is SetActive blueprint-internal, and does the use verb flip both the switch and the lights
    coop::dev::light_group_census::Tick();  // dev-only READ-ONLY light-GROUP census (ini lightgroup_census=1); self-installs
    coop::dev::keypad_probe::Install();  // dev-only keypad digit-entry RE probe (ini keypad_probe=1)
    coop::dev::keypad_probe::Tick();  // synthetic inputNumber sequence -> does it append inPassword + flip isAcc
}

}  // namespace coop::dev::dev_lanes
