// coop/net/session_lanes.h -- the GNS priority-lane mapping, the host-relay whitelist and the
// pre-world send gate, shared by the Session's send paths and its relay path. Internal to the
// Session TUs (src tree, not include/); the functions are inline, so the two TUs share one
// definition.

#pragma once

#include "coop/config/config.h"           // ResolveEnum, the drill knob
#include "coop/config/config_registry.h"  // rows::prop_syncer_load_lane
#include "coop/net/protocol.h"            // ReliableKind

namespace coop::net {

// The reliable lanes, MTA's two channels plus a lane for what names nothing. GNS drains by priority
// first and strictly (weights act only within one priority, isteamnetworkingsockets.h) and orders
// delivery within a lane: the three are three FIFO streams, drained High, Default, Transfer.
//   High      a kind that names nothing: the link probe and its echo, the join beacon, the vitals restore.
//   Default   a kind that names an element or the world. MTA carries every element message on one ordered
//             channel, PACKET_ORDERING_DEFAULT (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/
//             CPacket.h:43), so a message that names a prop meets that prop's spawn by FIFO order.
//   Transfer  a payload that can exceed the blob ceiling, in MTA's latent shape: a channel of its own at low
//             priority and a capped rate (reference/mtasa-blue/Shared/mods/deathmatch/logic/
//             CLatentSendQueue.cpp:46-81,129-130), the whole message wrapped into one buffer
//             (CLatentTransferManager.cpp:123-150).
// connection_tuning's TuneConnection hard-codes the count to 3, and session.cpp pins the two together with a
// static_assert on Lane::Count.
enum class Lane : int {
    High = 0,
    Default = 1,
    Transfer = 2,
    Count = 3,
};

// The FIFO exceptions: the save stream is taken by a bulk sink on receipt, before the inbox (nothing waits on
// its order), and the admission kinds are sent raw on lane 0 (SendRawReliableToConn), outside the send backlog.
//
// A divergence from MTA, which sends explosions, damage and element RPCs at HIGH priority on its DEFAULT
// channel (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CExplosionSyncPacket.h:41,
// CElementRPCPacket.h:23): a GNS lane fuses order and priority, so ours are ordered on Default at the
// middle priority. Element kinds stay off lane 0 because the link probe and the join beacon must never
// queue behind the ordered element backlog (the LinkProbe and JoinPhaseNote arms). They wait behind what
// that peer's Default lane holds, and on a joiner until its join gate closes.

// The three admission kinds appear in none of the three routers below, and that is not an
// omission: they never travel through a slot send (no slot exists yet; the raw connection send
// pins them to lane 0), they are never relayed (a proof is between two endpoints), and they
// have no pre-world gate because they run before its premise. This predicate lets the receive
// path drop a replay of one from an already-admitted peer.
inline bool IsAdmissionKind(ReliableKind k) {
    return k == ReliableKind::AuthHello || k == ReliableKind::AuthChallenge ||
           k == ReliableKind::AuthProof;
}

// The reliable kinds exempt from the send buffer's headroom reserve. That reserve exists to keep
// the unreliable pose and voice datagrams flowing while a bulk stream holds the buffer, and the
// link probe is the measurement of the round trip those datagrams take: holding it back would
// blind the one instrument that can see the reserve working. Bounded by construction -- 32 bytes,
// a bounded number outstanding -- so what it takes from the reserve is not worth counting.
// The join beacon is exempt for the same reason in the other direction: it is the joiner's only
// evidence that the host is still working, and the state it reports on -- a world blob streaming
// through the send buffer -- is exactly the state that holds the buffer at the brim. Held back, it
// would go quiet precisely when its silence means a failed join. Twelve bytes once a second.
inline bool IsReserveExemptKind(ReliableKind k) {
    return k == ReliableKind::LinkProbe || k == ReliableKind::LinkProbeReply ||
           k == ReliableKind::JoinPhaseNote;
}

// [dev] Drill knob prop_syncer_load_lane=default puts the load arm's filler on the ordered lane, the red of the
// lane rule. Resolved once: LaneForKind runs per send.
inline bool LoadFillerOnDefault() {
    static const bool onDefault =
        coop::config::ResolveEnum(coop::config_registry::rows::prop_syncer_load_lane) == "default";
    return onDefault;
}

inline Lane LaneForKind(ReliableKind k) {
    if (k == ReliableKind::DevLoadFiller && LoadFillerOnDefault()) return Lane::Default;
    switch (k) {
    // The link probe and its echo ride High because that is the lane whose delay the measurement
    // is about: a reading taken on Transfer would time the bulk queue instead of the path the pose
    // stream takes. Pinned as a pair -- an echo on another lane measures a different queue than
    // the probe it answers.
    case ReliableKind::LinkProbe:      return Lane::High;
    case ReliableKind::LinkProbeReply: return Lane::High;
    // The join beacon rides High for the reason it exists: queued behind the very world blob whose
    // progress it reports, it would arrive as news about a minute ago. It carries no state, only
    // what the host is doing now.
    case ReliableKind::JoinPhaseNote:  return Lane::High;
    case ReliableKind::RestoreVitals:  return Lane::High;
    // The save stream and the drill filler: the transfers that can exceed the blob ceiling. The filler's lane is
    // the drill knob's (LoadFillerOnDefault above), so a red arm can stand it on Default.
    // The lane is built, MTA's latent shape is not: the save stream is paced by save_transfer's pump, and only
    // the dev filler rides it beside. The first Transfer kind to take that shape owes five properties the lane
    // does not enforce: dispatch on completion, dropped if its element is gone or not yet added
    // (CLatentReceiver.cpp:160-186); an owner token cancelled when the peer leaves or the owner is destroyed
    // (CLatentTransferManager.cpp:85-115,188-210); one transfer in flight per peer (CLatentReceiver.cpp:121-123);
    // a size bound (:121-126); and its own rate cap (CLatentSendQueue.cpp:62-70).
    case ReliableKind::SaveTransferBegin: return Lane::Transfer;
    case ReliableKind::SaveTransferChunk: return Lane::Transfer;
    case ReliableKind::DevLoadFiller:     return Lane::Transfer;
    // These five rode High and now name an element, so they ride Default behind its spawn: a player
    // (ItemActivate, TeleportClient, CremationControl), a creature's Npc eid (PlayerHit) and an ignoreId (ExplosionSpawn).
    case ReliableKind::TeleportClient:    return Lane::Default;
    case ReliableKind::CremationControl:  return Lane::Default;
    case ReliableKind::ItemActivate:      return Lane::Default;
    case ReliableKind::PlayerHit:         return Lane::Default;
    case ReliableKind::ExplosionSpawn:    return Lane::Default;
    default:                              return Lane::Default;
    }
}

// The host relay: the reliable kinds the host forwards from one client to the others, peer-originated
// gameplay only. A client's equipment toggle must show on its puppet everywhere; a client's drop, destroy
// or throw must replicate; a symmetric interactable's edge must reach the other clients. Not relayed: the
// host-authoritative kinds (weather, entity spawns and destroys, the dev keys, damage, the refiner's decode)
// originate on the host and go direct, and the receive edge refuses a client's copy (client_authorship.h);
// the handshake kinds are point-to-point or host-originated. PropPose rides the unreliable relay.
constexpr bool IsClientRelayableReliableKind(ReliableKind k) {
    switch (k) {
    case ReliableKind::ItemActivate:
    case ReliableKind::PropSpawn:
    case ReliableKind::PropDestroy:
    case ReliableKind::PropRelease:
    case ReliableKind::PropStickState:    // a client's wall-attachable stick
    // DoorState is not relayable: the host authors it, and a client's own door verbs reach the host
    // as DoorVerbIntent, an intent addressed to the host alone. PowerControlState likewise: a press
    // reaches the host alone and the host authors the canonical.
    case ReliableKind::LightState:
    case ReliableKind::ContainerState:
    case ReliableKind::GarageDoorState:   // symmetric
    case ReliableKind::ApplianceState:    // symmetric
    case ReliableKind::OvenRepairState:   // symmetric, one way
    case ReliableKind::LockerDoorState:   // symmetric
    // DeskState is not relayable: adopt-only, host to joiner; live desk input rides DeskInput.
    case ReliableKind::DeskInput:         // presser-authored deltas
    case ReliableKind::DeskScanEvent:     // presser-authored; every mirror replays the visual
    case ReliableKind::DeskSndFx:         // presser-authored
    case ReliableKind::PlayDeckEvent:     // presser-authored; any peer may stop
    case ReliableKind::DishAimState:      // claim-owner-authoritative
    // KeypadState is not relayable: the host authors it, and a client's own keypad entries reach the
    // host as KeypadIntent. Nor are DrivePayload (the host authors a drive's row; a client's own rows go
    // to it alone) and DriveSlotState (the host relays only the slot lines it accepted).
    case ReliableKind::WindowCleanState:  // symmetric
    case ReliableKind::WindowStroke:      // presser-authored
    case ReliableKind::GrimeState:        // symmetric
    case ReliableKind::TrashPileState:    // symmetric
    case ReliableKind::FireflySpawn:      // each peer spawns near its own camera and shares
    case ReliableKind::OwnerEntitySpawn:  // each peer owns its stalker; every peer must see it
    case ReliableKind::OwnerEntityPose:
    case ReliableKind::OwnerEntityDestroy:
    case ReliableKind::HookState:         // a hook belongs to the player who fired it
    case ReliableKind::HookDestroy:
    // PropDriveEnd is not relayable: a client's End goes to the host, which judges it and re-publishes it. Nor is
    // PropSyncClaim (the host answers the sender alone); PropSyncAnswer is host-authored.
    // HookAnchorCommit is deliberately NOT relayable: it is an intent addressed to the host alone,
    // and the host's own HookAnchored is what the other peers get. Relaying it would hand every
    // peer a record the arbiter has not validated yet, and two of them would build the hook.
    case ReliableKind::InventoryPickup:   // peer-symmetric
    // ChatMessage is not relayable: chat is host-authored. A client's line reaches the host as an intent; the host
    // commits it with a sequence number and broadcasts an authored line to everyone. The relay fires on the net
    // thread at receive time, before a sequence number can exist, so a relayed copy would have no position in the
    // order. EmailAppend is not relayable either: emails are host-authored, and a client's append is refused at the
    // host's receive edge (client_authorship.h). CommandRequest likewise: the host answers the sender alone.
    case ReliableKind::EmailDelete:       // player-symmetric
    case ReliableKind::SavedSignalAppend: // producer-symmetric
    case ReliableKind::SavedSignalDelete: // player-symmetric
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
    // The link probe and its echo: the joiner's download is the one window whose link we most need
    // measured, and both are answered on the net thread and touch no world.
    case ReliableKind::LinkProbe:
    case ReliableKind::LinkProbeReply:
    // The join beacon is pre-world by definition: every phase it reports happens before the joiner
    // has a world, and the receiver only stamps a timestamp and two numbers.
    case ReliableKind::JoinPhaseNote:
    // A server-scope setting is pre-world by design: the joiner's snapshot goes when its slot is
    // ready, and the receiver only fills the config layer's session values and, for an announced
    // row, prints one chat line, touching no world.
    case ReliableKind::ServerSetting:
    // The machine's own local dev grants: the receiver fills one atomic word, touching no world.
    case ReliableKind::PermissionGrants:
    // A command's answer is a private text line; the receiver touches no world.
    case ReliableKind::CommandReply:
    // PropDriveEnd stays gated: it names a prop by eid, which a joiner has only after its world is up,
    // and the world-ready replay re-sends every driven prop's pose in any case. The syncer pair likewise.
        return true;
    default:
        return false;
    }
}

}  // namespace coop::net