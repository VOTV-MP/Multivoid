// coop/session/props_subsystems.cpp -- see coop/session/props_subsystems.h.

#include "coop/session/props_subsystems.h"

#include <cstdint>

#include "coop/dev/cleanup_trace.h"  // each call named as it runs when the row is set
#include "coop/props/bracket_position_wait.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_break.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/fireext_sync.h"
#include "coop/props/miner_sync.h"
#include "coop/props/fireext_thrust_sync.h"
#include "coop/props/food_tick_gate.h"
#include "coop/props/glowstick_sync.h"
#include "coop/props/hacksaw_cut.h"
#include "coop/props/pack_trash_intent.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_can_state.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_join.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_food_sync.h"
#include "coop/props/prop_hold_claim.h"
#include "coop/props/prop_nametag.h"
#include "coop/props/prop_pry_host.h"
#include "coop/props/prop_pry_sync.h"
#include "coop/props/prop_record_refresh.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/prop_stick_sync.h"
#include "coop/props/prop_throw.h"
#include "coop/props/prop_tool_flags.h"
#include "coop/props/remover_request.h"
#include "coop/props/roomba_sync.h"
#include "coop/props/snack_act.h"
#include "coop/props/trash_channel.h"
#include "coop/props/trash_clump_pose_stream.h"
#include "coop/props/trash_collect_sync.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/props/trash_sweep.h"
#include "coop/props/dingus_sync.h"
#include "coop/props/bucket_sync.h"

namespace coop::subsystems::props {

void OnPeerLeft(int slot) {
    // Abort any snapshot drain to this slot, so it does not iterate every candidate into a dead
    // connection.
    COOP_CLEANUP_STEP("peer-left", coop::prop_snapshot::CancelForSlot(slot));
    // A slot teardown is a roster row transition: the leaver's half assemblies and seed brackets
    // must not survive into a recycled occupant.
    COOP_CLEANUP_STEP("peer-left", coop::prop_save_data::OnPeerGone(static_cast<uint8_t>(slot)));
    COOP_CLEANUP_STEP("peer-left", coop::props::container_birth::OnPeerGone(static_cast<uint8_t>(slot)));  // a container transfer it had in flight
    COOP_CLEANUP_STEP("peer-left", coop::props::container_act_host::OnPeerGone(static_cast<uint8_t>(slot)));  // a record it held in escrow goes back
    COOP_CLEANUP_STEP("peer-left", coop::prop_nametag::OnPeerGone(static_cast<uint8_t>(slot)));  // a nametag it had half sent
    COOP_CLEANUP_STEP("peer-left", coop::prop_act_host::OnPeerGone(static_cast<uint8_t>(slot)));  // a take or use it had reserved
}

void OnDisconnect() {
    COOP_CLEANUP_STEP("disconnect", coop::remover_request::OnDisconnect());  // drop the remover queues and the shown notices
    COOP_CLEANUP_STEP("disconnect", coop::props::prop_damage_gate::OnDisconnect());  // say the cancelled counts, forget the once-per-class latches
    COOP_CLEANUP_STEP("disconnect", coop::props::container_act_host::OnDisconnect());  // restore the records held in escrow, drop the tallies and refusals
    COOP_CLEANUP_STEP("disconnect", coop::prop_act_host::OnDisconnect());  // drop the take-or-use tallies, refusals and reservations
    COOP_CLEANUP_STEP("disconnect", coop::snack_act::OnDisconnect());  // drop the snacks marked for publication
    COOP_CLEANUP_STEP("disconnect", coop::props::container_act_request::OnDisconnect());  // drop the pending container act, its expired records and unsent commits
    COOP_CLEANUP_STEP("disconnect", coop::prop_act_request::OnDisconnect());  // drop the pending take or use, its latch and its expired records
    COOP_CLEANUP_STEP("disconnect", coop::prop_hold_claim::OnDisconnect());  // drop the refusal records and the notice cooldown
    COOP_CLEANUP_STEP("disconnect", coop::props::prop_food_sync::OnDisconnect());  // the food lane's tallies, latches and waiting records
    COOP_CLEANUP_STEP("disconnect", coop::fireext_sync::OnDisconnect());  // the sprays shown, the thrusts waiting for a copy, the asks and the tallies
    COOP_CLEANUP_STEP("disconnect", coop::miner_sync::OnDisconnect());  // the rigs seen, the waiting states, the budgets and the tallies
    COOP_CLEANUP_STEP("disconnect", coop::food_tick_gate::OnDisconnect());  // say the refused food ticks, start the next session from zero
    COOP_CLEANUP_STEP("disconnect", coop::prop_tool_flags::OnDisconnect());  // drop the pending press and the counters
    COOP_CLEANUP_STEP("disconnect", coop::props::container_break::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::props::hacksaw_cut::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::prop_stick_sync::OnDisconnect());  // drop commit-pending stick records
    COOP_CLEANUP_STEP("disconnect", coop::prop_pry_sync::OnDisconnect());  // the session's pry counters and its pried-keys record
    COOP_CLEANUP_STEP("disconnect", coop::prop_pry_host::OnDisconnect());  // the host's pry counters (a sender's queue goes with its slot)
    COOP_CLEANUP_STEP("disconnect", coop::roomba_sync::OnDisconnect());  // the cleaners known, the presses and states queued
    COOP_CLEANUP_STEP("disconnect", coop::fireext_thrust_sync::OnDisconnect());  // the thrusting extinguishers it held
    COOP_CLEANUP_STEP("disconnect", coop::dingus_sync::OnDisconnect());  // the found set, the births queued, the judge's budgets and tallies
    COOP_CLEANUP_STEP("disconnect", coop::bucket_sync::OnDisconnect());  // what it knew of the buckets, and its counters
    COOP_CLEANUP_STEP("disconnect", coop::glowstick_sync::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::prop_save_data::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::props::container_contents_sync::OnDisconnect());  // dirty set + retry + parked + assembler
    COOP_CLEANUP_STEP("disconnect", coop::props::prop_record_refresh::OnDisconnect());  // session release, counters, the drill red's map
    COOP_CLEANUP_STEP("disconnect", coop::props::prop_can_state::OnDisconnect());  // its counters (what is known of every can goes with the world)
    COOP_CLEANUP_STEP("disconnect", coop::prop_throw::OnDisconnect());  // the thrower's records of its own throws
    COOP_CLEANUP_STEP("disconnect", coop::trash_pile_sync::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::pack_trash_intent::OnDisconnect());  // with no session the game's own bagging stands
    COOP_CLEANUP_STEP("disconnect", coop::trash_collect_sync::OnDisconnect());
    COOP_CLEANUP_STEP("disconnect", coop::trash_channel::OnDisconnect());  // drop the per-eid trash sync-time-context map
    COOP_CLEANUP_STEP("disconnect", coop::trash_sweep::OnDisconnect());  // drop the swept clumps still rolling
    COOP_CLEANUP_STEP("disconnect", coop::trash_clump_pose_stream::OnDisconnect());  // drop all client per-eid carry drives
    COOP_CLEANUP_STEP("disconnect", coop::prop_drive_host::OnDisconnect());  // drop the host's driven-prop set and the client syncers' table (prop_syncer_host)
    COOP_CLEANUP_STEP("disconnect", coop::prop_drive_join::Clear());  // the owed joins and owed members; the tie index is world state and stays
    COOP_CLEANUP_STEP("disconnect", coop::props::bracket_position_wait::Reset());  // the spawns waiting for a bracket that will not close go with the session
    COOP_CLEANUP_STEP("disconnect", coop::prop_drive_stream::OnDisconnect());  // every driven prop here gets its physics back
}

}  // namespace coop::subsystems::props
