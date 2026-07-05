// coop/subsystems.cpp -- see coop/subsystems.h.
//
// Extracted from net_pump.cpp 2026-06-12 (modular soft cap): the five
// sync-module fan-out lists, moved VERBATIM. New sync features wire in here.

#include "coop/session/subsystems.h"

#include "coop/world/balance_sync.h"
#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/world/email_sync.h"
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/player/local_body.h"  // v93 skins: local first-person body owner
#include "coop/player/nameplate.h"   // v94: plate-pref session wiring (Install)
#include "coop/player/nick_color.h"  // v103 (12f): nick-color session wiring (Install)
#include "coop/player/sleep_sync.h"
#include "coop/creatures/wisp_attack_sync.h"   // Killer Wisp coop: host detect + neutralize + relay
#include "coop/creatures/wisp_grab_hold.h"     // Killer Wisp v2: grab-window body placement (per-slot/full teardown)
#include "coop/creatures/wisp_tear_mirror.h"   // Killer Wisp coop: victim kill + tear mirror
#include "coop/session/pause_guard.h"          // 2026-07-04: coop no-pause invariant (ESC pause froze clients)
#include "coop/items/player_inventory_sync.h"  // v73 per-player inventory (host file scaffold)
#include "coop/dev/inventory_probe.h"    // v73 Inc4: SP self-test for the apply (engine write) path
#include "coop/dev/sleep_probe.h"
#include "coop/voice/voice_chat.h"
#include "coop/dev/drone_probe.h"
#include "coop/dev/native_pile_inert_probe.h"
#include "coop/dev/client_model_probe.h"  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
#include "coop/dev/pinecone_probe.h"
#include "coop/session/ambient_spawner_suppress.h"  // Fork C: client ambient flora/forage spawner suppression
#include "coop/props/host_spawn_watcher.h"  // M2: HOST mirror of those ambient spawner outputs (the pinecone scare)
#include "coop/creatures/kerfur_convert.h"  // v67: host-authoritative kerfur on/off conversion (the dupe fix)
#include "coop/creatures/kerfur_command.h"  // v74: host-authoritative kerfur menu command relay + ownership follow
#include "coop/creatures/kerfur_menu_input.h"  // client radial-menu verb detection (InpActEvt_use PRE -> kerfur_command relay)
#include "coop/creatures/kerfur_entity.h"   // K-3: stable-KerfurId authority table (the redesign root fix)
#include "coop/props/prop_stick_sync.h"  // v68: wall-attachable stick mirror (camera-on-wall)
#include "coop/dev/teleport_client.h"  // TeleportSlotToHost: spawn a joiner at the host pose (connect edge)
#include "coop/dev/keypad_probe.h"
#include "coop/dev/door_probe.h"
#include "coop/dev/lightswitch_probe.h"
#include "coop/dev/perf_probe.h"
#include "coop/session/save_transfer.h"
#include "coop/interactables/grime_sync.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/drone_sync.h"
#include "coop/items/order_sync.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/firefly_sync.h"
#include "coop/player/inventory_pickup_sync.h"
#include "coop/comms/chat_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/interactables/power_sync.h"
#include "coop/world/sky_sync.h"
#include "coop/world/time_sync.h"
#include "coop/interactables/window_sync.h"
#include "coop/session/join_progress.h"
#include "coop/interactables/garbage_sync.h"
#include "coop/props/trash_channel.h"
#include "coop/player/puppet_carry_drive.h"
#include "coop/props/trash_clump_pose_stream.h"
#include "coop/props/trash_collect_sync.h"
#include "coop/props/trash_proxy.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/session/save_block.h"
#include "coop/session/save_button_disable.h"
#include "coop/props/grab_observer.h"
#include "coop/player/item_activate.h"
#include "coop/player/player_damage.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // K-6
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_sync.h"
#include "coop/creatures/npc_world_enum.h"  // K-0: RegisterExistingWorldNpcs (moved out of npc_sync)
#include "coop/creatures/world_actor_sync.h"  // v80 (B3b): non-Character event-actor transform mirror (sibling of npc_sync)
#include "coop/creatures/piramid_sync.h"      // v97: piramid event choreography lane (mirror brain suppression + PyramidGather)
#include "coop/player/players_registry.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/world/alarm_sync.h"         // v101 base radar alarm shared-world toggle (docs/events/alarm.md)
#include "coop/world/event_active_sync.h"  // join-during-event Phase 0: native activeEvents registry probe (docs/COOP_EVENT_JOIN.md)
#include "coop/world/weather_sync.h"

#include "ue_wrap/log.h"
#include "ue_wrap/walk_timer.h"  // L5 per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::subsystems {

void Install(coop::net::Session& session) {
    coop::grab_observer::Install();
    coop::prop_lifecycle::InstallInventory(&session);
    coop::prop_lifecycle::Install(&session);
    coop::npc_sync::Install(&session);
    coop::world_actor_sync::Install(&session);  // v80 (B3b): non-Character event-actor mirror (2nd BeginDeferred interceptor, disjoint allowlist)
    coop::piramid_sync::Install(&session);      // v97: piramid event choreography lane (hooks arm lazily on the first piramid element)
    coop::item_activate::Install(&session);  // Phase 5F flashlight
    coop::player_damage::Install(&session);  // vitals Inc3-WIRE damage relay (send + owner-apply)
    coop::weather_sync::Install(&session);   // Phase 5W weather
    coop::interactable_sync::Install(&session);  // Phase 5D doors + lights + container lids
    coop::keypad_sync::Install(&session);    // v33 password-keypad mirror (its own module)
    coop::time_sync::Install(&session);      // v36 host-authoritative world clock (time-of-day / dark-world fix)
    coop::sky_sync::Install(&session);       // v44 host-authoritative night-sky orientation + moon phase
    coop::power_sync::Install(&session);     // v46 base power-panel breakers (its own module -- 5 bools)
    coop::atv_sync::Install(&session);       // v47 ATV body pose (occupant-authoritative keyed stream)
    coop::drone_sync::Install(&session);     // v48 delivery drone body pose (host-authoritative singleton)
    coop::order_sync::Install(&session);     // v49 delivery-drone economy: client->host shop-order forward
    coop::firefly_sync::Install(&session);   // v51 peer-symmetric ambient firefly mirror (each peer captures+shares its own)
    coop::event_cue_sync::Install(&session); // v79 HOST-AUTH cosmetic emitter-cue mirror (B1: starfall etc. -- host detects PSC, client replays)
    coop::event_fire_sync::Install(&session); // v95 HOST-AUTH scheduled-event replay (passEvents growth poll -> EventFire; client suppress + policy replay)
    coop::event_active_sync::Install(&session); // join-during-event Phase 0 (probe): host 1 Hz activeEvents_senders membership diff -> BEGIN/END edge log
    coop::alarm_sync::Install(&session);     // v101 base radar alarm shared-world toggle (1 Hz active poll both roles; docs/events/alarm.md)
    coop::inventory_pickup_sync::Install(&session);  // v58 inventory-collect blip (PlaySound2D observer)
    coop::chat_sync::Install(&session);      // v60 T-chat (the ui/chat_input send path)
    coop::local_body::Install(&session);     // v93 skins: local first-person body + SkinChange announce
    coop::local_body::Tick();                // applies the persisted skin to the local pawn + 1 Hz convergence
    coop::nameplate::Install(&session);      // v94: plate-pref announce path (F1 checkbox -> NameplateChange)
    coop::nick_color::Install(&session);     // v103 (12f): nick-color announce path + local-slot mirror refresh
    coop::turbine_sync::Install(&session);   // v61 wind-turbine facing/spin mirror (host-auth ~1 Hz)
    coop::device_occupancy::Install(&session);  // v63 enterable-device occupancy (busy claim + E deny gate)
    coop::console_state_sync::Install(&session);  // v64 signal-catcher state mirror (sky signals + desk + dish aim)
    coop::signal_catch_sync::Install(&session);   // v70: the signal-catch consume replay (dish slew + downloader arm on every peer)
    coop::email_sync::Install(&session);     // v64 inc 2: meadow-PC email mirror (watermark -> chunked rows -> addEmail)
    coop::signal_sync::Install(&session);    // v65: desk signal-library mirror (savedSignals_0 shadow/diff)
    coop::comp_sync::Install(&session);      // v65: refiner decode pane (single-simulator stream + passive mirrors)
    coop::voice_chat::Install(&session);     // v66: proximity voice chat (opus over the session; PTT X)
    coop::window_sync::Install(&session);    // v41 base-window dirt scalar (the "main huge window")
    coop::grime_sync::Install(&session);     // v42 surface grime (walls/ceiling/floor dirt decals)
    coop::trash_pile_sync::Install(&session);  // v57 trashBitsPile collect counters (uses 6/7)
    coop::trash_collect_sync::Install(&session);  // chipPile grab observer (InpActEvt_use PRE -> PropDestroy(eid); replaces the retired pile death-watch)
    coop::garbage_sync::SetSession(&session);
    coop::garbage_sync::Install();           // Phase 5G garbage
    coop::ambient_spawner_suppress::Install(&session);  // Fork C: client ambient flora/forage spawner suppression (host results stream)
    coop::host_spawn_watcher::Install(&session);  // M2: HOST mirrors the ambient spawner outputs (the pinecone scare) the line above cancels on the client -- BeginDeferred POST -> PropSpawn-by-eid
    coop::kerfur_entity::SetSession(&session);  // K-3: stable-KerfurId authority table (cache session for the host AllocHostId role gate; K-4 broadcasts through it)
    coop::kerfur_convert::Install(&session);  // v67: host-authoritative kerfur on/off conversion (the dupe fix -- client menu cancel -> request; host verb + converge)
    coop::kerfur_command::Install(&session);  // v74: host-authoritative kerfur menu command relay + ownership-aware Follow
    coop::kerfur_menu_input::Install(&session);  // client radial-menu verb detect (InpActEvt_use PRE -- the actionName dispatch is PE-invisible) -> kerfur_command relay
    coop::prop_stick_sync::Install(&session); // v68: wall-attachable stick mirror (camera-on-wall -- commit observer -> PropStickState; receiver replays forceStick)
    coop::sleep_sync::Install(&session);      // v71: the Minecraft sleep gate (isSleep edge poll -> host tally -> accelerate/end phases)
    coop::wisp_attack_sync::Install(&session); // v72: Killer Wisp coop -- AddPlayerDamage PRE-cancel (host neutralize) + host detect/relay
    // v73 per-player inventory: Install() moved to StartCoopSession (PRE-WORLD). This
    // subsystems::Install only runs at world-up (net_pump gates it on g_netLocal), but the
    // apply-blob receiver + the pre-materialize SaveObjectReadyHook must be live BEFORE the join's
    // world loads -- installing here silently dropped every apply chunk during the menu-mode wait.
    // The Tick (stream/self-test) + EnsurePlayerFile/OnDisconnect hooks elsewhere in this file stay
    // (post-world / per-slot edges).
    coop::balance_sync::SetSession(&session); // v30 shared host-authoritative balance
    // (trash_collect_sync has no observer to install -- playerTryToCollect is
    // BP-internal; it acts on the held-prop edge in local_streams, see
    // EnsureHeldItemBroadcast.)
    // PR-FOUNDATION-2 (B): client world-save block (host-only persistence).
    // No-op on the host; on the client installs the SaveGameToSlot detour once.
    coop::save_block::Install(&session);
    // PR-FOUNDATION-2 (B part 2): grey out the client pause-menu "Save Game"
    // button (honest UX over the hard block). No-op on the host.
    coop::save_button_disable::Install(&session);
    // NOTE: coop::shutdown::Install / UpdateWindowTitle are called from
    // the timeline tick lambda DIRECTLY in harness.cpp -- they MUST NOT
    // be gated on the local player like this function is (HWND subclass +
    // window title must work BEFORE the local player has been possessed,
    // e.g. on the OMEGA splash where the user might X-close before
    // gameplay).
}

namespace {
// Per-slot "join placement already done" latch. ConnectReplayForSlot is the
// host's response to ClientWorldReady, which a client now re-fires on EVERY
// world-change re-seed (join double-load AND mid-session cave/level travel) so
// the host re-asserts host-authoritative world state into the client's
// freshly-reloaded world -- the only way its keyless chipPiles (eid-only
// identity, no key to re-match) re-acquire their host eid after the re-seed
// mints fresh local ones. The full state replay below is idempotent (adopt=1
// snapshots + RegisterPropMirror dedup), so re-running it is correct. But
// placing the joiner AT the host is a JOIN-ONCE action: re-running it when a
// mid-session traveller re-announces would teleport a settled player to the
// host. Gate ONLY the teleport on this latch; everything else re-runs.
// GAME-THREAD only (event_feed drain). Reset per slot in DisconnectSlot so a
// rejoin re-places normally.
bool g_joinPlaced[coop::players::kMaxPeers] = {};
}  // namespace

void ConnectReplayForSlot(int slot) {
    if (slot < 1 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    UE_LOGI("net: slot %d world-ready -- replaying snapshot + flashlight + weather + peer states", slot);
    // R2 (MTA Packet_EntityRemove): BEFORE the snapshot bracket, send explicit
    // per-key PropDestroy for props this joiner's blob HAD that the host's live
    // world no longer has (e.g. a prop grabbed/destroyed during the ~30-60s
    // download+load). The client drops exactly those instead of the divergence
    // sweep INFERRING the delete. Bulk lane ahead of TriggerForSlot -> removes
    // land before the snapshot's adds. No-op for a fresh-New-Game / stale-fallback
    // joiner (no blob baseline -> the sweep, bounded by R3, still owns that).
    coop::save_transfer::SendBlobDivergenceDeletes(slot);
    coop::prop_snapshot::TriggerForSlot(slot);
    // b3 (v90): deliver the CURRENT position of any save-authoritative chipPile this joiner's host MOVED in its
    // connect window (the move's PropConvert was dropped pre-world, and chipPiles carry no position in the
    // snapshot above). Closes the connect-snapshot's save-authoritative hole; the client snaps the bound native
    // at quiescence. AFTER TriggerForSlot so it rides Bulk behind the snapshot. (#1 kerfur is unaffected -- the
    // active kerfur is already delivered via EntitySpawn / npc_sync below.)
    coop::save_transfer::FlushDivergedPilePositionsForSlot(slot);
    coop::item_activate::QueueConnectBroadcastForSlot(slot);
    coop::weather_sync::QueueConnectBroadcastForSlot(slot);
    coop::interactable_sync::QueueConnectBroadcastForSlot(slot);  // door/light/container states
    coop::keypad_sync::QueueConnectBroadcastForSlot(slot);        // v33 keypad states
    coop::time_sync::QueueConnectBroadcastForSlot(slot);          // v36 world clock -> joiner immediately
    coop::sky_sync::QueueConnectBroadcastForSlot(slot);           // v44 night-sky orientation + moon phase
    coop::power_sync::QueueConnectBroadcastForSlot(slot);         // v46 base power-panel breakers
    coop::atv_sync::QueueConnectBroadcastForSlot(slot);           // v47 ATV body pose (adopt=1)
    coop::drone_sync::QueueConnectBroadcastForSlot(slot);         // v48 delivery drone pose (adopt=1)
    coop::turbine_sync::QueueConnectBroadcastForSlot(slot);       // v61 wind-turbine facing/spin snap
    coop::device_occupancy::QueueConnectBroadcastForSlot(slot);   // v63 live device claims (busy table)
    coop::console_state_sync::QueueConnectBroadcastForSlot(slot); // v64 sky-signal snapshot + desk adopt
    coop::signal_catch_sync::QueueConnectBroadcastForSlot(slot);  // v70 in-flight catch replay (AFTER the desk adopt queued the progress)
    coop::sleep_sync::QueueConnectBroadcastForSlot(slot);         // v71 a joiner arrives awake -- end a running accelerate + re-tally
    coop::comp_sync::QueueConnectBroadcastForSlot(slot);          // v65 decode-pane adopt (CompState + CompData)
    coop::voice_chat::ReplayPeerStatesToSlot(slot);               // v66 voice mute/disabled states -> joiner
    coop::window_sync::QueueConnectBroadcastForSlot(slot);        // v41 base-window clean (adopt=1)
    coop::grime_sync::QueueConnectBroadcastForSlot(slot);         // v42 surface grime process (adopt=1)
    coop::trash_pile_sync::QueueConnectBroadcastForSlot(slot);    // v57 pile counters (adopt=1) + depleted-key replay
    coop::npc_world_enum::RegisterExistingWorldNpcs(coop::npc_world_enum::NpcEnumOrigin::ConnectEdge);  // pre-existing/level-load NPCs (the save's kerfur) -> joiner adopts its twin
    coop::npc_sync::QueueConnectBroadcastForSlot(slot);           // existing NPCs -> joiner
    coop::world_actor_sync::QueueConnectBroadcastForSlot(slot);   // v80 (B3b): existing event WorldActors -> joiner
    coop::balance_sync::OnClientConnect(slot);                    // v30: host's current balance
    coop::player_inventory_sync::EnsurePlayerFile(slot);         // v73: ensure this peer's per-save inventory file exists
    // v73 Inc4: the host->client apply-blob push is NOT here -- ConnectReplayForSlot fires at
    // ClientWorldReady, AFTER the joiner already loaded its world, so the apply blob would miss
    // the pre-materialize hook. It is driven from the host tick's connect-edge detector instead
    // (player_inventory_sync, fires right after the Join when the guid is known -- pre-world).
    // v34: spawn the joiner AT THE HOST -- reuse the existing teleport-to-host
    // pose mechanism (the client applies it on its local player, which is in
    // world by construction now: WorldReady is what fired this). JOIN-ONCE: a
    // client re-fires ClientWorldReady on every world-change re-seed (incl.
    // mid-session cave/level travel), so guard the placement to the first
    // world-ready per slot -- otherwise a traveller would be yanked back to the
    // host on every cave exit. Subsequent re-syncs re-deliver state only.
    if (!g_joinPlaced[slot]) {
        coop::dev::teleport_client::TeleportSlotToHost(slot);
        g_joinPlaced[slot] = true;
    } else {
        UE_LOGI("net: slot %d re-sync replay -- skipping join teleport (already placed)", slot);
    }
    // T2-4: catch the new client up to EXISTING peers' current item state.
    coop::item_activate::ReplayPeerStatesToSlot(slot);
    // join-during-event Phase 1 (v98): one EventSnapshot per in-flight registry entry -- the
    // joiner replays replay-safe rows with the active-override (COOP_EVENT_JOIN.md 3.2).
    coop::event_active_sync::SendJoinSnapshotForSlot(slot);
    // alarm lane late-join answer (COOP_EVENT_JOIN.md 3.4): the CURRENT alarm state,
    // unconditionally -- a mid-alarm joiner starts its klaxon on arrival (v101).
    coop::alarm_sync::QueueConnectBroadcastForSlot(slot);
    // piramid lane late-join answer (COOP_EVENT_JOIN.md 3.4): re-send an in-flight gather
    // commit ToSlot. AFTER world_actor_sync/npc_sync queued their snapshots above (the joiner's
    // mirrors must exist for the replay's eid lookups; its 5 s retry window absorbs drain skew).
    coop::piramid_sync::QueueConnectBroadcastForSlot(slot);
    // event_cue late-join answer (join-during-event Phase 2): re-send live already-broadcast
    // cosmetic cue emitters (starRain...) ToSlot -- a mid-shower joiner replays the emitter.
    coop::event_cue_sync::QueueConnectBroadcastForSlot(slot);
}

void ClientConnectEdge(coop::net::Session& session) {
    coop::item_activate::QueueConnectBroadcastForSlot(0);
    coop::save_transfer::ClientNoteConnected();
    // v56: the HOST always has a world -- open OUR send gate toward
    // it immediately (the gate exists for host->joiner traffic).
    session.MarkSlotWorldReady(0, true);
}

void DisconnectSlot(coop::net::Session& session, int slot) {
    // Abort any pending/in-progress snapshot drain to this slot so we
    // don't iterate ~1700 candidates calling SendReliableToSlot into a
    // dead connection.
    coop::prop_snapshot::CancelForSlot(slot);
    // v56: drop any in-flight save stream + close the world-ready send
    // gate for the departed slot (a rejoin re-opens both fresh).
    coop::save_transfer::CancelForSlot(slot);
    session.MarkSlotWorldReady(slot, false);
    if (slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers))
        g_joinPlaced[slot] = false;  // rejoin re-places the joiner at the host
    // Per-slot subsystem cleanup. Only subsystems with actual per-slot
    // state get a call here. prop_lifecycle / npc_sync / weather_sync
    // hold GLOBAL state that DisconnectAll handles correctly on full
    // disconnect.
    coop::trash_proxy::OnDisconnectForSlot(slot);   // phase 1: retire the leaver's trash proxies BEFORE the generic mirror drain (else the rooted AStaticMeshActor leaks -- CRITICAL-1)
    coop::trash_channel::OnGrabHolderLeft(slot);    // v84 Increment 2: free any pile the leaver held via a client grab
    coop::puppet_carry_drive::OnPeerLeft(slot);     // v84 Increment 2: drop the leaver's puppet-held clump drive
    coop::wisp_grab_hold::OnPeerLeft(static_cast<uint8_t>(slot));  // v2 wisp choreography: drop the leaver's grab-window puppet hold
    coop::remote_prop::OnDisconnectForSlot(slot);
    coop::item_activate::OnDisconnectForSlot(slot);
    coop::device_occupancy::OnDisconnectForSlot(slot);  // v63: release a leaver's device claims
    coop::comp_sync::OnPeerDisconnect(static_cast<uint8_t>(slot));  // v65: pause the mirror if the decode simulator left
    coop::kerfur_command::OnPeerDisconnect(static_cast<uint8_t>(slot));  // v74: release any kerfur the leaver was owned-following
    coop::voice_chat::OnDisconnectSlot(slot);  // v66: drop the leaver's voice channel + icon state
    coop::sleep_sync::OnDisconnectForSlot(slot);  // v71: drop the leaver from the sleep tally (re-gate)
    coop::player_inventory_sync::OnDisconnectForSlot(slot);  // v73: flush the leaver's inventory to <guid>.json
}

DisconnectStats DisconnectAll() {
    // PHASE 1: tear down trash proxies FIRST -- before ForceRelease (which can
    // ConsumeLocalActor a carried proxy without un-rooting -> a rooted leak).
    // RetireProxy un-roots + destroys + evicts the drive, so ForceRelease below
    // sees no live/rooted proxy and no stale drive entry (structural no-leak).
    coop::trash_proxy::OnDisconnect();
    coop::remote_prop::ForceRelease();
    // P2: a disconnect mid-snapshot must drop the armed claim set (dangling
    // actor pointers must not survive into the next session); no sweep.
    coop::join_membership_sweep::ResetClaimTracking();
    DisconnectStats stats;
    stats.initProcessedDropped = coop::prop_lifecycle::OnDisconnect().initProcessedDropped;
    stats.snapPending = coop::prop_snapshot::OnDisconnect();
    coop::npc_sync::OnDisconnect();
    coop::world_actor_sync::OnDisconnect();    // v80 (B3b): drain WorldActor mirrors (K2 client ones) + clear host reverse-map
    coop::piramid_sync::OnDisconnect();        // v97: drop pending gather + gather-edge map + restored-tick set (hooks stay latched)
    coop::npc_adoption::OnSessionEnd();        // v75: drop pending deferred adoptions + reset latches
    coop::kerfur_prop_adoption::OnSessionEnd(); // K-6: drop pending prop-form kerfur adoptions
    coop::host_spawn_watcher::OnDisconnect();  // M2: drop the ambient-prop death-watch list
    coop::kerfur_convert::OnDisconnect();      // v67: drop pending host-menu converges
    coop::kerfur_entity::OnDisconnect();       // K-3: clear the KerfurId table + free its reserved host ids
    coop::kerfur_command::OnDisconnect();      // v74: drop pending menu commands + owned-follow map
    coop::kerfur_menu_input::OnDisconnect();   // drop the cached session (the InpActEvt_use observer stays registered)
    coop::prop_stick_sync::OnDisconnect();     // v68: drop commit-pending stick records
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
    coop::firefly_sync::OnDisconnect();
    coop::event_cue_sync::OnDisconnect();    // v79 clear the cosmetic-cue poll snapshot
    coop::event_fire_sync::OnDisconnect();   // v95 restore the client scheduler (allEvents.Num) + drop poll baseline/queues
    coop::event_active_sync::OnDisconnect(); // join-during-event Phase 0: drop tracked membership + cached gamemode
    coop::alarm_sync::OnDisconnect();        // v101 drop the cached trigger + poll baseline
    coop::inventory_pickup_sync::OnDisconnect();
    coop::chat_sync::OnDisconnect();
    coop::turbine_sync::OnDisconnect();
    coop::device_occupancy::OnDisconnect();
    coop::console_state_sync::OnDisconnect();
    coop::signal_catch_sync::OnDisconnect();
    coop::sleep_sync::OnDisconnect();
    coop::wisp_attack_sync::OnDisconnect();  // v72: clear damage-cancel latch + handled-wisp edges + pending despawns
    coop::wisp_tear_mirror::OnDisconnect();  // v72: clear any armed victim-death deadline
    coop::wisp_grab_hold::OnDisconnect();    // v2 choreography: release a live self-grab (un-strand MOVE_None) + drop holds
    coop::player_inventory_sync::OnDisconnect();  // v73: host flush all inventories; client clear send-dedup
    coop::email_sync::OnDisconnect();
    coop::signal_sync::OnDisconnect();
    coop::comp_sync::OnDisconnect();
    coop::voice_chat::OnDisconnect();
    coop::window_sync::OnDisconnect();
    coop::grime_sync::OnDisconnect();
    coop::trash_pile_sync::OnDisconnect();
    coop::trash_collect_sync::OnDisconnect();
    coop::trash_channel::OnDisconnect();  // docs/piles/08: drop the per-eid trash sync-time-context map
    coop::puppet_carry_drive::OnDisconnect();  // v84 Increment 2: drop all puppet-held clump drives
    coop::trash_clump_pose_stream::OnDisconnect();  // v85 Increment 2: drop all client per-eid carry drives
    coop::balance_sync::OnDisconnect();  // v30: reset the balance broadcast dedup
    return stats;
}

void TickGameplay(coop::net::Session& session, bool isConnected, bool isHost,
                  bool fleeing) {
    namespace PP = coop::dev::perf_probe;
    // Per-tick drains for subsystems that internally retry until the reliable
    // channel accepts a queued connect-time broadcast (item_activate +
    // weather_sync) and apply any per-peer payloads that arrived BEFORE the
    // corresponding puppet was spawned. Cheap early-return when no pending state.
    { PP::Scope _s{PP::Bucket::ItemConnect};   coop::item_activate::TickConnect(); }
    { PP::Scope _s{PP::Bucket::WeatherConnect}; coop::weather_sync::TickConnect(); }
    // L5 per-sync attribution (2026-06-23, DIAGNOSTIC): the perf_probe Interactable bucket is a CATCH-ALL
    // for this whole block, so it named the BLOCK (~45-100ms/2s steady-state spike) but not the member.
    // ScopedWalkTimer logs [WALK-TIME] sync:<name> per call >=1ms -> the culprit self-identifies in one run
    // (cheap no-op syncs stay silent). Remove once the heavy one is found + fixed. WT = ue_wrap::ScopedWalkTimer.
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:interactable"}; coop::interactable_sync::Tick(); }  // retry deferred door/light/container applies (still streaming in)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:keypad"}; coop::keypad_sync::Tick(); }        // v33 keypad poll + deferred-apply retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:time"}; coop::time_sync::Tick(); }          // v36 world clock: host throttled poll+broadcast (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sky"}; coop::sky_sync::Tick(); }           // v44 night-sky: host throttled push (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:power"}; coop::power_sync::Tick(); }          // v46 base power panel: poll breaker edges + deferred-apply retry (symmetric)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:atv"}; coop::atv_sync::Tick(); }            // v47 ATV: occupant streams its pose / mirror drives the interp (host+client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drone"}; coop::drone_sync::Tick(); }          // v48 delivery drone: host streams transform / client suppresses tick + mirrors
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:turbine"}; coop::turbine_sync::Tick(); }        // v61 wind turbines: host ~1 Hz driver-float poll / client deferred-apply retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:event_cue"}; coop::event_cue_sync::Tick(); }      // v79 cosmetic event cues (B1): host ~1 Hz new-PSC poll -> EventCue broadcast (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:event_fire"}; coop::event_fire_sync::Tick(); }     // v95 scheduled events: host 1 Hz passEvents growth poll -> EventFire / client allEvents suppress + replay drain
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:event_active"}; coop::event_active_sync::Tick(); }  // join-during-event Phase 0: host 1 Hz activeEvents_senders diff -> BEGIN/END edge log (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:alarm"}; coop::alarm_sync::Tick(); }               // v101 base radar alarm: 1 Hz active-bit poll BOTH roles (host broadcasts transitions; client forwards local ones)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:device_occupancy"}; coop::device_occupancy::Tick(); }    // v63 device occupancy: activeInterface edge poll + pending claim retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:console_state"}; coop::console_state_sync::Tick(); }  // v64 signal-catcher: host sky poll / client mirror sweep / desk + dish owner streams
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal_catch"}; coop::signal_catch_sync::Tick(); }   // v70: catch/cleared detectors (1 Hz) + the joiner's pending download adopt
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:email"}; coop::email_sync::Tick(); }          // v64 inc 2: email shadow poll (1 Hz; appends -> chunked broadcast, shrinks -> content-keyed deletes)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal"}; coop::signal_sync::Tick(); }         // v65: saved-signals shadow poll (same shape on gamemode.savedSignals_0)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:comp"}; coop::comp_sync::Tick(); }           // v65: decode-pane simulator stream + comp_data edges + client world-up unlatch
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:voice"}; coop::voice_chat::Tick(); }          // v66: voice frame pump (mic drain -> send; inbox -> jitter; positions; state edges)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:order"}; coop::order_sync::Tick(); }           // v49 drone economy: client polls+forwards orders / host commits assembled orders
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:window"}; coop::window_sync::Tick(); }         // v41 base-window clean: poll for wipes + deferred-apply retry (symmetric)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:grime"}; coop::grime_sync::Tick(); }          // v42 surface grime: poll wipes + death-watch destroy + deferred-apply retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:npc_host"}; coop::npc_sync::TickPoseStream(); }    // v37 HOST: read NPCs -> publish EntityPose batch (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:npc_client"}; coop::npc_mirror::TickClientNpcs(); }  // v37 CLIENT: apply batch + drive mirror interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:worldactor_host"}; coop::world_actor_sync::TickPoseStream(); }       // v80 HOST: read event WorldActors -> publish WorldActorPose batch (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:worldactor_client"}; coop::world_actor_sync::TickClientWorldActors(); } // v80 CLIENT: apply batch + drive WorldActor mirror interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:piramid"}; coop::piramid_sync::Tick(); }             // v97: pre-arm probe (250 ms gate) / host gather-edge sweep (1 s) / client mirror restore + pending gather replay
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:trash_clump_pose"}; coop::trash_clump_pose_stream::TickApplyAndDrive(session); } // v85 CLIENT: apply host-auth carry/flight pose batch + per-eid interp (client-only, no-op on host)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::host_spawn_watcher::TickWatchedProps(&session); }  // M2: ambient-prop (pinecone) SetLifeSpan-expiry / consumption despawn -> PropDestroy(eid)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_convert::Tick(); }  // v67: drain deferred kerfur conversion requests/converges (cheap no-op when empty)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::kerfur_command::Tick(); }  // v74: drain menu commands + advance the ownership-follow loop (cheap no-op when idle)
    { PP::Scope _s{PP::Bucket::TrashWatch};    coop::prop_stick_sync::Tick(); }  // v68: broadcast recorded stick commits NOW -- must precede local_streams' release edge (net_pump runs TickGameplay first)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:pause_guard"}; coop::pause_guard::Tick(isConnected); }  // 2026-07-04: coop no-pause invariant -- un-pause the world while connected (ESC menu stays usable; a paused peer froze its pose stream)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:save_cycle_off"}; coop::save_block::Tick(&session); }  // 2026-07-04: client native save-cycle OFF -- hold gamemode.disableSave=true (saveSlot_C::save gates gather+write on it); the SaveGameToSlot disk hook stays as the belt
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:sleep"}; coop::sleep_sync::Tick(); }  // v71: isSleep edge poll + WAITING dilation enforcement + the client need clamp
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:wisp_attack"}; coop::wisp_attack_sync::Tick(); }  // v72: host detect wisp-grabs-client -> neutralize + relay (host-only, no-op on client)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:wisp_tear"}; coop::wisp_tear_mirror::Tick(); }  // v72: discharge the victim's scheduled ragdoll death (any peer, no-op until armed)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:player_inventory"}; coop::player_inventory_sync::Tick(); }  // v73: inventory read-verify self-test (Inc2; no-op unless inventory_selftest=1)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:inventory_probe"}; coop::dev::inventory_probe::Tick(); }  // v73 Inc4: SP apply round-trip self-test (no-op unless inventory_probe=1)
    // v57: trashBitsPile collect-counter poll + depletion death-watch. (The chipPile mirror-PILE
    // death-watch that used to run here was RETIRED 2026-06-17 -- a chipPile re-grab now fires from
    // the InpActEvt_use PRE observer that trash_collect_sync::Install registers, not a per-tick
    // near-camera liveness sweep that misread any non-grab pile death as a grab.)
    { PP::Scope _s{PP::Bucket::TrashWatch};
      const bool inTransition = fleeing || coop::join_progress::Active();
      coop::trash_pile_sync::Tick(inTransition); }  // counter poll + depletion death-watch (transition-gated)
    if (isHost) { PP::Scope _s{PP::Bucket::TrashWatch};
      coop::trash_channel::TickCarry(session);          // docs/piles/08 + CLOSE-B: pending-grab TTL + land-settle commit (closes the carry latch on the real land)
      coop::puppet_carry_drive::Tick(session); }        // v84/v85 Increment 2: drive each puppet-held clump to its hand + publish the host-auth carry/flight pose batch (AFTER TickCarry so the latch is current)
      // (trash_collect_sync::Tick -- the proximity re-pile death-watch -- is RETIRED 2026-06-21, RULE 2:
      //  the re-pile is caught deterministically at its BeginDeferred via the UFunction::Func thunk.)
    { PP::Scope _s{PP::Bucket::Balance};       coop::balance_sync::Tick(); }       // v30: host polls saveSlot.Points + broadcasts on change; client retries the pending mirror apply
    coop::dev::drone_probe::Install();  // dev-only delivery-drone RE probe (ini drone_probe=1; self-latches + retries until the BP class loads)
    coop::dev::drone_probe::Tick(isConnected, isHost);  // polls drone/order/radar; with drone_probe_drive=1 ALSO auto-fires one delivery (host) / order (client)
    coop::dev::native_pile_inert_probe::Install();  // GO/NO-GO gate for nativizing the trash mirror (ini native_pile_inert_probe=1)
    coop::dev::native_pile_inert_probe::Tick(isConnected, isHost);  // spawns 1 rooted runtime chipPile, logs [INERT-PROBE] IsLive/class 60s -> does a live-ubergraph native stay inert?
    coop::dev::client_model_probe::Install();  // kel-vs-scientist side-by-side visual check (ini client_model_probe=1)
    coop::dev::client_model_probe::Tick(isConnected, isHost);  // spawns the comparison pair in front of the player -> one clean look settles the cook verdict
    coop::dev::pinecone_probe::Install();  // dev-only pinecone-scare sync verification (ini pinecone_probe=1)
    coop::dev::pinecone_probe::Tick(isConnected, isHost);  // host force-spawns one pinecone ~5s after a client connects -> confirm it mirrors
    coop::dev::sleep_probe::Install();     // dev-only v71 sleep-gate exerciser (ini sleep_probe=1)
    coop::dev::sleep_probe::Tick(isConnected, isHost);  // client sleeps T+15, host T+25 (ACCELERATE), host wakes T+40 (END)
    coop::dev::lightswitch_probe::Install();  // dev-only light-switch sync RE probe (ini lightswitch_probe=1)
    coop::dev::lightswitch_probe::Tick();     // one-shot synthetic flip -> is SetActive BP-internal + does use() flip both switch+lights
    coop::dev::keypad_probe::Install();        // dev-only keypad digit-entry RE probe (ini keypad_probe=1)
    coop::dev::keypad_probe::Tick();           // synthetic inputNumber sequence -> does it append inPassword + flip isAcc (increment-2 design)
    coop::dev::door_probe::Install();          // dev-only door state-machine RE probe (ini door_probe=1)
    coop::dev::door_probe::Tick();             // scripted doorOpen/suppress/settime experiment -> what re-closes a host door?
}

}  // namespace coop::subsystems
