// coop/props/hit_target_prop.h -- a prop the host owns as the target of a client's hit (coop/player/hit_intent).
//
// A swing is a player moving a prop, as a throw is, and takes the throw's mechanism. The client half claims a swing at a
// prop mirror this client does not author (coop/props/prop_damage_gate's test): it takes a swing sequence from
// coop/props/prop_throw, sends the intent with it, and lets the call run, so the copy takes the swing's damage and the
// game's own shove at once. The host half judges the swing and elects the sender the prop's syncer
// (coop/props/prop_syncer_host); the sender's simulation and its break then stand, and the host takes the prop back at
// rest. A lethal break before the grant waits for it (prop_damage_gate). A refused swing makes the host drive its own
// copy as a host row, so the swinger's shoved copy parks to it. A swing at a mirror not yet bound to a host element is
// cancelled, lost, and said once. Game thread.

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

namespace coop::hit_target_prop {

// CLIENT: the element id of the prop mirror this addDamage call strikes, when this machine's own player is the damager
// and this client does not author the prop; kInvalidId when the call is not such a hit; coop::hit_intent::kClaimedUnsent
// when it is but the mirror is bound to no element yet (the call is cancelled, nothing is sent). A claim fills `swing`,
// the one read of the call the sender then uses, with the sequence the swing minted. The id is whatever element the
// actor is bound to; the host checks the element's type.
uint32_t ClientClaim(const ue_wrap::script_gate::Call& c, coop::hit_intent::Swing& swing);

// CLIENT: the claimed swing reached the host's queue; said, up to a session's first lines.
void NoteSent(uint32_t eid, float damage, uint16_t seq);

// CLIENT: the claimed swing was not sent; the sequence it minted ends at once.
void NoteUnsent(void* actor);

// HOST: the live prop the element id names, else null.
void* HostTarget(uint32_t eid);

// HOST: the judged swing of the client in `slot` at the prop `eid`: elect that client the prop's syncer, echoing the
// swing's sequence, with the host's own place for the prop as the start. False when the prop is gone, the struck point
// lies more than 100 cm from the prop, or the election was refused (prop_syncer_host counts and says it); `why` names
// the refusal. The host applies no damage.
bool HostApply(uint32_t eid, uint8_t slot, const coop::net::HitIntentPayload& judged, const char*& why);

// HOST: a swing at `eid` was refused after its target resolved, by the judge, the election or the struck point's
// distance from the prop. A prop no row streams is driven as a host row from where it rests; its End brings the
// swinger's shoved copy back, and the line and the count say it only when a row opened. A no-op for an element that
// is no live prop. Game thread.
void OnRefused(uint32_t eid);

// Net disconnect: the once-per-session log latches are forgotten.
void OnDisconnect();

}  // namespace coop::hit_target_prop
