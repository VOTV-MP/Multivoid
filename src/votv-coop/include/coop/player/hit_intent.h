// coop/player/hit_intent.h -- a client's hit on an entity the host owns, as an intent the host judges.
//
// A client's swing (mainPlayer_C::attack) ends in the struck actor's own addDamage, with the player as the damager.
// On a client the struck entity is a mirror of the host's, whose own damage would land nowhere or diverge. The
// mirror's addDamage is watched instead: a target claims the call when this machine's own player is the damager --
// a parked creature (coop/creatures/hit_target_creature), a prop this client does not author (coop/props/
// hit_target_prop). The client then sends HitIntent -- the target's element id, the damage and impact the swing
// claimed and the point it struck -- and cancels the call. The target's health and death stay the host's.
//
// The host queues a client's intents (cap 8, a pace of 4 then 2 a second, which bounds a swing rate), captures the
// sender's eye and newest streamed aim from its puppet when the intent arrives, and serves one head a tick through
// coop/player/hit_judge: the held item, the sender alive, the reach, the facing, the sight and the point on the
// target. A pass runs the target's own addDamage with a null damager, so a client's kill credits no host
// achievement. Nothing is sent back. A refusal is counted, and said once per sender and reason. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net {
class Session;
struct HitIntentPayload;
}  // namespace coop::net

namespace ue_wrap::script_gate {
struct Call;
}  // namespace ue_wrap::script_gate

namespace coop::hit_intent {

// A target's ClientClaim answers with the target's element id, coop::element::kInvalidId when the call is not its hit, or
// this when the call is its hit but there is nothing to send: it is cancelled and no intent goes. It lies past
// kMaxElements, so no element carries it.
inline constexpr uint32_t kClaimedUnsent = 0xFFFFFFFEu;

// What a swing's addDamage call carries. hitLoc is the point the swing struck: the call's hit parameter, else the struck
// actor's location.
struct Swing {
    ue_wrap::FVector hitLoc{};
    ue_wrap::FVector impact{};
    float            damage = 0.f;
};

// CLIENT, for a target's claim: read the swing of an addDamage call whose damager is this machine's own player,
// whoever called (the drill's reflected call too). False when the call's parameters did not resolve (said once) or
// when another actor is the damager.
bool ReadLocalSwing(const ue_wrap::script_gate::Call& c, Swing& out);

// Cache the session; a client takes the addDamage watch (once per process). Called each time the install fanout runs.
void Install(coop::net::Session* session);

// Settle the watch's registration with one line; on a running host, serve each sender's queue head. Game thread.
void Tick();

// HOST: a client's intent, queued with the sender's eye and aim as the host holds them now. A full queue or an
// unknown sender is refused into the count. The format was checked at the receive edge. Game thread.
//
// Precedent. MTA applies a hit on a ped at the machine that syncs it and takes the reported health
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPedSync.cpp:243-275); the attacker's ray is replayed on every
// client that streams the shooter (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPlayer.cpp:260-320: it
// gives the remote ped the weapon and fires the bullet). Source re-traces the swing on the server from its own eye
// (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:292-302). Here the host has no
// copy of the swing's inputs: the Blueprint swing reads the swinging pawn's own weapon data and camera and applies its
// hit to every actor its line trace strikes, and the host's puppet is not known to hold those inputs, so the
// swing is not replayed on it. The client reports the struck point and the host judges it.
void OnIntent(int senderSlot, const coop::net::HitIntentPayload& payload);

// Net disconnect: the once-per-session log latches are forgotten. A sender's queue and counts go with its occupant
// (the roster ledger clears them), not here.
void OnDisconnect();

// [dev] HOST: the hits judged and applied to a creature since the process began.
uint32_t AppliedCount();

// [dev] HOST: how many intents of `slot`'s current occupant were refused for the reason `reason` (compared as text).
uint32_t RefusedCount(int slot, const char* reason);

}  // namespace coop::hit_intent
