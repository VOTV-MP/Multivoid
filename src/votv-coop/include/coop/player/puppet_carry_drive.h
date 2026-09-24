// coop/player/puppet_carry_drive.h -- HOST-side per-tick drive of a PUPPET-held trash clump to its hand.
//
// When a client grabs a chipPile, the host executes playerGrabbed on that client's puppet. The grab
// ENGAGES and HOLDS on an unpossessed puppet, on the puppet's own physics handle, but the puppet's
// tick is off, and that tick is what feeds the handle its target every frame
// (SetTargetLocationAndRotation) -- so the held clump hangs at the grab spot and never tracks to the
// hand. The host streams the clump's HOST-side pose to every peer, so the clump must BE at the
// puppet's hand ON THE HOST. This module makes the one call the puppet's tick does not: it gives
// the handle its target each tick, from the puppet's synced aim. The clump stays a simulating body
// on a force-limited spring, as it is in the player's own hand: what it is pushed into pushes back.
//
// HOST-ONLY. Game-thread only (all entries run on the net-pump game thread, like trash_channel).
// One feature per file: NOT folded into local_streams (the LOCAL pose stream) nor trash_channel (the
// state machine) -- this is the puppet's handle feeder and its pose publisher, a subsystem of its own.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::puppet_carry_drive {

// HOST: puppet at peer `slot` just grabbed `clump` (the garbageClump in its grabbing_actor) for trash
// entity `eid`, via trash_channel::OnGrabIntent. Register it for the per-tick hand-follow drive. The
// clump's GUObjectArray index is captured here for cross-tick liveness (IsLiveByIndex). `clumpRot` is
// the rotation the grab's convert placed the clump at for every peer; the hold keeps it, turned with the
// puppet's view. Idempotent: a re-register for the same eid updates the clump/slot in place. Game thread.
void NotePuppetHeld(coop::element::ElementId eid, uint8_t slot, void* clump, const ue_wrap::FRotator& clumpRot);

// HOST: entity `eid` left its holder's hand and lives on -- a throw, or a holder that is gone
// (trash_channel has released the puppet's handle). Stop feeding the handle but KEEP streaming the
// free body's pose, so every client renders the arc, until the carry latch closes (the land's
// commit, or the clump at rest). Game thread.
void NoteLetGo(coop::element::ElementId eid);

// HOST: per-gameplay-tick pump (called from subsystems::TickGameplay AFTER trash_channel::TickCarry, so
// the carry latch is current before the drive guards on IsCarrying). For each registered held clump:
// guard (latch open, clump live, puppet able to hold); if NOT flying, the puppet's physics handle is given its target (camera + aim*grabLen);
// then PUBLISH its pose on `s`'s host-originated TrashCarryPose queue (carry + flight). Drops the
// entry when the clump dies or the carry latch closes (the land, the rest); a puppet that is gone
// lets its clump go (trash_channel::OnHolderGone) and the entry streams on as a flight. Game thread.
void Tick(coop::net::Session& s);

// HOST: the host's own hand or a broom stroke took entity `eid` from a client's carry -- out of the
// puppet's hand, or a pile the throw landed as, re-grabbed or swept before its land committed. The
// puppet's handle lets go if it still holds (two handles must not pull one body) and the drive ends;
// nothing is retired: the entity lives on in the taker's clump. Game thread.
void OnTakenOver(coop::element::ElementId eid);

// HOST: full reset (net disconnect). Game thread.
void OnDisconnect();

}  // namespace coop::puppet_carry_drive
