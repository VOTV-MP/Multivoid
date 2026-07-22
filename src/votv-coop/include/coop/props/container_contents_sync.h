// coop/props/container_contents_sync.h -- take-4 R11: a WORLD container's CONTENTS as
// host-authored state.
//
// RE of record: research/findings/inventory-items/votv-container-contents-gobjstack-RE-2026-07-22.md
//
// THE ROOT, stated as an invariant rather than a site list: a container's contents are NOT on the
// container. Every container in the game reads them from ONE global per-peer array
// `saveSlot.GObjStack : TArray<struct_mObject>`, addressed by `propInventory_C.Index`; the actor
// holds only that index and a cached volume. The client receives GObjStack once inside the join
// save-transfer blob and NEVER again, because every mutating verb (addObject / addLoot / takeObj)
// dispatches EX_LocalVirtualFunction -- BP-internal, invisible to both the ProcessEvent detour and
// the UFunction::Func patch. So the drone delivery landed full on the host and 0.0 on the client,
// ~19 minutes after the container itself had been adopted. R11 is one visible symptom of that one
// missing lane, not a drone bug.
//
// AUTHORITY was already correct and is NOT changed here: drone_sync parks the client's drone
// ReceiveTick, so the client never ran a delivery. This module adds only the missing LANE.
//
// SHAPE
//   edge   -- ue_wrap/core/vm_dispatch (the GNatives 0x45 swap) on `addObject` + `takeObj`.
//             The callback only MARKS THE COMPONENT DIRTY; it takes no action and reads no
//             arguments, which is why it is correct for every caller in the firing set
//             (drone::compileOrder x2, propInventory::addLoot x4, prop_container::
//             putObjectIn_overlap, the prop_container ubergraph, lib::findInventoryObject,
//             prop_container::{extract,getObject}, checkObjectsVolume).
//   send   -- HOST ONLY, on the sweep after the verb: read the container's GObjStack slice and
//             broadcast it whole. Clients never send; world containers are host-owned.
//   apply  -- raw-write the receiver's own GObjStack slot, then re-derive the SETTER-MANAGED
//             state through the engine's own verbs: Aprop_container_C::updateVolumesAndMass()
//             (currVol + Mass) and propInventory_C::recalculateNames() (display). We never
//             raw-write currVol/Mass/names ([[feedback-no-raw-write-of-setter-managed-fields]]).
//             `addObject` CANNOT be the apply verb -- measured, it takes a live AActor* and
//             serializes it itself, so it cannot ingest a record off the wire.
//             `checkObjectsVolume` is NOT called on apply -- measured, it calls takeObj and is an
//             overflow EJECTOR; calling it would destroy contents. updateVolumesAndMass is safe:
//             measured, it calls only `Get Volume`.
//
// TWO HARD BOUNDARIES, both fail-closed
//   1. PERSONAL inventory is NOT this lane's business. mainPlayer and ui_playerInventory are
//      backed by the SAME global GObjStack, so a host-authored write over another peer's slice
//      would WIPE that player's inventory. The discriminator is `propInventory_C.Player`
//      (bool @ 0x00F9, true only on prop_inventoryContainer_player's component template). If it
//      is true -- OR if the offset cannot be resolved at all -- the container is SKIPPED.
//   2. A NESTED container ships WITHOUT its index. struct_save does not contain itself: a
//      container inside a container is represented by INDIRECTION -- its record's ints[] carries
//      its own GObjStack index, which names a slot in the SENDER's array and is meaningless on
//      the receiver (it would resolve to some unrelated container's contents -- worse than
//      empty). So a record whose class descends from prop_container_C ships with ints[] CLEARED
//      and arrives EMPTY-not-BROKEN. The transitive walk (with a receiver-side visited set,
//      depth cap and total-size cap -- a sender-side cap is not a cap) is INCREMENT 2.
//
// Wire: ReliableKind::ContainerContents (BlobChunkPayload; blob = [u8 op=0][u32 eid][u16 n] then
// n x the coop/items/save_record_wire per-record grammar). Never refanned; receivers accept
// senderSlot==0 only. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync {

void Install(coop::net::Session* session);

// Registers the 0x45 verbs on first call, then drives the vm_dispatch FName resolve and the
// dirty-set drain. Near-free when the dirty set is empty (the steady state).
void Tick();

// ContainerContents chunks.
void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: ship the joiner one contents blob per live world container (principle 8 -- the join
// save-transfer blob is a snapshot taken at OnRequest time; this is the anchor that corrects it).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::props::container_contents_sync
