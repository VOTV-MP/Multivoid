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
#include "coop/interactables/deck_play_sync.h"  // deck playback lane
#include "coop/interactables/physmods_sync.h"  // desk physical-modules lane
#include "coop/items/order_queue_sync.h"  // the delivery order queue, the host's, mirrored
#include "coop/interactables/drive_sync.h"  // drive chain (slots + payloads)
#include "coop/interactables/drive_rack_sync.h"  // rack storage
#include "coop/interactables/meadow_db_sync.h"  // meadow signal-DB mirror (multiset shadow + join seed)
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/desk_sim_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/tape_caddy_sync.h"
#include "coop/world/daily_task_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/world/email_sync.h"
#include "coop/interactables/laptop_sync.h"  // the stationary PC lane
#include "coop/interactables/laptop_buffer_sync.h"  // the PC buffer quad lane
#include "coop/interactables/portable_pc_lid.h"  // the portable PC's lid lane
#include "coop/interactables/floppy_slot_entry.h"
#include "coop/interactables/floppy_slot_sync.h"
#include "coop/interactables/floppybox_sync.h"  // the disc crate LIFO lane
#include "coop/props/container_contents_sync.h"  // the world-container GObjStack slice
#include "coop/props/container_view_close.h"  // a view into a container ends where the player's reach does
#include "coop/props/prop_food_state.h"  // the food family's live state and who authors it
#include "coop/props/prop_record_refresh.h"  // a drive box or reel case republishes its record in place
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/player/movement_ledger.h"
#include "coop/player/run_end_travel.h"
#include "coop/player/hand_item.h"  // hotbar hand-item display axis (connect replay)
#include "coop/player/local_body.h"  // skins: local first-person body owner
#include "coop/player/nameplate.h"  // plate-pref session wiring (Install)
#include "coop/player/nick_color.h"  // nick-color session wiring (Install)
#include "coop/player/sleep_sync.h"
#include "coop/creatures/wisp_attack_sync.h"  // Killer Wisp coop: host detect + neutralize + relay
#include "coop/creatures/wisp_grab_hold.h"  // Killer Wisp: grab-window body placement (per-slot/full teardown)
#include "coop/creatures/wisp_tear_mirror.h"  // Killer Wisp coop: victim kill + tear mirror
#include "coop/session/rig_ready.h"
#include "coop/session/pause_guard.h"  // coop no-pause invariant (ESC pause froze clients)
#include "coop/items/player_inventory_sync.h"  // per-player inventory (host file scaffold)
#include "coop/voice/voice_chat.h"
#include "coop/element/death_seam.h"  // an element's end of play, handed to the lanes that subscribed
#include "coop/props/host_spawn_watcher.h"  // HOST mirror of those ambient spawner outputs (the pinecone scare)
#include "coop/props/prop_drop_intent.h"  // client-place -> host-auth keyed-prop DROP INTENT
#include "coop/props/prop_spawn_authoring.h"  // a PLAYER's spawn verb vs the world's own spawns
#include "coop/creatures/kerfur_convert.h"  // host-authoritative kerfur on/off conversion (the dupe fix)
#include "coop/creatures/kerfur_command.h"  // host-authoritative kerfur menu command relay + ownership follow
#include "coop/creatures/kerfur_menu_input.h"  // client radial-menu verb detection (InpActEvt_use PRE -> kerfur_command relay)
#include "coop/creatures/kerfur_entity.h"  // the stable-KerfurId authority table
#include "coop/creatures/kerfur_form_assembler.h"  // script-body gate consumer (observe-only + containment counter)
#include "coop/props/prop_stick_sync.h"  // wall-attachable stick mirror (camera-on-wall)
#include "coop/session/teleport_client.h"  // TeleportSlotToHost: the admin bring-to-host action
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
#include "coop/world/event_fire_sync.h"
#include "coop/world/firefly_sync.h"
#include "coop/items/inventory_pickup_sync.h"
#include "coop/comms/chat_sync.h"
#include "coop/interactables/upgrade_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/interactables/power_sync.h"
#include "coop/world/sky_sync.h"
#include "coop/world/time_sync.h"
#include "coop/interactables/window_stroke_sync.h"
#include "coop/interactables/window_sync.h"
#include "coop/session/join_progress.h"
#include "coop/interactables/garbage_sync.h"
#include "coop/props/pile_look.h"
#include "coop/props/trash_channel.h"
#include "coop/player/local_streams.h"  // LastHeldActor
#include "coop/player/puppet_carry_drive.h"
#include "coop/props/trash_clump_pose_stream.h"
#include "coop/props/trash_sweep.h"
#include "coop/props/prop_drive_host.h"    // HOST: the props a hook drags, streamed while they move
#include "coop/props/prop_drive_stream.h"  // CLIENT: park and drive those props
#include "coop/props/trash_collect_sync.h"
#include "coop/items/broom_stroke.h"
#include "coop/props/pack_trash_intent.h"
#include "coop/interactables/verb_lanes.h"  // the interactable lanes on a device's own verbs, at the script gate
#include "coop/creatures/kerfus_lanes.h"  // the plain kerfur: its brain on the host, its state and drive, its verbs
#include "coop/items/broom_push.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/save/save_block.h"
#include "coop/save/save_button_disable.h"
#include "coop/props/grab_observer.h"
#include "coop/player/item_activate.h"
#include "coop/player/player_damage.h"
#include "coop/session/player_handshake.h"  // TickSkinConverge
#include "coop/player/skin_preview.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_sync.h"
#include "coop/creatures/npc_world_enum.h"  // RegisterExistingWorldNpcs
#include "coop/world/world_actor_sync.h"  // non-Character event-actor transform mirror (sibling of npc_sync)
#include "coop/creatures/piramid_sync.h"  // piramid event choreography lane (mirror brain suppression + PyramidGather)
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
#include "coop/creatures/roach_sync.h"  // host-authoritative roach-infestation mirror
#include "coop/creatures/owner_entity_sync.h"
#include "coop/items/hook_anchor.h"
#include "coop/items/hook_sync.h"  // the hook lane: owner-phase stream, mirrors, the tick park
#include "coop/world/event_active_sync.h"  // the native activeEvents registry probe
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
}

void ReleaseSessionGateHold() {
    if (!g_sessionHoldsGate) return;
    ue_wrap::script_gate::Release("the coop session");
    coop::element::death_seam::Release("the coop session");
    g_sessionHoldsGate = false;
}

void Install(coop::net::Session& session) {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&session);
    coop::prop_lifecycle::Install(&session);
    coop::npc_sync::Install(&session);
    coop::world_actor_sync::Install(&session);  // non-Character event-actor mirror (2nd BeginDeferred interceptor, disjoint allowlist)
    coop::piramid_sync::Install(&session);  // piramid event choreography lane (hooks arm lazily on the first piramid element)
    coop::item_activate::Install(&session);  // flashlight
    coop::player_damage::Install(&session);  // vitals damage relay (send + owner-apply)
    coop::weather_sync::Install(&session);  // weather
    coop::interactable_sync::Install(&session);  // doors + lights + container lids
    coop::keypad_sync::Install(&session);  // the password keypads' index and wire (its own module)
    coop::time_sync::Install(&session);  // host-authoritative world clock (time-of-day / dark-world fix)
    coop::sky_sync::Install(&session);  // host-authoritative night-sky orientation, moon phase and eye
    coop::power_sync::Install(&session);  // base power-panel breakers (its own module -- 5 bools)
    coop::atv_sync::Install(&session);  // ATV body pose (occupant-authoritative keyed stream)
    coop::drone_sync::Install(&session);  // delivery drone body pose (host-authoritative singleton)
    coop::order_sync::Install(&session);  // delivery-drone economy: client->host shop-order forward
    coop::coingun_sync::Install(&session);  // the sell gun + host-minted coins
    coop::firefly_sync::Install(&session);  // peer-symmetric ambient firefly mirror (each peer captures+shares its own)
    coop::event_cue_sync::Install(&session);  // cosmetic emitter cues: the host's runEvent watch sends them, the client replays
    coop::event_fire_sync::Install(&session);  // scheduled events: the host's runEvent watch (settime caller) -> EventFire; client hold + policy replay
    coop::event_active_sync::Install(&session);  // the host's setEvent watch: begin/end edges; the join snapshot reads the game's registry
    coop::alarm_sync::Install(&session);  // base radar alarm shared-world toggle (a 1 Hz active poll on both roles)
    coop::serverbox_sync::Install(&session);  // signal-server sim state: host polls+broadcasts, client drive-reals + kills its ticker_serverBreaker
    coop::floppy_slot_sync::Install(&session);  // a disc-holding device's slot: host-canonical, a peer claims the outcome of its own insert or eject
    coop::floppy_slot_entry::Install(&session);  // the slot's overlap entry: no device swallows a disc still in transit out of one
    coop::roach_sync::Install(&session);  // roach infestation: host paged snapshots, client ordinal apply + consumption intents
    coop::owner_entity_sync::Install(&session);  // owner-entity lane: eyer per-peer owned + cross-peer display mirrors
    coop::hook_sync::Install(&session);  // hook lane: owner-phase stream + mirrors, and the ReceiveTick park they depend on
    coop::inventory_pickup_sync::Install(&session);  // inventory-collect blip (PlaySound2D observer)
    coop::chat_sync::Install(&session);  // T-chat (the ui/chat_input send path)
    coop::local_body::Install(&session);  // skins: local first-person body + SkinChange announce
    coop::local_body::Tick();  // applies the persisted skin to the local pawn + 1 Hz convergence
    coop::nameplate::Install(&session);  // plate-pref announce path (F1 checkbox -> NameplateChange)
    coop::nick_color::Install(&session);  // nick-color announce path + local-slot mirror refresh
    coop::turbine_sync::Install(&session);  // wind-turbine facing/spin mirror (host-auth ~1 Hz)
    coop::upgrade_sync::Install(&session);  // a client's upgrade purchase is charged and written by the host
    coop::device_occupancy::Install(&session);  // enterable-device occupancy (busy claim + E deny gate)
    coop::console_state_sync::Install(&session);  // signal-catcher state mirror (sky signals + desk + dish aim)
    coop::signal_catch_sync::Install(&session);  // the signal-catch consume replay (dish slew + downloader arm on every peer)
    coop::laptop_sync::Install(&session);  // the stationary PC power + floppy lane
    coop::portable_pc_lid::Install(&session);  // the portable PC's lid, on its own verb
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
    coop::drive_sync::Install(&session);  // drive-chain lanes (verb dirty-marks + sweeps; owns ALL chain verb watches)
    coop::drive_rack_sync::Install(&session);  // rack storage lane (marks forwarded from drive_sync)
    coop::desk_sim_sync::Install(&session);  // download-SIM host-authoritative output stream (decoded/needle/rate/frData/poData/offsets; client overwrites)
    coop::dish_sync::Install(&session);  // host-auth dish pose mirror + host-polarity ARM edge + symmetric calibration lane (client sim parked)
    coop::tape_caddy_sync::Install(&session);  // caddy reel slots (presser edges) + host accrual corrector (client accrual NOT parked -- corrector-bounded)
    coop::daily_task_sync::Install(&session);  // saveSlot.taskNew host mirror (rollover/sell are host-only live)
    coop::email_sync::Install(&session);  // meadow-PC email mirror (watermark -> chunked rows -> addEmail)
    coop::signal_sync::Install(&session);  // desk signal-library mirror (savedSignals_0 shadow/diff)
    coop::meadow_db_sync::Install(&session);  // meadow-DB mirror (content-hash multiset + id-preserving replay)
    coop::comp_sync::Install(&session);  // refiner decode pane (single-simulator stream + passive mirrors)
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
    coop::kerfus_lanes::Install(session);  // the plain kerfur: a client refuses its brain and its verbs, the host sends its state
    coop::kerfur_command::Install(&session);  // host-authoritative kerfur menu command relay + ownership-aware Follow
    coop::kerfur_menu_input::Install(&session);  // client radial-menu verb detect (InpActEvt_use PRE -- the actionName dispatch is PE-invisible) -> kerfur_command relay
    coop::kerfur_form_assembler::Install(&session);  // script-body gate consumer: watch the two conversion verbs
    coop::prop_stick_sync::Install(&session);  // wall-attachable stick mirror (camera-on-wall -- commit observer -> PropStickState; receiver replays forceStick)
    coop::sleep_sync::Install(&session);  // the Minecraft sleep gate (isSleep edge poll -> host tally -> accelerate/end phases)
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

namespace {
// ConnectReplayForSlot is the host's response to ClientWorldReady, which a client re-fires on
// every world-change re-seed (the join double-load and a mid-session level travel), so the host
// re-asserts world state into the client's reloaded world; its keyless chipPiles, eid-only,
// re-acquire their host eid that way. The replay is idempotent (adopt snapshots, deduplicated
// mirror registration), so re-running it is correct.
}  // namespace

void ConnectReplayForSlot(int slot) {
    if (slot < 1 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    UE_LOGI("net: slot %d world-ready -- replaying snapshot + flashlight + weather + peer states", slot);
    coop::rig_ready::Say("peer-world-ready", slot);
    // Before the snapshot, explicit per-key destroys for props this joiner's blob had that the
    // host's live world no longer has (grabbed or destroyed during the download and load), so the
    // client drops exactly those rather than the divergence sweep inferring the deletes. On the
    // bulk lane ahead of the snapshot, so the removes land before the adds. A no-op for a joiner
    // with no blob baseline, which the sweep still owns.
    coop::join_window_baseline::SendDivergenceDeletes(slot);
    coop::prop_snapshot::TriggerForSlot(slot);
    coop::prop_drive_host::OnPeerWorldReady();  // every driven prop's pose again, so the joiner parks the resting ones the delta gate would never send it
    coop::kerfus_lanes::OnPeerWorldReady(slot);  // and every Kerfus's on, charging and energy
    // Deliver the current position of any save-authoritative chipPile the host moved in this
    // joiner's connect window (the move's convert was dropped pre-world, and chipPiles carry no
    // position in the snapshot). After the snapshot, so it rides the bulk lane behind it; the
    // client snaps the bound native at quiescence.
    coop::join_window_baseline::FlushDivergedPositions(slot);
    coop::item_activate::QueueConnectBroadcastForSlot(slot);
    coop::weather_sync::QueueConnectBroadcastForSlot(slot);
    coop::interactable_sync::QueueConnectBroadcastForSlot(slot);  // door/light/container states
    coop::keypad_sync::QueueConnectBroadcastForSlot(slot);  // keypad states
    coop::sky_sync::QueueConnectBroadcastForSlot(slot);  // night-sky orientation, moon phase, eye
    coop::power_sync::QueueConnectBroadcastForSlot(slot);  // base power-panel breakers
    coop::atv_sync::QueueConnectBroadcastForSlot(slot);  // ATV body pose (adopt=1)
    coop::drone_sync::QueueConnectBroadcastForSlot(slot);  // delivery drone pose (adopt=1)
    coop::order_queue_sync::QueueConnectBroadcastForSlot(slot);  // the delivery order queue: a reset + every queued order
    coop::turbine_sync::QueueConnectBroadcastForSlot(slot);  // wind-turbine facing/spin snap
    coop::device_occupancy::QueueConnectBroadcastForSlot(slot);  // live device claims (busy table)
    coop::console_state_sync::QueueConnectBroadcastForSlot(slot);  // sky-signal snapshot + desk adopt
    coop::desk_input_sync::SeedPingAttributionFromMachine();  // a SOLO host's ping edge is absorbed unwired (PollOnce gated on connected) -- re-derive from ground truth so a mid-ping joiner gets the FSM-hold
    coop::desk_snd_fx::QueueConnectBroadcastForSlot(slot);  // desk loop-sound ground truth (a mid-loop joiner gets the ON)
    coop::physmods_sync::QueueConnectBroadcastForSlot(slot);  // canonical module array (ground truth over save drift)
    coop::drive_sync::QueueConnectBroadcastForSlot(slot);  // slot lines + drive payloads
    coop::drive_rack_sync::QueueConnectBroadcastForSlot(slot);  // rack canonicals (AFTER the payloads -- the shipped seed order on the one pinned lane)
    coop::meadow_db_sync::QueueConnectBroadcastForSlot(slot);  // the seedDelta(h) join seed (blob-instant snapshot vs live)
    coop::signal_sync::QueueConnectBroadcastForSlot(slot);  // the join-window saved-signal seed, both signs
    coop::email_sync::QueueConnectBroadcastForSlot(slot);  // the join-window email seed, both signs
    coop::signal_catch_sync::QueueConnectBroadcastForSlot(slot);  // the in-flight catch replay (the identity half) and the kind-2 state seed
    coop::laptop_sync::QueueConnectBroadcastForSlot(slot);  // PC power/slot state + the slot's content (ground truth)
    coop::portable_pc_lid::QueueConnectBroadcastForSlot(slot);  // every open portable PC lid
    coop::laptop_buffer_sync::QueueConnectBroadcastForSlot(slot);  // the canonical quad (in-lane after the op=3 + slot content)
    coop::floppybox_sync::QueueConnectBroadcastForSlot(slot);  // one canonical per live box
    coop::props::container_contents_sync::QueueConnectBroadcastForSlot(slot);  // one slice per live world container (principle 8 anchor over the join snapshot)
    coop::dish_sync::QueueConnectBroadcastForSlot(slot);  // dish snapshot + (if armed) the DishArm row -- AFTER the desk rows + the kind=0 catch row (same ordered lane)
    coop::sleep_sync::QueueConnectBroadcastForSlot(slot);  // a joiner arrives awake -- end a running accelerate + re-tally
    coop::comp_sync::QueueConnectBroadcastForSlot(slot);  // decode-pane adopt (CompState + CompData)
    coop::voice_chat::ReplayPeerStatesToSlot(slot);  // voice mute/disabled states -> joiner
    coop::window_sync::QueueConnectBroadcastForSlot(slot);  // base-window clean (adopt=1)
    coop::grime_sync::QueueConnectBroadcastForSlot(slot);  // surface grime process (adopt=1)
    coop::trash_pile_sync::QueueConnectBroadcastForSlot(slot);  // pile counters (adopt=1) + depleted-key replay
    coop::npc_world_enum::RegisterExistingWorldNpcs(coop::npc_world_enum::NpcEnumOrigin::ConnectEdge);  // pre-existing/level-load NPCs (the save's kerfur) -> joiner adopts its twin
    coop::npc_sync::QueueConnectBroadcastForSlot(slot);  // existing NPCs -> joiner
    coop::world_actor_sync::QueueConnectBroadcastForSlot(slot);  // existing event WorldActors -> joiner
    coop::balance_sync::SendCurrentToSlot(slot);  // host's current balance
    coop::upgrade_sync::SendCurrentToSlot(slot);  // and the upgrade levels: the joiner's save was taken at the handshake
    // The host-to-client apply-blob push is not here: this fires at ClientWorldReady, after the
    // joiner loaded its world, so the blob would miss the pre-materialise hook; the host tick's
    // connect-edge detector drives it pre-world instead. No join teleport here either: the
    // joiner places itself at its world appearance, and a host teleport at world-ready
    // overwrote it, since world-ready arrives after the client has placed itself. The teleport
    // helper stays for the admin bring-to-host action. Then catch the new client up to the
    // existing peers' item state.
    coop::item_activate::ReplayPeerStatesToSlot(slot);
    // Existing peers' hand items to the joiner, as display mirrors.
    coop::hand_item::ReplayPeerStatesToSlot(slot);
    // One EventSnapshot per in-flight registry entry; the joiner replays the replay-safe rows with
    // the active override.
    coop::event_active_sync::SendJoinSnapshotForSlot(slot);
    // The alarm lane's late-join answer: the current alarm state, unconditionally, so a mid-alarm
    // joiner starts its klaxon on arrival.
    coop::alarm_sync::QueueConnectBroadcastForSlot(slot);
    coop::serverbox_sync::QueueConnectBroadcastForSlot(slot);  // current server state to the joiner
    coop::floppy_slot_sync::QueueConnectBroadcastForSlot(slot);  // every device slot: the joiner's world came from a save frozen before the first insert
    coop::hook_anchor::QueueConnectBroadcastForSlot(slot);  // anchored hooks the joiner's save capture missed; it dedups by key
    coop::roach_sync::QueueConnectBroadcastForSlot(slot);  // current roach population to the joiner
    // The piramid lane's late-join answer: re-send an in-flight gather commit to the slot, after
    // the world-actor and NPC snapshots above, since the replay's eid lookups need the joiner's
    // mirrors; its retry window absorbs drain skew.
    coop::piramid_sync::QueueConnectBroadcastForSlot(slot);
    // The cue lane's late-join answer: re-send the live cosmetic cue emitters, so a mid-shower
    // joiner replays the emitter.
    coop::event_cue_sync::QueueConnectBroadcastForSlot(slot);
    // The chat lane's late-join answer: the lobby's chat record, oldest first, one message per
    // line. It lands retained on the joiner: arriving in a lobby must not replay a conversation
    // you were not in across your screen; it is there when you open the chat.
    coop::chat_sync::QueueConnectBroadcastForSlot(slot);
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
    coop::signal_sync::OnDisconnectSlot(slot);
    coop::email_sync::OnDisconnectSlot(slot);
    // Shut the chat lane's per-slot seed gate: the next occupant's applied range starts empty, so
    // it must get its seed before it hears a live line.
    coop::chat_sync::OnSlotDisconnected(slot);
    // Per-slot cleanup: only subsystems with per-slot state are called here; the global-state ones
    // are handled by DisconnectAll.
    coop::trash_mirror::OnDisconnectForSlot(slot);  // phase 1: retire the leaver's trash mirrors BEFORE the generic mirror drain (else the rooted actor leaks)
    coop::trash_channel::OnGrabHolderLeft(session, static_cast<uint8_t>(slot));  // the leaver's carried clump is let go: it falls, streams, lands
    coop::broom_stroke::OnPeerLeft(static_cast<uint8_t>(slot));  // the leaver's stroke rate goes with it
    coop::pack_trash_intent::OnPeerLeft(static_cast<uint8_t>(slot));  // and so does its pack queue
    coop::verb_lanes::OnPeerLeft(static_cast<uint8_t>(slot));  // and its console presses, door verbs, keypad entries
    coop::kerfus_lanes::OnPeerLeft(static_cast<uint8_t>(slot));  // and its Kerfus verbs
    coop::wisp_grab_hold::OnPeerLeft(static_cast<uint8_t>(slot));  // drop the leaver's grab-window puppet hold
    coop::remote_prop::OnDisconnectForSlot(slot);
    coop::item_activate::OnDisconnectForSlot(slot);
    coop::device_occupancy::OnDisconnectForSlot(slot);  // release a leaver's device claims
    coop::desk_input_sync::OnPeerLeft(slot);  // clear a leaver's dangling coordIsPing (its ping would swallow every peer's desk keys)
    coop::desk_snd_fx::OnPeerLeft(slot);  // host-owned teardown of the leaver's loop sounds (broadcast OFF)
    coop::upgrade_sync::OnPeerLeft(static_cast<uint8_t>(slot));  // and its queued purchases
    coop::comp_sync::OnPeerDisconnect(static_cast<uint8_t>(slot));  // pause the mirror if the decode simulator left
    coop::kerfur_command::OnPeerDisconnect(static_cast<uint8_t>(slot));  // release any kerfur the leaver was owned-following
    coop::voice_chat::OnDisconnectSlot(slot);  // drop the leaver's voice channel + icon state
    coop::sleep_sync::OnDisconnectForSlot(slot);  // drop the leaver from the sleep tally (re-gate)
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
    coop::npc_adoption::OnSessionEnd();  // drop pending deferred adoptions + reset latches
    coop::kerfur_prop_adoption::OnSessionEnd();  // drop pending prop-form kerfur adoptions
    coop::dev::dev_lanes::EndSession();  // [dev] the probes' verdicts, before the prop seams reset below, then each drill back to its first step
    coop::prop_drop_intent::Reset();  // clear the client park set + pending places
    coop::prop_spawn_authoring::Reset();  // drop the menu bracket + the resolved spawn verbs
    coop::host_spawn_watcher::OnDisconnect();  // drop the ambient-prop death-watch list
    coop::kerfur_convert::OnDisconnect();  // clear the open conversion verbs
    coop::kerfur_form_assembler::OnDisconnect();  // dump the containment SUMMARY (always)
    // The session's hold on the script gate ends with the session's state, released where the old
    // switch turned the gate off, so the lanes below tear down as they always have; a session that
    // lives on (a host whose last client left) takes it again on its next tick.
    ReleaseSessionGateHold();
    coop::kerfur_entity::OnDisconnect();  // clear the KerfurId table + free its reserved host ids
    coop::kerfur_command::OnDisconnect();  // drop pending menu commands + owned-follow map
    coop::kerfur_menu_input::OnDisconnect();  // drop the cached session (the InpActEvt_use observer stays registered)
    coop::prop_stick_sync::OnDisconnect();  // drop commit-pending stick records
    coop::item_activate::OnDisconnect();
    coop::weather_sync::OnDisconnect();
    coop::interactable_sync::OnDisconnect();
    coop::keypad_sync::OnDisconnect();
    coop::time_sync::OnDisconnect();
    coop::sky_sync::OnDisconnect();
    coop::power_sync::OnDisconnect();
    coop::atv_sync::OnDisconnect();
    coop::drone_sync::OnDisconnect();
    coop::order_sync::OnDisconnect();
    coop::coingun_sync::OnDisconnect();  // dump the lane summary + drop the sold-set, the barrier queue and the cached gun
    coop::firefly_sync::OnDisconnect();
    coop::event_cue_sync::OnDisconnect();  // drop the session the cue watch sends on
    coop::event_fire_sync::OnDisconnect();  // restore the client scheduler (allEvents.Num) + drop the queued fires
    coop::event_active_sync::OnDisconnect();  // drop the begun-events map and its world stamp
    coop::alarm_sync::OnDisconnect();  // drop the cached trigger + poll baseline
    coop::serverbox_sync::OnDisconnect();  // drop cached gamemode/offsets + baseline + breaker-kill latch
    coop::floppy_slot_sync::OnDisconnect();  // drop the slot shadows, the retry set and the per-sender rate windows
    coop::floppy_slot_entry::OnDisconnect();  // counters, then the transit marks and the interceptors: the line above drops the UFunctions they name
    coop::roach_sync::OnDisconnect();  // drop snapshot assembly + tracked set + baselines
    coop::owner_entity_sync::OnDisconnect();  // destroy ALL owner-entity mirrors (our spawned actors must not linger into SP)
    coop::hook_sync::OnDisconnect();  // same, for every hook mirror, anchored included
    coop::skin_preview::OnDisconnect();  // despawn the F1-skins mannequin
    coop::inventory_pickup_sync::OnDisconnect();
    coop::chat_sync::OnDisconnect();
    coop::turbine_sync::OnDisconnect();
    coop::upgrade_sync::OnDisconnect();  // and its own upgrade panel buys locally again
    coop::device_occupancy::OnDisconnect();
    coop::console_state_sync::OnDisconnect();
    coop::signal_catch_sync::OnDisconnect();
    coop::prop_save_data::OnDisconnect();
    coop::laptop_sync::OnDisconnect();
    coop::portable_pc_lid::OnDisconnect();
    coop::laptop_buffer_sync::OnDisconnect();  // quad shadow + assembler + selftest
    coop::floppybox_sync::OnDisconnect();  // box shadows + taken-ring + pendings
    coop::props::container_contents_sync::OnDisconnect();  // dirty set + retry + parked + assembler
    coop::props::prop_record_refresh::OnDisconnect();  // per-prop coalescing state
    coop::dev::dev_lanes::RearmSelftests();  // [dev] the selftests re-arm for the next session
    coop::desk_cursor_sync::OnDisconnect();
    coop::desk_sim_sync::OnDisconnect();
    coop::dish_sync::OnDisconnect();  // wire-residue sweep + ticker restores (the suppression loan)
    coop::tape_caddy_sync::OnDisconnect();  // poll baselines + IsRecent stamps + the singleton cache (no suppression -- nothing to restore)
    coop::daily_task_sync::OnDisconnect();  // change-hash baseline
    coop::desk_input_sync::OnDisconnect();
    coop::desk_snd_fx::OnDisconnect();
    coop::deck_play_sync::OnDisconnect();  // gen counters + ring + self-test latch
    coop::physmods_sync::OnDisconnect();  // verb snapshots + parked canonical + deny records
    coop::drive_sync::OnDisconnect();  // slot/payload baselines + latch/dirty state + pending
    coop::drive_rack_sync::OnDisconnect();  // rack baselines/shadow + pending + deny/taken rings
    coop::sleep_sync::OnDisconnect();
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
    coop::verb_lanes::OnDisconnect();  // and each verb lane's queues, rates and summary line
    coop::kerfus_lanes::OnDisconnect();  // and the Kerfus lanes' records, waiting states and queues
    coop::broom_push::OnDisconnect();  // forget pushes not yet handed on
    coop::trash_collect_sync::OnDisconnect();
    coop::trash_channel::OnDisconnect();  // drop the per-eid trash sync-time-context map
    coop::puppet_carry_drive::OnDisconnect();  // drop all puppet-held clump drives
    coop::trash_sweep::OnDisconnect();  // drop the swept clumps still rolling
    coop::trash_clump_pose_stream::OnDisconnect();  // drop all client per-eid carry drives
    coop::prop_drive_host::OnDisconnect();  // drop the host's driven-prop set
    coop::prop_drive_stream::OnDisconnect();  // every driven prop here gets its physics back
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
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:power"}; coop::power_sync::Tick(); }  // base power panel: poll breaker edges + deferred-apply retry (symmetric)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:atv"}; coop::atv_sync::Tick(); }  // ATV: occupant streams its pose / mirror drives the interp (host+client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drone"}; coop::drone_sync::Tick(); }  // delivery drone: host streams transform / client suppresses tick + mirrors
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:turbine"}; coop::turbine_sync::Tick(); }  // wind turbines: host ~1 Hz driver-float poll / client deferred-apply retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:alarm"}; coop::alarm_sync::Tick(); }  // base radar alarm: 1 Hz active-bit poll BOTH roles (host broadcasts transitions; client forwards local ones)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:server"}; coop::serverbox_sync::Tick(); }  // signal-server sim: HOST 1 Hz state poll -> broadcast on change; CLIENT keeps its ticker_serverBreaker neutralized
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:slot"}; coop::floppy_slot_sync::Tick(); }  // device slots: 1 Hz digest-gated poll -> HOST canonical, CLIENT claim
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:roach"}; coop::roach_sync::Tick(); }  // roach infestation: HOST 1 Hz population poll -> paged broadcast; CLIENT liveness-scan -> consumption intents
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:owner_entity"}; coop::owner_entity_sync::Tick(); }  // owner-entity: 4 Hz own-pose stream + keepalive + death-watch + mirror prune
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:hook"}; coop::hook_sync::Tick(); }  // hook: one activeHook read, then a gated 4/20 Hz head poll only while a hook exists
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:prop_drive_host"}; coop::prop_drive_host::Tick(session); }  // HOST: publish the driven props that moved, close the streams that rested (after the hook poll that feeds it; an empty set costs one size check)
    coop::dev::dev_lanes::TickDrills(session);  // [dev] the drills' and censuses' legs (a single read each when off)
    coop::player_damage::Tick();  // impact-entry PRE cancels lazy install (non-local bodies)
    coop::player_handshake::TickSkinConverge();  // heal a join-window deferred skin apply (~2 s throttle)
    coop::skin_preview::Tick();  // F1-skins live mannequin preview (spawn/apply/position/hide)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:device_occupancy"}; coop::device_occupancy::Tick(); }  // device occupancy: activeInterface edge poll + pending claim retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:console_state"}; coop::console_state_sync::Tick(); }  // signal-catcher: host sky poll / client mirror sweep / desk + dish owner streams
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal_catch"}; coop::signal_catch_sync::Tick(); }  // the catch and cleared detectors, 1 Hz
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:propsave"}; coop::prop_save_data::Drive(); }  // budgeted park applies + the 1 Hz assembler sweep
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:laptop"}; coop::laptop_sync::Tick(); }  // PC power/floppy edge polls (4 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pc_lid"}; coop::portable_pc_lid::Tick(); }  // the lid's watch until live, then waiting lines and held edges (1 Hz while any)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:laptop_quad"}; coop::laptop_buffer_sync::Tick(); }  // quad int pre-filter poll (4 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:floppybox"}; coop::floppybox_sync::Tick(); }  // box sweep (1 Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:containerContents"}; coop::props::container_contents_sync::Tick(); }  // edge-driven dirty drain (4 Hz gate)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:containerView"}; coop::props::container_view_close::Tick(); }  // a client's view into a container, closed out of reach
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_cursor"}; coop::desk_cursor_sync::Tick(); }  // coords-panel live cursor -- holder streams viewCoordinate / mirror interpolates (50ms) + WriteCursorOnly
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_sim"}; coop::desk_sim_sync::Tick(); }  // download-SIM -- host streams outputs (10Hz) / client interpolates + WriteSimOutputs
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:dish"}; coop::dish_sync::Tick(); }  // host pose sweep + arm poll (4Hz) / client apply + park latch / calib diff-poll (1Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:reel"}; coop::tape_caddy_sync::Tick(); }  // 4Hz slot sentinel poll (both peers) + host 1Hz corrector / client exact-snap apply
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:task"}; coop::daily_task_sync::Tick(); }  // host 1Hz taskNew change-hash poll (fires a few times per game-day)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_input"}; coop::desk_input_sync::Tick(); }  // 250ms input-field poll -> claim-free DeskInput deltas + cooldown charge/scan classification
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_snd"}; coop::desk_snd_fx::Tick(); }  // audio-seam ring flush + lazy hook install + pending loop retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:deck_play"}; coop::deck_play_sync::Tick(); }  // deck playback ring flush + lazy Deactivate/fin seam install + gen author
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:physmods"}; coop::physmods_sync::Tick(); }  // parked-canonical apply at desk resolve
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive"}; coop::drive_sync::Tick(); }  // barrier drain + 1 Hz drive-chain sweeps
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive_rack"}; coop::drive_rack_sync::Tick(); }  // rack barrier drain + 1 Hz sweep
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:email"}; coop::email_sync::Tick(); }  // email shadow poll (1 Hz; appends -> chunked broadcast, shrinks -> content-keyed deletes)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal"}; coop::signal_sync::Tick(); }  // saved-signals shadow poll (same shape on gamemode.savedSignals_0)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:meadow"}; coop::meadow_db_sync::Tick(); }  // meadow-DB: the writers' watches until settled, then retries (1 Hz) while a line, a tombstone or an owed order waits
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:comp"}; coop::comp_sync::Tick(); }  // decode-pane simulator stream + comp_data edges + client world-up unlatch
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
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:piramid"}; coop::piramid_sync::Tick(); }  // pre-arm probe (250 ms gate) / host gather-edge sweep (1 s) / client mirror restore + pending gather replay
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:trash_clump_pose"}; coop::trash_clump_pose_stream::TickApplyAndDrive(session); }  // CLIENT: apply host-auth carry/flight pose batch + per-eid interp (client-only, no-op on host)
    if (!isHost) { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:prop_drive_stream"}; coop::prop_drive_stream::TickApplyAndDrive(session); }  // CLIENT: park + drive the host's driven props (the host originates them and never receives any)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::host_spawn_watcher::TickWatchedProps(&session); }  // ambient-prop (pinecone) SetLifeSpan-expiry / consumption despawn -> PropDestroy(eid)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::host_spawn_watcher::DrainPendingSpawns(&session); }  // adopt+express FinishSpawningActor Func-seam spawns (R-drop/place/Q-menu) one tick after Finish (key restored, hand actor excluded)
    if (isHost) { PP::Scope _s{PP::Bucket::Interactable}; coop::broom_push::Tick(); }  // HOST: stream the props and clumps a broom stroke pushed (AFTER the drain above, which names the trash that same stroke dispensed)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_drop_intent::Tick(&session); }  // CLIENT: author a PropDropIntent for a detected place whose Key is parked (cheap no-op when empty / on host)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_form_assembler::Tick(); }  // GT FName-resolve the 2 verbs + bind the containment seams (latches once; no-op after)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_command::Tick(); }  // drain menu commands + advance the ownership-follow loop (cheap no-op when idle)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_stick_sync::Tick(); }  // broadcast recorded stick commits NOW -- must precede local_streams' release edge (net_pump runs TickGameplay first)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pause_guard"}; coop::pause_guard::Tick(isConnected); }  // coop no-pause invariant -- un-pause the world while connected (ESC menu stays usable; a paused peer froze its pose stream)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:run_end"}; coop::player::run_end_travel::Tick(); }  // the run-ending seam's session-scoped work: publish the seam's readiness (the verdict itself runs in the VM's body loop)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:save_cycle_off"}; coop::save_block::Tick(&session); }  // client native save-cycle OFF -- hold gamemode.disableSave=true (saveSlot_C::save gates gather+write on it); the SaveGameToSlot disk hook stays as the belt
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sleep"}; coop::sleep_sync::Tick(); }  // isSleep edge poll + WAITING dilation enforcement + the client need clamp
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
    { PP::Scope _s{PP::Bucket::Interactable}; coop::kerfus_lanes::Tick(session); }  // the Kerfus lanes' watches; CLIENT: waiting states; HOST: queued verbs
    { PP::Scope _s{PP::Bucket::Balance};       coop::balance_sync::Tick(); }  // host polls saveSlot.Points + broadcasts on change; client retries the pending mirror apply
    { PP::Scope _s{PP::Bucket::Balance};       ue_wrap::ScopedWalkTimer _w{"sync:upgrades"}; coop::upgrade_sync::Tick(session); }  // host polls the upgrade struct + broadcasts on change, and runs one queued purchase a tick a client; client retries the pending mirror
    coop::dev::dev_lanes::TickProbes(session, isConnected, isHost);  // [dev] the RE probes, each self-installing when its knob is set
}

}  // namespace coop::subsystems
