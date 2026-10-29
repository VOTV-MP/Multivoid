// coop/creatures/hit_target_creature.h -- a parked creature as the target of a client's hit (coop/player/hit_intent).
//
// The client half claims a swing at a parked family's mirror and plays the family's hit feedback at once, a cosmetic
// prediction. The host half names the creature an intent's element id stands for and runs the creature's own
// addDamage with a null damager. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
struct HitIntentPayload;
}  // namespace coop::net

namespace coop::hit_intent {
struct Swing;
}  // namespace coop::hit_intent

namespace ue_wrap::script_gate {
struct Call;
}  // namespace ue_wrap::script_gate

namespace coop::hit_target_creature {

// CLIENT: the element id of the parked creature mirror this addDamage call strikes, when this machine's own player is
// the damager; else kInvalidId. A claim fills `swing` (the one read of the call the sender then uses) and plays the
// family's hit feedback before it returns.
uint32_t ClientClaim(const ue_wrap::script_gate::Call& c, coop::hit_intent::Swing& swing);

// HOST: the live parked-family creature the element id names, else null.
void* HostTarget(uint32_t eid);

// HOST: run the creature's own addDamage with a null damager, with the damage the judge allowed. False when the call
// did not run.
bool HostApply(uint32_t eid, const coop::net::HitIntentPayload& judged);

// Net disconnect: the once-per-session log latch is forgotten.
void OnDisconnect();

}  // namespace coop::hit_target_creature
