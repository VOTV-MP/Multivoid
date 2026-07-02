// coop/net/session_lanes.h -- INTERNAL (src-tree) shared header for the GNS
// priority-lane mapping + the host-relay kind whitelist.
//
// Not a public API: it lives under src/ (not include/) and is included only by
// the Session implementation TUs (session.cpp + session_relay.cpp). It exists
// solely so the lane mapping is shared between the send paths (session.cpp's
// SendReliable / SendReliableToSlot) and the host-relay path
// (session_relay.cpp's RelayReliableToOtherClients) without duplicating the
// switch. Extracted at PR-FOUNDATION Tier 2 T2-3 when the relay subsystem
// moved into its own TU and session.cpp crossed the 800-LOC soft cap.
//
// The functions are `inline` (external linkage, ODR-safe across the two TUs);
// they were previously file-local (anon-namespace) inside session.cpp.

#pragma once

#include "coop/net/protocol.h"  // ReliableKind

namespace coop::net {

// PR-3 priority lanes. See research/findings/votv-gns-integration-plan-
// 2026-05-27.md section 5.4. session_status.cpp's ConfigureLanesForPeer
// hard-codes kLaneCount=3; session.cpp pins the two together with a
// static_assert on Lane::Count so a 4th lane can't silently desync them.
enum class Lane : int {
    High = 0,
    Normal = 1,
    Bulk = 2,
    Count = 3,
};

inline Lane LaneForKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::TeleportClient: return Lane::High;
    case ReliableKind::RestoreVitals:  return Lane::High;
    case ReliableKind::ItemActivate:   return Lane::High;
    case ReliableKind::PlayerDamage:   return Lane::High;  // combat event -- prioritize
    // Spawn + Destroy must share a lane: GNS guarantees in-order delivery WITHIN
    // a lane but NOT across lanes. If PropDestroy were on Normal while PropSpawn
    // were on Bulk, a Destroy could drain to the receiver before its Spawn under
    // backpressure -> phantom actor never cleaned up.
    case ReliableKind::PropSpawn:      return Lane::Bulk;
    case ReliableKind::PropDestroy:    return Lane::Bulk;
    // v52: PropConvert destroys the ball (oldEid) + spawns the pile (newEid). It MUST share the
    // Spawn/Destroy lane so GNS delivers it strictly after the ball's PropSpawn -- on a different
    // lane a convert could overtake the ball spawn under backpressure -> the ball mirror spawns
    // after the convert already ran -> a lingering, never-destroyed ball.
    case ReliableKind::PropConvert:    return Lane::Bulk;
    // b3 (v90): PropSnapPos rides Bulk so it delivers AFTER the connect-snapshot PropSpawns for the same join
    // (it corrects a save-authoritative native's position; ordering after the snapshot keeps it consistent).
    case ReliableKind::PropSnapPos:    return Lane::Bulk;
    case ReliableKind::EntitySpawn:    return Lane::Bulk;
    case ReliableKind::EntityDestroy:  return Lane::Bulk;
    // v80 (B3b): WorldActorSpawn + WorldActorDestroy share the Spawn/Destroy lane for the same reason --
    // GNS guarantees in-order delivery WITHIN a lane, so a destroy can't overtake its spawn (phantom
    // mirror) under backpressure. (Host-authoritative: NOT relayable, NOT pre-world-sendable.)
    case ReliableKind::WorldActorSpawn:   return Lane::Bulk;
    case ReliableKind::WorldActorDestroy: return Lane::Bulk;
    // v34: the loading-screen brackets MUST share PropSpawn's lane so GNS's in-lane
    // ordering delivers them strictly Begin -> [every PropSpawn] -> Complete. If
    // SnapshotComplete rode a different lane it could overtake the prop stream under
    // backpressure and lift the joiner's cover mid-build.
    case ReliableKind::SnapshotBegin:    return Lane::Bulk;
    case ReliableKind::SnapshotComplete: return Lane::Bulk;
    // v56: the save blob is phase-ordered BEFORE the snapshot bracket (the client
    // only sends ClientWorldReady after loading the save), so sharing Bulk costs
    // nothing -- and Begin must precede its chunks in-lane.
    case ReliableKind::SaveTransferBegin: return Lane::Bulk;
    case ReliableKind::SaveTransferChunk: return Lane::Bulk;
    // v73 Inc4: the HOST->CLIENT apply blob must reach the joiner BEFORE its world loads
    // (OnSaveObjectReady substitutes the inventory pre-materialize). Normal (priority 1, ahead of the
    // Bulk save-transfer + ~3000-prop snapshot at 2) + pre-world-sendable (below) deliver it promptly
    // during the joiner's pre-world wait rather than queued behind the bulk streams. It is a
    // self-contained blob with NO in-lane ordering dependency, so it rides Normal freely. (The
    // CLIENT->HOST persist stream shares this kind+lane -- small + post-world, unaffected; Normal
    // keeps it off the urgent High lane.)
    case ReliableKind::PlayerInventoryBlob: return Lane::Normal;
    // v68: PropStickState + PropRelease are ORDER-PAIRED: the receiver's release
    // gate (StickHoldsPhysicsOff) reads the frozen state the stick writes, and the
    // sender emits stick-then-release ~one pump pass apart -- a release overtaking
    // its stick re-enables physics on the just-stuck mirror (the falling-camera
    // bug). They land on Normal via default: anyway; these explicit cases pin the
    // pairing against a future single-kind lane move (the Spawn/Destroy rule above).
    case ReliableKind::PropStickState: return Lane::Normal;
    case ReliableKind::PropRelease:    return Lane::Normal;
    // v76/v77: AtvState + AtvRelease + AtvSpawn + AtvDestroy are ORDER-PAIRED -- the grabber streams
    // pose then emits AtvRelease one pass later, and a purchased ATV must AtvSpawn (client fresh-spawn)
    // before its first AtvState pose / after its last before AtvDestroy. A release/destroy overtaking
    // the last pose would re-enable physics / tear down on a mirror mid-update. All land on Normal via
    // default: anyway; pin them so a future single-kind lane move can't split the group.
    case ReliableKind::AtvState:       return Lane::Normal;
    case ReliableKind::AtvRelease:     return Lane::Normal;
    case ReliableKind::AtvSpawn:       return Lane::Normal;
    case ReliableKind::AtvDestroy:     return Lane::Normal;
    default:                           return Lane::Normal;
    }
}

// Host-relay topology (PR-FOUNDATION Tier 2 T2-3): which reliable kinds the
// host forwards from one client to the others. PEER-ORIGINATED gameplay only:
//   - ItemActivate: a client's equipment toggle (flashlight) must show on
//     its puppet for every peer.
//   - PropSpawn / PropDestroy / PropRelease: a client's inventory drop /
//     destroy / throw must replicate to every peer (the Aprop_C lineage is
//     host-authoritative for spawns the HOST detects, but a client's own
//     takeObj-path drop originates client-side and needs cross-peer fan-out).
//   - DoorState / LightState / ContainerState / WindowCleanState / GrimeState:
//     keyed interactables + dirt are SYMMETRIC (any peer can toggle / wipe one
//     locally), so a client-originated edge must reach the OTHER clients via the host.
// NOT relayed:
//   - Weather / RedSky / LightningStrike / EntitySpawn / EntityDestroy /
//     RestoreVitals / TeleportClient / PlayerDamage: host-authoritative -- they
//     ORIGINATE on the host and are sent directly to the target client (fan-out
//     or SendReliableToSlot); a client never legitimately sends them (and
//     event_feed trust-gates them on senderPeerSlot==0). PlayerDamage is
//     point-to-point host->owner (Inc3-WIRE combat relay), never client-forwarded.
//   - Join / AssignPeerSlot / PlayerJoined: handshake. Join is point-to-point
//     host<->client; AssignPeerSlot + PlayerJoined are host-originated (the
//     latter IS the cross-peer identity relay from T2-1).
// PropPose rides the unreliable relay (T2-2), not this path.
inline bool IsClientRelayableReliableKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::ItemActivate:
    case ReliableKind::PropSpawn:
    case ReliableKind::PropDestroy:
    case ReliableKind::PropConvert:       // v52: a client's clump ball->pile convert must reach the other clients
    case ReliableKind::PropRelease:
    case ReliableKind::PropStickState:    // v68: a client's wall-attachable stick (camera on a wall) must reach the other clients
    case ReliableKind::DoorState:
    case ReliableKind::LightState:
    case ReliableKind::ContainerState:
    case ReliableKind::GarageDoorState:   // v44: garage door is SYMMETRIC -- relay a client's open/close to the others
    case ReliableKind::ApplianceState:    // v45: appliance on/off toggles are SYMMETRIC -- relay a client's edge to the others
    case ReliableKind::LockerDoorState:   // v62: locker/console doors are SYMMETRIC -- relay a client's toggle to the others
    case ReliableKind::PowerControlState: // v46: base power panel breakers are SYMMETRIC -- relay a client's edge to the others
    case ReliableKind::AtvState:          // v47: ATV body pose is OCCUPANT-OR-GRABBER-authoritative -- relay a client driver's/grabber's pose to the other clients
    case ReliableKind::AtvRelease:        // v76: ATV grab-release/throw edge -- relay a client grabber's release to the other clients (companion to AtvState)
    case ReliableKind::DeskState:         // v64: desk scalars are CLAIM-OWNER-authoritative (+ any-peer button edges) -- relay a client's edge to the others
    case ReliableKind::DishAimState:      // v64: dish aim is CLAIM-OWNER-authoritative -- relay a client occupant's stream to the others
    case ReliableKind::KeypadState:
    case ReliableKind::WindowCleanState:  // v41: base-window clean is SYMMETRIC -- relay a client's wipe to the others
    case ReliableKind::GrimeState:        // v42: surface grime is SYMMETRIC -- relay a client's wipe
    case ReliableKind::TrashPileState:    // v57: trash-pile collect counters are SYMMETRIC -- relay a client's collect to the others
    case ReliableKind::FireflySpawn:      // v51: fireflies are PEER-SYMMETRIC -- each peer spawns near its OWN camera + shares; relay a client's spawn to the others so all peers see everyone's
    case ReliableKind::InventoryPickup:   // v58: the inventory-collect blip is PEER-SYMMETRIC -- relay a client's collect so every peer hears it
    case ReliableKind::ChatMessage:       // v60: T-chat is PEER-SYMMETRIC -- relay a client's line so every peer reads it
    case ReliableKind::EmailAppend:       // v64: emails are PRODUCER-SYMMETRIC (any peer's local pipeline can append) -- relay a client's rows to the others
    case ReliableKind::EmailDelete:       // v65: email deletes are PLAYER-SYMMETRIC (any peer's del button) -- relay a client's delete to the others
    case ReliableKind::SavedSignalAppend: // v65: saved-signal saves are PRODUCER-SYMMETRIC (download-save/import/copy at the claimed desk) -- relay
    case ReliableKind::SavedSignalDelete: // v65: saved-signal deletes/export-moves are PLAYER-SYMMETRIC -- relay
    case ReliableKind::CompState:         // v65: the decode stream is SIMULATOR-authoritative (the peer whose latch is set) -- relay a client simulator to the others
    case ReliableKind::CompData:          // v65: comp_data_0 edges come from the claim-owner OR the simulator -- relay
    case ReliableKind::VoiceState:        // v66: voice mute/disabled display state is PLAYER-SYMMETRIC -- relay a client's edge to the others
    case ReliableKind::DeskLogLine:       // v70: coords-terminal event lines are PRODUCER-SYMMETRIC (the line originates where the action ran) -- relay a client's line to the others
        return true;
    default:
        return false;
    }
}

// v56 pre-world send gate (design-workflow B2, the MTA invariant): a menu-mode
// joining client is CONNECTED for ~30-60 s before it has a gameplay world
// (downloading + loading the host save). Host->client reliable kinds that
// MUTATE/ASSUME a world must not be sent to a slot until its ClientWorldReady --
// the world-ready connect replay reconstructs all of it by design. This is the
// allowlist of kinds that may flow BEFORE world-ready: handshake/identity + the
// save transfer itself.
inline bool IsPreWorldSendableKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::Join:
    case ReliableKind::AssignPeerSlot:
    case ReliableKind::PlayerJoined:        // roster identity -- engine-free on the receiver
    // v93 skins: SkinChange is the same roster-identity family and its receiver is
    // engine-free with no puppet spawned (StoreSkinForSlot just caches the name; the
    // puppet spawns later reading SkinForSlot). WITHOUT this, a skin changed while a
    // joiner is still in its 30-60 s load window is SILENTLY dropped by this gate and
    // the joiner renders the OLD skin for the whole session (audit 2026-07-02 HIGH --
    // the v90-b3 "mutation during the window" class, killed at the gate itself).
    case ReliableKind::SkinChange:
    case ReliableKind::SaveTransferRequest:
    case ReliableKind::SaveTransferBegin:
    case ReliableKind::SaveTransferChunk:
    // v73 Inc4: the per-player inventory APPLY blob is BY DESIGN pre-world -- the joiner must have it
    // buffered before OnSaveObjectReady fires (the one window to substitute the inventory before the
    // native loadObjects materializes the world). The receiver just deserializes + stores it
    // (g_pendingApply); it assumes NO world. Without this it was blocked until ClientWorldReady ==
    // too late, so the apply never ran ("no apply blob arrived" -- the user's "inventory never worked").
    case ReliableKind::PlayerInventoryBlob:
    case ReliableKind::ClientWorldReady:
        return true;
    default:
        return false;
    }
}

}  // namespace coop::net
