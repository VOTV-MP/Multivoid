// coop/props/hit_target_prop.h -- a prop the host owns as the target of a client's hit (coop/player/hit_intent).
//
// The client half claims a swing at a prop mirror this client does not author (coop/props/prop_damage_gate's test):
// the swing reaches the prop's own addDamage with the local player as damager, and the client sends it to the host and
// cancels it, so its copy neither takes the damage nor breaks. The host half names the prop an intent's element id
// stands for and runs the prop's own addDamage with a null damager, so the prop's graph decides the break on the
// authority. A swing at a mirror not yet bound to a host element is lost and said once. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
struct HitIntentPayload;
}  // namespace coop::net

namespace ue_wrap::script_gate {
struct Call;
}  // namespace ue_wrap::script_gate

namespace coop::hit_target_prop {

// CLIENT: the element id of the prop mirror this addDamage call strikes, when this machine's own player is the damager
// and this client does not author the prop; kInvalidId when the call is not such a hit; coop::hit_intent::kClaimedUnsent
// when it is but the mirror is bound to no element yet (the call is cancelled, nothing is sent).
uint32_t ClientClaim(const ue_wrap::script_gate::Call& c);

// HOST: the live prop the element id names, else null.
void* HostTarget(uint32_t eid);

// HOST: run the prop's own addDamage with a null damager, with the damage the judge allowed. False when the call did
// not run.
bool HostApply(uint32_t eid, const coop::net::HitIntentPayload& judged);

// Net disconnect: the once-per-session log latches are forgotten.
void OnDisconnect();

}  // namespace coop::hit_target_prop
