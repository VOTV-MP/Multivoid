// coop/moderation/moderation.h -- host-side player-admin actions (the player-list action menu).
//
// The host's interactive scoreboard (ui::scoreboard) calls these when the host clicks a player
// row, and this module is the single entry point for the three actions: KICK and BAN are always
// available to the host, TELEPORT-TO-ME is the dev-gated one -- the scoreboard shows it only
// when [dev] devkeys is on, and this module does not re-check the flag, it performs the
// teleport.
//
// All three are HOST-only and self-gate on Session::Role::Host: defence in depth, because a
// destructive kick or ban must never run off a client even if a future caller misuses it. Each
// action marshals onto the game thread, so the scoreboard's render-thread click does no net or
// disk work inline and the game-thread-asserted nick lookup is legal.
//
// Principle 7: policy orchestration over a policy-free net layer, which learns about bans only
// through the injected accept filter (Session::SetAcceptFilter), never by calling this module.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::moderation {

// THE TOKEN. Every slot-addressed destructive action takes a PlayerToken, not a bare slot, and
// the token is in the SIGNATURE so a tokenless call does not compile -- the defence cannot be
// forgotten at a call site.
//
// The measured defect it closes: the ban modal captured its target slot when the modal OPENED,
// then executed after an arbitrary typing delay, while slots recycle lowest-free. A permanent
// IP ban could therefore land on the SUCCESSOR -- a different person who merely inherited the
// seat.
//
// A token is (slot, playerNo, generation) read from ONE ledger row. The generation is validated
// against the LIVE net-layer authority at execution time, so a stale capture fails CLOSED.
// Validating playerNo against playerNo inside the same mirror would fail OPEN and merely narrow
// the window to a tick.

// The captured identity of a moderation target. Build it with TokenForSlot at
// the moment the admin picks the row; carry it, unchanged, to the action.
struct PlayerToken {
    int      slot = -1;
    uint16_t playerNo = 0;   // for the log line + the confirm dialog
    uint32_t generation = 0; // what the net layer validates against
    bool valid() const { return slot >= 1 && playerNo != 0 && generation != 0; }
};

// Build a token from a published roster row. PURE and thread-free on purpose:
// the scoreboard runs on the RENDER thread and must not read the game-thread
// ledger, so the capture rides the POD snapshot the game thread already
// publishes (coop::roster::Row).
inline PlayerToken TokenFor(int slot, uint16_t playerNo, uint32_t generation) {
    PlayerToken t;
    t.slot = slot;
    t.playerNo = playerNo;
    t.generation = generation;
    return t;
}

// Cache the Session pointer (used by KickPlayer / BanPlayer). Called once at host
// boot, alongside the other modules' SetSession.
void SetSession(coop::net::Session* session);

// Disconnect the captured player. Host-only. Safe to call from the render thread
// (posts to the game thread). No-op if the target has since left or been
// replaced -- see the token note above.
void KickPlayer(const PlayerToken& token);

// Permanently ban the captured player by IP, then kick them -- MTA's order in
// CStaticFunctionDefinitions::BanPlayer, whose KickPlayer/BanPlayer pair this module mirrors.
// Host-only. The ban survives host restarts (coop::ban_list persists to disk) and rejects that
// IP on future connects through the accept filter; `reason` is stored on the record for the
// admin's reference (null/empty is fine). Safe to call from the render thread. The address is
// read BEFORE the kick because Kick() zeroes the slot and it is unresolvable afterwards.
//
// ABORTS -- writing no ban and kicking nobody -- if the captured player is gone. This is the
// whole point of the token: a permanent ban is the least reversible thing the host can do, so
// it must never land on whoever happens to hold the seat now.
void BanPlayer(const PlayerToken& token, const char* reason);

// Permanently ban an OFFLINE player by its seen-players GUID (the F1
// Administration panel's Offline-section ban). Resolves the player's last known
// IP + nick from coop::seen_players; warns and does nothing if the record has
// no IP (nothing to enforce against). Host-only. Safe to call from the render
// thread.
void BanOffline(const char* guid, const char* reason);

// Remove an IP ban (the F1 Administration panel's Unban button). Thin
// pass-through to coop::ban_list::Remove -- runs inline (ban_list is
// thread-safe); the panel is host-gated upstream. Any thread.
void Unban(const char* ip);

// Teleport the captured player to the host's current pose. Host-only. Token-taking
// like the others: teleporting the wrong person is not destructive, but a
// consistent rule is what keeps the check from being forgotten where it matters.
void TeleportPlayerToMe(const PlayerToken& token);

}  // namespace coop::moderation
