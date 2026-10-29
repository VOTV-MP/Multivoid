// coop/props/prop_damage_gate.h -- a client never decides the damage of a prop it does not author.
//
// A prop's physics impact and fire both end in its own destroy, inside comp_physicsImpact_C: `impactDamage` and
// `fireDamage`. On a client the prop is a mirror of the host's, so its physics runs on a copy the host does not read,
// and the destroy seam would broadcast the result over the host's prop. Two script-gate watches on those bodies cancel
// the call on a connected client when the owner is a prop (prop_C) this client does not author; the host decides the
// prop's damage, and a break reaches the client as the host's destroy.
//
// This client authors the owner when it holds it in a hand grab, threw it (coop/props/prop_throw) or syncs it
// (coop/props/prop_syncer, a pending claim included): its own copy is then the truth and its damage stands. A caller's
// identity is never read: a physics impact carries the struck prop as its actor, and a drill's call nests under our code.

// The watches fail open: a dead one leaves that verb running on every copy, said in one ERROR line. Other owners (the
// ATV, the player, the car, the own-graph breakers) are outside it. The damaged and destroyed sounds and particles the
// component plays inside the cancelled body do not play on a client. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::prop_damage_gate {

// Cache the session; a client takes the two watches (once per process). Called each time the install fanout runs.
void Install(coop::net::Session* session);

// Settle the watches' registration with one line. Game thread.
void Tick();

// Net disconnect: the cancelled counts are said and cleared, and the once-per-class log latches are forgotten.
void OnDisconnect();

}  // namespace coop::props::prop_damage_gate
