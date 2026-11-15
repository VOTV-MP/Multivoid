// coop/interactables/drone_call_intent.h -- the garage console's call/send button, as a client's intent the host
// presses. Overview: docs/devices.md. Gameplay/network layer (principle 7); the engine is reached through ue_wrap::drone.
//
// The console's one action option dispatches on what the presser is looking at: its keyboard runs drone.triggerFly(console)
// and its other face toggles leaveAfter5min and clicks. The console holds its drone as a LEVEL reference, so every peer's
// console points at its own drone, and a client's is a mirror whose flight tick drone_sync suppresses.
// This lane refuses the client's own body at the script-body gate and sends the press. The console is level-baked and
// keyed by no lane, so the host resolves the one the sender stands at -- that resolve IS the reach test -- re-tests the
// lid, then calls/sends or flips its leaveAfter5min (DroneState carries it) and clicks. The presser's machine clicks for
// the body it cancelled. Nothing to replay on a late join.
// The drone's own sack option (drone_C::actionOptionIndex, action 7) is the third verb: it empties the cargo into a new
// sack, which only the host's drone may do. The host tests that the sender is within reach and that the drone is parked
// with its sack aboard, then runs dropSack. The sack reaches the peers on the prop and container lanes, the cargo on
// drone_cargo_sync.

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
