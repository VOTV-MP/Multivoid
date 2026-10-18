// coop/creatures/creature_hit_intent.h -- a client's hit on a parked creature as an intent the host judges.
//
// A client's swing (mainPlayer_C::attack) ends in the struck actor's own addDamage, with the player as the damager.
// On a client the struck creature is a mirror of the host's, whose Blueprint graph coop/creatures/npc_mirror_brain
// refuses, so the hit would land nowhere. The mirror's addDamage is watched instead: when this machine's own player
// is the damager and the mirror's family is parked (coop/creatures/npc_mirror_brain.h), the client plays the hit's
// feedback at once, a cosmetic prediction (the zombie family's emitter and sound), sends CreatureHitIntent -- the
// creature's element id, the damage and impact the swing claimed and the point it struck -- and cancels the call.
// The creature's health, death and corpse stay the host's: its death reaches the client as the creature's destroy.
//
// The host queues a client's intents (cap 8, a pace of 4 then 2 a second, which bounds a swing rate), captures the
// sender's eye from its puppet when the intent arrives, and serves one head a tick through coop/player/hit_judge:
// the held item, the reach, the facing, the sight and the point on the creature. A pass runs the creature's own
// addDamage with a null damager, so a client's kill credits no host achievement. Nothing is sent back. Each refusal
// is counted per reason and said once per sender and reason. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CreatureHitIntentPayload;
}  // namespace coop::net

namespace coop::creature_hit_intent {

// Cache the session; a client takes the addDamage watch (once per process). Called each time the install fanout runs.
void Install(coop::net::Session* session);

// Settle the watch's registration with one line; on a running host, serve each sender's queue head. Game thread.
void Tick();

// HOST: a client's intent, queued with the sender's eye as the host holds it now. A full queue or an unknown sender
// is refused into the count. The format was checked at the receive edge. Game thread.
//
// Precedent. MTA applies a hit on a ped at the machine that syncs it and takes the reported health
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPedSync.cpp:243-275), and replays the attacker's ray there
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPlayer.cpp:260-320); Source re-traces the swing on the
// server from its own eye (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:292-302).
// Here the Blueprint swing cannot be replayed on the host's puppet, so the client reports the struck point and the
// host judges it.
void OnIntent(int senderSlot, const coop::net::CreatureHitIntentPayload& payload);

// Net disconnect: the once-per-session log latches are forgotten. A sender's queue and counts go with its occupant
// (the roster ledger clears them), not here.
void OnDisconnect();

// [dev] HOST: the hits judged and applied to a creature since the process began.
uint32_t AppliedCount();

// [dev] HOST: how many intents of `slot`'s current occupant were refused for the reason `reason` (compared as text).
uint32_t RefusedCount(int slot, const char* reason);

}  // namespace coop::creature_hit_intent
