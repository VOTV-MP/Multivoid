// coop/session/connect_replay.cpp -- the host's connect replay for one slot; declared in
// coop/session/subsystems.h, whose fan-out lists call it from the world-ready answer.

#include "coop/session/subsystems.h"

#include "coop/world/balance_sync.h"
#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/physmods_sync.h"  // desk physical-modules lane
#include "coop/items/order_queue_sync.h"  // the delivery order queue, the host's, mirrored
#include "coop/interactables/drive_payload_sync.h"  // a drive's row, the host's
#include "coop/interactables/drive_sync.h"  // the drive slots
#include "coop/interactables/drive_rack_sync.h"  // rack storage
#include "coop/interactables/meadow_db_sync.h"  // meadow signal-DB mirror (multiset shadow + join seed)
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/dish_calib_sync.h"
#include "coop/interactables/dish_hashcode_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/download_arm_sync.h"
#include "coop/interactables/sat_console_sync.h"  // the SAT console's shared commands run on the host
#include "coop/world/daily_task_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/world/email_sync.h"
#include "coop/interactables/laptop_sync.h"  // the stationary PC lane
#include "coop/interactables/laptop_tab.h"  // the tab the PC's user is on
#include "coop/interactables/portable_pc_lid.h"  // the portable PC's lid lane
#include "coop/interactables/floppy_slot_sync.h"
#include "coop/interactables/floppybox_sync.h"  // the disc crate LIFO lane
#include "coop/props/container_contents_sync.h"  // the world-container GObjStack slice
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/player/hand_item.h"  // hotbar hand-item display axis (connect replay)
#include "coop/player/sleep_sync.h"
#include "coop/session/rig_ready.h"
#include "coop/voice/voice_chat.h"
#include "coop/save/join_window_baseline.h"  // the connect edge's removes and position corrections
#include "coop/interactables/grime_sync.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/drone_sync.h"
#include "coop/world/event_cue_sync.h"
#include "coop/comms/chat_sync.h"
#include "coop/interactables/upgrade_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/world/fuse_tower_rows.h"
#include "coop/world/power_grid.h"
#include "coop/world/power_panel.h"
#include "coop/world/sky_sync.h"
#include "coop/interactables/window_sync.h"
#include "coop/props/prop_drive_host.h"    // HOST: the props a hook drags, streamed while they move
#include "coop/interactables/verb_lanes.h"  // the interactable lanes on a device's own verbs, at the script gate
#include "coop/creatures/kerfus_lanes.h"  // the plain kerfur: its brain on the host, its state and drive, its verbs
#include "coop/props/trash_pile_sync.h"
#include "coop/player/item_activate.h"
#include "coop/net/snapshot_send_scope.h"  // the divergence deletes pass the slot's join gate
#include "coop/creatures/npc_sync.h"
#include "coop/creatures/npc_world_enum.h"  // RegisterExistingWorldNpcs
#include "coop/world/world_actor_sync.h"  // non-Character event-actor transform mirror (sibling of npc_sync)
#include "coop/creatures/piramid_sync.h"  // piramid event choreography lane (mirror brain suppression + PyramidGather)
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"
#include "coop/world/alarm_sync.h"  // base radar alarm shared-world toggle
#include "coop/interactables/serverbox_sync.h"  // host-authoritative signal-server sim state
#include "coop/interactables/server_upgrade_sync.h"  // the servers' physical upgrades
#include "coop/creatures/roach_sync.h"  // host-authoritative roach-infestation mirror
#include "coop/items/hook_anchor.h"
#include "coop/world/event_active_sync.h"  // the native activeEvents registry probe
#include "coop/world/creature_event_relay.h"  // the host's creature events, carried to every client's count
#include "coop/world/weather_sync.h"
#include "ue_wrap/core/log.h"

namespace coop::subsystems {

// ConnectReplayForSlot is the host's response to ClientWorldReady, which a client re-fires on
// every world-change re-seed (the join double-load and a mid-session level travel), so the host
// re-asserts world state into the client's reloaded world; its keyless chipPiles, eid-only,
// re-acquire their host eid that way. The replay is idempotent (adopt snapshots, deduplicated
// mirror registration), so re-running it is correct.

void ConnectReplayForSlot(int slot) {
    if (slot < 1 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    UE_LOGI("net: slot %d world-ready -- replaying snapshot + flashlight + weather + peer states", slot);
    coop::rig_ready::Say("peer-world-ready", slot);
    // Before the snapshot, explicit per-key destroys for props this joiner's blob had that the
    // host's live world no longer has (grabbed or destroyed during the download and load), so the
    // client drops exactly those rather than the divergence sweep inferring the deletes. Sent inside the
    // snapshot scope (its sends skip the join gate), so the removes land before the adds. A no-op for a joiner
    // with no blob baseline, which the sweep still owns.
    {
        coop::net::SnapshotSendScope scope(slot);  // the removes are the snapshot's, so the join gate passes them
        coop::join_window_baseline::SendDivergenceDeletes(slot);
    }
    // Every dish code, sent here and held by the join gate until the snapshot completes, so a set that goes whole lands
    // before its Complete and a joined client holds the host's codes, a midnight since the capture
    // included; a set not taken whole is retried from the lane's tick and can land after it.
    coop::dish_hashcode_sync::QueueConnectBroadcastForSlot(slot);
    coop::prop_snapshot::TriggerForSlot(slot);
    coop::prop_drive_host::OnPeerWorldReady();  // every driven prop's pose again, so the joiner parks the resting ones the delta gate would never send it
    coop::kerfus_lanes::OnPeerWorldReady(slot);  // and every Kerfus's on, charging and energy
    coop::sat_console_sync::OnPeerWorldReady(slot);  // a SAT console typist back at a terminal still running finds it
    // Deliver the current position of any save-authoritative chipPile the host moved in this
    // joiner's connect window (the move's convert was dropped pre-world, and chipPiles carry no
    // position in the snapshot). After the snapshot, so the join gate holds it behind the snapshot's spawns; the
    // client snaps the bound native at quiescence.
    coop::join_window_baseline::FlushDivergedPositions(slot);
    coop::item_activate::QueueConnectBroadcastForSlot(slot);
    coop::weather_sync::QueueConnectBroadcastForSlot(slot);
    coop::interactable_sync::QueueConnectBroadcastForSlot(slot);  // door/light/container states
    coop::keypad_sync::QueueConnectBroadcastForSlot(slot);  // keypad states
    coop::sky_sync::QueueConnectBroadcastForSlot(slot);  // night-sky orientation, moon phase, eye
    coop::power_grid::QueueConnectBroadcastForSlot(slot);  // the generators' rows, before the panel they gate
    coop::power_panel::QueueConnectBroadcastForSlot(slot);  // the panel's canonical
    coop::fuse_tower_rows::QueueConnectBroadcastForSlot(slot);  // every tower's row
    coop::atv_sync::QueueConnectBroadcastForSlot(slot);  // ATV body pose, stamped with the host's seat and syncer (adopt=1)
    coop::drone_sync::QueueConnectBroadcastForSlot(slot);  // delivery drone pose (adopt=1)
    coop::order_queue_sync::QueueConnectBroadcastForSlot(slot);  // the delivery order queue: a reset + every queued order
    coop::turbine_sync::QueueConnectBroadcastForSlot(slot);  // wind-turbine facing/spin snap
    coop::device_occupancy::QueueConnectBroadcastForSlot(slot);  // live device claims (busy table)
    coop::download_arm_sync::QueueConnectResetForSlot(slot);  // a reset since the capture, ahead of the desk's seed: the game's order is reset, catch, arm
    coop::console_state_sync::QueueConnectBroadcastForSlot(slot);  // sky-signal snapshot + desk adopt
    coop::desk_input_sync::SeedPingAttributionFromMachine();  // a SOLO host's ping edge is absorbed unwired (PollOnce gated on connected) -- re-derive from ground truth so a mid-ping joiner gets the FSM-hold
    coop::desk_snd_fx::QueueConnectBroadcastForSlot(slot);  // desk loop-sound ground truth (a mid-loop joiner gets the ON)
    coop::physmods_sync::QueueConnectBroadcastForSlot(slot);  // canonical module array (ground truth over save drift)
    coop::drive_sync::QueueConnectBroadcastForSlot(slot);  // slot lines
    coop::drive_payload_sync::QueueConnectBroadcastForSlot(slot);  // every drive row that differs from its class default
    coop::drive_rack_sync::QueueConnectBroadcastForSlot(slot);  // rack canonicals (AFTER the payloads -- the shipped seed order, all on the Default lane)
    coop::meadow_db_sync::QueueConnectBroadcastForSlot(slot);  // the seedDelta(h) join seed (blob-instant snapshot vs live)
    coop::signal_sync::QueueConnectBroadcastForSlot(slot);  // the join-window saved-signal seed, both signs
    coop::email_sync::QueueConnectBroadcastForSlot(slot);  // the join-window email seed, both signs
    coop::signal_catch_sync::QueueConnectBroadcastForSlot(slot);  // the in-flight catch replay (the identity half) and the kind-2 state seed
    coop::laptop_sync::QueueConnectBroadcastForSlot(slot);  // the PC's power (its disc slot is floppy_slot_sync's)
    coop::laptop_tab::QueueConnectBroadcastForSlot(slot);  // the tab the PC's user is on
    coop::portable_pc_lid::QueueConnectBroadcastForSlot(slot);  // every open portable PC lid
    coop::floppybox_sync::QueueConnectBroadcastForSlot(slot);  // one canonical per live box
    coop::props::container_contents_sync::QueueConnectBroadcastForSlot(slot);  // one slice per live world container (principle 8 anchor over the join snapshot)
    coop::dish_sync::QueueConnectBroadcastForSlot(slot);  // the dish snapshot -- AFTER the desk rows + the catch seed (same ordered lane)
    coop::download_arm_sync::QueueConnectArmForSlot(slot);  // an armed machine's ARM row, after the catch's seed and the dish snapshot
    coop::dish_calib_sync::QueueConnectBroadcastForSlot(slot);  // every dish's precision, the joiner's seed
    coop::sleep_sync::QueueConnectBroadcastForSlot(slot);  // the host's sleeping flag + every peer's rest state that is not awake
    coop::comp_sync::QueueConnectBroadcastForSlot(slot);  // the refiner's seed (CompState + CompData)
    coop::voice_chat::ReplayPeerStatesToSlot(slot);  // voice mute/disabled states -> joiner
    coop::window_sync::QueueConnectBroadcastForSlot(slot);  // base-window clean (adopt=1)
    coop::grime_sync::QueueConnectBroadcastForSlot(slot);  // surface grime process (adopt=1)
    coop::trash_pile_sync::QueueConnectBroadcastForSlot(slot);  // pile counters (adopt=1) + depleted-key replay
    coop::npc_world_enum::RegisterExistingWorldNpcs(slot);  // pre-existing/level-load NPCs (the save's kerfur) -> the peers already in; the joiner (slot) is skipped, its connect broadcast below sends it and it adopts its twin
    coop::npc_sync::QueueConnectBroadcastForSlot(slot);  // existing NPCs -> joiner
    coop::world_actor_sync::QueueConnectBroadcastForSlot(slot);  // existing event WorldActors -> joiner
    coop::balance_sync::SendCurrentToSlot(slot);  // host's current balance
    coop::upgrade_sync::SendCurrentToSlot(slot);  // and the upgrade levels: the joiner's save was taken at the handshake
    coop::daily_task_sync::SendCurrentToSlot(slot);  // and the daily task: a change in its load window was hashed but never sent to it
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
    // Each creature's count, after its NPC spawn (the queue above, same lane): the joiner applies a count at the bind.
    coop::creature_event_relay::SendJoinSnapshotForSlot(slot);
    // The alarm lane's late-join answer: the current alarm state, unconditionally, so a mid-alarm
    // joiner starts its klaxon on arrival.
    coop::alarm_sync::QueueConnectBroadcastForSlot(slot);
    coop::serverbox_sync::QueueConnectBroadcastForSlot(slot);  // current server state to the joiner
    coop::server_upgrade_sync::QueueConnectBroadcastForSlot(slot);  // every box's upgrade level
    coop::floppy_slot_sync::QueueConnectBroadcastForSlot(slot);  // every device slot, the laptop's file quad behind the laptop's: the joiner's world came from a save frozen before the first insert
    coop::verb_lanes::ConnectReplayForSlot(slot);  // the verb lanes' seeds: every corded prop's cord link
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

}  // namespace coop::subsystems
