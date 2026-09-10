// coop/net/session_lanes.h -- the GNS priority-lane mapping, the host-relay whitelist and the
// pre-world send gate, shared by the Session's send paths and its relay path. Internal to the
// Session TUs (src tree, not include/); the functions are inline, so the two TUs share one
// definition.

#pragma once

#include "coop/net/protocol.h"  // ReliableKind

namespace coop::net {

// The priority lanes. session_status's ConfigureLanesForPeer hard-codes the count to 3, and
// session.cpp pins the two together with a static_assert on Lane::Count.
enum class Lane : int {
    High = 0,
    Normal = 1,
    Bulk = 2,
    Count = 3,
};

// The three admission kinds appear in none of the three routers below, and that is not an
// omission: they never travel through a slot send (no slot exists yet; the raw connection send
// pins them to lane 0), they are never relayed (a proof is between two endpoints), and they
// have no pre-world gate because they run before its premise. This predicate lets the receive
// path drop a replay of one from an already-admitted peer.
inline bool IsAdmissionKind(ReliableKind k) {
    return k == ReliableKind::AuthHello || k == ReliableKind::AuthChallenge ||
           k == ReliableKind::AuthProof;
}

inline Lane LaneForKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::TeleportClient: return Lane::High;
    case ReliableKind::RestoreVitals:  return Lane::High;
    case ReliableKind::ItemActivate:   return Lane::High;
    case ReliableKind::PlayerDamage:   return Lane::High;  // combat event -- prioritize
    // Spawn and Destroy share a lane: GNS orders delivery within a lane, not across lanes, and a
    // destroy overtaking its spawn under backpressure leaves a phantom actor.
    case ReliableKind::PropSpawn:      return Lane::Bulk;
    case ReliableKind::PropDestroy:    return Lane::Bulk;
    // A prop's save record shares PropSpawn's lane on purpose: same FIFO, so a record can never
    // overtake the birth it belongs to. The intent rides it too, so a client's authored record
    // cannot pass the destroy or spawn that produced it.
    case ReliableKind::PropSaveData:       return Lane::Bulk;
    case ReliableKind::PropSaveDataIntent: return Lane::Bulk;
    // PropConvert destroys the ball and spawns the pile, so it shares the spawn lane; on another
    // lane a convert could overtake the ball's spawn and leave a never-destroyed ball.
    case ReliableKind::PropConvert:    return Lane::Bulk;
    // PropSnapPos rides Bulk so it lands after the connect snapshot's spawns for the same join.
    case ReliableKind::PropSnapPos:    return Lane::Bulk;
    // PropDropIntent is order-paired with PropDestroy: the race defence is that the pickup's husk
    // destroy delivers before the place's intent re-spawn, which holds only within a lane; on the
    // default lane the intent could overtake the destroy (the place lost) or the destroy land after
    // the re-spawn (a fresh copy retracted on every peer).
    case ReliableKind::PropDropIntent: return Lane::Bulk;
    // ReelEjectIntent, the same order-paired family: the ejecting client may pocket the reel a pass
    // later (a keyed destroy on Bulk), and in-lane ordering makes the host author the spawn first.
    case ReliableKind::ReelEjectIntent: return Lane::Bulk;
    case ReliableKind::EntitySpawn:    return Lane::Bulk;
    case ReliableKind::EntityDestroy:  return Lane::Bulk;
    // The world-actor pair shares the spawn lane for the same reason. Host-authoritative: not
    // relayable, not pre-world.
    case ReliableKind::WorldActorSpawn:   return Lane::Bulk;
    case ReliableKind::WorldActorDestroy: return Lane::Bulk;
    // The snapshot brackets share PropSpawn's lane, so they deliver as Begin, every spawn,
    // Complete; on another lane Complete could overtake the stream and lift the joiner's cover
    // mid-build.
    case ReliableKind::SnapshotBegin:    return Lane::Bulk;
    case ReliableKind::SnapshotComplete: return Lane::Bulk;
    // The save blob is phase-ordered before the bracket (the client sends ClientWorldReady only
    // after loading the save), so Bulk costs nothing, and Begin must precede its chunks in-lane.
    case ReliableKind::SaveTransferBegin: return Lane::Bulk;
    case ReliableKind::SaveTransferChunk: return Lane::Bulk;
    // The host-to-client inventory blob must reach the joiner before its world loads (the inventory
    // is substituted before materialisation), so it rides Normal, ahead of the Bulk streams, and is
    // pre-world sendable; a self-contained blob with no in-lane dependency.
    case ReliableKind::PlayerInventoryBlob: return Lane::Normal;
    // PropStickState and PropRelease are order-paired: the release gate reads the frozen state the
    // stick writes, and a release overtaking its stick re-enables physics on a just-stuck mirror.
    // Pinned, so a single-kind lane move cannot split them.
    case ReliableKind::PropStickState: return Lane::Normal;
    case ReliableKind::PropRelease:    return Lane::Normal;
    // The four ATV kinds are order-paired: a release or destroy overtaking the last pose would
    // re-enable physics or tear down a mirror mid-update, and a runtime-spawned ATV must spawn
    // before its first pose. Pinned together.
    case ReliableKind::AtvState:       return Lane::Normal;
    case ReliableKind::AtvRelease:     return Lane::Normal;
    case ReliableKind::AtvSpawn:       return Lane::Normal;
    case ReliableKind::AtvDestroy:     return Lane::Normal;
    // DeskState, DeskInput and DeskScanEvent are order-coupled (adopt before deltas, charge before
    // scan); pinned together.
    case ReliableKind::DeskState:      return Lane::Normal;
    case ReliableKind::DeskInput:      return Lane::Normal;
    case ReliableKind::DeskScanEvent:  return Lane::Normal;
    // LaptopState's scalar edges and their content chunks assume in-lane order; pinned. The disc
    // prop lifecycle rides Bulk independently by design: receivers never act on LaptopState for
    // destroys.
    case ReliableKind::LaptopState:    return Lane::Normal;
    // PlayDeckEvent is order-coupled with DeskInput and with SavedSignalAppend (a play must land
    // after its row's append); pinned.
    case ReliableKind::PlayDeckEvent:  return Lane::Normal;
    // PhysModsState's ops, canonical and deny assume in-lane order (an op must not overtake the
    // canonical it was diffed against); pinned. Not relayable: ops are host-terminal, and the
    // canonical is host-authored.
    case ReliableKind::PhysModsState:  return Lane::Normal;
    // The drive-chain trio assumes in-lane order between a slot line, the payload row it references
    // and a rack pair; pinned together.
    case ReliableKind::DriveSlotState: return Lane::Normal;
    case ReliableKind::DrivePayload:   return Lane::Normal;
    case ReliableKind::RackState:      return Lane::Normal;
    // MeadowAppend and MeadowDelete share one lane: the join seed's no-reorder argument assumes one
    // FIFO stream per connection.
    case ReliableKind::MeadowAppend:   return Lane::Normal;
    case ReliableKind::MeadowDelete:   return Lane::Normal;
    // CoinGunSell rides Bulk because PropDestroy does: the sale must arrive in front of the
    // sender's own destroy of the same prop, since the host mints from the sold prop's component,
    // and on separate lanes the destroy could overtake the sale. If PropDestroy moves lane, this
    // moves with it.
    case ReliableKind::CoinGunSell:    return Lane::Bulk;
    // CoinCollect rides Bulk with the coin family: it races the coin's own WorldActorDestroy, and
    // sharing the destroy lane keeps the forward in the same FIFO. Moves with PropDestroy.
    case ReliableKind::CoinCollect:    return Lane::Bulk;
    // CoinGunResult is host-to-one-client and deliberately unpinned: it is ordered against nothing,
    // and takes the default by decision.
    case ReliableKind::MeadowOrder:    return Lane::Normal;  // an order line must not overtake the append it references
    // RosterRow and the email and signal families are pinned to the default they ride: the
    // ready-edge seed's exactly-once and clear-before-chunks arguments need the roster transition,
    // the seed rows and the live rows in one FIFO per slot.
    case ReliableKind::RosterRow:         return Lane::Normal;
    case ReliableKind::EmailAppend:       return Lane::Normal;
    case ReliableKind::EmailDelete:       return Lane::Normal;
    case ReliableKind::SavedSignalAppend: return Lane::Normal;
    case ReliableKind::SavedSignalDelete: return Lane::Normal;
    // The laptop family shares LaptopState's lane: a park pairs with the content stream behind it,
    // the quad batches are cross-referenced with slot edges, and the floppy box rides the family
    // lane for the one-FIFO discipline.
    case ReliableKind::LaptopBlob:     return Lane::Normal;
    case ReliableKind::LaptopQuad:     return Lane::Normal;
    case ReliableKind::FloppyBoxState: return Lane::Normal;
    // The container-contents slice stays behind the entity lifecycle it references: a blob for an
    // eid whose spawn has not landed parks and retries, and one FIFO makes the park rare.
    case ReliableKind::ContainerContents: return Lane::Normal;
    default:                           return Lane::Normal;
    }
}

// The host relay: the reliable kinds the host forwards from one client to the others,
// peer-originated gameplay only. A client's equipment toggle must show on its puppet
// everywhere; a client's drop, destroy or throw must replicate; a symmetric interactable's edge
// must reach the other clients. Not relayed: the host-authoritative kinds (weather, entity
// spawns and destroys, the dev keys, damage) originate on the host and go direct, and the
// router trust-gates them on the host slot; the handshake kinds are point-to-point or
// host-originated. PropPose rides the unreliable relay.
inline bool IsClientRelayableReliableKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::ItemActivate:
    case ReliableKind::PropSpawn:
    case ReliableKind::PropDestroy:
    case ReliableKind::PropConvert:       // a client's clump-to-pile convert
    case ReliableKind::PropRelease:
    case ReliableKind::PropStickState:    // a client's wall-attachable stick
    case ReliableKind::DoorState:
    case ReliableKind::LightState:
    case ReliableKind::ContainerState:
    case ReliableKind::GarageDoorState:   // symmetric
    case ReliableKind::ApplianceState:    // symmetric
    case ReliableKind::LockerDoorState:   // symmetric
    case ReliableKind::PowerControlState: // symmetric
    case ReliableKind::AtvState:          // occupant- or grabber-authoritative
    case ReliableKind::AtvRelease:        // the grabber's release edge
    // DeskState is not relayable: adopt-only, host to joiner; live desk input rides DeskInput.
    case ReliableKind::DeskInput:         // presser-authored deltas
    case ReliableKind::DeskScanEvent:     // presser-authored; every mirror replays the visual
    case ReliableKind::DeskSndFx:         // presser-authored
    case ReliableKind::PlayDeckEvent:     // presser-authored; any peer may stop
    case ReliableKind::DriveSlotState:    // any-peer idempotent state; the host is canonical on conflict
    case ReliableKind::DrivePayload:      // writer-authored rows
    case ReliableKind::DishAimState:      // claim-owner-authoritative
    case ReliableKind::KeypadState:
    case ReliableKind::WindowCleanState:  // symmetric
    case ReliableKind::GrimeState:        // symmetric
    case ReliableKind::TrashPileState:    // symmetric
    case ReliableKind::FireflySpawn:      // each peer spawns near its own camera and shares
    case ReliableKind::OwnerEntitySpawn:  // each peer owns its stalker; every peer must see it
    case ReliableKind::OwnerEntityPose:
    case ReliableKind::OwnerEntityDestroy:
    case ReliableKind::InventoryPickup:   // peer-symmetric
    // ChatMessage is not relayable: chat is host-authored. A client's line reaches the host as an
    // intent; the host commits it with a sequence number and broadcasts an authored line to
    // everyone. The relay fires on the net thread at receive time, before a sequence number can
    // exist, so a relayed copy would have no position in the order. EmailAppend is not relayable
    // either: emails are host-authored, and a client's append is a protocol violation the handler
    // drops.
    case ReliableKind::EmailDelete:       // player-symmetric
    case ReliableKind::SavedSignalAppend: // producer-symmetric
    case ReliableKind::SavedSignalDelete: // player-symmetric
    case ReliableKind::CompState:         // simulator-authoritative
    case ReliableKind::CompData:          // from the claim owner or the simulator
    case ReliableKind::VoiceState:        // player-symmetric
    case ReliableKind::DeskLogLine:       // producer-symmetric
    case ReliableKind::ReelSlot:          // presser-authored
    case ReliableKind::MeadowAppend:      // presser-symmetric
    case ReliableKind::MeadowDelete:      // player-symmetric
        return true;
    default:
        return false;
    }
}

// The pre-world send gate: a menu-mode joiner is connected for tens of seconds before it has a
// world (downloading and loading the host save), and a host-to-client kind that mutates or
// assumes a world must not be sent to a slot before its ClientWorldReady, since the world-ready
// replay reconstructs all of it. The allowlist of kinds that may flow before world-ready.
inline bool IsPreWorldSendableKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::Join:
    case ReliableKind::AssignPeerSlot:
    case ReliableKind::RosterRow:        // roster identity -- engine-free on the receiver
    // SkinChange is the same roster-identity family, and its receiver only caches the name (the
    // puppet reads it when it spawns); gated, a skin changed during a joiner's load window would be
    // dropped, and the joiner would render the old skin all session.
    case ReliableKind::SkinChange:
    // The nameplate preference: the same family, a plain per-slot flag store.
    case ReliableKind::NameplateChange:
    // The nick colour: the same family, a per-slot atomic store.
    case ReliableKind::NickColorChange:
    case ReliableKind::SaveTransferRequest:
    case ReliableKind::SaveTransferBegin:
    case ReliableKind::SaveTransferChunk:
    // The per-player inventory blob is pre-world by design: the joiner must hold it before the save
    // object is ready, the one window to substitute the inventory. The receiver only deserialises
    // and stores it.
    case ReliableKind::PlayerInventoryBlob:
    // EventFire's receiver is engine-free pre-world: it policy-checks and queues until the eventer
    // resolves, so a story row fired during a joiner's load window is replayed rather than
    // swallowed; the client's dedupe makes the overlap with the snapshot idempotent.
    case ReliableKind::EventFire:
    case ReliableKind::ClientWorldReady:
        return true;
    default:
        return false;
    }
}

}  // namespace coop::net
