// coop/player/player_hit.h -- a hit on a player, delivered to the player's own machine.
//
// The host's copy of a hit lands on a peer's puppet, where coop/player/player_damage cancels it: the puppet is not
// the peer's body. The peer's body is its own player on its own machine, so the host SENDS the hit there (Deliver)
// and that machine runs the same verb on its own player (Apply), through the game's own branches: the ragdoll gate,
// the punch above 25, blood, the faint, death. Source runs the NPC on the server and sends the client the result
// (reference/source-sdk-2013/src/game/server/hl2/npc_BaseZombie.cpp:1267, the claw's hit applied where the NPC
// runs); MTA keeps a player's health on its own client (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CNetAPI.cpp:1189). This takes Source's delivery and MTA's ownership: the hit is judged where the creature runs
// and applied where the victim lives.
// This module has no notion of a creature: a sender is any seam that judges a hit. Game thread, but where a function
// says otherwise.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct PlayerHitPayload;
}  // namespace coop::net

namespace coop::player_hit {

// Store the session pointer for Deliver; idempotent, nullptr disables sends. Game thread.
void Install(coop::net::Session* session);

// Host: send `payload` to peer `slot` (a client's slot, 1 and up), `targetElementId` stamped from that slot's Player
// Element; the caller leaves it unset. No-op, logged, when the session is unset, the slot is invalid or has no Player
// Element yet. One reliable send per hit, never coalesced: a sum could cross the punch's 25 threshold. Game thread.
void Deliver(uint8_t slot, const coop::net::PlayerHitPayload& payload);

// Client: the wire. Checks the format only, as the host is the author: the verb 0 or 1, the bone under 6, a finite
// non-negative damage, every vector component finite and within 1e7. A payload that passes is applied on the game
// thread, if it names this machine's player. Any thread.
void OnReceived(const uint8_t* data, size_t len);

// The index of a bone name in the wire's table (None, chest, head, pelvis, torso, lowlegs); 0, None, for any other
// name, said once. What punch's `bn1` travels as. Game thread.
uint8_t BoneIndex(const std::wstring& name);

// True while Apply's verb runs on this machine's own player, so a watch on that verb can tell the delivered hit from
// a hit this machine made itself. Game thread.
bool InApply();

// [dev], for the drill alone: the hits applied to this machine's own player.
uint32_t AppliedCount();

}  // namespace coop::player_hit
