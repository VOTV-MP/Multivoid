// coop/dev/teleport_client.h -- F4 dev-key: teleport the client to the host's pose.
//
// Gated by votv-coop.ini ([dev] devkeys=1); OFF by default. While enabled:
//   F4 -- HOST snapshots own mainPlayer Location + Rotation, sends to the
//         client; client applies via K2_TeleportTo on its local pawn.
//
// Direction: HOST -> CLIENT only. F4 pressed on the client is a no-op (the
// hotkey thread sender-gates on Session::Role::Host). Mirrors VT's `!tphere`
// chat command but as a direct keybind. [[project-coop-foundation]] cross-ref.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::teleport_client {

// Cache the Session pointer so F4 can snapshot host pose + broadcast.
// Called once from harness boot, BEFORE Init().
void SetSession(coop::net::Session* session);

// Read votv-coop.ini; if [dev] devkeys=1 (and master not killed), start the
// F4 hotkey thread. No-op otherwise.
void Init();

// Receiver: apply the teleport on the local mainPlayer (K2_TeleportTo).
// Called from event_feed.cpp on incoming ReliableKind::TeleportClient AND
// gated to client-role receivers (host echo is a no-op). Game thread only.
struct ApplyArgs {
    float locX, locY, locZ;
    float rotPitch, rotYaw, rotRoll;
};
void ApplyLocally(const ApplyArgs& args);

}  // namespace coop::dev::teleport_client
