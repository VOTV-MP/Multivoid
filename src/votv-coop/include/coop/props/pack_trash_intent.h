// coop/props/pack_trash_intent.h -- bagging a trash pile, as a client's intent the host performs.
// Gameplay/network layer (principle 7); the engine is reached through ue_wrap::prop and
// ue_wrap::engine.
//
// The game's pack is local: a folded bag or a roll used on a pile or a clump spawns a filled bag at
// the target's transform with its chipType, destroys the target and spends the tool. On a client
// that authors nothing the host sees -- a client's prop birth is refused at the door and a pile
// carries no Key a keyed destroy could name -- so the pile stayed and the bag did not cross.
//
// This lane refuses the client's own body at the script-body gate and sends the target's element id
// instead. The host re-tests reach and type, then spawns the bag and destroys the target itself, so
// the prop seams carry both to every peer and into the host's save. The client spends its own tool:
// a hand item and the roll's counter are per-peer state with no lane. Late join: nothing to replay,
// since a pack the host has not run changed nothing.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PackTrashIntentPayload;
}  // namespace coop::net

namespace coop::pack_trash_intent {

// Watch the hand-use verb (client) and cache the session. Idempotent; retried until the gate's
// name resolves. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: keep the gate enabled for this session, consume the tools this peer's own presses
// spent (client), and run one queued pack per peer per token (host). Game thread.
void Tick(coop::net::Session& session);

// HOST: a client asks for a pack. Queued here and performed from Tick under the rate limit.
void OnPackTrashIntent(coop::net::Session& session, const coop::net::PackTrashIntentPayload& payload,
                       uint8_t senderSlot);

// A peer left: its queue and its rate bucket go with it, so the next occupant of the slot starts
// clean.
void OnPeerLeft(uint8_t slot);

// Session teardown: queues, buckets and the pending tool consumptions are dropped.
void OnDisconnect();

}  // namespace coop::pack_trash_intent
