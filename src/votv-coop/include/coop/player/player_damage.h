// coop/player_damage.h -- vitals Inc3-WIRE: reliable host->owner PlayerDamage relay.
//
// Combat loop closure. The host runs the enemies; when a host-side enemy hits peer
// N's PUPPET, the host sends a reliable PlayerDamage event to slot N. N's receiver
// runs "Add Player Damage" on its OWN possessed mainPlayer_C (per-peer-authoritative:
// N's private armor/inventory BP mitigates the hit), so N's saveSlot.health drops ->
// the existing Inc1 health stream + Inc3 hurt-flash fire automatically on every peer.
//
// MTA precedent (reference/mtasa-blue/): victim-authoritative damage -- the victim
// applies + reports its own resulting health; the server trusts + relays
// (CNetAPI.cpp:1238 / CPlayerPuresyncPacket.cpp:325). The ONE inversion: in MTA the
// victim detects its own hit; in our host-relay topology the HOST detects (the enemy
// is host-authoritative) and PUSHES the reliable event to the owner.
//
// SCOPE: this module is the SEND + owner-APPLY half (the "relay"), which is e2e-tested
// via DebugForceHitPuppet (host injects a synthetic hit, no real enemy needed). The
// real enemy->puppet DETECTION hook is DEFERRED behind a runtime probe -- IDA found the
// hook point is BP-bytecode-opaque (enemy melee likely routes through pure-BP addDamage
// whose ProcessEvent-vs-ProcessInternal dispatch can't be determined statically), and
// VOTV enemies don't ambient-spawn. See research/findings/votv-player-vitals-death-
// RE-2026-05-30.md (Inc3-WIRE design section).
//
// 2026-08-29 -- THE IMPACT-ENTRY CANCEL (the "kill a player -> the HOST dies" field
// report). The deferred probe RAN (VERDICT #6, real 2-peer log 01:11): the DIRECT
// entries ("Add Player Damage" / addDamage) are possession-guarded no-ops on an
// unpossessed puppet -- but the IMPACT surface is not those. Physical kills (ATV
// overrun, thrown props, coins) reach the player via the native impact system
// DISPATCHING the BP events impactDamage / impactDamageCPP / impactSquishCPP ON THE
// HIT BODY (native->BP dispatch == ProcessEvent == interceptable), and the BP body
// writes the PER-MACHINE saveSlot.health singleton no matter which body ran it. So a
// contact resolving against a PUPPET on the host's world drained the HOST -- lethal
// contact killed the host and took the server down. The victim's OWN machine computes
// the same contact against its possessed player natively (field-proven: victims DID
// die), so the fix is the MTA victim-authoritative shape with NO new wire: a PRE
// interceptor cancels all three impact entries whenever `self` is NOT the local
// possessed player (a puppet, or the F1 skin-preview mannequin). Each machine keeps
// exactly its own player's damage.

#pragma once

namespace coop::net { class Session; struct PlayerDamagePayload; }

namespace coop::player_damage {

// Store the session pointer for SendPlayerDamage. Idempotent; nullptr disables sends.
// Game thread.
void Install(coop::net::Session* session);

// Game-thread per-tick drive: lazily resolve mainPlayer_C's three impact entries
// (impactDamage / impactDamageCPP / impactSquishCPP) and register the non-local-body
// PRE cancel (see the header block above). Throttled internally; cheap once latched.
// Called from subsystems::TickGameplay.
void Tick();

// OWNER side (receiver). Validates the payload, then on the game thread verifies it
// addresses THIS peer (targetElementId == our local Player Element id) and runs
// "Add Player Damage" on our OWN possessed mainPlayer_C. The senderPeerSlot==0
// host-only trust gate is enforced by the event_feed case BEFORE this is called.
void OnWireDamage(const coop::net::PlayerDamagePayload& p);

// HOST side (sender). Send a PlayerDamage event to peer `ownerSlot` for `damage`,
// stamping targetElementId from that slot's Player Element. No-op (logs) if the
// session is unset, the slot has no Player Element yet, the slot is invalid, or the
// amount is non-finite/out-of-range. This is the entry the future enemy-detection
// hook will call; for now DebugForceHitPuppet drives it. Game thread only.
void SendPlayerDamage(int ownerSlot, float damage);

// e2e/test entry (host): confirm slot `ownerSlot` has a registered puppet (peer
// connected), then SendPlayerDamage(ownerSlot, damage). Returns true if a send was
// attempted. Drives the full relay path WITHOUT a real enemy. Game thread only.
bool DebugForceHitPuppet(int ownerSlot, float damage);

}  // namespace coop::player_damage
