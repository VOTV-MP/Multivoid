// coop/session/teleport_client.h -- teleport connected clients to the host's pose.
//
// A shipped moderation verb, not a dev toy: /tphere (the scoreboard's Teleport runs it) brings one
// client to the host through TeleportSlotToHostWithToken (moderation.cpp), event_feed applies the wire packet on the receiving
// side, and the F1 dev-menu button broadcasts to everyone. A JOIN does not use the wire verb -- a
// joiner places itself (net_pump, through ApplyLocally below), and a host teleport at world-ready
// overwrote that placement, since world-ready arrives after the client has already spawned.
//
// Direction: HOST -> CLIENT only. The action self-gates on Session::Role::Host and no-ops on a
// client. It mirrors MTA's `!tphere` chat command: /tphere is the command, and the scoreboard's
// Teleport button submits that line.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::teleport_client {

// Cache the Session pointer so the action can snapshot host pose + broadcast.
// Called once from harness boot.
void SetSession(coop::net::Session* session);

// Menu action (Player > Movement): HOST snapshots its own mainPlayer Location +
// Rotation and sends it to the clients (they K2_TeleportTo). HOST-only -- on a
// client this logs + no-ops. Safe to call off the game thread (the snapshot is
// posted to it).
void TeleportClientsToHost();

// The /tphere command: teleport ONE specific client (the peer at `peerSlot`,
// 1..kMaxPeers-1) to the host's pose, only while `generation` is still that slot's
// occupancy generation, so a successor admitted into the seat is never moved. GAME
// THREAD. The pose is read now and sent by a queued task that checks the generation
// again. Returns false, sending nothing, when this is not the host, the slot is out
// of range, the generation is not the slot's, or the host's pose cannot be read; the
// caller tells a changed seat from the rest by reading the slot's generation again.
bool TeleportSlotToHostWithToken(int peerSlot, uint32_t generation);

// Sends the TeleportClient order that moves the peer at `slot` to `at`, facing `rot`, reliably and to that peer only.
// The one place a pose order is built, for the callers that name a place other than the host's own (the /cremate
// verb and its drill and probe). False when the send was refused (not connected, channel closed). The caller owns the
// seat's occupancy check. Game thread.
bool OrderToPose(coop::net::Session& session, int slot, const ue_wrap::FVector& at, const ue_wrap::FRotator& rot);

// Receiver: apply the teleport on the local mainPlayer (K2_TeleportTo).
// Called from event_feed.cpp on incoming ReliableKind::TeleportClient AND
// gated to client-role receivers (host echo is a no-op). Game thread only.
struct ApplyArgs {
    float locX, locY, locZ;
    float rotPitch, rotYaw, rotRoll;
};
// Returns whether a teleport call REPORTED success (the primary teleportWObackrooms, or
// one of the two fallbacks). It is a report about the CALL, not about where the player
// ended up: the three-tier fallback means "something was dispatched", and VOTV constraints
// can still revert a K2_TeleportTo. A caller that needs to know the player actually MOVED
// must read the position back (coop::death_revive's revive conjunction does exactly that).
bool ApplyLocally(const ApplyArgs& args);

// The host's TeleportClient order, as event_feed receives it: ApplyLocally behind the /cremate drill's two default-off
// client knobs (an order that never applies; a player who dies as it does). Game thread.
bool ApplyHostOrder(const ApplyArgs& args);

}  // namespace coop::teleport_client
