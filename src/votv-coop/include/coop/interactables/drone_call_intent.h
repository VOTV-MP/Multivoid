// coop/interactables/drone_call_intent.h -- the garage console's call/send button and the drone's "take the sack off",
// as a client's intents the host runs. Overview: docs/devices.md. Gameplay/network layer (principle 7).
//
// The console's one action option dispatches on what the presser is looking at: its keyboard runs
// drone.triggerFly(console), and its other face toggles leaveAfter5min and clicks. Every peer's console points at its
// own drone, and a client's is a mirror whose flight tick drone_sync suppresses, so a client's press told nobody. This
// lane refuses the client's own body at the script-body gate and sends the press; the console, level-baked and keyed by
// no lane, is the one the host finds the sender standing at (that resolve IS the reach test), its lid re-tested. The
// presser's machine clicks for the body it cancelled. Nothing to replay on a late join.
//
// The drone's own menu rides the same intent: its "take the sack off" (actionOptionIndex 7, dropSack) on a client's
// stopped copy would drop a sack no other peer has, so it is refused there and sent as the sack verb; the host runs its
// own drone's choice for a sender within reach of a parked drone with its sack on.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DroneFlyIntentPayload;
}  // namespace coop::net

namespace coop::drone_call_intent {

// Watch the console's action verb (client) and cache the session. Idempotent; retried until the
// gate's name resolves. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: keep the gate enabled for this session and run one queued press per peer per
// token (host). Game thread.
void Tick(coop::net::Session& session);

// HOST: a client asks for a press. Queued here and performed from Tick under the rate limit.
void OnDroneFlyIntent(coop::net::Session& session, const coop::net::DroneFlyIntentPayload& payload,
                      uint8_t senderSlot);

// Session teardown: the counters are dropped. A sender's queue, its bucket and its said-once latch live in its
// per-slot state and go with its occupant, so the next occupant of the slot starts clean.
void OnDisconnect();

// The presses (either face) this client has sent the host (the drone call drill's proof that a press
// reached the lane's gate). Game thread.
uint64_t SentCount();

}  // namespace coop::drone_call_intent
