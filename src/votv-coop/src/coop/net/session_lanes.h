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
    case ReliableKind::EntitySpawn:    return Lane::Bulk;
    case ReliableKind::EntityDestroy:  return Lane::Bulk;
    // v34: the loading-screen brackets MUST share PropSpawn's lane so GNS's in-lane
    // ordering delivers them strictly Begin -> [every PropSpawn] -> Complete. If
    // SnapshotComplete rode a different lane it could overtake the prop stream under
    // backpressure and lift the joiner's cover mid-build.
    case ReliableKind::SnapshotBegin:    return Lane::Bulk;
    case ReliableKind::SnapshotComplete: return Lane::Bulk;
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
//   - DoorState / LightState / ContainerState: keyed interactables are SYMMETRIC
//     (any peer can toggle one locally), so a client-originated edge must reach
//     the OTHER clients via the host.
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
    case ReliableKind::PropRelease:
    case ReliableKind::DoorState:
    case ReliableKind::LightState:
    case ReliableKind::ContainerState:
    case ReliableKind::KeypadState:
        return true;
    default:
        return false;
    }
}

}  // namespace coop::net
