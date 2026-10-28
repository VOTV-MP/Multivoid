// coop/session/subsystems.cpp -- see coop/session/subsystems.h. The sync-module fan-out lists;
// a new sync feature wires in here.

#include "ue_wrap/core/gc_pin.h"
#include "coop/props/trash_mirror.h"
#include "coop/session/subsystems.h"

#include "coop/element/object_scan_hub.h"  // the shared pass over the object index
#include "coop/world/balance_sync.h"
#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_cursor_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_ping_sync.h"
#include "coop/interactables/deck_play_sync.h"  // deck playback lane
#include "coop/interactables/physmods_sync.h"  // desk physical-modules lane
#include "coop/items/order_queue_sync.h"  // the delivery order queue, the host's, mirrored
#include "coop/interactables/drive_payload_sync.h"  // a drive's row, the host's
#include "coop/interactables/drive_sync.h"  // the drive slots
#include "coop/interactables/drive_rack_sync.h"  // rack storage
#include "coop/interactables/meadow_db_sync.h"  // meadow signal-DB mirror (multiset shadow + join seed)
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/desk_sim_sync.h"
#include "coop/interactables/dish_calib_sync.h"
#include "coop/interactables/dish_hashcode_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/download_arm_sync.h"
#include "coop/interactables/sat_console_sync.h"  // the SAT console's shared commands run on the host
#include "coop/interactables/tape_caddy_sync.h"
#include "coop/world/daily_task_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/notebook_sync.h"
#include "coop/world/email_sync.h"
#include "coop/interactables/laptop_sync.h"  // the stationary PC lane
#include "coop/interactables/laptop_tab.h"  // the tab the PC's user is on
#include "coop/interactables/laptop_buffer_sync.h"  // the PC buffer quad lane
#include "coop/interactables/portable_pc_lid.h"  // the portable PC's lid lane
#include "coop/interactables/sleepingbag_sync.h"  // client fold/unfold verbs run on the host's source prop
#include "coop/interactables/floppy_slot_entry.h"
#include "coop/interactables/mirror_slot_entry.h"
#include "coop/interactables/floppy_slot_sync.h"
#include "coop/interactables/floppybox_sync.h"  // the disc crate LIFO lane
#include "coop/props/container_birth.h"
#include "coop/props/prop_key_index.h"  // Begin/End at the gate hold
#include "coop/props/container_contents_sync.h"  // the world-container GObjStack slice
#include "coop/props/container_view_close.h"  // a view into a container ends where the player's reach does
#include "coop/props/prop_food_state.h"  // the food family's live state and who authors it
#include "coop/props/prop_record_refresh.h"  // a drive box or reel case republishes its record in place
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/player/movement_ledger.h"
#include "coop/player/run_end_travel.h"
#include "coop/player/local_body.h"  // skins: local first-person body owner
#include "coop/player/nameplate.h"  // plate-pref session wiring (Install)
#include "coop/player/nick_color.h"  // nick-color session wiring (Install)
#include "coop/player/puppet_identity.h"  // a puppet's touch never runs this machine's player's effects
#include "coop/player/sleep_rest.h"
#include "coop/player/sleep_sync.h"
#include "coop/creatures/wisp_attack_sync.h"  // Killer Wisp coop: host detect + neutralize + relay
#include "coop/creatures/wisp_grab_hold.h"  // Killer Wisp: grab-window body placement (per-slot/full teardown)
#include "coop/creatures/wisp_tear_mirror.h"  // Killer Wisp coop: victim kill + tear mirror
#include "coop/session/pause_guard.h"  // coop no-pause invariant (ESC pause froze clients)
#include "coop/items/player_inventory_sync.h"  // per-player inventory (host file scaffold)
#include "coop/player/stat_orders.h"  // an admin's orders to a player's stats: the kept session, the tokens answered Left
#include "coop/voice/voice_chat.h"
#include "coop/element/bind_wait.h"  // OnSlotLeft / Clear: the messages held for a deferred bind
#include "coop/element/death_seam.h"  // an element's end of play, handed to the lanes that subscribed
#include "coop/props/host_spawn_watcher.h"  // HOST mirror of those ambient spawner outputs (the pinecone scare)
#include "coop/props/prop_drop_intent.h"  // client-place -> host-auth keyed-prop DROP INTENT
#include "coop/props/remover_request.h"  // the tool gun's remover as a request the host performs
#include "coop/creatures/creature_hit_intent.h"  // a client's hit on a creature as an intent the host judges
#include "coop/props/prop_act_host.h"  // a client's take or use of a shared prop, judged and committed on the host
#include "coop/props/prop_act_request.h"  // a client's take or use of a shared prop, asked first and replayed on a grant
#include "coop/props/prop_spawn_authoring.h"  // a PLAYER's spawn verb vs the world's own spawns
#include "coop/creatures/kerfur_convert.h"  // host-authoritative kerfur on/off conversion (the dupe fix)
#include "coop/creatures/kerfur_command.h"  // host-authoritative kerfur menu command relay, serving the requester
#include "coop/creatures/kerfur_convert_host.h"  // the host half of the kerfur conversion: its queue, served each tick
#include "coop/creatures/kerfur_menu_input.h"  // client radial-menu verb detection (InpActEvt_use PRE -> kerfur_command relay)
#include "coop/creatures/kerfur_entity.h"  // the stable-KerfurId authority table
#include "coop/creatures/kerfur_form_assembler.h"  // script-body gate consumer (observe-only + containment counter)
#include "coop/props/prop_stick_sync.h"  // wall-attachable stick mirror (camera-on-wall)
#include "coop/dev/dev_lanes.h"  // the developer drills, probes and selftests, wired in one place
#include "coop/dev/perf_probe.h"
#include "coop/save/join_window_baseline.h"  // the connect edge's removes and position corrections
#include "coop/save/save_transfer.h"
#include "coop/session/join_beacon.h"
#include "coop/interactables/grime_sync.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/drone_sync.h"
#include "coop/items/coingun_sync.h"
#include "coop/items/order_sync.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/explosion_sync.h"
#include "coop/world/explosion_parking.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/firefly_sync.h"
#include "coop/items/inventory_pickup_sync.h"
#include "coop/comms/chat_sync.h"
#include "coop/commands/command_sync.h"
#include "coop/interactables/upgrade_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/world/fuse_tower_ops.h"
#include "coop/world/fuse_tower_rows.h"
#include "coop/world/power_grid.h"
#include "coop/world/power_panel.h"
#include "coop/world/sky_sync.h"
#include "coop/world/time_sync.h"
#include "coop/interactables/window_stroke_sync.h"
#include "coop/interactables/window_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/world_load_episode.h"
#include "coop/interactables/garbage_sync.h"
#include "coop/props/pile_look.h"
#include "coop/props/trash_channel.h"
#include "coop/player/local_streams.h"  // LastHeldActor
#include "coop/player/puppet_carry_drive.h"
#include "coop/props/trash_clump_pose_stream.h"
#include "coop/props/trash_sweep.h"
#include "coop/props/prop_drive_host.h"    // HOST: the props a hook drags, streamed while they move
#include "coop/props/prop_drive_stream.h"  // park and drive those props (a client's, and the host's copies of a client's)
#include "coop/props/prop_syncer_host.h"   // HOST: judge a syncing client's rows
#include "coop/props/prop_park.h"  // the parks' held Characters, given back at session end
#include "coop/props/rider_hold.h"  // the holds on welded Characters' movement, released last
#include "coop/props/trash_collect_sync.h"
#include "coop/props/trash_native_hold.h"  // the load catch on the join's save spawns
#include "coop/items/broom_stroke.h"
#include "coop/props/pack_trash_intent.h"
#include "coop/interactables/verb_lanes.h"  // the interactable lanes on a device's own verbs, at the script gate
#include "coop/creatures/kerfus_lanes.h"  // the plain kerfur: its brain on the host, its state and drive, its verbs
#include "coop/creatures/served_player.h"  // a robot the host runs for a client reads that client as its player
#include "coop/items/broom_push.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/save/save_block.h"
#include "coop/save/save_button_disable.h"
#include "coop/props/grab_observer.h"
#include "coop/player/item_activate.h"
#include "coop/player/player_damage.h"
#include "coop/player/player_hit.h"
#include "coop/session/player_handshake.h"  // TickSkinConverge
#include "coop/player/skin_preview.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_sync.h"
#include "coop/world/world_actor_sync.h"  // non-Character event-actor transform mirror (sibling of npc_sync)
#include "coop/creatures/piramid_sync.h"  // piramid event choreography lane (mirror brain suppression + PyramidGather)
#include "coop/creatures/geom_octa_sync.h"  // the Geom Octa's lane: its host decisions and each peer's look, observed
#include "coop/creatures/geom_octa_relay.h"  // the Geom Octa's lane, the client's half: the host's decisions replayed
#include "coop/player/players_registry.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/pile_spawn_bind.h"       // OnDisconnect: a client's own parked clumps go back to the game
#include "coop/props/prop_element_tracker.h"  // reseed hub consumer install + drain
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // the join claim and sweep
#include "coop/world/alarm_sync.h"  // base radar alarm shared-world toggle
#include "coop/interactables/serverbox_sync.h"  // host-authoritative signal-server sim state
#include "coop/interactables/server_upgrade_sync.h"  // the servers' physical upgrades
#include "coop/creatures/roach_sync.h"  // host-authoritative roach-infestation mirror
#include "coop/creatures/owner_entity_sync.h"
#include "coop/creatures/npc_mirror_brain.h"  // a parked family's mirror runs none of its Blueprint graph
#include "coop/items/hook_anchor.h"
#include "coop/items/hook_sync.h"  // the hook lane: owner-phase stream, mirrors, the tick park
#include "coop/world/event_active_sync.h"  // the native activeEvents registry probe
#include "coop/world/creature_event_relay.h"  // the host's creature events, carried to every client's count
#include "coop/world/weather_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"  // the session's hold on the script-body gate
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)
#include "coop/items/hotbar_icon_edge.h"

namespace coop::subsystems {

namespace {
// Whether the session holds the script gate and the death seam: taken by HoldSessionGate, released
// by ReleaseSessionGateHold. Game thread.
bool g_sessionHoldsGate = false;
}  // namespace

void HoldSessionGate() {
    if (g_sessionHoldsGate) return;
    ue_wrap::script_gate::Acquire("the coop session");
    coop::element::death_seam::Acquire("the coop session");
    g_sessionHoldsGate = true;
    // The key index sees every key write from here on, so it seeds now (or on a later tick, once its watches
    // are live).
    coop::prop_key_index::Begin();
}

void ReleaseSessionGateHold() {
    if (!g_sessionHoldsGate) return;
    coop::prop_key_index::End();  // a write with the gate let go goes unobserved: the next hold seeds again
    ue_wrap::script_gate::Release("the coop session");
    coop::element::death_seam::Release("the coop session");
    g_sessionHoldsGate = false;
}

void InstallLoadWatchers(coop::net::Session& session) {
    coop::comp_sync::Install(&session);  // the refiner's decode: the host's machine alone, a joiner's restore refused
    coop::player_hit::Install(&session);  // the session the host's hit deliveries send through
    coop::explosion_sync::Install(&session);  // the birth seam on every Blueprint deferred spawn: a client's own explosions dissolve, from the load window on; also the request lane's session and client watches
    coop::explosion_parking::Install(&session);  // the explosion copy's parking watches: the four verbs, the rune check, the force narrowing (client session only)
    coop::world_load_episode::InstallReplayWatch(session);
    coop::trash_native_hold::Install(&session);  // the join load's clumps held still at their spawn: a catch installed with the world would arm after the load
}

void Install(coop::net::Session& session) {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&session);
    coop::prop_lifecycle::Install(&session);
    coop::npc_sync::Install(&session);
    coop::world_actor_sync::Install(&session);  // non-Character event-actor mirror (2nd BeginDeferred interceptor, disjoint allowlist)
    coop::piramid_sync::Install(&session);  // piramid event choreography lane (hooks arm lazily on the first piramid element)
    coop::geom_octa_sync::Install(&session);  // the Geom Octa's lane: the [OCTA] watches (live once the class loads)
    coop::item_activate::Install(&session);  // flashlight
    coop::player_damage::Install(&session);  // the refusals on a peer's puppet: its impacts, damage, fire and hit verbs
    coop::weather_sync::Install(&session);  // weather
    coop::interactable_sync::Install(&session);  // doors + lights + container lids
    coop::keypad_sync::Install(&session);  // the password keypads' index and wire (its own module)
    coop::time_sync::Install(&session);  // host-authoritative world clock (time-of-day / dark-world fix)
    coop::sky_sync::Install(&session);  // host-authoritative night-sky orientation, moon phase and eye
    coop::power_panel::Install(&session);  // the power panel: presses to the host, its canonical to every peer
    coop::power_grid::Install(&session);  // the grid runs on the host: its generators' rows, a client's ops
    coop::fuse_tower_rows::Install(&session);  // the towers roll and judge on the host: their rows to every peer
    coop::fuse_tower_ops::Install(&session);  // a client's presses and fuse claims, and its refused rolls
    coop::atv_sync::Install(&session);  // ATV body pose (the syncer the host assigned streams a keyed pose)
    coop::drone_sync::Install(&session);  // delivery drone body pose (host-authoritative singleton)
    coop::order_sync::Install(&session);  // delivery-drone economy: client->host shop-order forward
    coop::coingun_sync::Install(&session);  // the sell gun + host-minted coins
    coop::firefly_sync::Install(&session);  // peer-symmetric ambient firefly mirror (each peer captures+shares its own)
    coop::event_cue_sync::Install(&session);  // cosmetic emitter cues: the host's runEvent watch sends them, the client replays
    coop::event_fire_sync::Install(&session);  // scheduled events: the host's runEvent/runSpecialEvent watches -> EventFire; client hold + policy replay
    coop::event_active_sync::Install(&session);  // the host's setEvent watch: begin/end edges; the join snapshot reads the game's registry
    coop::creature_event_relay::Install(&session);  // a creature's setEvent: the host's count per creature, applied to its mirror on a client
    coop::npc_mirror_brain::Install();  // a parked family's mirrors: the graph watches and the end of a mirror's mark
    coop::alarm_sync::Install(&session);  // base radar alarm shared-world toggle (a 1 Hz active poll on both roles)
    coop::serverbox_sync::Install(&session);  // the server boxes' break state: the verbs run on the host, a client's repair sent there
    coop::server_upgrade_sync::Install(&session);  // the servers' upgrades: ops at the box's two verbs + host canonical
    coop::floppy_slot_sync::Install(&session);  // a disc-holding device's slot: host-canonical, a peer claims the outcome of its own insert or eject
    coop::floppy_slot_entry::Install(&session);  // the slot's overlap entry: no device swallows a disc still in transit out of one
    coop::mirror_slot_entry::Install(&session);  // no device's overlap takes a prop a remote player carries
    coop::puppet_identity::Install(&session);  // a puppet's touch of a wisp, a grunt, a trigger box or a dream box acts on no machine's player
    coop::roach_sync::Install(&session);  // roach infestation: host paged snapshots, client ordinal apply + consumption intents
    coop::owner_entity_sync::Install(&session);  // owner-entity lane: eyer per-peer owned + cross-peer display mirrors
    coop::hook_sync::Install(&session);  // hook lane: owner-phase stream + mirrors, and the ReceiveTick park they depend on
    coop::inventory_pickup_sync::Install(&session);  // inventory-collect blip (PlaySound2D observer)
    coop::chat_sync::Install(&session);  // T-chat (the ui/chat_input send path)
    coop::command_sync::Install(&session);  // the chat input's `/` route: the host's own line, a client's request
    coop::stat_orders::Install(&session);  // an admin's orders and queries of a player's stats: the session they are sent through
    coop::local_body::Install(&session);  // skins: local first-person body + SkinChange announce
    coop::local_body::Tick();  // applies the persisted skin to the local pawn + 1 Hz convergence
    coop::nameplate::Install(&session);  // plate-pref announce path (F1 checkbox -> NameplateChange)
    coop::nick_color::Install(&session);  // nick-color announce path + local-slot mirror refresh
    coop::turbine_sync::Install(&session);  // wind-turbine facing/spin mirror (host-auth ~1 Hz)
    coop::upgrade_sync::Install(&session);  // a client's upgrade purchase is charged and written by the host
    coop::device_occupancy::Install(&session);  // enterable-device occupancy (busy claim + E deny gate)
    coop::notebook_sync::Install(&session);  // a notebook editor's page text rides the prop record lane: the editor's commit and the close
    coop::console_state_sync::Install(&session);  // signal-catcher state mirror (sky signals + desk + dish aim)
    coop::signal_catch_sync::Install(&session);  // the signal-catch consume replay (dish slew + downloader arm on every peer)
    coop::laptop_sync::Install(&session);  // the stationary PC's power and the portable PC lid's wire
    coop::laptop_tab::Install(&session);  // the PC user's tab: the three tab watches, and the receive paths' session
    coop::portable_pc_lid::Install(&session);  // the portable PC's lid, on its own verb
    coop::sleepingbag_sync::Install(&session);  // sleepingbag transitions use host-authored prop births
    coop::laptop_buffer_sync::Install(&session);  // the PC buffer quad
    coop::floppybox_sync::Install(&session);  // the disc crate stack
    coop::props::container_contents_sync::Install(&session);  // container contents
    coop::props::prop_record_refresh::Install(&session);  // drive box + reel case republish their own record
    coop::props::prop_food_state::Install();  // the food family claims its own live state off the record lane
    coop::desk_cursor_sync::Install(&session);  // coords-panel live-cursor unreliable motion stream (interpolated mirror)
    coop::desk_input_sync::Install(&session);  // the claim-free field-granular desk input lane
    coop::desk_snd_fx::Install(&session);  // desk audio-effect mirror (Func-patch audio seam)
    coop::deck_play_sync::Install(&session);  // deck playback edge mirror (audio-seam Activate/Deactivate + gen guard)
    coop::physmods_sync::Install(&session);  // physMods slot ops at the desk's verbs + host-canonical array
    coop::drive_sync::Install(&session);  // the drive chain's slot lane (verb dirty-marks + the slot sweep; the slot and rack verb watches)
    coop::drive_payload_sync::Install(&session);  // a drive's row: the host authors it at prop_drive_C::upd
    coop::drive_rack_sync::Install(&session);  // rack storage lane (marks forwarded from drive_sync)
    coop::desk_ping_sync::Install(&session);  // the ping's verdict: a client's refused and rolled on the host's desk
    coop::desk_sim_sync::Install(&session);  // download-SIM host-authoritative output stream (decoded/needle/rate/frData/poData/offsets; client overwrites)
    coop::dish_sync::Install(&session);  // host-auth dish pose mirror (client sim parked)
    coop::download_arm_sync::Install(&session);  // the download machine's arm and reset, the host's, replayed through the game's verbs
    coop::dish_calib_sync::Install(&session);  // the dishes' precision: the host authors it, a client's own verbs are intents
    coop::dish_hashcode_sync::Install(&session);  // the dishes' hash codes: the host's rollover sends them, a client refuses its own
    coop::sat_console_sync::Install(&session);  // the SAT console: a client's shared commands run on a terminal the host keeps for it
    coop::tape_caddy_sync::Install(&session);  // caddy reel slots (presser edges) + host accrual corrector (client accrual NOT parked -- corrector-bounded)
    coop::daily_task_sync::Install(&session);  // saveSlot.taskNew host mirror (rollover/sell are host-only live)
    coop::email_sync::Install(&session);  // meadow-PC email mirror (watermark -> chunked rows -> addEmail)
    coop::signal_sync::Install(&session);  // desk signal-library mirror (savedSignals_0 shadow/diff)
    coop::meadow_db_sync::Install(&session);  // meadow-DB mirror (content-hash multiset + id-preserving replay)
    coop::voice_chat::Install(&session);  // proximity voice chat (opus over the session; PTT X)
    coop::window_sync::Install(&session);  // base-window dirt scalar (the "main huge window")
    coop::window_stroke_sync::Install(&session);  // the bay window's render-target dabs
    coop::grime_sync::Install(&session);  // surface grime (walls/ceiling/floor dirt decals)
    coop::trash_pile_sync::Install(&session);  // trash pile collect counters
    coop::broom_stroke::Install(&session);  // a client's broom stroke is run by the host, with what the client's stroke read
    coop::pack_trash_intent::Install(&session);  // a client's bagging of a pile is run by the host
    coop::verb_lanes::Install(session);  // drone console, doors, light groups, keypads: each on its own verbs
    coop::broom_push::Install(&session);  // a host's broom push streams what it moves
    coop::trash_collect_sync::Install(&session);  // the chipPile grab observer (the use-press PRE observer, then a PropDestroy by eid)
    coop::garbage_sync::SetSession(&session);
    coop::garbage_sync::Install();  // garbage
    coop::dev::dev_lanes::Install(session);  // [dev] the drills', probes' and selftests' installs (each a no-op unless its knob is set)
    coop::host_spawn_watcher::Install(&session);  // HOST mirrors the ambient spawner outputs (the pinecone scare) the line above cancels on the client -- BeginDeferred POST -> PropSpawn-by-eid
    coop::prop_spawn_authoring::Install(session.role() == coop::net::Role::Client);  // CLIENT: the script-gate bracket that tells a PLAYER's spawn (menu / toolgun) from the world's own -- read by the seam below
    coop::prop_drop_intent::Install(&session);  // CLIENT FinishSpawn post-hook (chains after host_spawn_watcher's) -> place detect -> host DROP INTENT
    coop::kerfur_entity::SetSession(&session);  // the stable-KerfurId authority table: the session for the host id-allocation role gate and the broadcasts
    coop::kerfur_convert::Install(&session);  // host-authoritative kerfur on/off conversion (a client's verb refused -> request; the host's verb converges at its return)
    coop::served_player::Install(&session);  // HOST: the player-0 read seams the next two lanes serve through
    coop::kerfus_lanes::Install(session);  // the plain kerfur: a client refuses its brain and its verbs, the host sends its state
    coop::kerfur_command::Install(&session);  // host-authoritative kerfur menu relay, serving the requester
    coop::remover_request::Install(&session);  // CLIENT: the remover press becomes a request (init watch); HOST: its judge
    coop::creature_hit_intent::Install(&session);  // CLIENT: a swing that hit a creature's mirror becomes an intent (addDamage watch); HOST: its judge
    coop::prop_act_host::Install(&session);  // HOST: the judge of a client's take or use of a shared prop
    coop::prop_act_request::Install(&session);  // the six take-or-use watches (CLIENT asks the host, HOST honours a reservation); CLIENT: the host's answer, and the replay of a grant
    coop::kerfur_menu_input::Install(&session);  // client radial-menu verb detect (InpActEvt_use PRE -- the actionName dispatch is PE-invisible) -> kerfur_command relay
    coop::kerfur_form_assembler::Install(&session);  // script-body gate consumer: watch the two conversion verbs
    coop::prop_stick_sync::Install(&session);  // wall-attachable stick mirror (camera-on-wall -- commit observer -> PropStickState; receiver replays forceStick)
    coop::sleep_sync::Install(&session);  // who rests: each peer's rest state (client -> host -> clients) and the host's sleeping flag
    coop::sleep_rest::Install(&session);  // a client's rest at the native rate: its own sleep watch and the factor on its body and effects
    coop::wisp_attack_sync::Install(&session);  // Killer Wisp coop -- AddPlayerDamage PRE-cancel (host neutralize) + host detect/relay
    // The per-player inventory installs at session start, pre-world: this install runs at
    // world-up, but the apply-blob receiver and the pre-materialise save hook must be live before
    // the join's world loads. Its tick and per-slot hooks below stay.
    coop::balance_sync::SetSession(&session);  // shared host-authoritative balance
    // The client world-save block: registers its gate and arms the one save detour, in every
    // role (the gate reads the role when a save fires; a host's save passes and is reported).
    coop::save_block::Install(&session);
    // Grey out the client pause menu's Save button; a no-op on the host.
    coop::save_button_disable::Install(&session);
    // The shutdown install and the window title are called from the boot timeline directly, not
    // here: they must work before the local player is possessed, since a player may close the
    // window on the splash.
}

void ClientConnectEdge(coop::net::Session& session) {
    coop::item_activate::QueueConnectBroadcastForSlot(0);
    coop::save_transfer::ClientNoteConnected();
    // The host always has a world: open our send gate toward it immediately (the gate exists for
    // host-to-joiner traffic).
    session.MarkSlotWorldReady(0, true);
}

void DisconnectSlot(coop::net::Session& session, int slot) {
    // Abort any snapshot drain to this slot, so it does not iterate every candidate into a dead
    // connection.
    coop::prop_snapshot::CancelForSlot(slot);
    // Drop any in-flight save stream and close the world-ready send gate for the departed slot; a
    // rejoin re-opens both fresh.
    coop::save_transfer::CancelForSlot(slot);
    // Nothing to beacon to a peer that left; a recycled slot must start silent rather than
    // inherit the departed joiner's phase.
    coop::join_beacon::CancelForSlot(slot);
    session.MarkSlotWorldReady(slot, false);
    // A slot teardown is a roster row transition: the leaver's half assemblies and seed brackets
    // must not survive into a recycled occupant.
    coop::prop_save_data::OnPeerGone(static_cast<uint8_t>(slot));
    coop::floppy_slot_sync::OnPeerGone(static_cast<uint8_t>(slot));
    coop::dish_hashcode_sync::OnPeerGone(static_cast<uint8_t>(slot));
    coop::sat_console_sync::OnPeerGone(static_cast<uint8_t>(slot));  // its terminal kept, running its command, for its return
    coop::daily_task_sync::OnPeerGone(static_cast<uint8_t>(slot));  // a departed slot is owed no task
    coop::props::container_birth::OnPeerGone(static_cast<uint8_t>(slot));  // a container transfer it had in flight
    coop::prop_act_host::OnPeerGone(static_cast<uint8_t>(slot));  // a take or use it had reserved
    coop::signal_sync::OnDisconnectSlot(slot);
    coop::email_sync::OnDisconnectSlot(slot);
    // Shut the chat lane's per-slot seed gate: the next occupant's applied range starts empty, so
    // it must get its seed before it hears a live line.
    coop::chat_sync::OnSlotDisconnected(slot);
    coop::command_sync::OnSlotDisconnected(slot);  // the leaver's command rate bucket and notice clocks
    coop::stat_orders::OnSlotDisconnected(static_cast<uint8_t>(slot));  // every order and query still waiting on the leaver is answered Left
    // Per-slot cleanup: only subsystems with per-slot state are called here; the global-state ones
    // are handled by DisconnectAll.
    coop::trash_mirror::OnDisconnectForSlot(slot);  // phase 1: retire the leaver's trash mirrors BEFORE the generic mirror drain (else the rooted actor leaks)
    coop::trash_channel::OnGrabHolderLeft(session, static_cast<uint8_t>(slot));  // the leaver's carried clump is let go: it falls, streams, lands
    coop::broom_stroke::OnPeerLeft(static_cast<uint8_t>(slot));  // the leaver's stroke rate goes with it
    coop::kerfus_lanes::OnPeerLeft(static_cast<uint8_t>(slot));  // and the Kerfuses it turned on follow the host again (its verbs' queue goes with its slot)
    coop::dish_calib_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and its precision-intent budget
    coop::drive_payload_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and its drive rows in assembly, its brought drives
    coop::wisp_grab_hold::OnPeerLeft(static_cast<uint8_t>(slot));  // drop the leaver's grab-window puppet hold
    coop::remote_prop::OnDisconnectForSlot(slot);
    coop::prop_syncer_host::OnSlotLeft(static_cast<uint8_t>(slot));  // the rows the leaver synced end, with no word to it
    coop::element::bind_wait::OnSlotLeft(slot);  // and the messages it sent that wait for a prop to bind
    coop::item_activate::OnDisconnectForSlot(slot);
    coop::device_occupancy::OnDisconnectForSlot(slot);  // release a leaver's device claims
    coop::desk_input_sync::OnPeerLeft(slot);  // clear a leaver's dangling coordIsPing (its ping would swallow every peer's desk keys)
    coop::desk_ping_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and its unrun verdict and the scan it pinged
    coop::signal_catch_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and the catch marked as its
    coop::desk_snd_fx::OnPeerLeft(slot);  // host-owned teardown of the leaver's loop sounds (broadcast OFF)
    coop::comp_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and the refiner's decode it began is the host's
    coop::served_player::OnPeerLeft(static_cast<uint8_t>(slot));  // robots serving the leaver read the host
    coop::power_panel::OnPeerLeft(static_cast<uint8_t>(slot));  // and its presses that wait for its body
    coop::power_grid::OnPeerLeft(static_cast<uint8_t>(slot));  // and its generator ops
    coop::fuse_tower_ops::OnPeerLeft(static_cast<uint8_t>(slot));  // and its tower ops
    coop::voice_chat::OnDisconnectSlot(slot);  // drop the leaver's voice channel + icon state
    coop::sleep_sync::OnDisconnectForSlot(slot);  // the leaver's rest state back to awake, told to every client
    coop::owner_entity_sync::OnPeerLeftSlot(slot);  // destroy the leaver's owner-entity mirrors (its eyer dies with it)
    coop::hook_sync::OnPeerLeftSlot(slot);  // the leaver's OWNER-PHASE hook mirrors; anchored ones are the host's and stay
    coop::player_inventory_sync::OnDisconnectForSlot(slot);  // re-arm the on-join push; the leaver's profile stays held
}

DisconnectStats DisconnectAll() {
    // The trash mirrors first, before ForceRelease, which can consume a carried one without
    // releasing its GC pin -- and a rooted pending-kill actor anchors its world's whole Outer
    // chain. Retiring them here un-roots, destroys and evicts each drive, so ForceRelease sees no
    // live mirror and no stale drive entry.
    coop::trash_mirror::OnDisconnect();
    coop::pile_look::OnDisconnect();
    coop::pile_spawn_bind::OnDisconnect();   // this client's own parked clumps go back to the game, before ForceRelease
    coop::trash_native_hold::ReleaseUnbound();   // and the load hold's unbound ones, the same way
    coop::remote_prop::ForceRelease();
    // A disconnect mid-snapshot drops the armed claim set (dangling actor pointers must not survive
    // into the next session); no sweep.
    coop::join_membership_sweep::ResetClaimTracking();
    DisconnectStats stats;
    stats.initProcessedDropped = coop::prop_lifecycle::OnDisconnect().initProcessedDropped;
    stats.snapPending = coop::prop_snapshot::OnDisconnect();
    coop::join_beacon::OnDisconnect();
    coop::npc_sync::OnDisconnect();
    coop::world_actor_sync::OnDisconnect();  // drain WorldActor mirrors (K2 client ones) + clear host reverse-map
    coop::piramid_sync::OnDisconnect();  // drop pending gather + gather-edge map + restored-tick set (hooks stay latched)
    coop::geom_octa_sync::OnDisconnect();  // the host's verdict counts and the counters (the watches stay latched)
    coop::npc_adoption::OnSessionEnd();  // drop pending deferred adoptions + reset latches
    coop::kerfur_prop_adoption::OnSessionEnd();  // drop pending prop-form kerfur adoptions
    coop::dev::dev_lanes::EndSession();  // [dev] the probes' verdicts, before the prop seams reset below, then each drill back to its first step
    coop::prop_drop_intent::Reset();  // clear the client park set + pending places
    coop::prop_spawn_authoring::Reset();  // drop the menu bracket + the resolved spawn verbs
    coop::host_spawn_watcher::OnDisconnect();  // drop the ambient-prop death-watch list
    coop::kerfur_convert::OnDisconnect();  // clear the open conversion verbs
    coop::kerfur_form_assembler::OnDisconnect();  // dump the containment SUMMARY (always)
    // The session's hold on the script gate ends with the session's state here -- the one release path,
    // reached from net_pump::TearDownSession -- so the lanes below tear down as they always have; a
    // session that lives on (a host whose last client left) takes it again on its next tick.
    ReleaseSessionGateHold();
    coop::kerfur_entity::OnDisconnect();  // clear the KerfurId table + free its reserved host ids
    coop::kerfur_command::OnDisconnect();  // drop pending menu commands
    coop::remover_request::OnDisconnect();  // drop the remover queues and the shown notices
    coop::creature_hit_intent::OnDisconnect();  // forget the once-per-session log latches
    coop::prop_act_host::OnDisconnect();  // drop the take-or-use tallies, refusals and reservations
    coop::prop_act_request::OnDisconnect();  // drop the pending take or use, its latch and its expired records
    coop::kerfur_menu_input::OnDisconnect();  // drop the cached session (the InpActEvt_use observer stays registered)
    coop::prop_stick_sync::OnDisconnect();  // drop commit-pending stick records
    coop::sleepingbag_sync::OnDisconnect();
    coop::item_activate::OnDisconnect();
    coop::weather_sync::OnDisconnect();
    coop::explosion_sync::OnDisconnect();  // the class table and the counters, and the request lane's queues, shot tables and client state
    coop::explosion_parking::OnDisconnect();  // the counters, after a summary
    coop::interactable_sync::OnDisconnect();
    coop::keypad_sync::OnDisconnect();
    coop::time_sync::OnDisconnect();
    coop::sky_sync::OnDisconnect();
    coop::power_panel::OnDisconnect();
    coop::power_grid::OnDisconnect();
    coop::fuse_tower_ops::OnDisconnect();
    coop::fuse_tower_rows::OnDisconnect();
    coop::atv_sync::OnDisconnect();
    coop::drone_sync::OnDisconnect();
    coop::order_sync::OnDisconnect();
    coop::coingun_sync::OnDisconnect();  // dump the lane summary + drop the sold-set, the barrier queue and the cached gun
    coop::firefly_sync::OnDisconnect();
    coop::event_cue_sync::OnDisconnect();  // drop the session the cue watch sends on
    coop::event_fire_sync::OnDisconnect();  // restore the client scheduler (allEvents.Num) + drop the queued fires
    coop::event_active_sync::OnDisconnect();  // drop the begun-events map and its world stamp
    coop::creature_event_relay::OnDisconnect();  // a client's applied creature events back out; both halves' state; the session
    coop::npc_mirror_brain::OnDisconnect();  // what the graph watches refused, and every mark
    coop::alarm_sync::OnDisconnect();  // drop the cached trigger + poll baseline
    coop::serverbox_sync::OnDisconnect();  // the poll baseline and the session
    coop::server_upgrade_sync::OnDisconnect();  // verb snapshots + parked canonical + deny records
    coop::floppy_slot_sync::OnDisconnect();  // drop the slot shadows, the retry set and the per-sender rate windows
    coop::floppy_slot_entry::OnDisconnect();  // counters, then the transit marks and the interceptors: the line above drops the UFunctions they name
    coop::mirror_slot_entry::OnDisconnect();
    coop::puppet_identity::OnDisconnect();  // the resolved classes and stubs, the grunt and wisp_p windows
    coop::roach_sync::OnDisconnect();  // drop snapshot assembly + tracked set + baselines
    coop::owner_entity_sync::OnDisconnect();  // destroy ALL owner-entity mirrors (our spawned actors must not linger into SP)
    coop::hook_sync::OnDisconnect();  // same, for every hook mirror, anchored included
    coop::skin_preview::OnDisconnect();  // despawn the F1-skins mannequin
    coop::inventory_pickup_sync::OnDisconnect();
    coop::chat_sync::OnDisconnect();
    coop::command_sync::OnDisconnect();  // every command rate bucket and notice clock
    coop::stat_orders::OnDisconnect();  // every order and query still waiting is answered Left, the kept session stays
    coop::turbine_sync::OnDisconnect();
    coop::upgrade_sync::OnDisconnect();  // and its own upgrade panel buys locally again
    coop::device_occupancy::OnDisconnect();
    coop::notebook_sync::OnDisconnect();  // and its last-published text
    coop::console_state_sync::OnDisconnect();
    coop::signal_catch_sync::OnDisconnect();
    coop::prop_save_data::OnDisconnect();
    coop::laptop_sync::OnDisconnect();
    coop::laptop_tab::OnDisconnect();
    coop::portable_pc_lid::OnDisconnect();
    coop::laptop_buffer_sync::OnDisconnect();  // quad shadow + assembler
    coop::floppybox_sync::OnDisconnect();  // box shadows + taken-ring + canonical retries
    coop::props::container_contents_sync::OnDisconnect();  // dirty set + retry + parked + assembler
    coop::props::prop_record_refresh::OnDisconnect();  // per-prop coalescing state
    coop::dev::dev_lanes::RearmSelftests();  // [dev] the selftests re-arm for the next session
    coop::desk_cursor_sync::OnDisconnect();
    coop::desk_ping_sync::OnDisconnect();
    coop::desk_sim_sync::OnDisconnect();
    coop::dish_sync::OnDisconnect();  // wire-residue sweep + the disher's restore (the suppression loan)
    coop::download_arm_sync::OnDisconnect();
    coop::dish_hashcode_sync::OnDisconnect();
    coop::sat_console_sync::OnDisconnect();  // the terminals it keeps, and a busy flag mirrored here
    coop::tape_caddy_sync::OnDisconnect();  // poll baselines + IsRecent stamps + the singleton cache (no suppression -- nothing to restore)
    coop::daily_task_sync::OnDisconnect();  // change-hash baseline
    coop::desk_input_sync::OnDisconnect();
    coop::desk_snd_fx::OnDisconnect();
    coop::deck_play_sync::OnDisconnect();  // gen counters + ring + self-test latch
    coop::physmods_sync::OnDisconnect();  // verb snapshots + parked canonical + deny records
    coop::drive_sync::OnDisconnect();  // slot baselines + latch/dirty state
    coop::drive_payload_sync::OnDisconnect();  // held and sent rows, notes
    coop::drive_rack_sync::OnDisconnect();  // rack baselines/shadow + deny/taken rings
    coop::sleep_sync::OnDisconnect();
    coop::sleep_rest::OnDisconnect();  // a rest in progress goes back to factor 1
    coop::wisp_attack_sync::OnDisconnect();  // clear damage-cancel latch + handled-wisp edges + pending despawns
    coop::wisp_tear_mirror::OnDisconnect();  // clear any armed victim-death deadline
    coop::wisp_grab_hold::OnDisconnect();  // release a live self-grab (un-strand MOVE_None) and drop the holds
    coop::player_inventory_sync::OnDisconnect();  // client: clear the send-dedup and any pending apply; host: nothing
    coop::email_sync::OnDisconnect();
    coop::signal_sync::OnDisconnect();
    coop::meadow_db_sync::OnDisconnect();  // shadow + pending + tombstones + seed snapshots
    coop::comp_sync::OnDisconnect();
    coop::voice_chat::OnDisconnect();
    coop::window_sync::OnDisconnect();
    coop::window_stroke_sync::OnDisconnect();
    coop::trash_pile_sync::OnDisconnect();
    coop::broom_stroke::OnDisconnect();  // with no session the game's broom sweeps as written
    coop::pack_trash_intent::OnDisconnect();  // with no session the game's own bagging stands
    coop::verb_lanes::OnDisconnect();  // and each verb lane's counters and summary line (a sender's queue goes with its slot)
    coop::kerfus_lanes::OnDisconnect();  // and the Kerfus lanes' records and waiting states
    coop::served_player::OnDisconnect();  // and every served robot's record: its seams disarm
    coop::broom_push::OnDisconnect();  // forget pushes not yet handed on
    coop::trash_collect_sync::OnDisconnect();
    coop::trash_channel::OnDisconnect();  // drop the per-eid trash sync-time-context map
    coop::puppet_carry_drive::OnDisconnect();  // drop all puppet-held clump drives
    coop::trash_sweep::OnDisconnect();  // drop the swept clumps still rolling
    coop::trash_clump_pose_stream::OnDisconnect();  // drop all client per-eid carry drives
    coop::prop_drive_host::OnDisconnect();  // drop the host's driven-prop set and the client syncers' table (prop_syncer_host)
    coop::element::bind_wait::Clear("disconnect");  // the messages waiting for a bind go with the session
    coop::prop_drive_stream::OnDisconnect();  // every driven prop here gets its physics back
    coop::prop_park::OnDisconnect();  // after both receivers gave their props back: the holds a park still has
    coop::rider_hold::OnDisconnect();  // last: any movement still held ticks again
    coop::balance_sync::OnDisconnect();  // reset the balance broadcast dedup
    // Last, after every element drain: the client's transfer state, its received identity map and
    // the ephemeral zcoop_<pid> slot the join wrote. The map's binds resolve against Elements, so
    // it must outlive their drain; the slot file is read once at the join and never again, and a
    // leftover one only waits for the boot sweep's one-hour cutoff. On a host this finds an empty
    // buffer, an unarmed map and no such file, and clears its own per-slot streams.
    coop::save_transfer::OnDisconnect();
    // Every pin on a world-scoped object is session-scoped, so after a full teardown the
    // world-scoped pin count must be zero. A non-zero answer names the module still anchoring the
    // departing world's Outer chain, the condition that makes the next map load in this process
    // adopt an uncollected corpse and die on its null WorldSettings.
    ue_wrap::GcPin::ReportWorldScopedPins("DisconnectAll");
    return stats;
}

void TickGameplay(coop::net::Session& session, bool isConnected, bool isHost,
                  bool fleeing) {
    namespace PP = coop::dev::perf_probe;
    // Per-tick drains for subsystems that retry until the reliable channel accepts a queued
    // connect-time broadcast, and apply per-peer payloads that arrived before the puppet spawned.
    // A cheap early return when nothing is pending.
    { PP::Scope _s{PP::Bucket::ItemConnect};   coop::item_activate::TickConnect(); }
    { PP::Scope _s{PP::Bucket::WeatherConnect}; coop::weather_sync::TickConnect(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:explosion"}; coop::explosion_sync::Tick(); }  // the explosion classes: reset at a world change, resolved until all three are; the request lane's source classes (client) and its host queues
    // The ends of play the engine announced since the last tick, handed to the lanes that subscribed
    // before the hub's pass can complete: a pass prunes what has died from its consumers' indexes, and a
    // lane names an ended actor by its index (grime's position key).
    { PP::Scope _s{PP::Bucket::Interactable}; coop::element::death_seam::Drain(); }
    // The walk timer logs each sync's call over a millisecond, so a heavy one names itself; the
    // perf-probe bucket covers the whole block. The shared pass over the object index serves every
    // index consumer, and runs before the consumers' ticks so a completed pass's fresh index is
    // visible in the same pump tick.
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:scan_hub"}; coop::element::scan_hub::Tick(); }
    // The steady prop re-seed consumer registers itself once; the budget drain carries its own
    // walk-time label.
    { PP::Scope _s{PP::Bucket::Interactable};
      coop::prop_element_tracker::InstallReseedScanConsumer();
      coop::prop_element_tracker::DrainReseedQueue(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:interactable"}; coop::interactable_sync::Tick(); }  // retry deferred door/light/container applies (still streaming in)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:keypad"}; coop::keypad_sync::Tick(); }  // a keypad state that arrived before its keypad was indexed
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:time"}; coop::time_sync::Tick(); }  // the world clock: the host hands the net thread a sample when one is due; the client applies at its cycle's own tick
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sky"}; coop::sky_sync::Tick(); }  // night-sky: the eye gate's name (both roles), then the host's throttled push
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:power"}; coop::power_panel::Tick(); coop::power_grid::Tick(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:towers"}; coop::fuse_tower_ops::Tick(); coop::fuse_tower_rows::Tick(); }  // the towers: the gates settle, the host's ops and rows, a client's panels  // the panel's and the grid's gates settle; what came before the panel or the generators resolved is taken
    coop::dish_calib_sync::Tick();  // settle the precision lane's three watches
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:atv"}; coop::atv_sync::Tick(); }  // ATV: the host judges its own seat and grab, a client asks the host; the syncer streams its pose (host+client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drone"}; coop::drone_sync::Tick(); }  // delivery drone: host streams transform / client suppresses tick + mirrors
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:turbine"}; coop::turbine_sync::Tick(); }  // wind turbines: host ~1 Hz driver-float poll / client deferred-apply retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:alarm"}; coop::alarm_sync::Tick(); }  // base radar alarm: 1 Hz active-bit poll BOTH roles (host broadcasts transitions; client forwards local ones)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:server"}; coop::serverbox_sync::Tick(); }  // the server boxes: the watches settled, then the HOST's 1 Hz poll -> broadcast on change
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:server_upgrade"}; coop::server_upgrade_sync::Tick(); }  // a parked canonical, applied once the servers resolve
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:slot"}; coop::floppy_slot_sync::Tick(); coop::mirror_slot_entry::Tick(); }  // device slots: 1 Hz digest-gated poll -> HOST canonical, CLIENT claim; the mirror entry's watches settle
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"puppet_identity.settle"}; coop::puppet_identity::Tick(); }  // the watches settle, the per-world pointers follow the class set
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:roach"}; coop::roach_sync::Tick(); }  // roach infestation: HOST 1 Hz population poll -> paged broadcast; CLIENT liveness-scan -> consumption intents
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:owner_entity"}; coop::owner_entity_sync::Tick(); }  // owner-entity: 4 Hz own-pose stream + keepalive + death-watch + mirror prune
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:hook"}; coop::hook_sync::Tick(); }  // hook: one activeHook read, then a gated 4/20 Hz head poll only while a hook exists
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:explosion_coast"}; coop::explosion_parking::Tick(&session); }  // HOST: the props a blast flung, handed to the driven-prop channel (before its tick below; an empty list costs one size check)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:prop_drive_host"}; coop::prop_drive_host::Pump(session, isHost); }  // the driven props, both roles: HOST judges a syncing client's rows, publishes the props that moved, closes the streams that rested (after the hook poll that feeds it); then both park + drive what the host streams (an empty set costs one size check)
    coop::dev::dev_lanes::TickDrills(session);  // [dev] the drills' and censuses' legs (a single read each when off)
    coop::player_damage::Tick();  // impact-entry PRE cancels lazy install (non-local bodies)
    coop::player_handshake::TickSkinConverge();  // heal a join-window deferred skin apply (~2 s throttle)
    coop::skin_preview::Tick();  // F1-skins live mannequin preview (spawn/apply/position/hide)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:laptop_tab"}; coop::laptop_tab::Tick(session); }  // the tab watches until live, the refusal window, and a tab report owed by its pace; BEFORE the occupancy poll, so a leaving user's last tab goes ahead of the claim's release
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:device_occupancy"}; coop::device_occupancy::Tick(); }  // device occupancy: activeInterface edge poll + pending claim retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:console_state"}; coop::console_state_sync::Tick(); }  // signal-catcher: host sky poll / client mirror sweep / desk + dish owner streams
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal_catch"}; coop::signal_catch_sync::Tick(); }  // the catch and cleared detectors, 1 Hz
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:propsave"}; coop::prop_save_data::Drive(); }  // budgeted park applies + the 1 Hz assembler sweep
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:laptop"}; coop::laptop_sync::Tick(); }  // PC power edge poll (4 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pc_lid"}; coop::portable_pc_lid::Tick(); }  // the lid's watch until live, then the held lid edges (1 Hz while any)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:laptop_quad"}; coop::laptop_buffer_sync::Tick(); }  // quad int pre-filter poll (4 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:floppybox"}; coop::floppybox_sync::Tick(); }  // box sweep (1 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:containerContents"}; coop::props::container_contents_sync::Tick(); }  // edge-driven dirty drain (4 Hz gate)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:containerView"}; coop::props::container_view_close::Tick(); }  // a client's view into a container, closed out of reach
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_cursor"}; coop::desk_cursor_sync::Tick(); }  // coords-panel live cursor -- holder streams viewCoordinate / mirror interpolates (50ms) + WriteCursorOnly
    { PP::Scope _s{PP::Bucket::Interactable}; coop::desk_ping_sync::Tick(); }  // the ping lane's watches until live
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_sim"}; coop::desk_sim_sync::Tick(); }  // download-SIM -- host streams outputs (10Hz) / client interpolates + WriteSimOutputs
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:dish"}; coop::dish_sync::Tick(); }  // host pose sweep (4Hz) / client apply + park latch / calib diff-poll (1Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; coop::download_arm_sync::Tick(); }  // the desk watches' name resolve
    { PP::Scope _s{PP::Bucket::Interactable}; coop::dish_hashcode_sync::Tick(); }  // host: the marked codes, a joiner's owed set / client: rows waiting for their dish
    { PP::Scope _s{PP::Bucket::Interactable}; coop::sat_console_sync::Tick(); }  // owed blobs, and a terminal whose world is gone
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:reel"}; coop::tape_caddy_sync::Tick(); }  // 4Hz slot sentinel poll (both peers) + host 1Hz corrector / client exact-snap apply
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:task"}; coop::daily_task_sync::Tick(); }  // host 1Hz taskNew change-hash poll (fires a few times per game-day)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_input"}; coop::desk_input_sync::Tick(); }  // 250ms input-field poll -> claim-free DeskInput deltas + cooldown charge/scan classification
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_snd"}; coop::desk_snd_fx::Tick(); }  // audio-seam ring flush + lazy hook install + pending loop retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:deck_play"}; coop::deck_play_sync::Tick(); }  // deck playback ring flush + lazy Deactivate/fin seam install + gen author
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:physmods"}; coop::physmods_sync::Tick(); }  // parked-canonical apply at desk resolve
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive"}; coop::drive_sync::Tick(); coop::drive_payload_sync::Tick(); }  // the slot lane's drain and sweep; the rows' enrolments
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive_rack"}; coop::drive_rack_sync::Tick(); }  // rack barrier drain + 1 Hz sweep
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:email"}; coop::email_sync::Tick(); }  // email shadow poll (1 Hz; appends -> chunked broadcast, shrinks -> content-keyed deletes)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal"}; coop::signal_sync::Tick(); }  // saved-signals shadow poll (same shape on gamemode.savedSignals_0)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:meadow"}; coop::meadow_db_sync::Tick(); }  // meadow-DB: the writers' watches until settled, the pen's drain once the database is back, then retries (1 Hz) while a line, a tombstone or an owed order waits
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:comp"}; coop::comp_sync::Tick(); }  // the refiner: the host's stream and comp_data edges, the mirror's world edges
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:voice"}; coop::voice_chat::Tick(); }  // voice frame pump (mic drain -> send; inbox -> jitter; positions; state edges)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:order"}; coop::order_sync::Tick(); }  // drone economy: host commits assembled client orders; the queue mirror's retry
    // The barrier for coingun_sync: the client's own coins are captured inside the gun's verb
    // bracket (reads only; an engine call mid-bytecode corrupts) and destroyed here, one pump tick
    // later, where a dispatch is safe.
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:coingun"}; coop::coingun_sync::Tick(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:window"}; coop::window_sync::Tick(); }  // base-window clean: deferred-apply retry (wipes go at the verb)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:window_stroke"}; coop::window_stroke_sync::Tick(); }  // bay-window dabs: send observed, replay received
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:grime"}; coop::grime_sync::Tick(); }  // surface grime: the lowering verbs' watches + the end-of-play subscription, until they take
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:movement_ledger"}; coop::movement_ledger::Tick(session); }  // HOST: throttled per-slot summary + the wire-vs-actor divergence sample (an ENGINE read, hence game thread)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:npc_host"}; coop::npc_sync::TickPoseStream(); }  // HOST: read NPCs -> publish EntityPose batch (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:npc_client"}; coop::npc_mirror::TickClientNpcs(); }  // CLIENT: apply batch + drive mirror interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:worldactor_host"}; coop::world_actor_sync::TickPoseStream(); }  // HOST: read event WorldActors -> publish WorldActorPose batch (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:worldactor_client"}; coop::world_actor_sync::TickClientWorldActors(); }  // CLIENT: apply batch + drive WorldActor mirror interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:octa_relay"}; coop::geom_octa_relay::Tick(); }  // CLIENT: the Octa's held Aggro and paired verdicts, replayed (a role test when idle)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:piramid"}; coop::piramid_sync::Tick(); }  // pre-arm probe (250 ms gate) / host gather-edge sweep (1 s) / client mirror restore + pending gather replay
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:trash_clump_pose"}; coop::trash_clump_pose_stream::TickApplyAndDrive(session); }  // CLIENT: apply host-auth carry/flight pose batch + per-eid interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::host_spawn_watcher::TickWatchedProps(&session); }  // ambient-prop (pinecone) SetLifeSpan-expiry / consumption despawn -> PropDestroy(eid)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::host_spawn_watcher::DrainPendingSpawns(&session); }  // adopt+express FinishSpawningActor Func-seam spawns (R-drop/place/Q-menu) one tick after Finish (key restored, hand actor excluded)
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; coop::broom_push::Tick(); }  // HOST: stream the props and clumps a broom stroke pushed (AFTER the drain above, which names the trash that same stroke dispensed)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_drop_intent::Tick(&session); }  // CLIENT: author a PropDropIntent for a detected place whose Key is parked (cheap no-op when empty / on host)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_form_assembler::Tick(); }  // GT FName-resolve the 2 verbs + bind the containment seams (latches once; no-op after)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_command::Tick(); }  // menu commands; HOST: each sender's queued request, paced
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::remover_request::Tick(); }  // the init watch's settle; HOST: each sender's queued remover press, judged and paced
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::creature_hit_intent::Tick(); }  // the addDamage watch's settle; HOST: each sender's queued creature hit, judged and paced
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_act_host::Tick(); }  // HOST: each sender's queued take or use, judged and paced
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_act_request::Tick(); }  // the take-or-use watches' settle; CLIENT: the pending take or use's wait bound
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_convert_host::Tick(); }  // HOST: each sender's queued kerfur conversion, paced
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_stick_sync::Tick(); }  // broadcast recorded stick commits NOW -- must precede local_streams' release edge (net_pump runs TickGameplay first)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pause_guard"}; coop::pause_guard::Tick(isConnected); }  // coop no-pause invariant -- un-pause the world while connected (ESC menu stays usable; a paused peer froze its pose stream)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:run_end"}; coop::player::run_end_travel::Tick(); }  // the run-ending seam's session-scoped work: publish the seam's readiness (the verdict itself runs in the VM's body loop)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:save_cycle_off"}; coop::save_block::Tick(&session); }  // client native save-cycle OFF -- hold gamemode.disableSave=true (saveSlot_C::save gates gather+write on it); the SaveGameToSlot disk hook stays as the belt
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sleep"}; coop::sleep_sync::Tick(); }  // the local rest-state poll + its send + the client's dream hold
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sleep_rest"}; coop::sleep_rest::Tick(); }  // a client's rest: the sleep watch until live, then the effects' factor and food and coffee while it lasts
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:wisp_attack"}; coop::wisp_attack_sync::Tick(); }  // host detect wisp-grabs-client -> neutralize + relay (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:wisp_tear"}; coop::wisp_tear_mirror::Tick(); }  // discharge the victim's scheduled ragdoll death (any peer, no-op until armed)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:player_inventory"}; coop::player_inventory_sync::Tick(); }  // the client's profile stream / the host's on-join push (+ the inventory_selftest=1 read-verify)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:hotbar_icons"}; coop::hotbar_icon_edge::Tick(); }  // the game publishes its prop-icon tables AFTER its own post-load refresh of the quick-slot bar and tells only the equipment panel -- rebuild the bar once per world when they arrive
    coop::dev::dev_lanes::TickInventory(session);  // [dev] the live store readout and the pickup drill, each in its perf bucket
    // The trash pile collect-counter poll and depletion death-watch; a chipPile re-grab fires from
    // the use-press observer that trash_collect_sync installs, not from a per-tick liveness sweep.
    { PP::Scope _s{PP::Bucket::TrashWatch};
      const bool inTransition = fleeing || coop::join_progress::Active();
      coop::trash_pile_sync::Tick(inTransition); }  // counter poll + depletion death-watch (transition-gated)
    if (isHost) { PP::Scope _s{PP::Bucket::TrashWatch};
      coop::trash_channel::TickCarry(session, coop::local_streams::LastHeldActor());  // birth-cert prune + land-settle commit + guaranteed carry termination (dead/rest lanes close)
      coop::trash_sweep::Tick(session, coop::players::Registry::Get().Local());  // open the clumps a broom stroke made or pushed + publish their roll (AFTER TickCarry so the latch is current; reads the local hand live)
      coop::puppet_carry_drive::Tick(session); }  // drive each puppet-held clump to its hand + publish the host-auth carry/flight pose batch (AFTER TickCarry so the latch is current)
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; coop::broom_stroke::Tick(session); }  // HOST: run the clients' queued broom strokes, one a tick a client (last: what a stroke spawns, sweeps and pushes is picked up next tick, as for the host's own)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pack_trash"}; coop::pack_trash_intent::Tick(session); }  // client: spend the tools its presses sent; HOST: run one queued pack a tick a client
    { PP::Scope _s{PP::Bucket::Interactable}; coop::verb_lanes::Tick(session); }  // settle each verb lane's watches; HOST: run their queued intents
    { PP::Scope _s{PP::Bucket::Interactable}; coop::sleepingbag_sync::Tick(session); }  // settle the gates and serve bounded fold/unfold intents
    { PP::Scope _s{PP::Bucket::Interactable}; coop::kerfus_lanes::Tick(session); }  // the Kerfus lanes' watches; CLIENT: waiting states; HOST: queued verbs
    { PP::Scope _s{PP::Bucket::Interactable}; coop::served_player::Tick(); }  // HOST: the served robots' seams
    { PP::Scope _s{PP::Bucket::Balance};       coop::balance_sync::Tick(); }  // host polls saveSlot.Points + broadcasts on change; client retries the pending mirror apply
    { PP::Scope _s{PP::Bucket::Balance};       ue_wrap::ScopedWalkTimer _w{"sync:upgrades"}; coop::upgrade_sync::Tick(session); }  // host polls the upgrade struct + broadcasts on change, and runs one queued purchase a tick a client; client retries the pending mirror
    coop::dev::dev_lanes::TickProbes(session, isConnected, isHost);  // [dev] the RE probes, each self-installing when its knob is set
}

}  // namespace coop::subsystems
