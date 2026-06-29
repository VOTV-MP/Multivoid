// coop/moderation.h -- host-side player-admin actions (player-list action menu).
//
// The host's interactive scoreboard (ui::scoreboard) calls these when the
// host clicks a player row. This module is the single entry point for the three
// actions: KICK + BAN are always available to the host; TELEPORT-TO-ME is the
// dev-gated one (the scoreboard only shows it when [dev] devkeys is on -- this
// module doesn't re-check the dev flag, it just performs the teleport).
//
// All three are HOST-only and self-gate on Session::Role::Host (defense in
// depth -- the scoreboard already only renders actions for the host, but a
// destructive kick/ban must never run off a client even if a future caller
// misuses it). Each action marshals onto the game thread (GT::Post) so the
// scoreboard's render-thread click does no net/disk work inline, and so the
// nick lookup (player_handshake::NicknameForSlot, game-thread-asserted) is legal.
//
// principle-7: this is gameplay/policy orchestration (coop/), wiring the net
// mechanism (Session::Kick / GetPeerAddress), the persistence policy
// (coop::ban_list), and the existing teleport feature together. The net layer
// stays policy-free -- it learns about bans only through the injected accept
// filter (Session::SetAcceptFilter), never by calling this module.
//
// MTA precedent: CStaticFunctionDefinitions::KickPlayer / BanPlayer ->
// CGame::QuitPlayer (reference/mtasa-blue/Server/.../CStaticFunctionDefinitions
// .cpp:11876 / :11924). MTA's BanPlayer adds the ban THEN kicks the matching
// live player -- BanSlot below does the same (capture IP -> add ban -> kick).

#pragma once

namespace coop::net { class Session; }

namespace coop::moderation {

// Cache the Session pointer (used by KickSlot / BanSlot). Called once at host
// boot, alongside the other modules' SetSession.
void SetSession(coop::net::Session* session);

// Disconnect the client at peerSlot (1..kMaxPeers-1). Host-only. Safe to call
// from the render thread (posts to the game thread).
void KickSlot(int peerSlot);

// Permanently ban the client at peerSlot by IP, then kick it. Host-only. The
// ban survives host restarts (coop::ban_list persists to disk) and rejects that
// IP on future connects (via the accept filter). Safe to call from the render
// thread.
void BanSlot(int peerSlot);

// Teleport the client at peerSlot to the host's current pose (the dev-gated
// action -- the scoreboard only offers it under [dev] devkeys). Host-only.
// Thin pass-through to coop::dev::teleport_client::TeleportSlotToHost.
void TeleportSlotToMe(int peerSlot);

}  // namespace coop::moderation
