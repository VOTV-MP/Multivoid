// coop/player_damage.h -- who takes a hit. Each machine keeps exactly its own player's damage
// (MTA's victim-authoritative shape, reference/mtasa-blue/ CNetAPI.cpp): the physical impact
// system dispatches impactDamage, impactDamageCPP and impactSquishCPP on the hit body, and the BP
// writes the per-machine saveSlot.health whatever body ran it, so a contact resolved against a
// puppet in the host's world drained the host; a PRE interceptor cancels those three entries
// whenever `self` is not the local possessed player (a puppet, or the skin-preview mannequin),
// while the victim's own machine computes the same contact natively. The reliable host-to-owner
// PlayerDamage relay below is the send-and-apply half for host-detected enemy hits; no enemy
// detection hook drives it yet, and the test entry does.

#pragma once

namespace coop::net { class Session; struct PlayerDamagePayload; }

namespace coop::player_damage {

// Store the session pointer for SendPlayerDamage; idempotent, nullptr disables sends. Game thread.
void Install(coop::net::Session* session);

// Per tick, game thread: lazily resolve mainPlayer_C's three impact entries and register the
// non-local-body PRE cancel; throttled, cheap once latched. Called from subsystems::TickGameplay.
void Tick();

// Owner side: validate the payload, verify on the game thread that it addresses this peer
// (targetElementId is our Player Element id) and run "Add Player Damage" on our own possessed
// mainPlayer_C, so our armour and inventory BP mitigate the hit. The host-only trust gate is
// event_feed's, before this.
void OnWireDamage(const coop::net::PlayerDamagePayload& p);

// Host side: send a PlayerDamage event to peer `ownerSlot`, targetElementId stamped from that
// slot's Player Element. No-op (logged) if the session is unset, the slot invalid or without a
// Player Element, or the amount non-finite or out of range. Game thread.
void SendPlayerDamage(int ownerSlot, float damage);

// Test entry (host): confirm slot `ownerSlot` has a puppet, then SendPlayerDamage; true if a send
// was attempted. Drives the relay without an enemy. Game thread.
bool DebugForceHitPuppet(int ownerSlot, float damage);

}  // namespace coop::player_damage
