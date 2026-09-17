// coop/interactables/drone_call_intent.h -- the garage console's call/send button, as a client's
// intent the host presses. Overview: docs/devices.md. Gameplay/network layer (principle 7); the
// engine is reached through ue_wrap::drone.
//
// The console's one action option dispatches on what the presser is looking at: its keyboard runs
// drone.triggerFly(console), the button behind the "drone is active" line, and its other face
// toggles leaveAfter5min. The console holds its drone as a LEVEL reference, so every peer's
// console points at its own drone -- and a client's is a mirror whose flight tick drone_sync
// suppresses, so a client's press reached a drone that cannot fly and told nobody.
//
// This lane refuses the client's own body at the script-body gate and sends the press. The
// console is not named: level-baked and keyed by no lane, it has neither a save key nor an
// element id, so the host resolves the one the sender stands at -- that resolve IS the reach
// test -- re-tests the lid, and runs the same verb. Nothing to replay on a late join: a press
// the host has not run changed nothing.

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

// A peer left: its queue and its rate bucket go with it, so the next occupant of the slot starts
// clean.
void OnPeerLeft(uint8_t slot);

// Session teardown: the queues and the buckets are dropped.
void OnDisconnect();

}  // namespace coop::drone_call_intent
