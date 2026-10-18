// coop/player/player_damage.h -- who takes a hit. Each machine keeps exactly its own player's
// damage (MTA's victim-authoritative shape, reference/mtasa-blue/ CNetAPI.cpp): the physical impact
// system dispatches impactDamage, impactDamageCPP and impactSquishCPP on the hit body, and the BP
// writes the per-machine saveSlot.health whatever body ran it, so a contact resolved against a
// puppet in the host's world drained the host; a PRE interceptor cancels those three entries
// whenever `self` is not the local possessed player (a puppet, or the skin-preview mannequin),
// while the victim's own machine computes the same contact natively. The same refusal covers the
// four verbs a hit runs on a puppet -- `Add Player Damage`, `addDamage`, `punch` and `ragdollMode` --
// on every machine, whoever the caller is. On the host, a cancelled `addDamage` or `punch` whose caller
// is a creature of a parked family (coop/creatures/creature_hit.h) is delivered to the puppet's peer
// (coop/player/player_hit.h), which runs the same verb on its own player.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::player_damage {

// Store the session pointer the refusals read their role from; idempotent, nullptr disables
// them. Game thread.
void Install(coop::net::Session* session);

// Per tick, game thread: once mainPlayer_C is loaded, resolve its three impact entries and register
// the non-local-body PRE cancel, settled on that first attempt; register the verb refusals by name
// (settled on the first call); one latched read after. Called from subsystems::TickGameplay.
void Tick();

// The refusals so far on this machine: the four verbs refused on a peer's puppet (all four in one
// count), and ignite. What the dmghazard probe judges by. Any thread.
uint32_t RefusedDamage();
uint32_t RefusedIgnites();

// [dev], for the drill alone. The hits delivered to a peer: every call of player_hit::Deliver this
// watch made. Any thread.
uint32_t DeliveredCount();

// [dev], for the drill alone. On a client, the calls of addDamage or punch on this machine's own player
// whose caller was a creature of a parked family and that player_hit's apply did not make: a mirror
// that still hits. A tripwire; the call runs. Any thread.
uint32_t MirrorHitCount();

// Test entry (host): confirm slot `ownerSlot` has a puppet, then deliver an addDamage of `damage`
// with no source to that peer. True if a delivery was attempted. Drives the relay without a
// creature. Game thread.
bool DebugForceHitPuppet(int ownerSlot, float damage);

}  // namespace coop::player_damage
