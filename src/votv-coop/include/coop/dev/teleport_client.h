// coop/dev/teleport_client.h -- teleport connected clients to the host's pose.
//
// Driven by the ImGui dev menu (Player > Movement > "Teleport clients to me").
// The legacy F4 hotkey was RETIRED 2026-06-02 (RULE [[feedback-dev-features-in-
// imgui-menu]]: dev features live in the F1 menu, not ad-hoc hotkeys).
//
// Direction: HOST -> CLIENT only. The action is a no-op on a client (it self-
// gates on Session::Role::Host). Mirrors MTA's `!tphere` chat command but as a
// menu button. [[project-coop-foundation]] cross-ref.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::teleport_client {

// Cache the Session pointer so the action can snapshot host pose + broadcast.
// Called once from harness boot.
void SetSession(coop::net::Session* session);

// Menu action (Player > Movement): HOST snapshots its own mainPlayer Location +
// Rotation and sends it to the clients (they K2_TeleportTo). HOST-only -- on a
// client this logs + no-ops. Safe to call off the game thread (the snapshot is
// posted to it).
void TeleportClientsToHost();

// Phase 2 host action menu (TAB scoreboard): teleport ONE specific client (the
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
void ApplyLocally(const ApplyArgs& args);

}  // namespace coop::dev::teleport_client
