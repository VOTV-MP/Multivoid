// coop/session/teleport_client.h -- teleport connected clients to the host's pose.
//
// A shipped join and moderation verb, not a dev toy: every join spawns the joiner at the host pose
// through TeleportSlotToHost on the connect edge (subsystems.cpp), the F1 admin scoreboard
// teleports one client through it (moderation.cpp), event_feed applies its wire packet, and the F1
// dev-menu button is one more caller.
//
// Direction: HOST -> CLIENT only. The action self-gates on Session::Role::Host and no-ops on a
// client. It mirrors MTA's `!tphere` chat command as a menu button.

#pragma once

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

// Host action menu (player-list scoreboard): teleport ONE specific client (the
// peer at `peerSlot`, 1..kMaxPeers-1) to the host's pose. Same snapshot as
// TeleportClientsToHost but a single-target SendReliableToSlot instead of a
// broadcast, so only the clicked client moves. HOST-only; no-ops on a client or
// for an out-of-range slot. Safe to call off the game thread.
void TeleportSlotToHost(int peerSlot);

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

}  // namespace coop::teleport_client
