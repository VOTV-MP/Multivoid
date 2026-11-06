// coop/player/cremator_hit.h -- a cremator's hit on a player, delivered to the player's own machine.
//
// A cremator is the host's machine. A peer burning in it is hit on the host, where coop/player/player_damage cancels
// the hit on the peer's puppet: the puppet is not the peer's body. The peer's body is its own player on its own
// machine, so the host SENDS the hit there (Deliver) and that machine applies it to its own player (OnReceived), by
// the Blueprint's own split on its own health. MTA's player owns its health: the owner's client reports it
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:1189) and applies the damage to its local player
// (CClientGame.cpp:4543). Source's NPC hits where it runs, on the server
// (reference/source-sdk-2013/src/game/server/hl2/npc_BaseZombie.cpp:1267). The delivery is Source's and the
// ownership MTA's. It diverges from MTA's server-computed health change (SET_ELEMENT_HEALTH,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1771): the amount is the
// Blueprint's rule on the owner's own save slot, not a value the host computes. This is a third
// kind beside PlayerHit because that one names its source by an Npc element id, which a cremator does not have, and
// its apply resolves a source only through the Npc mirrors; widening it would mix a keyed device into the creature
// path. Game thread, but where a function says otherwise.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct CrematorHitPayload;
}  // namespace coop::net

namespace coop::cremator_hit {

// Host: send a cremator's hit to peer `slot` (a client's slot, 1 and up). `key` is the cremator's lane key,
// `verb` a kCrematorHitVerb*, `igniteSeconds` is the ignite verb's fuel and is not read for damage. One reliable send
// per hit, never coalesced. True when the hit was handed to the session's send; false when nothing was sent (an empty
// or over-long key, a bad slot, or the session refused: a slot that is not world-ready, a full backlog), each counted
// and said once per reason. Game thread.
bool Deliver(coop::net::Session& session, uint8_t slot, const std::wstring& key, uint8_t verb, float igniteSeconds);

// Client: the wire, checked by the dispatcher for its role, sender, size, verb and key length. A non-finite or
// negative ignite time is dropped. Posts the apply to the game thread, where a hit that finds no live player is
// dropped and counted. A damage whose cremator key this machine's lane does not resolve is applied with no source,
// counted and said once per key: the cremator is the damage's attribution, and the amount does not read it. A sourceless
// lethal hit lands (health, death, the achievement) with no punch and no source text, since the Blueprint's addDamage
// takes both only from a valid source actor. Any thread.
void OnReceived(const coop::net::CrematorHitPayload& payload);

// Session end, either role: says the hits sent, refused, applied (and applied with no source), dropped and failed in
// one line when any is non-zero, then forgets the counts and the once-per-session latches. Game thread.
void OnDisconnect();

}  // namespace coop::cremator_hit
