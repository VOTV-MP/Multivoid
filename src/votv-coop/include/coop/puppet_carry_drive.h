// coop/puppet_carry_drive.h -- HOST-side per-tick drive of a PUPPET-held trash clump to its hand.
//
// Increment 2 (chipPile CLIENT-grab, docs/piles/08): when a client grabs a pile, the host executes
// playerGrabbed on puppet-N. The probe (research/findings/votv-puppet-grab-feasibility-RE-2026-06-22)
// proved the grab ENGAGES + HOLDS on an unpossessed puppet, but the puppet's own ReceiveTick does NOT
// drive the PHC's per-tick SetTargetLocationAndRotation -- so the held clump FLOATS at the grab spot,
// it never tracks to the puppet's hand. Because the host streams the clump's HOST-side pose to all
// peers (trash_channel + PropPose), the clump must BE at the puppet's hand ON THE HOST. This module
// kinematically positions it each tick from the puppet's synced aim -- the same kinematic drive
// local_streams applies to the host's OWN held props (MTA CClientVehicle target-follow shape).
//
// HOST-ONLY. Game-thread only (all entries run on the net-pump game thread, like trash_channel).
// One-feature-per-file (RULE 2026-05-25): NOT folded into local_streams (the LOCAL pose stream) nor
// trash_channel (the state machine) -- this is the puppet-held kinematic follower, a distinct subsystem.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::puppet_carry_drive {

// HOST: puppet at peer `slot` just grabbed `clump` (the garbageClump in its grabbing_actor) for trash
// entity `eid`, via trash_channel::OnGrabIntent. Register it for the per-tick hand-follow drive. The
// clump's GUObjectArray index is captured here for cross-tick liveness (IsLiveByIndex). Idempotent: a
// re-register for the same eid updates the clump/slot in place. Game thread.
void NotePuppetHeld(coop::element::ElementId eid, uint8_t slot, void* clump);

// HOST: per-gameplay-tick pump (called from subsystems::TickGameplay AFTER trash_channel::TickCarry,
// so the carry latch is current before the drive guards on IsCarrying). For each registered held clump:
// guard (clump live, puppet live, still carrying) then SetActorLocation(clump, puppetHead + aim*grabLen).
// Drops the entry when the clump dies, the puppet leaves, or the carry latch closes (the re-pile land).
// Game thread.
void Tick();

// HOST: peer `slot` disconnected -- drop all its held-clump drive entries. Game thread.
void OnPeerLeft(uint8_t slot);

// HOST: full reset (net disconnect). Game thread.
void OnDisconnect();

}  // namespace coop::puppet_carry_drive
